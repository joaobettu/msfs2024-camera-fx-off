// End-to-end test of the add-on inside a real ReShade, on WARP (software, the default) or on the GPU.
// Runs from a folder holding a copy of ReShade's dxgi.dll and the add-on. It writes the add-on settings and a .twins file,
// draws stand-ins of the MSFS shaders and reads the pixels back.
//   fixture [--adapter=gpu] --sun-flare-off=1 --bloom-off=1 [--keep-glow=1 --keep-star=0 --keep-ghosts=0]
//       stand-in shader package with a no-bloom twin: checks the twin swap. Keep settings left out use the add-on defaults.
//   fixture [--adapter=gpu] --pak-next-to-exe ...
//       the stand-in shader package in a Shaders folder next to this executable and no pak_path in the settings: the
//       add-on's default lookup, as players have it with the game
//   fixture [--adapter=gpu] --real-pak=<SHADERSPAK.*.SM6.PC.PAK> --real-twins=<.twins> ...
//       the game's shader package and twins file: also builds a pipeline from the game's own bloom tonemapping shader and
//       checks the add-on creates the real no-bloom twin (pipeline only, never drawn)

#include "../src/classify.hpp"
#include "../src/twins.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

#include "ps_flare.h"
#include "ps_light.h"
#include "ps_material.h"
#include "ps_material_b.h"
#include "ps_material_c.h"
#include "ps_tonemap_bloom.h"
#include "ps_tonemap_plain.h"
#include "vs_flare.h"
#include "vs_fullscreen.h"
#include "vs_fullscreen_tex.h"
#include "vs_light.h"
#include "vs_material.h"

using Microsoft::WRL::ComPtr;

namespace
{
	int g_failures = 0;

	void check(bool condition, const char *what)
	{
		printf("%s: %s\n", condition ? "ok  " : "FAIL", what);
		if (!condition)
			++g_failures;
	}

	void check(bool condition, const std::string &what)
	{
		check(condition, what.c_str());
	}

	void require(HRESULT hr, const char *what)
	{
		if (FAILED(hr))
		{
			printf("FAIL: %s (hr 0x%08lX)\n", what, static_cast<unsigned long>(hr));
			ExitProcess(1);
		}
	}

	std::wstring exe_directory()
	{
		wchar_t path[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir = path;
		return dir.substr(0, dir.find_last_of(L"\\/") + 1);
	}

	bool write_file(const std::wstring &path, const void *data, size_t size)
	{
		FILE *file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
			return false;
		const bool ok = fwrite(data, 1, size, file) == size;
		fclose(file);
		return ok;
	}

	std::string read_text(const std::wstring &path)
	{
		// Shared open: the add-on still has its log open for writing.
		std::string text;
		if (FILE *file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO))
		{
			char buffer[4096];
			for (size_t n; (n = fread(buffer, 1, sizeof(buffer), file)) != 0;)
				text.append(buffer, n);
			fclose(file);
		}
		return text;
	}

	// The log line holding 'needle', empty if there is none.
	std::string log_line(const std::string &log, const std::string &needle)
	{
		const size_t at = log.find(needle);
		if (at == std::string::npos)
			return {};
		const size_t begin = log.rfind('\n', at);
		const size_t end = log.find('\n', at);
		return log.substr(begin == std::string::npos ? 0 : begin + 1, end == std::string::npos ? std::string::npos : end - (begin == std::string::npos ? 0 : begin + 1));
	}

	std::string digest_hex(const unsigned char *code)
	{
		camfx::digest16 d;
		dxbc::digest(code, 32, d.bytes);
		return camfx::to_hex(d);
	}

	uint32_t container_size(const unsigned char *code)
	{
		uint32_t size;
		std::memcpy(&size, code + 24, 4);
		return size;
	}

	std::string narrow(const std::wstring &s)
	{
		std::string result(WideCharToMultiByte(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr), '\0');
		WideCharToMultiByte(CP_ACP, 0, s.data(), int(s.size()), result.data(), int(result.size()), nullptr, nullptr);
		return result;
	}

	// Pipeline stream for ID3D12Device2::CreatePipelineState, the path MSFS uses for many of its pipelines.
	// Each subobject is pointer aligned, as D3D12 requires, which pads some of them on purpose.
#pragma warning(push)
#pragma warning(disable : 4324)
	template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
	struct alignas(void *) stream_subobject
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
		T value = {};
	};
