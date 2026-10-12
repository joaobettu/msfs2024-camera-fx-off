// MSFS 2024 Camera FX Off, by PdPanoo
// ReShade add-on that removes the "filmed with a camera" look from Microsoft Flight Simulator 2024:
// - the lens flare, by skipping the quads the game draws for it right after upscaling; its glow, star rays and ghosts can
//   each be kept from the overlay (by default the glow around the sun is);
// - bloom and its lens dirt, by binding the game's own tonemapping permutation without bloom in place of the one with it.
// It only uses ReShade events: no hooks of its own, and little per-call work (a lock-free table lookup on pipeline binds,
// a view format lookup on render target binds without depth).

#include <imgui.h>
#include <reshade.hpp>

#include "classify.hpp"
#include "twins.hpp"
#include "version.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <share.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace reshade::api;
using camfx::flare_part;
using camfx::pso_kind;

extern "C" __declspec(dllexport) const char *NAME = "MSFS 2024 Camera FX Off";
extern "C" __declspec(dllexport) const char *AUTHOR = "PdPanoo";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Removes the lens flare and the bloom with lens dirt from Microsoft Flight Simulator 2024.";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/joaobettu/msfs2024-camera-fx-off";
extern "C" __declspec(dllexport) const char *ISSUES = "https://github.com/joaobettu/msfs2024-camera-fx-off/issues";

namespace
{
	// ---------------------------------------------------------------------------------------------
	// Settings, stored in msfs2024-camera-fx-off.ini next to the add-on

	std::atomic<bool> g_sun_flare_off { true };
	// Parts of the lens flare still drawn with its removal on, by camfx::flare_part: only the glow by default.
	std::atomic<bool> g_keep[camfx::flare_part_count] = { true, false, false };
	const wchar_t *const g_keep_keys[camfx::flare_part_count] = { L"keep_glow", L"keep_star", L"keep_ghosts" };
	std::atomic<bool> g_bloom_off { true };
	// Pressed together with Ctrl+Shift
	UINT g_key_flare = VK_F5, g_key_bloom = VK_F6, g_key_trace = VK_F7;

	std::wstring g_ini_path, g_log_path, g_trace_path, g_twins_path, g_pak_override;

	std::string key_name(UINT vk)
	{
		if (vk >= VK_F1 && vk <= VK_F24)
			return "F" + std::to_string(vk - VK_F1 + 1);
		char text[16];
		snprintf(text, sizeof(text), "0x%02X", vk);
		return text;
	}

	std::wstring widen(const std::string &s)
	{
		std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
		return w;
	}

	void load_settings()
	{
		const auto get = [](const wchar_t *key, int def) { return int(GetPrivateProfileIntW(L"settings", key, def, g_ini_path.c_str())); };
		g_sun_flare_off = get(L"sun_flare_off", 1) != 0;
		for (size_t i = 0; i < camfx::flare_part_count; ++i)
			g_keep[i] = get(g_keep_keys[i], i == size_t(flare_part::glow)) != 0;
		g_bloom_off = get(L"bloom_off", 1) != 0;
		g_key_flare = UINT(get(L"key_flare", VK_F5));
		g_key_bloom = UINT(get(L"key_bloom", VK_F6));
		g_key_trace = UINT(get(L"key_trace", VK_F7));

		wchar_t pak[MAX_PATH] = L"";
		GetPrivateProfileStringW(L"settings", L"pak_path", L"", pak, MAX_PATH, g_ini_path.c_str());
		g_pak_override = pak;
	}

	void save_settings()
	{
		const auto put = [](const wchar_t *key, bool value) { WritePrivateProfileStringW(L"settings", key, value ? L"1" : L"0", g_ini_path.c_str()); };
		put(L"sun_flare_off", g_sun_flare_off);
		for (size_t i = 0; i < camfx::flare_part_count; ++i)
			put(g_keep_keys[i], g_keep[i]);
		put(L"bloom_off", g_bloom_off);
		// Keys of earlier versions
		for (const wchar_t *const key : { L"flare_keep", L"light_glow_off", L"light_point_off" })
			WritePrivateProfileStringW(L"settings", key, nullptr, g_ini_path.c_str());
	}

	std::string keep_text()
	{
		char text[64];
		snprintf(text, sizeof(text), "keep_glow=%d keep_star=%d keep_ghosts=%d",
			int(g_keep[size_t(flare_part::glow)].load()), int(g_keep[size_t(flare_part::star)].load()), int(g_keep[size_t(flare_part::ghosts)].load()));
		return text;
	}

	// ---------------------------------------------------------------------------------------------
	// Log, rewritten on every launch

	std::mutex g_log_mutex;
	FILE *g_log = nullptr;

