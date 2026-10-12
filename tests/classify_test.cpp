// Offline checks against the game's real shader package (opened read-only):
// - the add-on recognizes exactly the expected flare, light and bloom tonemapping shaders;
// - every bloom permutation has a twin in the .twins file, and each twin is the matching permutation without bloom;
// - the parsers never read out of bounds on corrupted bytecode (inputs end right before an inaccessible page).
#include "../src/classify.hpp"
#include "../src/twins.hpp"

#include <algorithm>
#include <cstdio>
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
		int sink = int(camfx::classify_vertex_shader(code, size)) + int(camfx::classify_pixel_shader(code, size)) + int(camfx::entry_point(code, size).size());
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
		printf("usage: classify_test <SHADERSPAK.*.SM6.PC.PAK> [<.twins file>]\n");
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

	size_t billboard = 0, tonemap = 0, counts[camfx::pso_kind_count] = {};
	std::set<std::string> glow_digests;
	std::vector<container> bloom_tonemaps;
	for (const container &c : containers)
	{
		const uint8_t *code = pak.data() + c.offset;
		const std::string_view entry = camfx::entry_point(code, c.size);
		billboard += entry == "vh_billboard_lights";
		tonemap += entry == "ph_tonemapping";

		const camfx::pso_kind vs = camfx::classify_vertex_shader(code, c.size);
		const camfx::pso_kind ps = camfx::classify_pixel_shader(code, c.size);
		counts[size_t(vs)]++;
		counts[size_t(ps)]++;
		if (vs == camfx::pso_kind::light_glow)
			glow_digests.insert(camfx::to_hex(digest_of(code, c.size)));
		if (ps == camfx::pso_kind::tonemap_bloom)
			bloom_tonemaps.push_back(c);
	}
	const size_t glow = counts[size_t(camfx::pso_kind::light_glow)], point = counts[size_t(camfx::pso_kind::light_point)], bloom = counts[size_t(camfx::pso_kind::tonemap_bloom)];
	printf("vh_billboard_lights %zu, ph_tonemapping %zu, light_glow %zu, light_point %zu, tonemap_bloom %zu\n", billboard, tonemap, glow, point, bloom);

	check(glow >= 1 && point >= 1, "billboard light shaders split into glow and point variants");
	check(glow + point == billboard, "every vh_billboard_lights is classified");
	check(bloom >= 1 && bloom < tonemap, "some, not all, tonemapping permutations have bloom");

	// Numbers from the analysis of this exact package (sim version of October 2026).
	if (pak_path.find(L"SHADERSPAK.d5d2118d.SM6.PC.PAK") != std::wstring::npos)
	{
		check(containers.size() == 8350, "8350 containers");
		check(glow == 4 && point == 2, "4 light glow and 2 light point vertex shaders");
		check(tonemap == 770 && bloom == 384, "770 tonemapping permutations, 384 with bloom");
		check(glow_digests == std::set<std::string> {
			"dfdd7d87da240aa9bffcbc1760c20c2b", "51760125040cb4c13216d1654508ff66", "6c1e08b0f01d87e2d0b5f6269b312754", "4f0dd64fa43a92d53a366e73e006ccac" },
			"light glow vertex shaders are the 4 that output FlareUV");
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