#pragma warning(pop)

	// The first tonemapping permutation with bloom in the game's shader package.
	struct real_shaders
	{
		std::vector<uint8_t> pak;
		const uint8_t *tonemap_ps = nullptr;
	};

	bool load_real_shaders(const std::wstring &path, real_shaders &out)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(file, &size) != FALSE;
		out.pak.resize(ok ? size_t(size.QuadPart) : 0);
		for (size_t done = 0; ok && done < out.pak.size();)
		{
			DWORD read = 0;
			const DWORD chunk = DWORD(out.pak.size() - done < (64u << 20) ? out.pak.size() - done : (64u << 20));
			ok = ReadFile(file, out.pak.data() + done, chunk, &read, nullptr) && read == chunk;
			done += read;
		}
		CloseHandle(file);
		if (!ok)
			return false;

		const std::vector<uint8_t> &pak = out.pak;
		for (size_t i = 0; i + 32 <= pak.size() && out.tonemap_ps == nullptr; ++i)
		{
			if (pak[i] != 'D' || std::memcmp(&pak[i], "DXBC", 4) != 0)
				continue;
			uint16_t major;
			uint32_t container, count;
			std::memcpy(&major, &pak[i + 20], 2);
			std::memcpy(&container, &pak[i + 24], 4);
			std::memcpy(&count, &pak[i + 28], 4);
			if (major != 1 || count < 1 || count > 32 || container < 32 || i + container > pak.size())
				continue;
			if (camfx::classify_pixel_shader(&pak[i], container) == camfx::pso_kind::tonemap_bloom)
				out.tonemap_ps = &pak[i];
			i += container - 1;
		}
		return out.tonemap_ps != nullptr;
	}
}

