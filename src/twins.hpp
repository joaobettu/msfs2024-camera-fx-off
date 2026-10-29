// Pairs of tonemapping permutations: each one with bloom and the game's own permutation without it.
// The pairs come from tools/gen_twins.ps1, which matches them by comparing their disassembled code,
// and are stored in "msfs2024-camera-fx-off.twins" next to the add-on.
#pragma once

#include "dxbc.hpp"

#include <Windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace camfx
{
	struct digest16
	{
		uint8_t bytes[16] = {};

		bool operator==(const digest16 &other) const { return std::memcmp(bytes, other.bytes, 16) == 0; }
	};

	struct digest16_hash
	{
		size_t operator()(const digest16 &d) const
		{
			uint64_t h;
			std::memcpy(&h, d.bytes, 8);
			return size_t(h);
		}
	};

	inline std::string to_hex(const digest16 &d)
	{
		static const char digits[] = "0123456789abcdef";
		std::string s(32, '0');
		for (int i = 0; i < 16; ++i)
		{
			s[i * 2] = digits[d.bytes[i] >> 4];
			s[i * 2 + 1] = digits[d.bytes[i] & 15];
		}
		return s;
	}

	inline bool parse_hex(std::string_view s, digest16 &out)
	{
		if (s.size() != 32)
			return false;
		for (int i = 0; i < 32; ++i)
		{
			const char c = s[i];
			const int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
			if (v < 0)
				return false;
			out.bytes[i / 2] = uint8_t((i % 2) ? (out.bytes[i / 2] | v) : (v << 4));
		}
		return true;
	}

	struct twin_entry
	{
		digest16 twin;
		uint64_t offset = 0;
		uint32_t size = 0;
	};

	struct twin_table
	{
		std::string pak_name;
		uint64_t pak_size = 0;
		std::unordered_map<digest16, twin_entry, digest16_hash> entries;
	};

	inline bool read_file(const std::wstring &path, std::string &out)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
		if (ok)
		{
			out.resize(size_t(size.QuadPart));
			DWORD read = 0;
			ok = out.empty() || (ReadFile(file, out.data(), DWORD(out.size()), &read, nullptr) && read == out.size());
		}
		CloseHandle(file);
		return ok;
	}

	// Format: '#' comments, one "pak <file name> <size>" line, then "<bloom digest> <twin digest> <offset> <size>" per pair.
	inline bool parse_twin_table(std::string_view text, twin_table &out, std::string &error)
	{
		out = {};
		size_t line_number = 0;
		while (!text.empty())
		{
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
			++line_number;

			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
				line.remove_suffix(1);
			if (line.empty() || line[0] == '#')
				continue;

			std::string_view fields[4];
			size_t count = 0;
			while (!line.empty() && count < 4)
			{
				const size_t space = line.find(' ');
				fields[count++] = line.substr(0, space);
				line = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
				while (!line.empty() && line[0] == ' ')
					line.remove_prefix(1);
			}

			const auto parse_u64 = [](std::string_view s, uint64_t &v) {
				if (s.empty() || s.size() > 19)
					return false;
				v = 0;
				for (const char c : s)
				{
					if (c < '0' || c > '9')
						return false;
					v = v * 10 + uint64_t(c - '0');
				}
				return true;
			};

			if (count == 3 && fields[0] == "pak")
			{
				out.pak_name = std::string(fields[1]);
				if (!parse_u64(fields[2], out.pak_size))
					return error = "bad pak size on line " + std::to_string(line_number), false;
				continue;
			}

			digest16 bloom, twin;
			uint64_t offset = 0, size = 0;
			if (count != 4 || !parse_hex(fields[0], bloom) || !parse_hex(fields[1], twin) || !parse_u64(fields[2], offset) || !parse_u64(fields[3], size) || size == 0 || size > (1 << 20))
				return error = "malformed line " + std::to_string(line_number), false;
			out.entries[bloom] = { twin, offset, uint32_t(size) };
		}

		if (out.pak_name.empty() || out.pak_name.find_first_of("\\/:") != std::string::npos)
			return error = "missing or invalid 'pak' line", false;
		return true;
	}

	// Reads one shader container from the game's shader package and checks it is exactly the expected twin. 'differs', if
	// given, tells a container that is not the twin (a package of another sim version) from a package that cannot be read.
	inline bool read_twin(const std::wstring &pak_path, const twin_entry &entry, std::vector<uint8_t> &out, std::string &error, bool *differs = nullptr)
	{
		if (differs != nullptr)
			*differs = false;
		const HANDLE file = CreateFileW(pak_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return error = "cannot open shader package (error " + std::to_string(GetLastError()) + ")", false;

		out.resize(entry.size);
		LARGE_INTEGER position;
		position.QuadPart = LONGLONG(entry.offset);
		DWORD read = 0;
		const bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, out.data(), entry.size, &read, nullptr) && read == entry.size;
		CloseHandle(file);
		if (!ok)
			return error = "cannot read shader package", false;

		digest16 actual;
		uint32_t container_size = 0;
		if (!dxbc::digest(out.data(), out.size(), actual.bytes) || !dxbc::read_u32({ out.data(), out.size() }, 24, container_size) || container_size != entry.size || !(actual == entry.twin))
		{
			if (differs != nullptr)
				*differs = true;
			return error = "the shader package holds something else where the .twins file has the twin: the .twins file is for another version of the sim's shaders", false;
		}
		return true;
	}
}
