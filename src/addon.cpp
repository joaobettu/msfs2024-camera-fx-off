// MSFS 2024 Camera FX Off, by PdPanoo
// ReShade add-on that removes the "filmed with a camera" look from Microsoft Flight Simulator 2024:
// - the lens flare, by skipping the quads the game draws for it right after upscaling; its glow, star rays and ghosts can
//   each be kept from the overlay (the glow is by default), at the game's size or smaller (50% by default);
// - bloom and its lens dirt, by binding the game's own tonemapping permutation without bloom in place of the one with it,
//   or, for a weaker bloom, the tonemapping shader rebuilt with the bloom scaled down;
// - the star rays on lights (city, airport and aircraft lights), by binding their glow and flare pixel shaders rebuilt
//   without them, with a smaller glow (50% by default).
// It only uses ReShade events: no hooks of its own, and little per-call work (a lock-free table lookup on pipeline binds,
// a view format lookup on render target binds without depth).

#include <imgui.h>
#include <reshade.hpp>

#include "classify.hpp"
#include "dxil_patch.hpp"
#include "shortcut.hpp"
#include "twins.hpp"
#include "version.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Removes the lens flare and the star rays on lights from Microsoft Flight Simulator 2024, and removes or weakens its bloom with lens dirt.";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/joaobettu/msfs2024-camera-fx-off";
extern "C" __declspec(dllexport) const char *ISSUES = "https://github.com/joaobettu/msfs2024-camera-fx-off/issues";

namespace
{
	// ---------------------------------------------------------------------------------------------
	// Settings, stored in msfs2024-camera-fx-off.ini next to the add-on

	std::atomic<bool> g_sun_flare_off { true };
	// Parts of the lens flare still drawn with its removal on, by camfx::flare_part: the glow by default.
	std::atomic<bool> g_keep[camfx::flare_part_count] = { true, false, false };
	const wchar_t *const g_keep_keys[camfx::flare_part_count] = { L"keep_glow", L"keep_star", L"keep_ghosts" };
	// Size of the kept parts, as an index in dxil::glow_sizes (the game's size first): 50% by default. Stored in percent.
	std::atomic<uint32_t> g_flare_size { 2 };
	// Bloom strength, as an index in dxil::bloom_strengths (the game's first): none by default. Stored in percent. It applies
	// while g_bloom_off is set; Ctrl+Shift+F6 clears it to go back to the game's bloom.
	std::atomic<uint32_t> g_bloom_strength { dxil::bloom_strength_count - 1 };
	std::atomic<bool> g_bloom_off { true };
	// Star rays on the glow of lights: the light glow pixel shaders are rebuilt without them.
	std::atomic<bool> g_light_star_off { true };
	// Size of the glow left around those lights, as an index in dxil::glow_sizes (the game's size first): 50% by default.
	// Stored in percent.
	std::atomic<uint32_t> g_light_glow_size { 2 };
	// Shortcuts, by action: Ctrl+Shift+F5, F6 and F7 by default, and any other from the overlay. F9 to F12 are taken by the
	// Glass Cockpit DLSS Fix add-on by default. The frame trace has no shortcut: it is recorded from the overlay. Used on the
	// present thread only (presents and the overlay).
	enum action : size_t
	{
		action_flare,
		action_bloom,
		action_light_star,
		action_count,
	};
	constexpr camfx::shortcut default_shortcuts[action_count] = { { VK_F5, true, true, false }, { VK_F6, true, true, false }, { VK_F7, true, true, false } };
	camfx::shortcut g_shortcuts[action_count] = { default_shortcuts[0], default_shortcuts[1], default_shortcuts[2] };
	const wchar_t *const g_shortcut_keys[action_count] = { L"key_flare", L"key_bloom", L"key_light_star" };
	// Settings that could not be read, for the log (which is opened after the settings are read).
	std::string g_settings_notes;

	std::wstring g_ini_path, g_log_path, g_trace_path, g_twins_path, g_pak_override, g_dxcompiler_override;

	std::wstring widen(const std::string &s)
	{
		std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
		return w;
	}

	std::string narrow(const std::wstring &s)
	{
		std::string n(WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr), '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), n.data(), int(n.size()), nullptr, nullptr);
		return n;
	}

	// A path without its folders. The log only names files, so it never shows where the user installed the game,
	// which matters since people attach it to public bug reports.
	std::wstring file_name(const std::wstring &path)
	{
		return path.substr(path.find_last_of(L"\\/") + 1);
	}

	void load_settings()
	{
		const auto get = [](const wchar_t *key, int def) { return int(GetPrivateProfileIntW(L"settings", key, def, g_ini_path.c_str())); };
		// The offered percentage nearest to the one stored, as an index in 'offered'.
		const auto nearest = [&](const wchar_t *key, int def, const int *offered, size_t count) {
			const int percent = get(key, def);
			uint32_t found = 0;
			for (uint32_t i = 1; i < count; ++i)
				if (std::abs(offered[i] - percent) < std::abs(offered[found] - percent))
					found = i;
			return found;
		};
		g_sun_flare_off = get(L"sun_flare_off", 1) != 0;
		for (size_t i = 0; i < camfx::flare_part_count; ++i)
			g_keep[i] = get(g_keep_keys[i], i == size_t(flare_part::glow)) != 0;
		g_flare_size = nearest(L"flare_size", 50, dxil::glow_sizes, dxil::glow_size_count);
		g_bloom_off = get(L"bloom_off", 1) != 0;
		g_bloom_strength = nearest(L"bloom_strength", 0, dxil::bloom_strengths, dxil::bloom_strength_count);
		g_light_star_off = get(L"light_star_off", 1) != 0;
		g_light_glow_size = nearest(L"light_glow_size", 50, dxil::glow_sizes, dxil::glow_size_count);
		// Shortcuts as text ("Ctrl+Shift+F5"), or the key code of earlier versions, always pressed with Ctrl+Shift.
		for (size_t i = 0; i < action_count; ++i)
		{
			wchar_t text[64] = L"";
			if (GetPrivateProfileStringW(L"settings", g_shortcut_keys[i], L"", text, DWORD(std::size(text)), g_ini_path.c_str()) == 0)
				continue;
			camfx::shortcut read;
			if (camfx::parse_shortcut(narrow(text), read))
				g_shortcuts[i] = read;
			else
				g_settings_notes += "settings: " + narrow(g_shortcut_keys[i]) + "=" + narrow(text) + " not understood, so " +
					camfx::shortcut_text(g_shortcuts[i]) + " is used\n";
		}

		wchar_t path[MAX_PATH] = L"";
		GetPrivateProfileStringW(L"settings", L"pak_path", L"", path, MAX_PATH, g_ini_path.c_str());
		g_pak_override = path;
		GetPrivateProfileStringW(L"settings", L"dxcompiler_path", L"", path, MAX_PATH, g_ini_path.c_str());
		g_dxcompiler_override = path;
	}

