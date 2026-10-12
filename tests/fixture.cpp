// End-to-end test of the add-on inside a real ReShade, on WARP (software, the default) or on the GPU.
// Runs from a folder holding a copy of ReShade's dxgi.dll and the add-on. It writes the add-on settings and a .twins file,
// draws stand-ins of the MSFS shaders and reads the pixels back. The settings keep one of the two lens flare elements.
//   fixture [--adapter=gpu] --sun-flare-off=1 --bloom-off=1 --light-glow-off=0 --light-point-off=0
//       stand-in shader package with a no-bloom twin: checks the twin swap
//   fixture [--adapter=gpu] --real-pak=<SHADERSPAK.*.SM6.PC.PAK> --real-twins=<.twins> ...
//       the game's shader package and twins file: also builds pipelines from the game's own light and tonemapping
//       shaders and checks the add-on recognizes them and creates the real no-bloom twin (pipelines only, never drawn)

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

	std::string digest_hex(const unsigned char *code)
	{
		camfx::digest16 d;
		dxbc::digest(code, 32, d.bytes);
		return camfx::to_hex(d);
	}

	// The name the add-on gives a lens flare element: the first 8 hex digits of its vertex and pixel shader digests.
	std::string element_tag(const unsigned char *vs, const unsigned char *ps)
	{
		return digest_hex(vs).substr(0, 8) + ":" + digest_hex(ps).substr(0, 8);
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

	// Shaders picked from the game's package, so their outputs and inputs link: light glow and light point vertex/pixel
	// pairs (without the TAAMASK output), and one tonemapping permutation with bloom.
	struct real_shaders
	{
		std::vector<uint8_t> pak;
		const uint8_t *glow_vs = nullptr, *glow_ps = nullptr, *point_vs = nullptr, *point_ps = nullptr, *tonemap_ps = nullptr;
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
		for (size_t i = 0; i + 32 <= pak.size(); ++i)
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

			const uint8_t *code = &pak[i];
			const std::string_view entry = camfx::entry_point(code, container);
			const dxbc::span osg = dxbc::find_part(code, container, dxbc::fourcc('O', 'S', 'G', '1'));
			const dxbc::span isg = dxbc::find_part(code, container, dxbc::fourcc('I', 'S', 'G', '1'));
			if (entry == "vh_billboard_lights" && !dxbc::contains_string(osg, "TAAMASK"))
			{
				const uint8_t *&slot = dxbc::contains_string(osg, "FlareUV") ? out.glow_vs : out.point_vs;
				if (slot == nullptr)
					slot = code;
			}
			if (entry == "ph_billboard_lights" && !dxbc::contains_string(isg, "TAAMASK"))
			{
				const uint8_t *&slot = dxbc::contains_string(isg, "FlareUV") ? out.glow_ps : out.point_ps;
				if (slot == nullptr)
					slot = code;
			}
			if (out.tonemap_ps == nullptr && camfx::classify_pixel_shader(code, container) == camfx::pso_kind::tonemap_bloom)
				out.tonemap_ps = code;
			i += container - 1;
		}
		return out.glow_vs && out.glow_ps && out.point_vs && out.point_ps && out.tonemap_ps;
	}
}