	void log(const char *format, ...)
	{
		char message[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(message, sizeof(message), format, args);
		va_end(args);

		SYSTEMTIME t;
		GetLocalTime(&t);
		const std::lock_guard<std::mutex> lock(g_log_mutex);
		if (g_log == nullptr)
			return;
		fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);
		fflush(g_log);
	}

	// ---------------------------------------------------------------------------------------------
	// Pipelines this add-on acts on

	struct pso_info
	{
		pso_kind kind = pso_kind::none;
		// No-bloom pipeline bound in place of a tonemap_bloom one (zero if none could be made)
		uint64_t twin = 0;
		// Material pipelines: first 4 bytes of the vertex and pixel shader digests (vs << 32 | ps), which name a lens flare element.
		uint64_t tag = 0;
	};

	// Open-addressing table looked up on every pipeline bind without taking a lock (MSFS binds thousands per frame
	// from several threads). Writers serialize on g_pso_mutex and publish the key last.
	class pso_table
	{
	public:
		bool find(uint64_t handle, pso_info &out) const
		{
			if (handle == empty || handle == tombstone)
				return false;
			for (size_t i = slot(handle), n = 0; n < capacity; i = (i + 1) & (capacity - 1), ++n)
			{
				const uint64_t key = _keys[i].load(std::memory_order_acquire);
				if (key == handle)
				{
					out.kind = pso_kind(_kinds[i].load(std::memory_order_relaxed));
					out.twin = _twins[i].load(std::memory_order_relaxed);
					out.tag = _tags[i].load(std::memory_order_relaxed);
					return out.kind != pso_kind::none;
				}
				if (key == empty)
					return false;
			}
			return false;
		}

		bool insert(uint64_t handle, const pso_info &info)
		{
			size_t reuse = capacity;
			for (size_t i = slot(handle), n = 0; n < capacity; i = (i + 1) & (capacity - 1), ++n)
			{
				const uint64_t key = _keys[i].load(std::memory_order_relaxed);
				if (key == handle)
					return write(i, handle, info), true;
				if (key == tombstone && reuse == capacity)
					reuse = i;
				if (key == empty)
					return write(reuse != capacity ? reuse : i, handle, info), true;
			}
			return reuse != capacity ? (write(reuse, handle, info), true) : false;
		}

		bool erase(uint64_t handle, pso_info &out)
		{
			for (size_t i = slot(handle), n = 0; n < capacity; i = (i + 1) & (capacity - 1), ++n)
			{
				const uint64_t key = _keys[i].load(std::memory_order_relaxed);
				if (key == handle)
				{
					out.kind = pso_kind(_kinds[i].load(std::memory_order_relaxed));
					out.twin = _twins[i].load(std::memory_order_relaxed);
					out.tag = _tags[i].load(std::memory_order_relaxed);
					_kinds[i].store(uint8_t(pso_kind::none), std::memory_order_relaxed);
					_twins[i].store(0, std::memory_order_relaxed);
					_tags[i].store(0, std::memory_order_relaxed);
					_keys[i].store(tombstone, std::memory_order_release);
					return true;
				}
				if (key == empty)
					return false;
			}
			return false;
		}

	private:
		// Power of two. Material pipelines are tracked too, and MSFS creates thousands of them.
		static constexpr size_t capacity = 65536;
		static constexpr uint64_t empty = 0, tombstone = ~uint64_t(0);

		static size_t slot(uint64_t h)
		{
			h ^= h >> 33;
			h *= 0xff51afd7ed558ccdull;
			h ^= h >> 33;
			return size_t(h) & (capacity - 1);
		}

		void write(size_t i, uint64_t handle, const pso_info &info)
		{
			_kinds[i].store(uint8_t(info.kind), std::memory_order_relaxed);
			_twins[i].store(info.twin, std::memory_order_relaxed);
			_tags[i].store(info.tag, std::memory_order_relaxed);
			_keys[i].store(handle, std::memory_order_release);
		}

		std::atomic<uint64_t> _keys[capacity] = {};
		std::atomic<uint8_t> _kinds[capacity] = {};
		std::atomic<uint64_t> _twins[capacity] = {};
		std::atomic<uint64_t> _tags[capacity] = {};
	};

	std::mutex g_pso_mutex;
	pso_table g_psos;
	std::atomic<uint32_t> g_pso_count[camfx::pso_kind_count] = {};
	std::atomic<uint32_t> g_twin_count { 0 }, g_twin_missing { 0 }, g_twin_failed { 0 };

	// Entry points and short digests of every pipeline's shaders, to label frame traces and the log.
	std::shared_mutex g_name_mutex;
	std::unordered_map<uint64_t, std::string> g_names;