	void save_settings()
	{
		const auto put = [](const wchar_t *key, bool value) { WritePrivateProfileStringW(L"settings", key, value ? L"1" : L"0", g_ini_path.c_str()); };
		put(L"sun_flare_off", g_sun_flare_off);
		for (size_t i = 0; i < camfx::flare_part_count; ++i)
			put(g_keep_keys[i], g_keep[i]);
		WritePrivateProfileStringW(L"settings", L"flare_size", std::to_wstring(dxil::glow_sizes[g_flare_size]).c_str(), g_ini_path.c_str());
		put(L"bloom_off", g_bloom_off);
		WritePrivateProfileStringW(L"settings", L"bloom_strength", std::to_wstring(dxil::bloom_strengths[g_bloom_strength]).c_str(), g_ini_path.c_str());
		put(L"light_star_off", g_light_star_off);
		WritePrivateProfileStringW(L"settings", L"light_glow_size", std::to_wstring(dxil::glow_sizes[g_light_glow_size]).c_str(), g_ini_path.c_str());
		for (size_t i = 0; i < action_count; ++i)
			WritePrivateProfileStringW(L"settings", g_shortcut_keys[i], widen(camfx::shortcut_text(g_shortcuts[i])).c_str(), g_ini_path.c_str());
		// Keys of earlier versions
		for (const wchar_t *const key : { L"flare_keep", L"light_glow_off", L"light_point_off", L"key_trace" })
			WritePrivateProfileStringW(L"settings", key, nullptr, g_ini_path.c_str());
	}

	std::string keep_text()
	{
		char text[96];
		snprintf(text, sizeof(text), "keep_glow=%d keep_star=%d keep_ghosts=%d flare_size=%d",
			int(g_keep[size_t(flare_part::glow)].load()), int(g_keep[size_t(flare_part::star)].load()), int(g_keep[size_t(flare_part::ghosts)].load()),
			dxil::glow_sizes[g_flare_size]);
		return text;
	}

	// The bloom strength in use, as an index in dxil::bloom_strengths: the game's (0) while Ctrl+Shift+F6 has switched to it.
	uint32_t bloom_in_use()
	{
		return g_bloom_off.load(std::memory_order_relaxed) ? g_bloom_strength.load(std::memory_order_relaxed) : 0;
	}

	std::string bloom_text()
	{
		char text[48];
		snprintf(text, sizeof(text), "bloom_off=%d bloom_strength=%d", int(g_bloom_off.load()), dxil::bloom_strengths[g_bloom_strength]);
		return text;
	}