int wmain(int argc, wchar_t **argv)
{
	// Keep settings at -1 are left out of the .ini, so the add-on uses its defaults.
	int sun_flare_off = 1, bloom_off = 1, keep_glow = -1, keep_star = -1, keep_ghosts = -1;
	bool gpu = false, pak_next_to_exe = false;
	std::wstring real_pak, real_twins;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		swscanf_s(argv[i], L"--sun-flare-off=%d", &sun_flare_off);
		swscanf_s(argv[i], L"--bloom-off=%d", &bloom_off);
		swscanf_s(argv[i], L"--keep-glow=%d", &keep_glow);
		swscanf_s(argv[i], L"--keep-star=%d", &keep_star);
		swscanf_s(argv[i], L"--keep-ghosts=%d", &keep_ghosts);
		if (arg == L"--adapter=gpu")
			gpu = true;
		if (arg == L"--pak-next-to-exe")
			pak_next_to_exe = true;
		if (arg.rfind(L"--real-pak=", 0) == 0)
			real_pak = arg.substr(11);
		if (arg.rfind(L"--real-twins=", 0) == 0)
			real_twins = arg.substr(13);
	}
	const bool real = !real_pak.empty();
	// What the add-on should use: only the glow is kept by default.
	const bool glow_kept = keep_glow != 0, star_kept = keep_star > 0, ghosts_kept = keep_ghosts > 0;
	printf("fixture: %s, sun_flare_off=%d keep_glow=%d keep_star=%d keep_ghosts=%d%s bloom_off=%d%s%s\n",
		gpu ? "GPU" : "WARP", sun_flare_off, int(glow_kept), int(star_kept), int(ghosts_kept),
		keep_glow < 0 && keep_star < 0 && keep_ghosts < 0 ? " (defaults)" : "", bloom_off, real ? ", game shaders" : "",
		pak_next_to_exe ? ", shader package in Shaders next to the executable" : "");

	// Add-on inputs, written before ReShade loads it.
	const std::wstring dir = exe_directory();
	std::wstring pak_path;
	std::string twins;
	real_shaders game;
	camfx::twin_table game_twins;
	if (real)
	{
		pak_path = real_pak;
		twins = read_text(real_twins);
		std::string error;
		require(camfx::parse_twin_table(twins, game_twins, error) ? S_OK : E_FAIL, "parse the game's .twins file");
		require(load_real_shaders(real_pak, game) ? S_OK : E_FAIL, "find a bloom tonemapping shader in the game's package");
	}
	else
	{
		if (pak_next_to_exe)
			CreateDirectoryW((dir + L"Shaders").c_str(), nullptr);
		pak_path = dir + (pak_next_to_exe ? L"Shaders\\fixture.pak" : L"fixture.pak");
		constexpr uint32_t twin_offset = 4096;
		std::vector<unsigned char> pak(twin_offset, 0xCD);
		pak.insert(pak.end(), g_ps_tonemap_plain, g_ps_tonemap_plain + container_size(g_ps_tonemap_plain));
		pak.insert(pak.end(), 100, 0xCD);
		require(write_file(pak_path, pak.data(), pak.size()) ? S_OK : E_FAIL, "write fixture.pak");
		twins = "# fixture\npak fixture.pak " + std::to_string(pak.size()) + "\n" +
			digest_hex(g_ps_tonemap_bloom) + " " + digest_hex(g_ps_tonemap_plain) + " " + std::to_string(twin_offset) + " " + std::to_string(container_size(g_ps_tonemap_plain)) + "\n";
	}
	require(write_file(dir + L"msfs2024-camera-fx-off.twins", twins.data(), twins.size()) ? S_OK : E_FAIL, "write .twins");

	// Keys of earlier versions are in there too, and must do nothing (light_glow_off used to hide the lights).
	std::string ini = "[settings]\r\nsun_flare_off=" + std::to_string(sun_flare_off) + "\r\nbloom_off=" + std::to_string(bloom_off) +
		"\r\nlight_glow_off=1\r\nlight_point_off=1\r\nflare_keep=" + digest_hex(g_vs_material).substr(0, 8) + ":" + digest_hex(g_ps_material_c).substr(0, 8) + "\r\n";
	if (!pak_next_to_exe)
		ini += "pak_path=" + narrow(pak_path) + "\r\n";
	if (keep_glow >= 0)
		ini += "keep_glow=" + std::to_string(keep_glow) + "\r\n";
	if (keep_star >= 0)
		ini += "keep_star=" + std::to_string(keep_star) + "\r\n";
	if (keep_ghosts >= 0)
		ini += "keep_ghosts=" + std::to_string(keep_ghosts) + "\r\n";
	require(write_file(dir + L"msfs2024-camera-fx-off.ini", ini.data(), ini.size()) ? S_OK : E_FAIL, "write .ini");
	DeleteFileW((dir + L"msfs2024-camera-fx-off.log").c_str());

	// Creating the factory goes through ReShade (the dxgi.dll next to this executable), which then hooks D3D12.
	ComPtr<IDXGIFactory6> factory;
	require(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
	ComPtr<IDXGIAdapter1> adapter;
	if (gpu)
		require(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "EnumAdapterByGpuPreference");
	else
		require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
	DXGI_ADAPTER_DESC1 adapter_desc = {};
	adapter->GetDesc1(&adapter_desc);
	printf("adapter: %S\n", adapter_desc.Description);
	if (gpu && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
		require(E_FAIL, "a hardware adapter");
	ComPtr<ID3D12Device2> device;
	require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
	check(GetModuleHandleW(L"msfs2024-camera-fx-off.addon64") != nullptr, "ReShade loaded the add-on");

	// Root signature with the lens dirt texture at t8, like the game's tonemapping pass.
	D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8, 0, 0 };
	D3D12_ROOT_PARAMETER parameter = {};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameter.DescriptorTable = { 1, &range };
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	const D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	ComPtr<ID3DBlob> root_blob, root_error;
	require(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &root_error), "D3D12SerializeRootSignature");
	ComPtr<ID3D12RootSignature> root;
	require(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

	D3D12_BLEND_DESC blend = {};
	for (D3D12_RENDER_TARGET_BLEND_DESC &rt : blend.RenderTarget)
		rt = { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
	D3D12_RASTERIZER_DESC raster = {};
	raster.FillMode = D3D12_FILL_MODE_SOLID;
	raster.CullMode = D3D12_CULL_MODE_NONE;
	raster.DepthClipEnable = TRUE;
	const D3D12_DEPTH_STENCILOP_DESC keep = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
	const D3D12_DEPTH_STENCIL_DESC no_depth = { FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, FALSE, 0xFF, 0xFF, keep, keep };

	const auto make_pso = [&](ID3D12RootSignature *signature, const unsigned char *vs, const unsigned char *ps, DXGI_FORMAT format, DXGI_FORMAT depth_format, const char *what) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		desc.pRootSignature = signature;
		desc.VS = { vs, container_size(vs) };
		desc.PS = { ps, container_size(ps) };
		desc.BlendState = blend;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState = raster;
		desc.DepthStencilState = no_depth;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = format;
		desc.DSVFormat = depth_format;
		desc.SampleDesc = { 1, 0 };
		ComPtr<ID3D12PipelineState> pso;
		require(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), what);
		return pso;
	};
	constexpr DXGI_FORMAT ldr = DXGI_FORMAT_R8G8B8A8_UNORM, hdr = DXGI_FORMAT_R11G11B10_FLOAT, depth = DXGI_FORMAT_D32_FLOAT, none = DXGI_FORMAT_UNKNOWN;
	const ComPtr<ID3D12PipelineState> pso_glow = make_pso(root.Get(), g_vs_flare, g_ps_flare, ldr, none, "create stand-in light glow pipeline");
	const ComPtr<ID3D12PipelineState> pso_point = make_pso(root.Get(), g_vs_light, g_ps_light, ldr, none, "create stand-in light point pipeline");
	const ComPtr<ID3D12PipelineState> pso_tonemap = make_pso(root.Get(), g_vs_fullscreen, g_ps_tonemap_bloom, ldr, none, "create stand-in tonemapping pipeline");
	// Lens flare stand-ins: three elements with shaders the add-on does not know (A, B, C).
	const ComPtr<ID3D12PipelineState> pso_material = make_pso(root.Get(), g_vs_material, g_ps_material, hdr, none, "create stand-in material pipeline A");
	const ComPtr<ID3D12PipelineState> pso_material_b = make_pso(root.Get(), g_vs_material, g_ps_material_b, hdr, none, "create stand-in material pipeline B");
	const ComPtr<ID3D12PipelineState> pso_material_c = make_pso(root.Get(), g_vs_material, g_ps_material_c, hdr, none, "create stand-in material pipeline C");
	const ComPtr<ID3D12PipelineState> pso_material_depth = make_pso(root.Get(), g_vs_material, g_ps_material, hdr, depth, "create stand-in material pipeline with depth");

	// A second light glow pipeline built from a pipeline state stream.
	struct
	{
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
		stream_subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
	} stream;
	stream.root.value = root.Get();
	stream.vs.value = { g_vs_flare, container_size(g_vs_flare) };
	stream.ps.value = { g_ps_flare, container_size(g_ps_flare) };
	stream.blend.value = blend;
	stream.raster.value = raster;
	stream.depth.value = no_depth;
	stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	stream.formats.value.NumRenderTargets = 1;
	stream.formats.value.RTFormats[0] = ldr;
	const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = { sizeof(stream), &stream };
	ComPtr<ID3D12PipelineState> pso_glow_stream;
	require(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso_glow_stream)), "CreatePipelineState (stream)");

	// The game's own bloom tonemapping shader, with a root signature that covers every register it uses (it is never drawn).
	ComPtr<ID3D12RootSignature> game_root;
	ComPtr<ID3D12PipelineState> pso_game_tonemap;
	if (real)
	{
		const D3D12_DESCRIPTOR_RANGE resources[2] = {
			{ D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
			{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND } };
		const D3D12_DESCRIPTOR_RANGE samplers = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0 };
		D3D12_ROOT_PARAMETER parameters[2] = {};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[0].DescriptorTable = { 2, resources };
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[1].DescriptorTable = { 1, &samplers };
		const D3D12_ROOT_SIGNATURE_DESC game_root_desc = { 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ComPtr<ID3DBlob> blob;
		require(D3D12SerializeRootSignature(&game_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &root_error), "serialize root signature for the game's shader");
		require(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&game_root)), "CreateRootSignature (game shader)");
		pso_game_tonemap = make_pso(game_root.Get(), g_vs_fullscreen_tex, game.tonemap_ps, ldr, none, "create pipeline from the game's bloom tonemapping shader");
	}

	// Targets (4x4): light glow, light glow through ExecuteIndirect, light point, tonemap (LDR), then HDR ones for the
	// material quads:
	// - the lens flare: one target bound once, then quads of A, B, C and C, each in its own column of pixels;
	// - a quad with depth bound (transparent geometry) and a 3-vertex draw, both never lens flare.
	enum : UINT { glow_target, glow_indirect_target, point_target, tonemap_target, flare_target, flare_depth_target, flare_triangle_target, targets };
	constexpr UINT size = 4;
	D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_HEAP_PROPERTIES upload_heap = { D3D12_HEAP_TYPE_UPLOAD };
	D3D12_HEAP_PROPERTIES readback_heap = { D3D12_HEAP_TYPE_READBACK };
	const auto target_format = [&](UINT i) { return i >= flare_target ? hdr : ldr; };

	D3D12_RESOURCE_DESC texture_desc = {};
	texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texture_desc.Width = size;
	texture_desc.Height = size;
	texture_desc.DepthOrArraySize = 1;
	texture_desc.MipLevels = 1;
	texture_desc.SampleDesc = { 1, 0 };
	texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets + 1 };
	ComPtr<ID3D12DescriptorHeap> rtv_heap;
	require(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap (RTV)");
	const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	const auto rtv = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * rtv_step; return h; };

	const float black[4] = { 0, 0, 0, 0 };
	ComPtr<ID3D12Resource> target[targets];
	for (UINT i = 0; i < targets; ++i)
	{
		texture_desc.Format = target_format(i);
		D3D12_CLEAR_VALUE clear = { texture_desc.Format, { 0, 0, 0, 0 } };
		require(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target[i])), "create target");
		device->CreateRenderTargetView(target[i].Get(), nullptr, rtv(i));
	}

	ComPtr<ID3D12Resource> dirt;
	texture_desc.Format = ldr;
	texture_desc.Width = texture_desc.Height = 1;
	require(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&dirt)), "create lens dirt");
	device->CreateRenderTargetView(dirt.Get(), nullptr, rtv(targets));
	D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	ComPtr<ID3D12DescriptorHeap> srv_heap;
	require(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap)), "CreateDescriptorHeap (SRV)");
	device->CreateShaderResourceView(dirt.Get(), nullptr, srv_heap->GetCPUDescriptorHandleForHeapStart());

	D3D12_RESOURCE_DESC depth_desc = texture_desc;
	depth_desc.Width = depth_desc.Height = size;
	depth_desc.Format = depth;
	depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE depth_clear = { depth, {} };
	depth_clear.DepthStencil = { 1.0f, 0 };
	ComPtr<ID3D12Resource> depth_buffer;
	require(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&depth_buffer)), "create depth buffer");
	D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	ComPtr<ID3D12DescriptorHeap> dsv_heap;
	require(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)), "CreateDescriptorHeap (DSV)");
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateDepthStencilView(depth_buffer.Get(), nullptr, dsv);

	D3D12_RESOURCE_DESC buffer_desc = {};
	buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
	buffer_desc.SampleDesc = { 1, 0 };
	buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	constexpr UINT row_pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	buffer_desc.Width = UINT64(targets) * size * row_pitch;
	ComPtr<ID3D12Resource> readback;
	require(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create readback buffer");

	buffer_desc.Width = sizeof(D3D12_DRAW_ARGUMENTS);
	ComPtr<ID3D12Resource> arguments;
	require(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&arguments)), "create indirect arguments");
	{
		void *data = nullptr;
		require(arguments->Map(0, nullptr, &data), "map indirect arguments");
		const D3D12_DRAW_ARGUMENTS draw = { 3, 1, 0, 0 };
		std::memcpy(data, &draw, sizeof(draw));
		arguments->Unmap(0, nullptr);
	}
	D3D12_INDIRECT_ARGUMENT_DESC argument = {};
	argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
	const D3D12_COMMAND_SIGNATURE_DESC signature_desc = { sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument, 0 };
	ComPtr<ID3D12CommandSignature> signature;
	require(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)), "CreateCommandSignature");

	// Record the draws as the game would issue them.
	D3D12_COMMAND_QUEUE_DESC queue_desc = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	ComPtr<ID3D12CommandQueue> queue;
	require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
	ComPtr<ID3D12CommandAllocator> allocator;
	require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
	ComPtr<ID3D12GraphicsCommandList> list;
	require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");

	const float gray[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
	for (UINT i = 0; i < targets; ++i)
		list->ClearRenderTargetView(rtv(i), black, 0, nullptr);
	list->ClearRenderTargetView(rtv(targets), gray, 0, nullptr);
	list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = { dirt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
	list->ResourceBarrier(1, &barrier);

	// Binding the game's bloom tonemapping pipeline makes the add-on bind the real no-bloom twin in its place (nothing is drawn with it).
	if (real)
		list->SetPipelineState(pso_game_tonemap.Get());

	ID3D12DescriptorHeap *const heaps[] = { srv_heap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetGraphicsRootSignature(root.Get());
	list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
	const D3D12_VIEWPORT viewport = { 0, 0, float(size), float(size), 0, 1 };
	const D3D12_RECT scissor = { 0, 0, LONG(size), LONG(size) };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);

	const auto draw_into = [&](UINT i, bool with_depth, ID3D12PipelineState *pso) {
		const D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv(i);
		list->OMSetRenderTargets(1, &handle, FALSE, with_depth ? &dsv : nullptr);
		list->SetPipelineState(pso);
	};

	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	draw_into(glow_target, false, pso_glow.Get());
	list->DrawInstanced(3, 1, 0, 0);
	draw_into(glow_indirect_target, false, pso_glow_stream.Get());
	list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
	draw_into(point_target, false, pso_point.Get());
	list->DrawInstanced(3, 1, 0, 0);
	draw_into(tonemap_target, false, pso_tonemap.Get());
	list->DrawInstanced(3, 1, 0, 0);

	// The lens flare, as the game draws it: the target bound once, then the glow, the star rays and the ghosts.
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const auto quad_in_column = [&](UINT column) {
		const D3D12_VIEWPORT one_column = { float(column), 0, 1, float(size), 0, 1 };
		list->RSSetViewports(1, &one_column);
		list->DrawInstanced(4, 1, 0, 0);
	};
	draw_into(flare_target, false, pso_material.Get());
	quad_in_column(0);
	list->SetPipelineState(pso_material_b.Get());
	quad_in_column(1);
	list->SetPipelineState(pso_material_c.Get());
	quad_in_column(2);
	quad_in_column(3);
	list->RSSetViewports(1, &viewport);

	draw_into(flare_depth_target, true, pso_material_depth.Get());
	list->DrawInstanced(4, 1, 0, 0);
	draw_into(flare_triangle_target, false, pso_material.Get());
	list->DrawInstanced(3, 1, 0, 0);

	for (UINT i = 0; i < targets; ++i)
	{
		barrier.Transition = { target[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
		list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION destination = { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		destination.PlacedFootprint = { UINT64(i) * size * row_pitch, { target_format(i), size, size, 1, row_pitch } };
		D3D12_TEXTURE_COPY_LOCATION source = { target[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		source.SubresourceIndex = 0;
		list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	}
	require(list->Close(), "Close");

	ID3D12CommandList *const lists[] = { list.Get() };
	queue->ExecuteCommandLists(1, lists);
	ComPtr<ID3D12Fence> fence;
	require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
	const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	require(queue->Signal(fence.Get(), 1), "Signal");
	require(fence->SetEventOnCompletion(1, done), "SetEventOnCompletion");
	check(WaitForSingleObject(done, 30000) == WAIT_OBJECT_0, "GPU work finished");
	CloseHandle(done);
	check(device->GetDeviceRemovedReason() == S_OK, "device not removed");

	// Pixels as 4 bytes (RGBA8, or the packed R11G11B10 bits): (x, 1) of a target.
	void *mapped = nullptr;
	require(readback->Map(0, nullptr, &mapped), "map readback");
	const auto *const pixels = static_cast<const unsigned char *>(mapped);
	const auto pixel = [&](UINT i, UINT x = 1) { const unsigned char *p = pixels + i * size * row_pitch + row_pitch + x * 4; return std::vector<int> { p[0], p[1], p[2], p[3] }; };
	const auto show = [](const std::vector<int> &p) { char text[48]; snprintf(text, sizeof(text), "(%d, %d, %d, %d)", p[0], p[1], p[2], p[3]); return std::string(text); };

	const std::vector<int> glow = pixel(glow_target), glow_indirect = pixel(glow_indirect_target), point = pixel(point_target), tonemap = pixel(tonemap_target);
	const std::vector<int> flare_glow = pixel(flare_target, 0), flare_star = pixel(flare_target, 1), flare_ghost_1 = pixel(flare_target, 2), flare_ghost_2 = pixel(flare_target, 3);
	const std::vector<int> flare_depth = pixel(flare_depth_target), flare_triangle = pixel(flare_triangle_target);
	printf("light glow %s, light glow (indirect) %s, light point %s, tonemap %s\n", show(glow).c_str(), show(glow_indirect).c_str(), show(point).c_str(), show(tonemap).c_str());
	printf("lens flare: glow %s, star rays %s, ghosts %s %s; material quad with depth %s; triangle %s\n", show(flare_glow).c_str(), show(flare_star).c_str(),
		show(flare_ghost_1).c_str(), show(flare_ghost_2).c_str(), show(flare_depth).c_str(), show(flare_triangle).c_str());

	const std::vector<int> nothing = { 0, 0, 0, 0 }, red = { 255, 0, 0, 255 }, green = { 0, 255, 0, 255 };
	check(glow == red, "light glow draw kept");
	check(glow_indirect == red, "light glow ExecuteIndirect kept (stream pipeline)");
	check(point == green, "light point draw kept");
	const auto drawn = [&](bool kept) { return std::string(!sun_flare_off ? "drawn (lens flare removal off)" : kept ? "kept" : "removed"); };
	check((flare_glow != nothing) == (!sun_flare_off || glow_kept), "lens flare glow (first quad) " + drawn(glow_kept));
	check((flare_star != nothing) == (!sun_flare_off || star_kept), "lens flare star rays (second quad) " + drawn(star_kept));
	check((flare_ghost_1 != nothing) == (!sun_flare_off || ghosts_kept) && (flare_ghost_2 != nothing) == (!sun_flare_off || ghosts_kept), "lens flare ghosts (third and fourth quads) " + drawn(ghosts_kept));
	check(flare_depth != nothing, "material quad with depth bound kept");
	check(flare_triangle != nothing, "material 3-vertex draw kept");
	const bool twin_ran = tonemap == std::vector<int> { 0, 0, 255, 255 };
	const bool bloom_ran = tonemap[0] == 255 && tonemap[1] == 0 && (tonemap[2] == 127 || tonemap[2] == 128) && tonemap[3] == 255;
	readback->Unmap(0, nullptr);

	const std::string log = read_text(dir + L"msfs2024-camera-fx-off.log");
	if (real)
	{
		// The stand-in tonemapping permutation is not in the game's twins file: the add-on must leave it alone.
		check(bloom_ran, "tonemapping permutation without a twin keeps rendering as is");
		check(log.find("no pair for tonemapping variant " + digest_hex(g_ps_tonemap_bloom)) != std::string::npos, "add-on reported the permutation without a twin");

		camfx::digest16 bloom_digest;
		dxbc::digest(game.tonemap_ps, 32, bloom_digest.bytes);
		const auto pair = game_twins.entries.find(bloom_digest);
		check(pair != game_twins.entries.end(), "the game's bloom permutation is in the twins file");
		if (pair != game_twins.entries.end())
			check(log.find("tonemapping " + digest_hex(game.tonemap_ps) + " uses no-bloom twin " + camfx::to_hex(pair->second.twin)) != std::string::npos,
				"add-on created the game's no-bloom twin from the shader package");
	}
	else
	{
		check(bloom_off ? twin_ran : bloom_ran, bloom_off ? "tonemapping ran the no-bloom twin read from the package" : "tonemapping kept the bloom permutation");
		check(log.find("uses no-bloom twin") != std::string::npos, "add-on paired the bloom permutation with its twin");
	}

	// People attach the log to public bug reports: it names files only, never the folders they are in.
	check(log.find(":\\") == std::string::npos && log.find(":/") == std::string::npos, "the log names no folders");

	char settings[96];
	snprintf(settings, sizeof(settings), "settings: sun_flare_off=%d keep_glow=%d keep_star=%d keep_ghosts=%d bloom_off=%d ",
		sun_flare_off, int(glow_kept), int(star_kept), int(ghosts_kept), bloom_off);
	check(log.find(settings) != std::string::npos, "add-on read the settings (only the glow kept unless set otherwise)");
	// The stand-in elements are not in the add-on's table of known shaders, so it goes by draw order.
	const auto element = [&](const unsigned char *ps, const char *part) {
		const std::string line = log_line(log, "VS:vh_base#" + digest_hex(g_vs_material).substr(0, 8) + " PS:ph_base#" + digest_hex(ps).substr(0, 8) + " has shaders this version does not know");
		return !line.empty() && line.find(std::string("so it is taken as the ") + part) != std::string::npos;
	};
	check(element(g_ps_material, "glow") && element(g_ps_material_b, "star rays") && element(g_ps_material_c, "ghosts"),
		"add-on told the unknown lens flare elements apart by draw order, and logged it");
	printf("---- add-on log ----\n%s--------------------\n", log.c_str());

	printf(g_failures == 0 ? "PASS\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