int wmain(int argc, wchar_t **argv)
{
	int sun_flare_off = 1, bloom_off = 1, glow_off = 0, point_off = 0;
	bool gpu = false;
	std::wstring real_pak, real_twins;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		swscanf_s(argv[i], L"--sun-flare-off=%d", &sun_flare_off);
		swscanf_s(argv[i], L"--bloom-off=%d", &bloom_off);
		swscanf_s(argv[i], L"--light-glow-off=%d", &glow_off);
		swscanf_s(argv[i], L"--light-point-off=%d", &point_off);
		if (arg == L"--adapter=gpu")
			gpu = true;
		if (arg.rfind(L"--real-pak=", 0) == 0)
			real_pak = arg.substr(11);
		if (arg.rfind(L"--real-twins=", 0) == 0)
			real_twins = arg.substr(13);
	}
	const bool real = !real_pak.empty();
	printf("fixture: %s, sun_flare_off=%d bloom_off=%d light_glow_off=%d light_point_off=%d%s\n",
		gpu ? "GPU" : "WARP", sun_flare_off, bloom_off, glow_off, point_off, real ? ", game shaders" : "");

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
		require(load_real_shaders(real_pak, game) ? S_OK : E_FAIL, "find light and tonemapping shaders in the game's package");
	}
	else
	{
		pak_path = dir + L"fixture.pak";
		constexpr uint32_t twin_offset = 4096;
		std::vector<unsigned char> pak(twin_offset, 0xCD);
		pak.insert(pak.end(), g_ps_tonemap_plain, g_ps_tonemap_plain + container_size(g_ps_tonemap_plain));
		pak.insert(pak.end(), 100, 0xCD);
		require(write_file(pak_path, pak.data(), pak.size()) ? S_OK : E_FAIL, "write fixture.pak");
		twins = "# fixture\npak fixture.pak " + std::to_string(pak.size()) + "\n" +
			digest_hex(g_ps_tonemap_bloom) + " " + digest_hex(g_ps_tonemap_plain) + " " + std::to_string(twin_offset) + " " + std::to_string(container_size(g_ps_tonemap_plain)) + "\n";
	}
	require(write_file(dir + L"msfs2024-camera-fx-off.twins", twins.data(), twins.size()) ? S_OK : E_FAIL, "write .twins");

	// Keep the second lens flare element. The other entries check the list parsing: the first names no pipeline, and the
	// malformed ones must be ignored.
	const std::string kept_element = element_tag(g_vs_material, g_ps_material_b);
	const std::string ini = "[settings]\r\nsun_flare_off=" + std::to_string(sun_flare_off) + "\r\nbloom_off=" + std::to_string(bloom_off) +
		"\r\nlight_glow_off=" + std::to_string(glow_off) + "\r\nlight_point_off=" + std::to_string(point_off) + "\r\npak_path=" + narrow(pak_path) +
		"\r\nflare_keep=deadbeef:01234567, nonsense,,  " + kept_element + " ,0123:4567,0123456789:abcdef,00000000:00000000\r\n";
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
	const ComPtr<ID3D12PipelineState> pso_material = make_pso(root.Get(), g_vs_material, g_ps_material, hdr, none, "create stand-in material pipeline");
	const ComPtr<ID3D12PipelineState> pso_material_depth = make_pso(root.Get(), g_vs_material, g_ps_material, hdr, depth, "create stand-in material pipeline with depth");
	const ComPtr<ID3D12PipelineState> pso_material_b = make_pso(root.Get(), g_vs_material, g_ps_material_b, hdr, none, "create stand-in material pipeline (second lens flare element)");

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

	// The game's own shaders, with a root signature that covers every register they use (they are never drawn).
	ComPtr<ID3D12RootSignature> game_root;
	ComPtr<ID3D12PipelineState> pso_game_glow, pso_game_point, pso_game_tonemap;
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
		require(D3D12SerializeRootSignature(&game_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &root_error), "serialize root signature for the game's shaders");
		require(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&game_root)), "CreateRootSignature (game shaders)");

		pso_game_glow = make_pso(game_root.Get(), game.glow_vs, game.glow_ps, DXGI_FORMAT_R16G16B16A16_FLOAT, none, "create pipeline from the game's light glow shaders");
		pso_game_point = make_pso(game_root.Get(), game.point_vs, game.point_ps, DXGI_FORMAT_R16G16B16A16_FLOAT, none, "create pipeline from the game's light point shaders");
		pso_game_tonemap = make_pso(game_root.Get(), g_vs_fullscreen_tex, game.tonemap_ps, ldr, none, "create pipeline from the game's bloom tonemapping shader");
	}

	// Targets (4x4): light glow, light glow through ExecuteIndirect, light point, tonemap (LDR), then HDR ones for the
	// material quads: no depth bound (lens flare), depth bound (transparent geometry), a 3-vertex draw, and a quad of the
	// second lens flare element (kept).
	enum : UINT { glow_target, glow_indirect_target, point_target, tonemap_target, flare_target, flare_depth_target, flare_triangle_target, flare_kept_target, targets };
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

	// Record: each target gets one draw, as the game would issue them.
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

	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	draw_into(flare_target, false, pso_material.Get());
	list->DrawInstanced(4, 1, 0, 0);
	draw_into(flare_depth_target, true, pso_material_depth.Get());
	list->DrawInstanced(4, 1, 0, 0);
	draw_into(flare_triangle_target, false, pso_material.Get());
	list->DrawInstanced(3, 1, 0, 0);
	draw_into(flare_kept_target, false, pso_material_b.Get());
	list->DrawInstanced(4, 1, 0, 0);

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

	// Pixel (1, 1) of every target, as 4 bytes (RGBA8, or the packed R11G11B10 bits).
	void *mapped = nullptr;
	require(readback->Map(0, nullptr, &mapped), "map readback");
	const auto *const pixels = static_cast<const unsigned char *>(mapped);
	const auto pixel = [&](UINT i) { const unsigned char *p = pixels + i * size * row_pitch + row_pitch + 4; return std::vector<int> { p[0], p[1], p[2], p[3] }; };
	const auto show = [](const std::vector<int> &p) { char text[48]; snprintf(text, sizeof(text), "(%d, %d, %d, %d)", p[0], p[1], p[2], p[3]); return std::string(text); };

	const std::vector<int> glow = pixel(glow_target), glow_indirect = pixel(glow_indirect_target), point = pixel(point_target), tonemap = pixel(tonemap_target);
	const std::vector<int> flare = pixel(flare_target), flare_depth = pixel(flare_depth_target), flare_triangle = pixel(flare_triangle_target), flare_kept = pixel(flare_kept_target);
	printf("light glow %s, light glow (indirect) %s, light point %s, tonemap %s\n", show(glow).c_str(), show(glow_indirect).c_str(), show(point).c_str(), show(tonemap).c_str());
	printf("material quad, no depth %s; with depth %s; triangle %s; kept element %s\n", show(flare).c_str(), show(flare_depth).c_str(), show(flare_triangle).c_str(), show(flare_kept).c_str());

	const std::vector<int> nothing = { 0, 0, 0, 0 }, red = { 255, 0, 0, 255 }, green = { 0, 255, 0, 255 };
	check(glow == (glow_off ? nothing : red), glow_off ? "light glow draw skipped (diagnostic)" : "light glow draw kept");
	check(glow_indirect == (glow_off ? nothing : red), glow_off ? "light glow ExecuteIndirect skipped (diagnostic, stream pipeline)" : "light glow ExecuteIndirect kept (stream pipeline)");
	check(point == (point_off ? nothing : green), point_off ? "light point draw skipped (diagnostic)" : "light point draw kept");
	check(sun_flare_off ? flare == nothing : flare != nothing, sun_flare_off ? "lens flare quad (HDR target, no depth) skipped" : "lens flare quad kept");
	check(flare_kept != nothing, sun_flare_off ? "lens flare quad of an element set to keep drawn" : "lens flare quad of the second element kept");
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
		check(log.find("pipeline: light_glow") != std::string::npos && log.find(digest_hex(game.glow_vs).substr(0, 8)) != std::string::npos, "add-on recognized the game's light glow vertex shader");
		check(log.find("pipeline: light_point") != std::string::npos && log.find(digest_hex(game.point_vs).substr(0, 8)) != std::string::npos, "add-on recognized the game's light point vertex shader");
		if (pair != game_twins.entries.end())
			check(log.find("tonemapping " + digest_hex(game.tonemap_ps) + " uses no-bloom twin " + camfx::to_hex(pair->second.twin)) != std::string::npos,
				"add-on created the game's no-bloom twin from the shader package");
	}
	else
	{
		check(bloom_off ? twin_ran : bloom_ran, bloom_off ? "tonemapping ran the no-bloom twin read from the package" : "tonemapping kept the bloom permutation");
		check(log.find("uses no-bloom twin") != std::string::npos, "add-on paired the bloom permutation with its twin");
	}
	if (sun_flare_off)
		check(log.find("lens flare: skipping quads of VS:vh_base#") != std::string::npos, "add-on logged the lens flare pipeline it skips");
	// The elements read from the settings come first (#1 and #2), so the one first drawn is #3; the second one is already listed.
	check(log.find(" flare_keep=deadbeef:01234567," + kept_element + " ") != std::string::npos, "add-on read the lens flare elements to keep");
	check(log.find("lens flare: element #3 is VS:vh_base#" + digest_hex(g_vs_material).substr(0, 8) + " PS:ph_base#" + digest_hex(g_ps_material).substr(0, 8)) != std::string::npos,
		"add-on listed the lens flare element it saw");
	check(log.find("lens flare: element #4") == std::string::npos, "only lens flare quads were listed as elements");
	printf("---- add-on log ----\n%s--------------------\n", log.c_str());

	printf(g_failures == 0 ? "PASS\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