	std::string lights_text()
	{
		char text[64];
		snprintf(text, sizeof(text), "light_star_off=%d light_glow_size=%d", int(g_light_star_off.load()), dxil::glow_sizes[g_light_glow_size]);
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
		// The address of the bloom_twins of a tonemap_bloom pipeline, or of the light_twins of a light_glow, light_flare or
		// material one (zero if none could be made)
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
	// Tonemaps whose bloom can be removed, and those whose bloom can be weakened (see bloom_twins). Twins of the package that
	// could not be used: not in the .twins file, different in the package (another sim version), or refused by the device.
	std::atomic<uint32_t> g_twin_count { 0 }, g_twin_missing { 0 }, g_twin_differs { 0 }, g_twin_failed { 0 }, g_weaker_count { 0 }, g_weaker_failed { 0 };

	// Entry points and short digests of every pipeline's shaders, to label frame traces and the log.
	std::shared_mutex g_name_mutex;
	std::unordered_map<uint64_t, std::string> g_names;

	// ---------------------------------------------------------------------------------------------
	// Tonemapping twins (pairs read from the .twins file, shaders read from the game's shader package)

	std::once_flag g_twins_once;
	camfx::twin_table g_twins;
	std::wstring g_pak_path;
	// Whether the twins can be read from the shader package, or why not. Without them, the add-on removes the bloom with the
	// tonemapping shaders it rebuilds (see create_bloom_twins).
	enum class twins_state : uint8_t
	{
		not_loaded,
		ok,
		missing,       // no .twins file next to the add-on
		unreadable,    // a .twins file that cannot be parsed
		no_package,    // the shader package it names is not where the sim keeps it
		other_package, // a shader package of another size: another sim version
	};
	std::atomic<twins_state> g_twins_state { twins_state::not_loaded };

	void load_twins()
	{
		// What the add-on does instead, for the log.
		static const char *const instead = "the add-on rebuilds the tonemapping shaders to remove the bloom instead";
		std::string text, error;
		if (!camfx::read_file(g_twins_path, text))
		{
			log("twins: %S not found next to the add-on; %s", file_name(g_twins_path).c_str(), instead);
			g_twins_state = twins_state::missing;
			return;
		}
		if (!camfx::parse_twin_table(text, g_twins, error))
		{
			log("twins: %S: %s; %s", file_name(g_twins_path).c_str(), error.c_str(), instead);
			g_twins_state = twins_state::unreadable;
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
			log("twins: shader package %S not found %s (sim updated?); %s", file_name(g_pak_path).c_str(),
				g_pak_override.empty() ? "in the game's Shaders folder" : "at the pak_path from the settings", instead);
			g_twins_state = twins_state::no_package;
			return;
		}
		const uint64_t size = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
		if (g_twins.pak_size != 0 && size != g_twins.pak_size)
		{
			log("twins: shader package is %llu bytes but the .twins file expects %llu: the .twins file is for another version of the sim's shaders "
				"(a matching one comes with the add-on version for that sim version, or from tools\\gen_twins.ps1); %s", size, g_twins.pak_size, instead);
			g_twins_state = twins_state::other_package;
			return;
		}

		g_twins_state = twins_state::ok;
		log("twins: %zu pairs for %S", g_twins.entries.size(), file_name(g_pak_path).c_str());
	}

	// Set while the add-on creates a pipeline of its own, so that on_init_pipeline leaves it alone if ReShade reports it.
	thread_local bool t_creating_twin = false;

	// The same pipeline description with another shader for one stage, which binds a subset of the same resources and
	// keeps the same signatures, so it works with the same root signature, root arguments and input layout. Zero if the
	// device refuses it.
	uint64_t create_pipeline_with(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline_subobject_type stage,
		const shader_desc &shader, const std::vector<uint8_t> &code)
	{
		struct guard
		{
			guard() { t_creating_twin = true; }
			~guard() { t_creating_twin = false; }
		} creating;
		shader_desc twin_shader = shader;
		twin_shader.code = code.data();
		twin_shader.code_size = code.size();
		std::vector<pipeline_subobject> desc(subobjects, subobjects + count);
		for (pipeline_subobject &subobject : desc)
		{
			if (subobject.type == stage)
			{
				subobject.count = 1;
				subobject.data = &twin_shader;
			}
		}
		pipeline twin = {};
		return dev->create_pipeline(layout, count, desc.data(), &twin) ? twin.handle : 0;
	}

	uint64_t create_twin(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, const shader_desc &ps)
	{
		std::call_once(g_twins_once, load_twins);

		camfx::digest16 digest;
		if (g_twins_state != twins_state::ok || !dxbc::digest(ps.code, ps.code_size, digest.bytes))
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
		bool differs = false;
		if (!camfx::read_twin(g_pak_path, it->second, code, error, &differs))
		{
			log("twins: tonemapping %s: %s", camfx::to_hex(digest).c_str(), error.c_str());
			(differs ? g_twin_differs : g_twin_failed)++;
			return 0;
		}

		// The pixel shader swapped for the game's permutation without bloom.
		const uint64_t twin = create_pipeline_with(dev, layout, count, subobjects, pipeline_subobject_type::pixel_shader, ps, code);
		if (twin == 0)
		{
			log("twins: creating the no-bloom pipeline for %s failed", camfx::to_hex(digest).c_str());
			g_twin_failed++;
			return 0;
		}

		log("twins: tonemapping %s uses no-bloom twin %s", camfx::to_hex(digest).c_str(), camfx::to_hex(it->second.twin).c_str());
		return twin;
	}

	// ---------------------------------------------------------------------------------------------
	// Lights without star rays: the light glow and light flare pixel shaders rebuilt by the game's dxcompiler.dll (see
	// dxil_patch.hpp), once per shader and glow size. Each of their pipelines then gets a twin with each, one of them bound
	// in its place.
	// The lens flare quads get smaller twins the same way, from their vertex shader: those are drawn in place of the game's
	// for the kept parts of the lens flare (see filter_draw).

	// The twins of a light glow, light flare or lens flare pipeline by size (dxil::glow_sizes). For lights, the one at the
	// game's size always exists (without star rays); for the lens flare, it never does (the game's own pipeline).
	struct light_twins
	{
		uint64_t pipelines[dxil::glow_size_count] = {};
	};

	std::mutex g_rebuild_mutex;
	dxil::compiler g_dxc;
	bool g_dxc_tried = false, g_dxc_ok = false;
	// Rebuilt shaders by the original's digest, one per size or strength; empty when it could not be rebuilt (the lights then
	// stay as they are, the lens flare keeps its size, and the bloom can only be kept or removed).
	std::unordered_map<camfx::digest16, std::vector<std::vector<uint8_t>>, camfx::digest16_hash> g_rebuilt;
	std::atomic<uint32_t> g_starless_count { 0 }, g_starless_failed { 0 }, g_quad_count { 0 };
	std::atomic<bool> g_dxc_unavailable { false };

	// Loads the game's dxcompiler.dll the first time; false if it cannot be used. Called with g_rebuild_mutex held.
	bool dxc_loaded()
	{
		if (!g_dxc_tried)
		{
			// The sim ships dxcompiler.dll next to its executable.
			g_dxc_tried = true;
			std::wstring path = g_dxcompiler_override;
			if (path.empty())
			{
				wchar_t exe[MAX_PATH] = L"";
				GetModuleFileNameW(nullptr, exe, MAX_PATH);
				path = exe;
				path.erase(path.find_last_of(L"\\/") + 1);
				path += L"Dxcompiler.DLL";
			}
			std::string error;
			g_dxc_ok = g_dxc.load(path, error);
			if (!g_dxc_ok)
			{
				log("dxcompiler: %s (%S); the star rays stay, the lens flare keeps its size, and bloom can only be kept or removed", error.c_str(), file_name(path).c_str());
				g_dxc_unavailable = true;
			}
		}
		return g_dxc_ok;
	}

	// The address of a new light_twins, or zero. 'shader' is the pixel shader of lights, the vertex shader of lens flare quads.
	uint64_t create_light_twins(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, const shader_desc &shader,
		dxil::light_shader kind)
	{
		const bool quad = kind == dxil::light_shader::flare_quad;
		camfx::digest16 digest;
		if (!dxbc::digest(shader.code, shader.code_size, digest.bytes))
			return 0;

		std::vector<std::vector<uint8_t>> code;
		{
			const std::lock_guard<std::mutex> lock(g_rebuild_mutex);
			if (!dxc_loaded())
				return 0;

			auto it = g_rebuilt.find(digest);
			if (it == g_rebuilt.end())
			{
				std::vector<std::vector<uint8_t>> made;
				std::string what;
				const ULONGLONG start = GetTickCount64();
				const bool ok = g_dxc.make_light_variants(kind, shader.code, shader.code_size, made, what);
				std::string sizes;
				for (size_t i = 0; ok && i < dxil::glow_size_count; ++i)
					if (!made[i].empty())
						sizes += (sizes.empty() ? "" : "/") + std::to_string(dxil::glow_sizes[i]);
				const std::string result = !ok ? "left as is" : quad ? "rebuilt with smaller quads, sizes " + sizes + "%" : "rebuilt without star rays, glow sizes " + sizes + "%";
				const std::string entry(camfx::entry_point(shader.code, shader.code_size));
				log("%s: %s %s %s in %llu ms: %s", quad ? "lens flare" : "lights", entry.c_str(), camfx::to_hex(digest).c_str(), result.c_str(), GetTickCount64() - start, what.c_str());
				it = g_rebuilt.emplace(digest, ok ? std::move(made) : std::vector<std::vector<uint8_t>>()).first;
			}
			code = it->second;
		}
		if (code.empty())
		{
			if (!quad)
				g_starless_failed++;
			return 0;
		}

		auto twins = std::make_unique<light_twins>();
		bool any = false;
		for (size_t i = 0; i < dxil::glow_size_count; ++i)
		{
			if (code[i].empty())
				continue;
			twins->pipelines[i] = create_pipeline_with(dev, layout, count, subobjects, quad ? pipeline_subobject_type::vertex_shader : pipeline_subobject_type::pixel_shader,
				shader, code[i]);
			any |= twins->pipelines[i] != 0;
			if (twins->pipelines[i] == 0)
				log("%s (size %d%%) for %s failed", quad ? "lens flare: creating the pipeline with smaller quads" : "lights: creating the pipeline without star rays",
					dxil::glow_sizes[i], camfx::to_hex(digest).c_str());
		}
		if (quad ? !any : twins->pipelines[0] == 0)
		{
			for (const uint64_t pipeline : twins->pipelines)
				if (pipeline != 0)
					dev->destroy_pipeline({ pipeline });
			if (!quad)
				g_starless_failed++;
			return 0;
		}
		(quad ? g_quad_count : g_starless_count)++;
		return reinterpret_cast<uint64_t>(twins.release());
	}

	// The pipelines bound in place of a tonemap_bloom one, by strength (dxil::bloom_strengths). The game's strength is its
	// own pipeline, so the first is always zero. The weaker ones use the tonemapping shader rebuilt with the bloom scaled
	// down (dxil::patch_bloom); the last, without bloom, uses the game's permutation without bloom from the shader package,
	// or the rebuilt shader when the package has none (permutations the sim compiles at run time, or an outdated .twins).
	struct bloom_twins
	{
		uint64_t pipelines[dxil::bloom_strength_count] = {};
	};

	// The address of a new bloom_twins, or zero.
	uint64_t create_bloom_twins(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, const shader_desc &ps)
	{
		constexpr size_t none = dxil::bloom_strength_count - 1;
		auto twins = std::make_unique<bloom_twins>();
		twins->pipelines[none] = create_twin(dev, layout, count, subobjects, ps);

		camfx::digest16 digest;
		std::vector<std::vector<uint8_t>> code;
		if (dxbc::digest(ps.code, ps.code_size, digest.bytes))
		{
			const std::lock_guard<std::mutex> lock(g_rebuild_mutex);
			if (dxc_loaded())
			{
				auto it = g_rebuilt.find(digest);
				if (it == g_rebuilt.end())
				{
					std::vector<std::vector<uint8_t>> made;
					std::string what;
					const ULONGLONG start = GetTickCount64();
					const bool ok = g_dxc.make_bloom_variants(ps.code, ps.code_size, made, what);
					std::string strengths;
					for (size_t i = 1; ok && i < dxil::bloom_strength_count; ++i)
						if (!made[i].empty())
							strengths += (strengths.empty() ? "" : "/") + std::to_string(dxil::bloom_strengths[i]);
					const std::string result = ok ? "rebuilt with weaker bloom, strengths " + strengths + "%" : "left as is";
					log("bloom: %s %s %s in %llu ms: %s", std::string(camfx::entry_point(ps.code, ps.code_size)).c_str(), camfx::to_hex(digest).c_str(), result.c_str(),
						GetTickCount64() - start, what.c_str());
					it = g_rebuilt.emplace(digest, ok ? std::move(made) : std::vector<std::vector<uint8_t>>()).first;
				}
				code = it->second;
			}
		}
		if (code.empty())
			g_weaker_failed++;

		bool weaker = false;
		for (size_t i = 1; i < code.size(); ++i)
		{
			if (code[i].empty() || twins->pipelines[i] != 0)
				continue;
			twins->pipelines[i] = create_pipeline_with(dev, layout, count, subobjects, pipeline_subobject_type::pixel_shader, ps, code[i]);
			if (twins->pipelines[i] == 0)
				log("bloom: creating the pipeline with weaker bloom (strength %d%%) for %s failed", dxil::bloom_strengths[i], camfx::to_hex(digest).c_str());
			else if (i == none)
				log("bloom: tonemapping %s has no usable twin in the shader package; the rebuilt shader removes its bloom", camfx::to_hex(digest).c_str());
			else
				weaker = true;
		}

		if (twins->pipelines[none] == 0 && !weaker)
			return 0;
		if (twins->pipelines[none] != 0)
			g_twin_count++;
		if (weaker)
			g_weaker_count++;
		return reinterpret_cast<uint64_t>(twins.release());
	}

	// ---------------------------------------------------------------------------------------------
	// Per command list state, and the frame trace (from the overlay): pipeline binds, render targets, draws and dispatches
	// recorded between two presents

	struct __declspec(uuid("2f9c1b7e-4a3d-4c8e-9b1f-6e2d8a0c5b71")) cmd_state
	{
		pso_kind kind = pso_kind::none;
		uint64_t pipeline = 0;
		pipeline_stage stages = pipeline_stage::all;
		// Lens flare element name of the bound material pipeline (pso_info::tag), and its light_twins with smaller quads
		uint64_t tag = 0, twins = 0;
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
			log("trace: cannot write %S", file_name(g_trace_path).c_str());
			return;
		}
		fprintf(file, "# MSFS 2024 Camera FX Off %s frame trace: sun_flare_off=%d %s %s %s, %zu lines%s\n",
			CAMFX_VERSION_STRING, int(g_sun_flare_off.load()), keep_text().c_str(), bloom_text().c_str(), lights_text().c_str(),
			lines.size(), lines.size() >= max_trace_lines ? " (truncated)" : "");
		for (const std::string &line : lines)
			fprintf(file, "%s\n", line.c_str());
		fclose(file);
		log("trace: wrote %zu lines to %S", lines.size(), file_name(g_trace_path).c_str());
	}

