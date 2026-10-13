// Builds, at run time, the game's light glow pixel shaders without their star rays, using the game's own dxcompiler.dll:
// disassemble, zero the star channel (and optionally shrink the glow), reassemble, sign. Nothing of the game's code ships
// with the add-on; each shader is made from the one the game creates, on the user's machine.
#pragma once

#include <windows.h>
// dxcapi.h needs the COM declarations that WIN32_LEAN_AND_MEAN leaves out of windows.h.
#include <unknwn.h>
#include <objidl.h>
#include <oleauto.h>
#include <dxcapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace dxil
{
	// Sizes of the glow left around lights without star rays, in percent of the game's; the first is the game's own.
	constexpr int glow_sizes[] = { 100, 75, 50, 35, 25 };
	constexpr size_t glow_size_count = std::size(glow_sizes);

	// The digest D3D12 checks in a DXIL container (bytes 4 to 19): MD5 with its own final padding (DXC's
	// ComputeHashRetail), over the container from byte 20 on.
	inline void hash(const uint8_t *data, uint32_t size, uint8_t out[16])
	{
		static constexpr uint32_t shift[64] = {
			7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
		static constexpr uint32_t k[64] = {
			0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
			0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
			0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
			0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
			0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
			0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
		static constexpr uint8_t padding[64] = { 0x80 };

		const auto block = [](uint32_t state[4], const uint32_t x[16]) {
			uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
			for (int i = 0; i < 64; ++i)
			{
				uint32_t f;
				int g;
				if (i < 16) { f = (b & c) | (~b & d); g = i; }
				else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
				else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
				else { f = c ^ (b | ~d); g = (7 * i) % 16; }
				const uint32_t sum = a + f + k[i] + x[g];
				a = d;
				d = c;
				c = b;
				b = b + ((sum << shift[i]) | (sum >> (32 - shift[i])));
			}
			state[0] += a; state[1] += b; state[2] += c; state[3] += d;
		};

		// Unlike plain MD5, the bit count goes first in the last block, and the last word holds 1 | (size << 1).
		const uint32_t left = size & 0x3f;
		const bool two_blocks = left >= 56;
		const uint32_t pad = two_blocks ? 120 - left : 56 - left;
		const uint32_t blocks = (size + pad + 8) >> 6;
		uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
		for (uint32_t i = 0, offset = 0; i < blocks; ++i, offset += 64)
		{
			uint32_t x[16] = {};
			uint8_t *const bytes = reinterpret_cast<uint8_t *>(x);
			const uint32_t rest = offset < size ? size - offset : 0;
			if (i + 1 < blocks && !(two_blocks && i + 2 == blocks))
			{
				std::memcpy(x, data + offset, 64);
			}
			else if (two_blocks && i + 2 == blocks)
			{
				std::memcpy(bytes, data + offset, rest);
				std::memcpy(bytes + rest, padding, pad - 56);
			}
			else
			{
				x[0] = size << 3;
				if (two_blocks)
				{
					std::memcpy(bytes + 4, padding + pad - 56, 56);
				}
				else
				{
					std::memcpy(bytes + 4, data + offset, rest);
					std::memcpy(bytes + 4 + rest, padding, pad);
				}
				x[15] = 1 | (size << 1);
			}
			block(state, x);
		}
		std::memcpy(out, state, 16);
	}

	inline void sign(std::vector<uint8_t> &container)
	{
		if (container.size() < 32)
			return;
		hash(container.data() + 20, uint32_t(container.size() - 20), container.data() + 4);
	}

	inline bool signed_correctly(const uint8_t *container, size_t size)
	{
		if (size < 32)
			return false;
		uint8_t digest[16];
		hash(container + 20, uint32_t(size - 20), digest);
		return std::memcmp(digest, container + 4, 16) == 0;
	}

	// The instructions of a disassembly, "%name = rest", with comments and metadata attachments cut from rest.
	struct instruction
	{
		std::string_view name, rest;
		size_t line_begin, line_end;
	};

	inline std::vector<instruction> instructions(std::string_view ir)
	{
		std::vector<instruction> result;
		for (size_t begin = 0; begin < ir.size();)
		{
			size_t end = ir.find('\n', begin);
			if (end == std::string_view::npos)
				end = ir.size();
			std::string_view line = ir.substr(begin, end - begin);
			const size_t first = line.find_first_not_of(" \t");
			if (first != std::string_view::npos && line[first] == '%')
			{
				line.remove_prefix(first);
				const size_t equals = line.find(" = ");
				if (equals != std::string_view::npos)
				{
					std::string_view rest = line.substr(equals + 3);
					for (const std::string_view cut : { std::string_view("  ;"), std::string_view(", !") })
						if (const size_t at = rest.find(cut); at != std::string_view::npos)
							rest = rest.substr(0, at);
					while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r'))
						rest.remove_suffix(1);
					result.push_back({ line.substr(0, equals), rest, begin, end });
				}
			}
			begin = end + 1;
		}
		return result;
	}

	// A float constant as LLVM writes it: the hex digits of the double holding the float's exact value.
	inline std::string float_constant(float value)
	{
		const double exact = value;
		uint64_t bits;
		std::memcpy(&bits, &exact, sizeof(bits));
		char text[24];
		snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(bits));
		return text;
	}

	inline bool starts_with(std::string_view s, std::string_view prefix)
	{
		return s.substr(0, prefix.size()) == prefix;
	}

	inline bool ends_with(std::string_view s, std::string_view suffix)
	{
		return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
	}

	// 'text' with each use of the value 'name' replaced by 'by'. A longer name that starts the same, like %801 for %80, is
	// not a use.
	inline std::string replace_value(std::string_view text, std::string_view name, std::string_view by)
	{
		std::string result;
		for (size_t at = 0;;)
		{
			const size_t found = text.find(name, at);
			if (found == std::string_view::npos)
				return result += text.substr(at), result;
			const size_t after = found + name.size();
			const bool whole = after == text.size() || !(std::isalnum(static_cast<unsigned char>(text[after])) || text[after] == '_' || text[after] == '.');
			result += text.substr(at, found - at);
			result += whole ? by : name;
			at = after;
		}
	}

	// A disassembly's instructions, with the lookups the patches below need. The views point into the text this was made
	// from, which must not change while this is in use.
	class ir_code
	{
	public:
		explicit ir_code(const std::string &ir) : _ir(ir), _code(instructions(ir)) {}

		const std::vector<instruction> &all() const { return _code; }

		// Where 'part' of the text starts.
		size_t offset(std::string_view part) const { return size_t(part.data() - _ir.data()); }

		// The indentation of an instruction's line.
		std::string indent(const instruction &i) const { return _ir.substr(i.line_begin, _ir.find('%', i.line_begin) - i.line_begin); }

		bool declares(std::string_view declaration) const { return _ir.find(declaration) != std::string::npos; }

		const instruction *defined(std::string_view name) const
		{
			for (const instruction &i : _code)
				if (i.name == name)
					return &i;
			return nullptr;
		}

		// The one instruction that matches, or null with 'what' telling there is none or more than one.
		template <typename Match>
		const instruction *find_one(Match &&match, const char *step, std::string &what) const
		{
			const instruction *found = nullptr;
			for (const instruction &i : _code)
			{
				if (!match(i))
					continue;
				if (found != nullptr)
					return what = std::string("more than one ") + step, nullptr;
				found = &i;
			}
			if (found == nullptr)
				what = std::string("no ") + step;
			return found;
		}

		// The two operands of "fmul fast float a, b".
		static bool multiply(const instruction &i, std::string_view &a, std::string_view &b)
		{
			constexpr std::string_view prefix = "fmul fast float ";
			if (!starts_with(i.rest, prefix))
				return false;
			const std::string_view operands = i.rest.substr(prefix.size());
			const size_t comma = operands.find(", ");
			if (comma == std::string_view::npos)
				return false;
			a = operands.substr(0, comma);
			b = operands.substr(comma + 2);
			return !a.empty() && !b.empty() && b.find(',') == std::string_view::npos;
		}

		// The one texture sample with implicit level of detail, and the instruction taking its .y.
		bool sample_y(const instruction *&sample, const instruction *&y, std::string &what) const
		{
			sample = find_one([](const instruction &i) { return starts_with(i.rest, "call %dx.types.ResRet.f32 @dx.op.sample.f32("); }, "texture sample", what);
			if (sample == nullptr)
				return false;
			const std::string extract = "extractvalue %dx.types.ResRet.f32 " + std::string(sample->name) + ", 1";
			y = find_one([&](const instruction &i) { return i.rest == extract; }, ".y of the texture sample", what);
			return y != nullptr;
		}

		// The disc around a light, (1 - saturate(r^2))^3 with r the distance to its center: the saturate whose complement is
		// squared. Its operand, r^2, goes to 'r2'.
		//   %d = call float @dx.op.unary.f32(i32 7, float %r2)
		//   %e = fsub fast float 1.000000e+00, %d
		//   %f = fmul fast float %e, %e
		const instruction *disc(std::string_view &r2, std::string &what) const
		{
			constexpr std::string_view saturate = "call float @dx.op.unary.f32(i32 7, float %";
			const instruction *const found = find_one([&](const instruction &i) {
				if (!starts_with(i.rest, saturate) || !ends_with(i.rest, ")"))
					return false;
				const std::string complement = "fsub fast float 1.000000e+00, " + std::string(i.name);
				for (const instruction &c : _code)
				{
					if (c.rest != complement)
						continue;
					const std::string squared = "fmul fast float " + std::string(c.name) + ", " + std::string(c.name);
					for (const instruction &s : _code)
						if (s.rest == squared)
							return true;
				}
				return false;
			}, "disc around the light", what);
			if (found != nullptr)
				r2 = found->rest.substr(saturate.size() - 1, found->rest.size() - saturate.size());
			return found;
		}

	private:
		const std::string &_ir;
		std::vector<instruction> _code;
	};

	// Changes to a disassembly by offset: each replaces a part of the text, or inserts text at a point. Applied from the last
	// one in the text to the first, so the offsets found before stay valid.
	class ir_edits
	{
	public:
		void replace(size_t at, size_t size, std::string text) { _edits.push_back({ at, size, std::move(text) }); }
		void insert(size_t at, std::string text) { replace(at, 0, std::move(text)); }

		void apply(std::string &ir)
		{
			std::sort(_edits.begin(), _edits.end(), [](const edit &a, const edit &b) { return a.at > b.at; });
			for (const edit &e : _edits)
				ir.replace(e.at, e.size, e.text);
			_edits.clear();
		}

	private:
		struct edit
		{
			size_t at, size;
			std::string text;
		};
		std::vector<edit> _edits;
	};

	// Removes the star rays of a light glow pixel shader and, below 100 percent, shrinks the glow left around the light.
	// The shader samples one light shape texture, with the glow in .x and the star rays in .y. The rays are scaled by an
	// angular term that ends in "+ 0.4" (0.7 * |cos| + 0.4 on city and airport lights, 0.6 * sin^2 + 0.4 on aircraft
	// lights), directly or through a few more multiplies (by the exposure on aircraft lights), in any order. Each step must
	// match once:
	//   %s = call %dx.types.ResRet.f32 @dx.op.sample.f32(..., float %u, float %v, ...)  ; the light shape
	//   %ray = extractvalue %dx.types.ResRet.f32 %s, 1                                   ; .y, the star rays
	//   %c = fmul fast float %ray, %b (or %b, %ray)                                      ; becomes 0.0 * %b
	// where %b is the angular term "fadd fast float %a, 0x3FD99999A0000000" (%a a multiply by a constant) or a product
	// with it, or %c itself is then multiplied by such a term.
	// The glow is shrunk by sampling the texture farther from its center (0.5, 0.5),
	// saturate((uv - 0.5) * 100 / glow_size + 0.5), in instructions inserted before the sample. Lights far away get the
	// disc of ir_code::disc instead of that glow; it is shrunk by the same factor by multiplying the r^2 it saturates by
	// (100 / glow_size)^2.
	inline bool patch_light(std::string &ir, int glow_size, std::string &what)
	{
		const ir_code code(ir);
		const auto angular = [&](std::string_view name) {
			const instruction *const i = code.defined(name);
			if (i == nullptr || !starts_with(i->rest, "fadd fast float %") || !ends_with(i->rest, ", 0x3FD99999A0000000"))
				return false;
			const instruction *const scaled = code.defined(i->rest.substr(16, i->rest.size() - 16 - 20));
			std::string_view x, y;
			return scaled != nullptr && ir_code::multiply(*scaled, x, y) && (x[0] != '%' || y[0] != '%');
		};
		// True when 'name' is the angular term or a product with it, at most 'depth' multiplies away.
		const auto scaled_by_angular = [&](auto &&self, std::string_view name, int depth) -> bool {
			if (angular(name))
				return true;
			const instruction *const i = depth > 0 ? code.defined(name) : nullptr;
			std::string_view x, y;
			return i != nullptr && ir_code::multiply(*i, x, y) && (self(self, x, depth - 1) || self(self, y, depth - 1));
		};
		// True when 'name' is multiplied by the angular term or a product with it, at most 'depth' multiplies later.
		const auto then_scaled_by_angular = [&](auto &&self, std::string_view name, int depth) -> bool {
			for (const instruction &i : code.all())
			{
				std::string_view x, y;
				if (depth == 0 || !ir_code::multiply(i, x, y) || (x != name && y != name) || x == y)
					continue;
				if (scaled_by_angular(scaled_by_angular, x == name ? y : x, 3) || self(self, i.name, depth - 1))
					return true;
			}
			return false;
		};

		const instruction *sample, *channel;
		if (!code.sample_y(sample, channel, what))
			return false;
		const instruction *const rays = code.find_one([&](const instruction &i) {
			std::string_view x, y;
			if (!ir_code::multiply(i, x, y) || (x != channel->name && y != channel->name) || x == y)
				return false;
			return scaled_by_angular(scaled_by_angular, x == channel->name ? y : x, 3) || then_scaled_by_angular(then_scaled_by_angular, i.name, 3);
		}, "multiply of the star rays by the angular term", what);
		if (rays == nullptr)
			return false;

		ir_edits edits;
		const std::string rays_before(rays->rest);
		edits.replace(code.offset(rays->rest), rays->rest.size(), replace_value(rays->rest, channel->name, "0.000000e+00"));

		if (glow_size != 100)
		{
			if (glow_size <= 0 || glow_size > 100)
				return what = "glow size out of range", false;
			if (!code.declares("declare float @dx.op.unary.f32(i32, float)"))
				return what = "no unary operation declared for the saturate", false;

			// The sample's operands: i32 opcode, texture, sampler, then the coordinates u and v.
			const std::string_view call = sample->rest;
			const size_t open = call.find('(');
			if (open == std::string_view::npos || call.back() != ')')
				return what = "cannot read the texture sample", false;
			std::vector<std::string_view> operands;
			for (size_t begin = open + 1; begin < call.size();)
			{
				size_t end = call.find(", ", begin);
				if (end == std::string_view::npos)
					end = call.size() - 1;
				operands.push_back(call.substr(begin, end - begin));
				begin = end + 2;
			}
			if (operands.size() < 5 || !starts_with(operands[3], "float ") || !starts_with(operands[4], "float "))
				return what = "cannot read the texture sample coordinates", false;

			const float scale = 100.0f / float(glow_size);
			const std::string indent = code.indent(*sample);
			std::string inserted, sampled = std::string(call.substr(0, open + 1));
			for (size_t i = 0; i < operands.size(); ++i)
			{
				if (i != 0)
					sampled += ", ";
				if (i != 3 && i != 4)
				{
					sampled += operands[i];
					continue;
				}
				const std::string name = std::string("%camfx_glow_") + (i == 3 ? "u" : "v");
				inserted += indent + name + "0 = fmul fast float " + std::string(operands[i].substr(6)) + ", " + float_constant(scale) + "\n";
				inserted += indent + name + "1 = fadd fast float " + name + "0, " + float_constant(0.5f - 0.5f * scale) + "\n";
				inserted += indent + name + " = call float @dx.op.unary.f32(i32 7, float " + name + "1)\n";
				sampled += "float " + name;
			}
			sampled += ")";
			edits.replace(code.offset(sample->rest), sample->rest.size(), std::move(sampled));
			edits.insert(sample->line_begin, std::move(inserted));

			std::string_view r2;
			const instruction *const disc = code.disc(r2, what);
			if (disc == nullptr)
				return false;
			edits.replace(code.offset(disc->rest), disc->rest.size(), "call float @dx.op.unary.f32(i32 7, float %camfx_disc)");
			edits.insert(disc->line_begin, indent + "%camfx_disc = fmul fast float " + std::string(r2) + ", " + float_constant(scale * scale) + "\n");
		}

		edits.apply(ir);
		what = "'" + std::string(rays->name) + " = " + rays_before + "' now multiplies 0 instead of the star channel";
		return true;
	}

	// Removes the star of a light flare pixel shader (the sprite over aircraft lights drawn after upscaling) and, below 100
	// percent, shrinks what is left. Its texture holds the star in .y, drawn over a dim round glow:
	//   %s = call %dx.types.ResRet.f32 @dx.op.sample.f32(...)  ; the flare texture
	//   %y = extractvalue %dx.types.ResRet.f32 %s, 1
	//   %a = fmul fast float %y, 0x3FECCCCCC0000000          ; 0.9 * star, becomes 0.9 * 0
	//   %b = fadd fast float %a, 0x3FB99999A0000000          ; + 0.1, the glow
	// That is then scaled by 1 / (1 + k r^2) and by the disc of ir_code::disc, r being the distance to the flare's center.
	// The flare is shrunk by multiplying r^2 by (100 / glow_size)^2 right where it is computed.
	inline bool patch_light_flare(std::string &ir, int glow_size, std::string &what)
	{
		const ir_code code(ir);
		const instruction *sample, *y;
		if (!code.sample_y(sample, y, what))
			return false;
		const instruction *const star = code.find_one([&](const instruction &i) {
			std::string_view a, b;
			return ir_code::multiply(i, a, b) && ((a == y->name && b == "0x3FECCCCCC0000000") || (b == y->name && a == "0x3FECCCCCC0000000"));
		}, "0.9 multiply of the star", what);
		if (star == nullptr)
			return false;
		const std::string glow = "fadd fast float " + std::string(star->name) + ", 0x3FB99999A0000000";
		if (code.find_one([&](const instruction &i) { return i.rest == glow; }, "+ 0.1 after it", what) == nullptr)
			return false;

		ir_edits edits;
		const std::string star_before(star->rest);
		edits.replace(code.offset(star->rest), star->rest.size(), replace_value(star->rest, y->name, "0.000000e+00"));

		if (glow_size != 100)
		{
			if (glow_size <= 0 || glow_size > 100)
				return what = "glow size out of range", false;
			std::string_view r2;
			if (code.disc(r2, what) == nullptr)
				return false;
			const instruction *const computed = code.defined(r2);
			if (computed == nullptr)
				return what = "r^2 is not computed in the shader", false;
			const float scale = 100.0f / float(glow_size);
			for (const instruction &i : code.all())
			{
				if (&i == computed)
					continue;
				const std::string uses = replace_value(i.rest, r2, "%camfx_r2");
				if (uses != i.rest)
					edits.replace(code.offset(i.rest), i.rest.size(), uses);
			}
			edits.insert(computed->line_end + 1, code.indent(*computed) + "%camfx_r2 = fmul fast float " + std::string(r2) + ", " + float_constant(scale * scale) + "\n");
		}

		edits.apply(ir);
		what = "'" + std::string(star->name) + " = " + star_before + "' now multiplies 0 instead of the star";
		return true;
	}

	// The game's dxcompiler.dll. Not thread-safe: callers serialize.
	class compiler
	{
	public:
		bool load(const std::wstring &path, std::string &error)
		{
			_module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			const auto create = _module != nullptr ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(_module, "DxcCreateInstance")) : nullptr;
			if (create == nullptr)
				return error = "cannot load dxcompiler.dll", false;
			if (FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void **>(&_utils))) ||
				FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler), reinterpret_cast<void **>(&_compiler))) ||
				FAILED(create(CLSID_DxcAssembler, __uuidof(IDxcAssembler), reinterpret_cast<void **>(&_assembler))))
				return error = "dxcompiler.dll lacks the disassembler or the assembler", false;
			return true;
		}

		// A light glow pixel shader (patch_light) or light flare pixel shader (patch_light_flare) without its star rays, once
		// per glow size: out[i] for glow_sizes[i], signed, or empty when that size could not be made. False when not even the
		// one at the game's size could. 'what' describes the change, then any failure.
		bool make_light_variants(bool flare, const void *code, size_t size, std::vector<std::vector<uint8_t>> &out, std::string &what)
		{
			out.assign(glow_size_count, {});
			std::string ir;
			if (!disassemble(code, size, ir, what))
				return false;
			for (size_t i = 0; i < glow_size_count; ++i)
			{
				std::string patched = ir, step;
				const bool ok = (flare ? patch_light_flare(patched, glow_sizes[i], step) : patch_light(patched, glow_sizes[i], step)) && assemble(patched, out[i], step);
				if (i == 0)
				{
					what = step;
					if (!ok)
						return false;
				}
				else if (!ok)
				{
					what += "; glow size " + std::to_string(glow_sizes[i]) + "% not made: " + step;
				}
			}
			return true;
		}

	private:
		bool disassemble(const void *code, size_t size, std::string &ir, std::string &error)
		{
			if (_utils == nullptr)
				return error = "dxcompiler.dll not loaded", false;
			IDxcBlobEncoding *source = nullptr, *disassembly = nullptr;
			const bool ok = SUCCEEDED(_utils->CreateBlob(code, UINT32(size), DXC_CP_ACP, &source)) && SUCCEEDED(_compiler->Disassemble(source, &disassembly));
			if (ok)
			{
				ir.assign(static_cast<const char *>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
				while (!ir.empty() && ir.back() == '\0')
					ir.pop_back();
			}
			else
			{
				error = "disassembling failed";
			}
			for (IUnknown *p : std::initializer_list<IUnknown *> { source, disassembly })
				if (p != nullptr)
					p->Release();
			return ok;
		}

		// Assembles and signs.
		bool assemble(const std::string &ir, std::vector<uint8_t> &out, std::string &error)
		{
			IDxcBlobEncoding *text = nullptr, *errors = nullptr;
			IDxcOperationResult *result = nullptr;
			IDxcBlob *assembled = nullptr;
			HRESULT status = E_FAIL;
			const bool ok = SUCCEEDED(_utils->CreateBlob(ir.data(), UINT32(ir.size()), CP_UTF8, &text)) && SUCCEEDED(_assembler->AssembleToContainer(text, &result)) &&
				SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status) && SUCCEEDED(result->GetResult(&assembled)) && assembled != nullptr;
			if (ok)
			{
				const auto *bytes = static_cast<const uint8_t *>(assembled->GetBufferPointer());
				out.assign(bytes, bytes + assembled->GetBufferSize());
				sign(out);
			}
			else if (result != nullptr && SUCCEEDED(result->GetErrorBuffer(&errors)) && errors != nullptr && errors->GetBufferSize() != 0)
			{
				error = "reassembling failed: " + std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()).substr(0, 200);
			}
			else
			{
				error = "reassembling failed";
			}
			for (IUnknown *p : std::initializer_list<IUnknown *> { text, errors, result, assembled })
				if (p != nullptr)
					p->Release();
			return ok;
		}

		HMODULE _module = nullptr;
		IDxcUtils *_utils = nullptr;
		IDxcCompiler *_compiler = nullptr;
		IDxcAssembler *_assembler = nullptr;
	};
}