	// ---------------------------------------------------------------------------------------------
	// Tonemapping twins (pairs read from the .twins file, shaders read from the game's shader package)

	std::once_flag g_twins_once;
	camfx::twin_table g_twins;
	std::wstring g_pak_path;
	std::atomic<bool> g_twins_ok { false };
	std::atomic<bool> g_twins_tried { false };

	void load_twins()
	{
		g_twins_tried = true;
		std::string text, error;
		if (!camfx::read_file(g_twins_path, text))
		{
			log("twins: %S not found, bloom removal unavailable (run tools\\gen_twins.ps1)", g_twins_path.c_str());
			return;
		}
		if (!camfx::parse_twin_table(text, g_twins, error))
		{
			log("twins: %S: %s", g_twins_path.c_str(), error.c_str());
			return;
		}

		if (!g_pak_override.empty())
		{
			g_pak_path = g_pak_override;
		}
		else
		{
			wchar_t exe[MAX_PATH] = L"";
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring dir = exe;
			dir.erase(dir.find_last_of(L"\\/") + 1);
			g_pak_path = dir + L"Shaders\\" + widen(g_twins.pak_name);
		}

		WIN32_FILE_ATTRIBUTE_DATA attributes;
		if (!GetFileAttributesExW(g_pak_path.c_str(), GetFileExInfoStandard, &attributes))
		{
			log("twins: shader package %S not found (sim updated? run tools\\gen_twins.ps1 again)", g_pak_path.c_str());
			return;
		}
		const uint64_t size = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
		if (g_twins.pak_size != 0 && size != g_twins.pak_size)
		{
			log("twins: shader package is %llu bytes but the twins file expects %llu (sim updated? run tools\\gen_twins.ps1 again)", size, g_twins.pak_size);
			return;
		}

		g_twins_ok = true;
		log("twins: %zu pairs for %S", g_twins.entries.size(), g_pak_path.c_str());
	}

	uint64_t create_twin(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, const shader_desc &ps)
	{
		std::call_once(g_twins_once, load_twins);

		camfx::digest16 digest;
		if (!g_twins_ok || !dxbc::digest(ps.code, ps.code_size, digest.bytes))
		{
			g_twin_missing++;
			return 0;
		}
		const auto it = g_twins.entries.find(digest);
		if (it == g_twins.entries.end())
		{
			// The sim also compiles some permutations at run time; those are not in the shader package.
			log("twins: no pair for tonemapping variant %s (compiled by the sim at run time, or the sim was updated)", camfx::to_hex(digest).c_str());
			g_twin_missing++;
			return 0;
		}

		std::vector<uint8_t> code;
		std::string error;
		if (!camfx::read_twin(g_pak_path, it->second, code, error))
		{
			log("twins: %s", error.c_str());
			g_twin_failed++;
			return 0;
		}

		// Same pipeline description with the pixel shader swapped for the game's permutation without bloom.
		// That one binds a subset of the same resources, so it works with the same root signature and root arguments.
		shader_desc twin_ps = ps;
		twin_ps.code = code.data();
		twin_ps.code_size = code.size();
		std::vector<pipeline_subobject> desc(subobjects, subobjects + count);
		for (pipeline_subobject &subobject : desc)
		{
			if (subobject.type == pipeline_subobject_type::pixel_shader)
			{
				subobject.count = 1;
				subobject.data = &twin_ps;
			}
		}

		pipeline twin = {};
		if (!dev->create_pipeline(layout, count, desc.data(), &twin))
		{
			log("twins: creating the no-bloom pipeline for %s failed", camfx::to_hex(digest).c_str());
			g_twin_failed++;
			return 0;
		}

		g_twin_count++;
		log("twins: tonemapping %s uses no-bloom twin %s", camfx::to_hex(digest).c_str(), camfx::to_hex(it->second.twin).c_str());
		return twin.handle;
	}

	// ---------------------------------------------------------------------------------------------
	// Per command list state, and the frame trace (Ctrl+Shift+F7): pipeline binds, render targets, draws and dispatches
	// recorded between two presents

	struct __declspec(uuid("2f9c1b7e-4a3d-4c8e-9b1f-6e2d8a0c5b71")) cmd_state
	{
		pso_kind kind = pso_kind::none;
		uint64_t pipeline = 0;
		// Lens flare element name of the bound material pipeline (pso_info::tag)
		uint64_t tag = 0;
		// Render target 0 is an R11G11B10 HDR target with no depth bound: where the game draws its lens flare after upscaling.
		bool flare_target = false;
		// Lens flare quads drawn since that target was bound
		uint32_t flare_quads = 0;
		uint32_t id = 0;
	};