	// ---------------------------------------------------------------------------------------------
	// Statistics

	std::atomic<uint64_t> g_flare_skipped { 0 }, g_flare_kept { 0 }, g_bloom_swapped { 0 }, g_bloom_kept { 0 }, g_star_swapped { 0 };

	struct rates
	{
		uint64_t flare_skipped, flare_kept, bloom_swapped, bloom_kept, star_swapped;

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
		const rates totals = { g_flare_skipped.load(), g_flare_kept.load(), g_bloom_swapped.load(), g_bloom_kept.load(), g_star_swapped.load() };
		const auto per_second = [seconds](uint64_t a, uint64_t b) { return uint64_t(double(a - b) / seconds + 0.5); };
		g_rates = {
			per_second(totals.flare_skipped, g_totals.flare_skipped),
			per_second(totals.flare_kept, g_totals.flare_kept),
			per_second(totals.bloom_swapped, g_totals.bloom_swapped),
			per_second(totals.bloom_kept, g_totals.bloom_kept),
			per_second(totals.star_swapped, g_totals.star_swapped) };
		g_totals = totals;
		g_rate_tick = now;

		// Log only when something changed, to keep the log small.
		if (now - g_log_tick >= 10000 && !(g_rates == g_logged))
		{
			log("stats/s: flare quads skipped %llu, flare quads kept %llu, tonemap binds swapped %llu, tonemap with bloom drawn %llu, light glow binds without star rays %llu",
				g_rates.flare_skipped, g_rates.flare_kept, g_rates.bloom_swapped, g_rates.bloom_kept, g_rates.star_swapped);
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

	bool flare_quad_guarded(const shader_desc *vs)
	{
		__try
		{
			return vs != nullptr && camfx::is_flare_quad_vs(vs->code, vs->code_size);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
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

	void destroy_twin(device *dev, const pso_info &info)
	{
		if (info.twin == 0)
			return;
		if (info.kind == pso_kind::light_glow || info.kind == pso_kind::light_flare || info.kind == pso_kind::material)
		{
			const light_twins *const twins = reinterpret_cast<const light_twins *>(info.twin);
			for (const uint64_t pipeline : twins->pipelines)
				if (pipeline != 0)
					dev->destroy_pipeline({ pipeline });
			delete twins;
			(info.kind == pso_kind::material ? g_quad_count : g_starless_count)--;
			return;
		}
		if (info.kind == pso_kind::tonemap_bloom)
		{
			const bloom_twins *const twins = reinterpret_cast<const bloom_twins *>(info.twin);
			bool weaker = false;
			for (size_t i = 0; i < dxil::bloom_strength_count; ++i)
			{
				if (twins->pipelines[i] == 0)
					continue;
				dev->destroy_pipeline({ twins->pipelines[i] });
				weaker |= i != dxil::bloom_strength_count - 1;
			}
			if (twins->pipelines[dxil::bloom_strength_count - 1] != 0)
				g_twin_count--;
			if (weaker)
				g_weaker_count--;
			delete twins;
		}
	}

	void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipeline)
	{
		if (t_creating_twin)
			return;
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
				destroy_twin(dev, existing);
			}
		}
		if (kind == pso_kind::none)
			return;

		pso_info info;
		info.kind = kind;
		if (kind == pso_kind::tonemap_bloom)
			info.twin = create_bloom_twins(dev, layout, count, subobjects, *ps);
		if (kind == pso_kind::light_glow || kind == pso_kind::light_flare)
			info.twin = create_light_twins(dev, layout, count, subobjects, *ps, kind == pso_kind::light_flare ? dxil::light_shader::flare : dxil::light_shader::glow);
		if (kind == pso_kind::material)
		{
			info.tag = camfx::flare_tag(digest_prefix_guarded(vs), digest_prefix_guarded(ps));
			// Pipelines that may draw a lens flare quad get twins with smaller quads, for its kept parts.
			if (flare_quad_guarded(vs))
				info.twin = create_light_twins(dev, layout, count, subobjects, *vs, dxil::light_shader::flare_quad);
		}

		bool inserted;
		{
			const std::lock_guard<std::mutex> lock(g_pso_mutex);
			inserted = g_psos.insert(pipeline.handle, info);
		}
		if (!inserted)
		{
			log("pipeline table full, ignoring %s pipeline %016llx", camfx::kind_name(kind), pipeline.handle);
			destroy_twin(dev, info);
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
			destroy_twin(dev, info);
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
			state->twins = 0;
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
		const uint64_t bloom_twin = kind == pso_kind::tonemap_bloom && info.twin != 0 ? reinterpret_cast<const bloom_twins *>(info.twin)->pipelines[bloom_in_use()] : 0;
		if (bloom_twin != 0)
		{
			// Called right after the game's SetPipelineState, so this replaces it for the draws that follow.
			cmd->bind_pipeline(stages, { bloom_twin });
			g_bloom_swapped.fetch_add(1, std::memory_order_relaxed);
			kind = pso_kind::none;
			swapped = true;
		}
		else if ((kind == pso_kind::light_glow || kind == pso_kind::light_flare) && info.twin != 0 && g_light_star_off.load(std::memory_order_relaxed))
		{
			const light_twins &twins = *reinterpret_cast<const light_twins *>(info.twin);
			const uint64_t sized = twins.pipelines[g_light_glow_size.load(std::memory_order_relaxed)];
			cmd->bind_pipeline(stages, { sized != 0 ? sized : twins.pipelines[0] });
			g_star_swapped.fetch_add(1, std::memory_order_relaxed);
			swapped = true;
		}

		if (cmd_state *const state = cmd->get_private_data<cmd_state>())
		{
			state->kind = kind;
			state->pipeline = pipeline.handle;
			state->stages = stages;
			state->tag = known ? info.tag : 0;
			state->twins = known && info.kind == pso_kind::material ? info.twin : 0;
		}

		if (g_tracing.load(std::memory_order_relaxed))
			trace(cmd, "bind %s%s%s", pipeline_label(pipeline.handle).c_str(), known ? (std::string(" [") + camfx::kind_name(info.kind) + "]").c_str() : "", swapped ? " -> twin" : "");
	}

	enum class draw_type
	{
		draw,
		draw_indexed,
		indirect,
	};

	// 'first_vertex' and 'first_instance' are only used for draws of type draw.
	bool filter_draw(command_list *cmd, draw_type type, uint32_t count, uint32_t instances, uint32_t first_vertex = 0, uint32_t first_instance = 0)
	{
		cmd_state *const state = cmd->get_private_data<cmd_state>();
		// What the draw was recognized as, for the trace
		const char *what = nullptr;
		bool skip = false, kept = false, smaller = false;
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
					// A kept part at a smaller size: drawn here with the twin, then the game's pipeline is bound again for what
					// follows, and the game's own draw is skipped.
					const uint32_t size = g_flare_size.load(std::memory_order_relaxed);
					const uint64_t twin = size != 0 && state->twins != 0 ? reinterpret_cast<const light_twins *>(state->twins)->pipelines[size] : 0;
					if (twin != 0)
					{
						cmd->bind_pipeline(state->stages, { twin });
						cmd->draw(count, instances, first_vertex, first_instance);
						cmd->bind_pipeline(state->stages, { state->pipeline });
						skip = smaller = true;
					}
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
			// Drawn with the game's bloom although another strength is set: no twin for it.
			if (bloom_in_use() != 0)
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
				snprintf(note, sizeof(note), " [%s%s]", smaller ? "kept, smaller: " : skip ? "skipped: " : kept ? "kept: " : "", what);
			trace(cmd, "%s %u %u%s", names[int(type)], count, instances, note);
		}
		return skip;
	}

	bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
	{
		return filter_draw(cmd, draw_type::draw, vertices, instances, first_vertex, first_instance);
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

	// The shortcut being set from the overlay (action_count when none), and whether one was set this frame: the keys pressed
	// for it must not also fire it.
	size_t g_capture = action_count;
	bool g_captured_now = false;

	void on_present(effect_runtime *runtime)
	{
		if (g_tracing.load())
			finish_trace();
		else if (g_trace_requested.exchange(false))
			g_tracing = true;

		// A shortcut fires when its key is pressed with exactly its modifiers.
		if (g_capture == action_count && !g_captured_now)
		{
			const bool ctrl = runtime->is_key_down(VK_CONTROL), shift = runtime->is_key_down(VK_SHIFT), alt = runtime->is_key_down(VK_MENU);
			const auto pressed = [&](action a) {
				const camfx::shortcut &s = g_shortcuts[a];
				return s.key != 0 && s.ctrl == ctrl && s.shift == shift && s.alt == alt && runtime->is_key_pressed(s.key);
			};
			if (pressed(action_flare))
				toggle(g_sun_flare_off, "lens flare removal");
			if (pressed(action_bloom))
			{
				// Between the strength set and the game's bloom.
				g_bloom_off = !g_bloom_off;
				save_settings();
				log("bloom at %d%%%s", dxil::bloom_strengths[bloom_in_use()], g_bloom_off ? "" : " (the game's)");
			}
			if (pressed(action_light_star))
				toggle(g_light_star_off, "light star ray removal");
		}
		g_captured_now = false;

		update_rates();
	}

	// ReShade's own shortcut that 's' is, by the name of its setting ([INPUT] in ReShade.ini: "key code,ctrl,shift,alt"), or
	// null.
	const char *reshade_shortcut(effect_runtime *runtime, const camfx::shortcut &s)
	{
		static const char *const keys[] = { "KeyOverlay", "KeyEffects", "KeyScreenshot", "KeyReload", "KeyNextPreset", "KeyPreviousPreset", "KeyPerformanceMode",
			"KeyFPS", "KeyFrameTime" };
		for (const char *const key : keys)
		{
			char value[64] = "";
			size_t size = sizeof(value);
			if (!reshade::get_config_value(runtime, "INPUT", key, value, &size))
				continue;
			unsigned code = 0, ctrl = 0, shift = 0, alt = 0;
			if (sscanf_s(value, "%u,%u,%u,%u", &code, &ctrl, &shift, &alt) == 4 && camfx::shortcut { code, ctrl != 0, shift != 0, alt != 0 } == s)
				return key;
		}
		return nullptr;
	}

	// While a shortcut is being set: the next key pressed, with the modifiers held, unless it is already a shortcut of this
	// add-on or of ReShade. Esc cancels, Backspace removes the shortcut. The sim gets no keyboard input meanwhile.
	std::string g_shortcut_message;

	void capture_shortcut(effect_runtime *runtime)
	{
		if (g_capture == action_count)
			return;
		runtime->block_input_next_frame();
		camfx::shortcut s;
		if (runtime->is_key_pressed(VK_ESCAPE))
		{
			g_capture = action_count;
			g_captured_now = true;
			return;
		}
		if (!runtime->is_key_pressed(VK_BACK))
		{
			for (uint32_t key = VK_TAB; key <= 0xFE && s.key == 0; ++key)
			{
				const bool ignored = key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_ESCAPE || key == VK_LWIN || key == VK_RWIN ||
					(key >= VK_LSHIFT && key <= VK_RMENU);
				if (!ignored && runtime->is_key_pressed(key))
					s.key = key;
			}
			if (s.key == 0)
				return;
			s.ctrl = runtime->is_key_down(VK_CONTROL);
			s.shift = runtime->is_key_down(VK_SHIFT);
			s.alt = runtime->is_key_down(VK_MENU);
		}

		const size_t i = g_capture;
		g_capture = action_count;
		g_captured_now = true;
		const std::string text = camfx::shortcut_text(s);
		static const char *const names[action_count] = { "the lens flare removal", "the bloom", "the star ray removal on lights" };
		if (s.key != 0)
		{
			for (size_t j = 0; j < action_count; ++j)
			{
				if (j != i && g_shortcuts[j] == s)
				{
					g_shortcut_message = text + " is already the shortcut of " + names[j] + ", so it was not changed.";
					return;
				}
			}
			if (const char *const reshade = reshade_shortcut(runtime, s))
			{
				g_shortcut_message = text + " is ReShade's " + reshade + " shortcut, so it was not changed.";
				return;
			}
		}
		g_shortcuts[i] = s;
		g_shortcut_message.clear();
		save_settings();
		log("shortcut of %s: %s", names[i], text.c_str());
	}

	// An option's label, with its shortcut if it has one. The ImGui ID stays the same when the shortcut changes.
	std::string with_shortcut(const char *label, action a, const char *id)
	{
		const camfx::shortcut &s = g_shortcuts[a];
		return std::string(label) + (s.key != 0 ? " (" + camfx::shortcut_text(s) + ")" : std::string()) + "###" + id;
	}

	// ---------------------------------------------------------------------------------------------
	// Settings panel in the ReShade overlay (Add-ons tab)

	void draw_overlay(effect_runtime *runtime)
	{
		capture_shortcut(runtime);

		bool value = g_sun_flare_off;
		if (ImGui::Checkbox(with_shortcut("Remove the sun lens flare", action_flare, "flare").c_str(), &value))
			toggle(g_sun_flare_off, "lens flare removal");

		// A slider over the offered percentages, from the smallest on the left to the game's on the right, showing the percentage
		// (the slider's own value is the position, so the text it shows has no format specifier). 'index' is in the table of
		// percentages, the game's first; true when the slider moved it.
		const auto percent_slider = [](const char *label, const char *const *texts, size_t count, uint32_t &index) {
			const int last = int(count) - 1;
			int position = last - int(index);
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
			if (!ImGui::SliderInt(label, &position, 0, last, texts[last - position], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput) || uint32_t(last - position) == index)
				return false;
			index = uint32_t(last - position);
			return true;
		};
		static const char *const sizes[dxil::glow_size_count] = { "100%% (the game's)", "75%%", "50%%", "25%%" };
		static_assert(dxil::glow_sizes[0] == 100 && dxil::glow_sizes[1] == 75 && dxil::glow_sizes[2] == 50 && dxil::glow_sizes[3] == 25);
		const auto size_slider = [&](const char *label, std::atomic<uint32_t> &setting, const char *log_name) {
			uint32_t size = setting;
			if (!percent_slider(label, sizes, dxil::glow_size_count, size))
				return;
			setting = size;
			save_settings();
			log("%s %d%%", log_name, dxil::glow_sizes[size]);
		};

		// Parts of the lens flare to keep, and their size
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
		size_slider("Size of the kept parts", g_flare_size, "lens flare size");
		ImGui::EndDisabled();
		ImGui::Unindent();

		// Bloom strength: the one in use, which is the game's while Ctrl+Shift+F6 has switched to it. Moving the slider sets the
		// strength and switches back to it.
		static const char *const strengths[dxil::bloom_strength_count] = { "100%% (the game's)", "75%%", "50%%", "25%%", "0%% (none)" };
		static_assert(dxil::bloom_strengths[0] == 100 && dxil::bloom_strengths[1] == 75 && dxil::bloom_strengths[2] == 50 && dxil::bloom_strengths[3] == 25 && dxil::bloom_strengths[4] == 0);
		uint32_t strength = bloom_in_use();
		if (percent_slider(with_shortcut("Bloom and lens dirt", action_bloom, "bloom").c_str(), strengths, dxil::bloom_strength_count, strength))
		{
			g_bloom_strength = strength;
			g_bloom_off = true;
			save_settings();
			log("bloom at %d%%", dxil::bloom_strengths[strength]);
		}

		value = g_light_star_off;
		if (ImGui::Checkbox(with_shortcut("Remove the star rays on lights", action_light_star, "stars").c_str(), &value))
			toggle(g_light_star_off, "light star ray removal");

		// Size of the glow left around those lights
		ImGui::Indent();
		ImGui::BeginDisabled(!g_light_star_off);
		size_slider("Glow size around the lights", g_light_glow_size, "light glow size");
		ImGui::EndDisabled();
		ImGui::Unindent();

		// Shortcuts: a button for each, which takes the next keys pressed.
		if (ImGui::CollapsingHeader("Shortcuts"))
		{
			static const char *const shortcut_labels[action_count] = { "Lens flare removal on or off", "Bloom: the strength set or the game's", "Star ray removal on lights on or off" };
			for (size_t i = 0; i < action_count; ++i)
			{
				ImGui::PushID(int(i));
				const std::string text = g_capture == i ? std::string("Press the new keys...") : camfx::shortcut_text(g_shortcuts[i]);
				if (ImGui::Button((text + "###key").c_str(), ImVec2(ImGui::GetFontSize() * 12, 0)))
				{
					g_capture = i;
					g_shortcut_message.clear();
				}
				ImGui::SameLine();
				ImGui::TextUnformatted(shortcut_labels[i]);
				ImGui::PopID();
			}
			ImGui::TextDisabled("Click a shortcut, then press the new keys: a key, with any of Ctrl, Shift and Alt. Esc cancels, Backspace removes the shortcut.");
			if (!g_shortcut_message.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", g_shortcut_message.c_str());
		}

		ImGui::Separator();
		ImGui::Text("Per second: %llu lens flare quads removed, %llu kept, %llu tonemap binds swapped", g_rates.flare_skipped, g_rates.flare_kept, g_rates.bloom_swapped);
		ImGui::Text("Sprite pipelines (lens flare) with smaller quads: %u", g_quad_count.load());
		if (g_rates.bloom_kept != 0)
			ImGui::Text("Per second: %llu tonemap draws with the game's bloom, for want of a twin", g_rates.bloom_kept);
		ImGui::Text("Tonemap pipelines with bloom: %u, %u with a twin without bloom, %u with twins with weaker bloom", g_pso_count[size_t(pso_kind::tonemap_bloom)].load(),
			g_twin_count.load(), g_weaker_count.load());
		ImGui::Text("Light glow pipelines: %u, light flare pipelines: %u, %u of them without star rays; per second: %llu binds without star rays",
			g_pso_count[size_t(pso_kind::light_glow)].load(), g_pso_count[size_t(pso_kind::light_flare)].load(), g_starless_count.load(), g_rates.star_swapped);
		if (g_dxc_unavailable)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "The game's dxcompiler.dll could not be used: the star rays stay, and the lens flare and bloom cannot be scaled (see the log)");
		else if (g_starless_failed != 0)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Star rays: %u light pipeline(s) left as they are (see the log)", g_starless_failed.load());
		if (!g_dxc_unavailable && g_weaker_failed != 0)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Bloom: %u tonemap pipeline(s) cannot have a weaker bloom (see the log)", g_weaker_failed.load());
		// Tonemaps whose bloom cannot be removed at all: neither a twin from the package nor a rebuilt shader.
		const uint32_t bloom_pipelines = g_pso_count[size_t(pso_kind::tonemap_bloom)].load(), removable = g_twin_count.load();
		if (bloom_pipelines > removable)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Bloom: %u tonemap pipeline(s) keep the game's bloom (see the log)", bloom_pipelines - removable);

		// Why twins from the shader package are not used, as a note: the rebuilt shaders remove the bloom in their place.
		const char *const instead = g_dxc_unavailable ? "" : "; rebuilt shaders remove the bloom instead";
		switch (g_twins_state.load())
		{
		case twins_state::missing:
			ImGui::TextDisabled("Bloom: no .twins file next to the add-on%s", instead);
			break;
		case twins_state::unreadable:
			ImGui::TextDisabled("Bloom: the .twins file could not be read (see the log)%s", instead);
			break;
		case twins_state::no_package:
			ImGui::TextDisabled("Bloom: the sim's shader package was not found (see the log)%s", instead);
			break;
		case twins_state::other_package:
			ImGui::TextDisabled("Bloom: the .twins file is for another version of the sim's shaders%s", instead);
			break;
		default:
			if (g_twin_differs != 0)
				ImGui::TextDisabled("Bloom: %u tonemap(s) differ from the .twins file, made for another version of the sim's shaders%s", g_twin_differs.load(), instead);
			if (g_twin_failed != 0)
				ImGui::TextDisabled("Bloom: %u twin pipeline(s) from the shader package could not be created (see the log)%s", g_twin_failed.load(), instead);
			break;
		}

		ImGui::Spacing();
		if (ImGui::Button("Record a one-frame trace"))
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
		log("host %S", file_name(exe).c_str());
		log("settings: sun_flare_off=%d %s %s %s keys %s/%s/%s", int(g_sun_flare_off.load()), keep_text().c_str(), bloom_text().c_str(), lights_text().c_str(),
			camfx::shortcut_text(g_shortcuts[action_flare]).c_str(), camfx::shortcut_text(g_shortcuts[action_bloom]).c_str(),
			camfx::shortcut_text(g_shortcuts[action_light_star]).c_str());
		for (size_t at = 0, end; (end = g_settings_notes.find('\n', at)) != std::string::npos; at = end + 1)
			log("%s", g_settings_notes.substr(at, end - at).c_str());

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
	{
		reshade::unregister_addon(module);
		// Never waits: a thread the exiting process ended may have been holding the lock.
		const std::unique_lock<std::mutex> lock(g_log_mutex, std::try_to_lock);
		if (lock.owns_lock() && g_log != nullptr)
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			fprintf(g_log, "%02u:%02u:%02u.%03u unloaded\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			fclose(g_log);
			g_log = nullptr;
		}
		break;
	}
	}
	return TRUE;
}
