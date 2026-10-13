// Offline checks against the game's real shader package (opened read-only):
// - the add-on recognizes exactly the expected bloom tonemapping shaders, and the lens flare elements of its table are
//   in the package, as material pipelines;
// - every bloom permutation has a twin in the .twins file, and each twin is the matching permutation without bloom;
// - the parsers never read out of bounds on corrupted bytecode (inputs end right before an inaccessible page).
#include "../src/classify.hpp"
#include "../src/twins.hpp"
#include "../src/dxil_patch.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

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

	struct container
	{
		size_t offset;
		uint32_t size;
	};

	bool read_whole_file(const std::wstring &path, std::vector<uint8_t> &out)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(file, &size) != FALSE;
		if (ok)
		{
			out.resize(size_t(size.QuadPart));
			for (size_t done = 0; ok && done < out.size();)
			{
				DWORD read = 0;
				const DWORD chunk = DWORD(std::min<size_t>(out.size() - done, 64 << 20));
				ok = ReadFile(file, out.data() + done, chunk, &read, nullptr) && read == chunk;
				done += read;
			}
		}
		CloseHandle(file);
		return ok;
	}

	std::vector<container> find_containers(const std::vector<uint8_t> &pak)
	{
		std::vector<container> result;
		for (size_t i = 0; i + 32 <= pak.size(); ++i)
		{
			if (pak[i] != 'D' || std::memcmp(&pak[i], "DXBC", 4) != 0)
				continue;
			uint16_t major;
			uint32_t size, count;
			std::memcpy(&major, &pak[i + 20], 2);
			std::memcpy(&size, &pak[i + 24], 4);
			std::memcpy(&count, &pak[i + 28], 4);
			if (major != 1 || count < 1 || count > 32 || size < 32 || i + size > pak.size())
				continue;
			result.push_back({ i, size });
			i += size - 1;
		}
		return result;
	}

	uint32_t container_size_of(const uint8_t *code)
	{
		uint32_t size;
		std::memcpy(&size, code + 24, 4);
		return size;
	}

	camfx::digest16 digest_of(const uint8_t *code, size_t size)
	{
		camfx::digest16 d;
		dxbc::digest(code, size, d.bytes);
		return d;
	}

	using binding = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

	std::set<binding> bindings_of(const uint8_t *code, size_t size)
	{
		std::set<binding> result;
		dxbc::psv0 psv;
		if (dxbc::parse_psv0(dxbc::find_part(code, size, dxbc::fourcc('P', 'S', 'V', '0')), psv))
		{
			for (uint32_t i = 0; i < psv.resource_count; ++i)
			{
				uint32_t v[4];
				std::memcpy(v, psv.resources + size_t(i) * psv.resource_stride, sizeof(v));
				result.emplace(v[0], v[1], v[2], v[3]);
			}
		}
		return result;
	}

	// Copy of 'size' bytes that ends right before a PAGE_NOACCESS page.
	class guarded_copy
	{
	public:
		guarded_copy(const uint8_t *source, size_t size)
		{
			SYSTEM_INFO info;
			GetSystemInfo(&info);
			const size_t page = info.dwPageSize;
			_length = (size + page - 1) / page * page + page;
			_base = static_cast<uint8_t *>(VirtualAlloc(nullptr, _length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			DWORD previous;
			VirtualProtect(_base + _length - page, page, PAGE_NOACCESS, &previous);
			_data = _base + _length - page - size;
			std::memcpy(_data, source, size);
		}
		~guarded_copy() { VirtualFree(_base, 0, MEM_RELEASE); }

		uint8_t *data() const { return _data; }

	private:
		uint8_t *_base = nullptr, *_data = nullptr;
		size_t _length = 0;
	};

	int run_parsers(const uint8_t *code, size_t size)
	{
		int sink = int(camfx::classify_pipeline(code, size, code, size)) + int(camfx::entry_point(code, size).size()) + int(camfx::digest_prefix(code, size) & 0xff);
		uint8_t d[16];
		sink += dxbc::digest(code, size, d) ? d[0] : 0;
		return sink;
	}

	bool parse_without_fault(const uint8_t *code, size_t size)
	{
		__try
		{
			volatile int sink = run_parsers(code, size);
			(void)sink;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2)
	{
		printf("usage: classify_test <SHADERSPAK.*.SM6.PC.PAK> [<.twins file> [<the game's Dxcompiler.DLL>]]\n");
		return 2;
	}
	const std::wstring pak_path = argv[1];
	std::vector<uint8_t> pak;
	if (!read_whole_file(pak_path, pak))
	{
		printf("cannot read %S\n", pak_path.c_str());
		return 2;
	}
	const std::vector<container> containers = find_containers(pak);
	printf("%zu shader containers in %S\n", containers.size(), pak_path.c_str());

	// The lens flare elements the add-on knows: vertex and pixel shader digest prefixes, and the part each one draws
	// (identified in game by keeping each one alone).
	struct flare_element
	{
		uint32_t vs, ps;
		camfx::flare_part part;
	};
	const flare_element flare_elements[] = {
		{ 0xaafcc02e, 0x7b83c99b, camfx::flare_part::glow },
		{ 0xaafcc02e, 0x7f937554, camfx::flare_part::star },
		{ 0x9bee5ecc, 0x2e20deea, camfx::flare_part::ghosts },
		{ 0x9bee5ecc, 0x85a3db94, camfx::flare_part::ghosts },
		{ 0xaafcc02e, 0x10d0a87f, camfx::flare_part::ghosts },
	};
	std::map<uint32_t, std::vector<const uint8_t *>> flare_shaders;
	for (const flare_element &e : flare_elements)
	{
		flare_shaders.try_emplace(e.vs);
		flare_shaders.try_emplace(e.ps);
	}

	size_t tonemap = 0, signed_ok = 0;
	std::vector<container> bloom_tonemaps, light_glows, light_flares;
	for (const container &c : containers)
	{
		const uint8_t *code = pak.data() + c.offset;
		tonemap += camfx::entry_point(code, c.size) == "ph_tonemapping";
		const camfx::pso_kind kind = camfx::classify_pixel_shader(code, c.size);
		if (kind == camfx::pso_kind::tonemap_bloom)
			bloom_tonemaps.push_back(c);
		if (kind == camfx::pso_kind::light_glow)
			light_glows.push_back(c);
		if (kind == camfx::pso_kind::light_flare)
			light_flares.push_back(c);
		const auto it = flare_shaders.find(camfx::digest_prefix(code, c.size));
		if (it != flare_shaders.end())
			it->second.push_back(code);
		// The hash the add-on signs rebuilt shaders with must be the one the game's compiler signed these with.
		signed_ok += dxil::signed_correctly(code, c.size);
	}
	const size_t bloom = bloom_tonemaps.size();
	printf("ph_tonemapping %zu, tonemap_bloom %zu, light_glow %zu, light_flare %zu\n", tonemap, bloom, light_glows.size(), light_flares.size());
	check(bloom >= 1 && bloom < tonemap, "some, not all, tonemapping permutations have bloom");
	check(signed_ok == containers.size(), "the DXIL hash matches the digest of every container (" + std::to_string(signed_ok) + " of " + std::to_string(containers.size()) + ")");

	// Every light glow and light flare pixel shader rebuilt without star rays by the game's dxcompiler.dll, at every glow
	// size: signed, and still recognized as what it was.
	if (argc >= 4)
	{
		dxil::compiler dxc;
		std::string error;
		if (!dxc.load(argv[3], error))
			check(false, ("load the game's dxcompiler.dll: " + error).c_str());
		else
		{
			const auto rebuild = [&](const std::vector<container> &shaders, camfx::pso_kind kind) {
				size_t rebuilt = 0;
				for (const container &c : shaders)
				{
					const uint8_t *code = pak.data() + c.offset;
					std::vector<std::vector<uint8_t>> variants;
					std::string what;
					const bool ok = dxc.make_light_variants(kind == camfx::pso_kind::light_flare, code, c.size, variants, what);
					printf("  %s %s: %s\n", std::string(camfx::entry_point(code, c.size)).c_str(), camfx::to_hex(digest_of(code, c.size)).substr(0, 8).c_str(), what.c_str());
					size_t good = 0;
					for (const std::vector<uint8_t> &v : variants)
						good += !v.empty() && dxil::signed_correctly(v.data(), v.size()) && camfx::classify_pixel_shader(v.data(), v.size()) == kind;
					rebuilt += ok && good == dxil::glow_size_count;
				}
				return !shaders.empty() && rebuilt == shaders.size();
			};
			check(rebuild(light_glows, camfx::pso_kind::light_glow), "every light glow pixel shader rebuilt without star rays at every glow size, signed and still recognized");
			check(rebuild(light_flares, camfx::pso_kind::light_flare), "every light flare pixel shader rebuilt without its star at every glow size, signed and still recognized");
		}
	}

	// Numbers from the analysis of this exact package (sim version of October 2026).
	if (pak_path.find(L"SHADERSPAK.d5d2118d.SM6.PC.PAK") != std::wstring::npos)
	{
		check(containers.size() == 8350, "8350 containers");
		check(tonemap == 770 && bloom == 384, "770 tonemapping permutations, 384 with bloom");
		size_t billboard = 0, macro = 0;
		for (const container &c : light_glows)
		{
			const std::string_view entry = camfx::entry_point(pak.data() + c.offset, c.size);
			billboard += entry == "ph_billboard_lights";
			macro += entry == "ph_macro_lights";
		}
		check(billboard == 3 && macro == 2 && light_glows.size() == 5, "5 light glow pixel shaders: 3 for city and airport lights, 2 for aircraft lights");
		const auto flare = [&](size_t i) { return light_flares.size() > i ? camfx::to_hex(digest_of(pak.data() + light_flares[i].offset, light_flares[i].size)).substr(0, 8) : std::string(); };
		check(light_flares.size() == 2 && flare(0) == "2b398c27" && flare(1) == "979163b3", "2 light flare pixel shaders, 2b398c27 and 979163b3 (of 3078 ph_base)");

		size_t found = 0;
		for (const flare_element &e : flare_elements)
		{
			const std::vector<const uint8_t *> &vs = flare_shaders[e.vs], &ps = flare_shaders[e.ps];
			if (vs.size() != 1 || ps.size() != 1)
				continue;
			const size_t vs_size = container_size_of(vs[0]), ps_size = container_size_of(ps[0]);
			camfx::flare_part part;
			if (camfx::classify_pipeline(vs[0], vs_size, ps[0], ps_size) == camfx::pso_kind::material &&
				camfx::known_flare_part(camfx::flare_tag(camfx::digest_prefix(vs[0], vs_size), camfx::digest_prefix(ps[0], ps_size)), part) && part == e.part)
				++found;
		}
		printf("known lens flare elements found: %zu of %zu\n", found, std::size(flare_elements));
		check(found == std::size(flare_elements), "each known lens flare element is a vh_base/ph_base pipeline of this package, with its part");
	}

	if (argc >= 3)
	{
		std::string text, error;
		camfx::twin_table table;
		const bool parsed = camfx::read_file(argv[2], text) && camfx::parse_twin_table(text, table, error);
		check(parsed, ("twins file parses" + (error.empty() ? std::string() : ": " + error)).c_str());
		if (parsed)
		{
			check(table.pak_size == pak.size(), "twins file was made for this package size");
			size_t paired = 0, valid = 0, via_reader = 0;
			for (const container &c : bloom_tonemaps)
			{
				const uint8_t *code = pak.data() + c.offset;
				const auto it = table.entries.find(digest_of(code, c.size));
				if (it == table.entries.end())
					continue;
				++paired;

				const camfx::twin_entry &e = it->second;
				if (e.offset + e.size > pak.size())
					continue;
				const uint8_t *twin = pak.data() + e.offset;
				std::set<binding> expected = bindings_of(code, c.size);
				expected.erase({ 3u, 0u, 8u, 8u }); // t8, the lens dirt
				expected.erase({ 1u, 0u, 8u, 8u }); // s8, its sampler
				if (digest_of(twin, e.size) == e.twin && camfx::entry_point(twin, e.size) == "ph_tonemapping" &&
					camfx::classify_pixel_shader(twin, e.size) == camfx::pso_kind::none && bindings_of(twin, e.size) == expected)
					++valid;

				std::vector<uint8_t> read;
				if (via_reader < 8 && camfx::read_twin(pak_path, e, read, error) && read.size() == e.size && std::memcmp(read.data(), twin, e.size) == 0)
					++via_reader;
			}
			printf("bloom permutations %zu, paired %zu, valid twins %zu\n", bloom_tonemaps.size(), paired, valid);
			check(paired == bloom_tonemaps.size(), "every bloom permutation has a twin");
			check(valid == paired, "every twin is a ph_tonemapping permutation without bloom and the same other bindings");
			check(via_reader == std::min<size_t>(8, paired), "read_twin returns the twin bytes from the package");
		}
	}

	// Robustness: random corruption of real shaders, and truncations, never fault.
	std::mt19937 rng(12345);
	size_t runs = 0, faults = 0;
	for (size_t n = 0; n < containers.size(); n += 7)
	{
		const container &c = containers[n];
		const uint8_t *code = pak.data() + c.offset;
		for (int round = 0; round < 24; ++round)
		{
			std::vector<uint8_t> bytes(code, code + c.size);
			const size_t span = std::min<size_t>(bytes.size(), 4096);
			for (int flips = 1 + int(rng() % 6); flips > 0; --flips)
				bytes[rng() % span] = uint8_t(rng());
			const size_t size = round % 3 == 0 ? rng() % (bytes.size() + 1) : bytes.size();
			const guarded_copy copy(bytes.data(), size);
			faults += !parse_without_fault(copy.data(), size);
			++runs;
		}
	}
	printf("fuzz: %zu runs, %zu faults\n", runs, faults);
	check(faults == 0, "parsers never read out of bounds");

	printf(g_failures == 0 ? "PASS\n" : "FAILED (%d)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