	std::atomic<uint32_t> g_next_cmd_id { 1 };
	std::atomic<bool> g_tracing { false }, g_trace_requested { false };
	std::mutex g_trace_mutex;
	std::vector<std::string> g_trace;
	constexpr size_t max_trace_lines = 200000;

	void trace(const command_list *cmd, const char *format, ...)
	{
		char line[512];
		const cmd_state *const state = cmd->get_private_data<cmd_state>();
		const int prefix = snprintf(line, sizeof(line), "cl%u ", state != nullptr ? state->id : 0);
		va_list args;
		va_start(args, format);
		vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
		va_end(args);

		const std::lock_guard<std::mutex> lock(g_trace_mutex);
		if (g_trace.size() < max_trace_lines)
			g_trace.emplace_back(line);
	}

	std::string pipeline_label(uint64_t handle)
	{
		{
			const std::shared_lock<std::shared_mutex> lock(g_name_mutex);
			const auto it = g_names.find(handle);
			if (it != g_names.end())
				return it->second;
		}
		char text[32];
		snprintf(text, sizeof(text), "pso %016llx", handle);
		return text;
	}

	format view_format(device *dev, resource_view view)
	{
		const resource_view_desc desc = dev->get_resource_view_desc(view);
		if (desc.format != format::unknown)
			return desc.format;
		const resource res = dev->get_resource_from_view(view);
		return res.handle != 0 ? dev->get_resource_desc(res).texture.format : format::unknown;
	}

	std::string describe_view(device *dev, resource_view view)
	{
		if (view.handle == 0)
			return "-";
		const resource res = dev->get_resource_from_view(view);
		if (res.handle == 0)
			return "?";
		const resource_desc desc = dev->get_resource_desc(res);
		char text[64];
		snprintf(text, sizeof(text), "%ux%u fmt%u", desc.texture.width, desc.texture.height, unsigned(view_format(dev, view)));
		return text;
	}

	void finish_trace()
	{
		g_tracing = false;
		std::vector<std::string> lines;
		{
			const std::lock_guard<std::mutex> lock(g_trace_mutex);
			lines.swap(g_trace);
		}

		FILE *file = _wfsopen(g_trace_path.c_str(), L"w", _SH_DENYWR);
		if (file == nullptr)
		{
			log("trace: cannot write %S", g_trace_path.c_str());
			return;
		}
		fprintf(file, "# MSFS 2024 Camera FX Off %s frame trace: sun_flare_off=%d %s bloom_off=%d, %zu lines%s\n",
			CAMFX_VERSION_STRING, int(g_sun_flare_off.load()), keep_text().c_str(), int(g_bloom_off.load()),
			lines.size(), lines.size() >= max_trace_lines ? " (truncated)" : "");
		for (const std::string &line : lines)
			fprintf(file, "%s\n", line.c_str());
		fclose(file);
		log("trace: wrote %zu lines to %S", lines.size(), g_trace_path.c_str());
	}

	// ---------------------------------------------------------------------------------------------
	// Statistics

	std::atomic<uint64_t> g_flare_skipped { 0 }, g_flare_kept { 0 }, g_bloom_swapped { 0 }, g_bloom_kept { 0 };

	struct rates
	{
		uint64_t flare_skipped, flare_kept, bloom_swapped, bloom_kept;

		bool operator==(const rates &) const = default;
	};

	rates g_rates = {}, g_totals = {}, g_logged = {};
	ULONGLONG g_rate_tick = 0, g_log_tick = 0;

	void update_rates()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_rate_tick < 1000)
			return;
		const double seconds = g_rate_tick != 0 ? double(now - g_rate_tick) / 1000.0 : 1.0;
		const rates totals = { g_flare_skipped.load(), g_flare_kept.load(), g_bloom_swapped.load(), g_bloom_kept.load() };
		const auto per_second = [seconds](uint64_t a, uint64_t b) { return uint64_t(double(a - b) / seconds + 0.5); };
		g_rates = {
			per_second(totals.flare_skipped, g_totals.flare_skipped),
			per_second(totals.flare_kept, g_totals.flare_kept),
			per_second(totals.bloom_swapped, g_totals.bloom_swapped),
			per_second(totals.bloom_kept, g_totals.bloom_kept) };
		g_totals = totals;
		g_rate_tick = now;

		// Log only when something changed, to keep the log small.
		if (now - g_log_tick >= 10000 && !(g_rates == g_logged))
		{
			log("stats/s: flare quads skipped %llu, flare quads kept %llu, tonemap binds swapped %llu, tonemap with bloom drawn %llu",
				g_rates.flare_skipped, g_rates.flare_kept, g_rates.bloom_swapped, g_rates.bloom_kept);
			g_logged = g_rates;
			g_log_tick = now;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Lens flare elements seen so far: one per pair of shaders (camfx::flare_tag), with the part of the lens flare it draws.
	// Looked up on draws without a lock.

	struct flare_element
	{
		// Written once, before g_element_count publishes the element.
		uint64_t tag = 0;
		flare_part part = flare_part::ghosts;
		// In the table of known shaders, rather than told by its draw order
		bool known = false;
	};

	constexpr size_t max_flare_elements = 32;
	flare_element g_elements[max_flare_elements];
	std::atomic<size_t> g_element_count { 0 };
	std::mutex g_element_mutex;

	const flare_element *find_element(uint64_t tag)
	{
		const size_t count = g_element_count.load(std::memory_order_acquire);
		for (size_t i = 0; i < count; ++i)
			if (g_elements[i].tag == tag)
				return &g_elements[i];
		return nullptr;
	}

	// The element of a lens flare quad, added the first time it is drawn. Its part comes from the table of known shaders or,
	// for other shaders (after a sim update), from the position of its first quad in the lens flare ('index').
	// Null for a pipeline without a name (unsigned shaders, which the game does not ship) or once the list is full.
	const flare_element *flare_element_of(uint64_t tag, uint32_t index, bool &added)
	{
		if (tag == 0)
			return nullptr;
		if (const flare_element *const element = find_element(tag))
			return element;
		if (g_element_count.load(std::memory_order_relaxed) == max_flare_elements)
			return nullptr;

		const std::lock_guard<std::mutex> lock(g_element_mutex);
		if (const flare_element *const element = find_element(tag))
			return element;
		const size_t count = g_element_count.load(std::memory_order_relaxed);
		if (count == max_flare_elements)
			return nullptr;
		flare_element &element = g_elements[count];
		element.tag = tag;
		element.known = camfx::known_flare_part(tag, element.part);
		if (!element.known)
			element.part = camfx::flare_part_by_order(index);
		g_element_count.store(count + 1, std::memory_order_release);
		added = true;
		return &element;
	}

	// ---------------------------------------------------------------------------------------------
	// Events

	const shader_desc *find_shader(uint32_t count, const pipeline_subobject *subobjects, pipeline_subobject_type type)
	{
		for (uint32_t i = 0; i < count; ++i)
		{
			if (subobjects[i].type != type || subobjects[i].count == 0 || subobjects[i].data == nullptr)
				continue;
			const auto *desc = static_cast<const shader_desc *>(subobjects[i].data);
			if (desc->code != nullptr && desc->code_size != 0)
				return desc;
		}
		return nullptr;
	}

	// The bytecode comes from the game and is well formed, but a bad pointer or size here would take the sim down with it.
	pso_kind classify_guarded(const shader_desc *vs, const shader_desc *ps)
	{
		__try
		{
			return camfx::classify_pipeline(vs != nullptr ? vs->code : nullptr, vs != nullptr ? vs->code_size : 0, ps != nullptr ? ps->code : nullptr, ps != nullptr ? ps->code_size : 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return pso_kind::none;
		}
	}

	// Entry point name plus the first 4 bytes of the container digest, e.g. "vh_base#1a2b3c4d".
	size_t label_guarded(const shader_desc *desc, char *out, size_t out_size)
	{
		__try
		{
			const std::string_view entry = camfx::entry_point(desc->code, desc->code_size);
			const size_t length = entry.size() < out_size - 10 ? entry.size() : out_size - 10;
			std::memcpy(out, entry.data(), length);
			uint8_t digest[16];
			if (dxbc::digest(desc->code, desc->code_size, digest))
				snprintf(out + length, 10, "#%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
			else
				out[length] = '\0';
			return length;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out[0] = '\0';
			return 0;
		}
	}

	uint32_t digest_prefix_guarded(const shader_desc *desc)
	{
		__try
		{
			return desc != nullptr ? camfx::digest_prefix(desc->code, desc->code_size) : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	void remember_names(uint64_t handle, const shader_desc *vs, const shader_desc *ps, const shader_desc *cs)
	{
		std::string name;
		char label[128];
		const auto add = [&](const char *stage, const shader_desc *desc) {
			if (desc == nullptr)
				return;
			if (!name.empty())
				name += ' ';
			name += stage;
			name += label_guarded(desc, label, sizeof(label)) != 0 ? label : "?";
		};
		add("VS:", vs);
		add("PS:", ps);
		add("CS:", cs);
		if (name.empty())
			return;

		const std::unique_lock<std::shared_mutex> lock(g_name_mutex);
		g_names[handle] = std::move(name);
	}

	void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipeline)
	{
		const shader_desc *const vs = find_shader(count, subobjects, pipeline_subobject_type::vertex_shader);
		const shader_desc *const ps = find_shader(count, subobjects, pipeline_subobject_type::pixel_shader);
		const shader_desc *const cs = find_shader(count, subobjects, pipeline_subobject_type::compute_shader);
		remember_names(pipeline.handle, vs, ps, cs);

		const pso_kind kind = (vs != nullptr || ps != nullptr) ? classify_guarded(vs, ps) : pso_kind::none;

		pso_info existing;
		const bool known = g_psos.find(pipeline.handle, existing);
		// ReShade may report the same pipeline more than once.
		if (known && existing.kind == kind)
			return;
		if (known)
		{
			// A new pipeline reusing the handle value of a tracked one whose destruction went unreported: drop the stale entry.
			bool erased;
			{
				const std::lock_guard<std::mutex> lock(g_pso_mutex);
				erased = g_psos.erase(pipeline.handle, existing);
			}
			if (erased)
			{
				g_pso_count[size_t(existing.kind)]--;
				if (existing.twin != 0)
				{
					dev->destroy_pipeline({ existing.twin });
					g_twin_count--;
				}
			}
		}
		if (kind == pso_kind::none)
			return;

		pso_info info;
		info.kind = kind;
		if (kind == pso_kind::tonemap_bloom)
			info.twin = create_twin(dev, layout, count, subobjects, *ps);
		if (kind == pso_kind::material)
			info.tag = camfx::flare_tag(digest_prefix_guarded(vs), digest_prefix_guarded(ps));

		bool inserted;
		{
			const std::lock_guard<std::mutex> lock(g_pso_mutex);
			inserted = g_psos.insert(pipeline.handle, info);
		}
		if (!inserted)
		{
			log("pipeline table full, ignoring %s pipeline %016llx", camfx::kind_name(kind), pipeline.handle);
			if (info.twin != 0)
				dev->destroy_pipeline({ info.twin });
			return;
		}

		const uint32_t n = ++g_pso_count[size_t(kind)];
		if (n <= 16 && kind != pso_kind::material)
			log("pipeline: %s #%u %016llx %s", camfx::kind_name(kind), n, pipeline.handle, pipeline_label(pipeline.handle).c_str());
	}

	void on_destroy_pipeline(device *dev, pipeline pipeline)
	{
		pso_info info;
		bool erased;
		{
			const std::lock_guard<std::mutex> lock(g_pso_mutex);
			erased = g_psos.erase(pipeline.handle, info);
		}
		if (erased)
		{
			g_pso_count[size_t(info.kind)]--;
			// The game only destroys a pipeline once the GPU is done with it, and the twin was only used in its place.
			if (info.twin != 0)
			{
				dev->destroy_pipeline({ info.twin });
				g_twin_count--;
			}
		}

		const std::unique_lock<std::shared_mutex> lock(g_name_mutex);
		g_names.erase(pipeline.handle);
	}

	void on_init_command_list(command_list *cmd)
	{
		cmd->create_private_data<cmd_state>()->id = g_next_cmd_id++;
	}

	void on_destroy_command_list(command_list *cmd)
	{
		cmd->destroy_private_data<cmd_state>();
	}

	void on_reset_command_list(command_list *cmd)
	{
		if (cmd_state *const state = cmd->get_private_data<cmd_state>())
		{
			state->kind = pso_kind::none;
			state->pipeline = 0;
			state->tag = 0;
			state->flare_target = false;
			state->flare_quads = 0;
		}
	}

	void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipeline)
	{
		pso_info info;
		const bool known = g_psos.find(pipeline.handle, info);
		pso_kind kind = known ? info.kind : pso_kind::none;

		bool swapped = false;
		if (kind == pso_kind::tonemap_bloom && info.twin != 0 && g_bloom_off.load(std::memory_order_relaxed))
		{
			// Called right after the game's SetPipelineState, so this replaces it for the draws that follow.
			cmd->bind_pipeline(stages, { info.twin });
			g_bloom_swapped.fetch_add(1, std::memory_order_relaxed);
			kind = pso_kind::none;
			swapped = true;
		}

		if (cmd_state *const state = cmd->get_private_data<cmd_state>())
		{
			state->kind = kind;
			state->pipeline = pipeline.handle;
			state->tag = known ? info.tag : 0;
		}

		if (g_tracing.load(std::memory_order_relaxed))
			trace(cmd, "bind %s%s%s", pipeline_label(pipeline.handle).c_str(), known ? (std::string(" [") + camfx::kind_name(info.kind) + "]").c_str() : "", swapped ? " -> no-bloom twin" : "");
	}

	enum class draw_type
	{
		draw,
		draw_indexed,
		indirect,
	};

	bool filter_draw(command_list *cmd, draw_type type, uint32_t count, uint32_t instances)
	{
		cmd_state *const state = cmd->get_private_data<cmd_state>();
		// What the draw was recognized as, for the trace
		const char *what = nullptr;
		bool skip = false, kept = false;
		switch (state != nullptr ? state->kind : pso_kind::none)
		{
		case pso_kind::material:
			// The lens flare: single quads drawn with material shaders into the HDR target right after upscaling, with no
			// depth bound. The same shaders draw transparent geometry in the main pass, always with depth.
			if (type == draw_type::draw && count == 4 && instances == 1 && state->flare_target)
			{
				const uint32_t index = state->flare_quads++;
				bool added = false;
				const flare_element *const element = flare_element_of(state->tag, index, added);
				if (added && element->known)
					log("lens flare: %s draws the %s", pipeline_label(state->pipeline).c_str(), camfx::part_name(element->part));
				else if (added)
					log("lens flare: %s has shaders this version does not know (sim updated?); its first quad is #%u of the lens flare, so it is taken as the %s",
						pipeline_label(state->pipeline).c_str(), index + 1, camfx::part_name(element->part));
				const flare_part part = element != nullptr ? element->part : camfx::flare_part_by_order(index);

				static const char *const names[camfx::flare_part_count] = { "lens flare glow", "lens flare star rays", "lens flare ghosts" };
				what = names[size_t(part)];
				if (!g_sun_flare_off.load(std::memory_order_relaxed))
					break;
				if (g_keep[size_t(part)].load(std::memory_order_relaxed))
				{
					g_flare_kept.fetch_add(1, std::memory_order_relaxed);
					kept = true;
				}
				else
				{
					g_flare_skipped.fetch_add(1, std::memory_order_relaxed);
					skip = true;
				}
			}
			break;
		case pso_kind::tonemap_bloom:
			what = "tonemap with bloom";
			g_bloom_kept.fetch_add(1, std::memory_order_relaxed);
			break;
		default:
			break;
		}

		if (g_tracing.load(std::memory_order_relaxed))
		{
			static const char *const names[] = { "draw", "draw_indexed", "indirect" };
			char note[64] = "";
			if (what != nullptr)
				snprintf(note, sizeof(note), " [%s%s]", skip ? "skipped: " : kept ? "kept: " : "", what);
			trace(cmd, "%s %u %u%s", names[int(type)], count, instances, note);
		}
		return skip;
	}

	bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
	{
		return filter_draw(cmd, draw_type::draw, vertices, instances);
	}

	bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
	{
		return filter_draw(cmd, draw_type::draw_indexed, indices, instances);
	}

	bool on_draw_or_dispatch_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t count, uint32_t)
	{
		// D3D12 ExecuteIndirect is reported as 'unknown': with a graphics pipeline bound it is drawing.
		if (type == indirect_command::dispatch || type == indirect_command::dispatch_mesh || type == indirect_command::dispatch_rays)
		{
			if (g_tracing.load(std::memory_order_relaxed))
				trace(cmd, "dispatch_indirect %u", count);
			return false;
		}
		return filter_draw(cmd, draw_type::indirect, count, 0);
	}

	bool on_dispatch(command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
	{
		if (g_tracing.load(std::memory_order_relaxed))
			trace(cmd, "dispatch %ux%ux%u", x, y, z);
		return false;
	}

	void update_flare_target(command_list *cmd, resource_view rtv, bool has_depth)
	{
		cmd_state *const state = cmd->get_private_data<cmd_state>();
		if (state == nullptr)
			return;
		// Only render target binds without depth need the format lookup, which keeps it off the main passes.
		state->flare_target = !has_depth && rtv.handle != 0 && view_format(cmd->get_device(), rtv) == format::r11g11b10_float;
		state->flare_quads = 0;
	}

	void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
	{
		update_flare_target(cmd, count != 0 ? rtvs[0] : resource_view {}, dsv.handle != 0);
		if (g_tracing.load(std::memory_order_relaxed))
		{
			device *const dev = cmd->get_device();
			trace(cmd, "targets %u rt0 %s ds %s", count, describe_view(dev, count != 0 ? rtvs[0] : resource_view {}).c_str(), describe_view(dev, dsv).c_str());
		}
	}

	bool on_begin_render_pass(command_list *cmd, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds, render_pass_flags)
	{
		const resource_view dsv = ds != nullptr ? ds->view : resource_view {};
		update_flare_target(cmd, count != 0 ? rts[0].view : resource_view {}, dsv.handle != 0);
		if (g_tracing.load(std::memory_order_relaxed))
		{
			device *const dev = cmd->get_device();
			trace(cmd, "render_pass %u rt0 %s ds %s", count, describe_view(dev, count != 0 ? rts[0].view : resource_view {}).c_str(), describe_view(dev, dsv).c_str());
		}
		return false;
	}

	void toggle(std::atomic<bool> &value, const char *what)
	{
		value = !value;
		save_settings();
		log("%s %s", what, value ? "on" : "off");
	}

	void on_present(effect_runtime *runtime)
	{
		if (g_tracing.load())
			finish_trace();
		else if (g_trace_requested.exchange(false))
			g_tracing = true;

		if (runtime->is_key_down(VK_CONTROL) && runtime->is_key_down(VK_SHIFT))
		{
			if (runtime->is_key_pressed(g_key_flare))
				toggle(g_sun_flare_off, "lens flare removal");
			if (runtime->is_key_pressed(g_key_bloom))
				toggle(g_bloom_off, "bloom removal");
			if (runtime->is_key_pressed(g_key_trace))
				g_trace_requested = true;
		}

		update_rates();
	}

	// ---------------------------------------------------------------------------------------------
	// Settings panel in the ReShade overlay (Add-ons tab)

	void draw_overlay(effect_runtime *)
	{
		bool value = g_sun_flare_off;
		if (ImGui::Checkbox(("Remove the sun lens flare (Ctrl+Shift+" + key_name(g_key_flare) + ")").c_str(), &value))
			toggle(g_sun_flare_off, "lens flare removal");

		// Parts of the lens flare to keep
		static const char *const labels[camfx::flare_part_count] = { "Keep the glow around the sun", "Keep the star rays", "Keep the lens ghosts" };
		static const char *const log_names[camfx::flare_part_count] = { "keep glow", "keep star rays", "keep ghosts" };
		ImGui::Indent();
		ImGui::BeginDisabled(!g_sun_flare_off);
		for (size_t i = 0; i < camfx::flare_part_count; ++i)
		{
			value = g_keep[i];
			if (ImGui::Checkbox(labels[i], &value))
				toggle(g_keep[i], log_names[i]);
		}
		ImGui::EndDisabled();
		ImGui::Unindent();

		value = g_bloom_off;
		if (ImGui::Checkbox(("Remove bloom and lens dirt (Ctrl+Shift+" + key_name(g_key_bloom) + ")").c_str(), &value))
			toggle(g_bloom_off, "bloom removal");

		ImGui::Separator();
		ImGui::Text("Per second: %llu lens flare quads removed, %llu kept, %llu tonemap binds swapped", g_rates.flare_skipped, g_rates.flare_kept, g_rates.bloom_swapped);
		if (g_rates.bloom_kept != 0)
			ImGui::Text("Per second: %llu tonemap draws still with bloom", g_rates.bloom_kept);
		ImGui::Text("Tonemap pipelines with bloom: %u, %u with a no-bloom twin", g_pso_count[size_t(pso_kind::tonemap_bloom)].load(), g_twin_count.load());

		if (g_twins_tried && !g_twins_ok)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Bloom: the .twins file is missing or made for another sim version (see the log)");
		else if (g_twin_failed != 0)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Bloom: %u no-bloom tonemap pipeline(s) could not be created (see the log)", g_twin_failed.load());

		ImGui::Spacing();
		if (ImGui::Button(("Record a one-frame trace (Ctrl+Shift+" + key_name(g_key_trace) + ")").c_str()))
			g_trace_requested = true;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		if (!reshade::register_addon(module))
			return FALSE;

		wchar_t path[MAX_PATH] = L"";
		GetModuleFileNameW(module, path, MAX_PATH);
		std::wstring base = path;
		base.erase(base.find_last_of(L'.'));
		g_ini_path = base + L".ini";
		g_log_path = base + L".log";
		g_trace_path = base + L".trace.txt";
		g_twins_path = base + L".twins";

		load_settings();
		g_log = _wfsopen(g_log_path.c_str(), L"w", _SH_DENYWR);

		wchar_t exe[MAX_PATH] = L"";
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		log("MSFS 2024 Camera FX Off %s (built %s %s, ReShade API %d)", CAMFX_VERSION_STRING, __DATE__, __TIME__, RESHADE_API_VERSION);
		log("host %S", exe);
		log("settings: sun_flare_off=%d %s bloom_off=%d keys Ctrl+Shift+%s/%s/%s", int(g_sun_flare_off.load()), keep_text().c_str(), int(g_bloom_off.load()),
			key_name(g_key_flare).c_str(), key_name(g_key_bloom).c_str(), key_name(g_key_trace).c_str());

		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_overlay(nullptr, draw_overlay);
		break;
	}
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		if (g_log != nullptr)
		{
			log("unloaded");
			const std::lock_guard<std::mutex> lock(g_log_mutex);
			fclose(g_log);
			g_log = nullptr;
		}
		break;
	}
	return TRUE;
}
