// Minimal, bounds-checked reader for DXBC containers holding DXIL (Shader Model 6).
// Only what the add-on needs: locating parts, the PSV0 entry point name and resource bindings, and signature names.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace dxbc
{
	constexpr uint32_t fourcc(char a, char b, char c, char d)
	{
		return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
	}

	struct span
	{
		const uint8_t *data = nullptr;
		size_t size = 0;

		bool valid() const { return data != nullptr; }
	};

	inline bool read_u32(span s, size_t offset, uint32_t &value)
	{
		if (s.data == nullptr || offset > s.size || s.size - offset < 4)
			return false;
		std::memcpy(&value, s.data + offset, 4);
		return true;
	}

	// The container hash DXC writes at bytes 4..19, which identifies the exact bytecode.
	inline bool digest(const void *code, size_t code_size, uint8_t out[16])
	{
		uint32_t magic = 0;
		if (!read_u32({ static_cast<const uint8_t *>(code), code_size }, 0, magic) || magic != fourcc('D', 'X', 'B', 'C') || code_size < 20)
			return false;
		std::memcpy(out, static_cast<const uint8_t *>(code) + 4, 16);
		return true;
	}

	// Body of the first part with the given four-character code, or an invalid span.
	inline span find_part(const void *code, size_t code_size, uint32_t part_fourcc)
	{
		const span c { static_cast<const uint8_t *>(code), code_size };
		uint32_t magic = 0, total = 0, count = 0;
		if (!read_u32(c, 0, magic) || magic != fourcc('D', 'X', 'B', 'C') || !read_u32(c, 24, total) || !read_u32(c, 28, count) || count > 64)
			return {};

		const span container { c.data, total < code_size ? total : code_size };
		for (uint32_t i = 0; i < count; ++i)
		{
			uint32_t offset = 0, tag = 0, size = 0;
			if (!read_u32(container, 32 + size_t(i) * 4, offset) || !read_u32(container, offset, tag) || !read_u32(container, size_t(offset) + 4, size))
				return {};
			const size_t body = size_t(offset) + 8;
			if (body > container.size || container.size - body < size)
				return {};
			if (tag == part_fourcc)
				return { container.data + body, size };
		}
		return {};
	}

	// True if the null-terminated string 'text' occurs in the part (signature and PSV0 string tables are null-terminated).
	inline bool contains_string(span part, std::string_view text)
	{
		if (!part.valid() || text.empty() || part.size <= text.size())
			return false;
		for (size_t i = 0; i + text.size() < part.size; ++i)
			if (part.data[i + text.size()] == 0 && std::memcmp(part.data + i, text.data(), text.size()) == 0)
				return true;
		return false;
	}

	// Semantic name and index of element 'i' of a signature part (ISG1, OSG1). Layout: u32 element count, u32 offset of
	// the elements, then 32-byte elements whose second u32 is the offset of the name (from the part start) and third the
	// semantic index.
	inline bool signature_element(span part, uint32_t i, std::string_view &name, uint32_t &semantic_index)
	{
		uint32_t count = 0, elements = 0, name_offset = 0;
		if (!read_u32(part, 0, count) || !read_u32(part, 4, elements) || i >= count)
			return false;
		const size_t element = size_t(elements) + size_t(i) * 32;
		if (!read_u32(part, element + 4, name_offset) || !read_u32(part, element + 8, semantic_index) || name_offset >= part.size)
			return false;
		const char *const text = reinterpret_cast<const char *>(part.data + name_offset);
		const size_t room = part.size - name_offset;
		size_t length = 0;
		while (length < room && text[length] != '\0')
			++length;
		if (length == room)
			return false;
		name = std::string_view(text, length);
		return true;
	}

	// PSVResourceType from DxilPipelineStateValidation.h
	enum class resource_type : uint32_t
	{
		invalid = 0,
		sampler = 1,
		cbv = 2,
		srv_typed = 3,
		srv_raw = 4,
		srv_structured = 5,
		uav_typed = 6,
		uav_raw = 7,
		uav_structured = 8,
		uav_structured_with_counter = 9,
	};

	struct psv0
	{
		std::string_view entry; // Empty when the container predates PSV0 version 3
		const uint8_t *resources = nullptr;
		uint32_t resource_count = 0;
		uint32_t resource_stride = 0;
	};

	// Layout: u32 runtime info size, runtime info, u32 resource count, [u32 bind info size, bind infos],
	// then (runtime info 1 and later) u32 string table size and the string table.
	inline bool parse_psv0(span part, psv0 &out)
	{
		out = {};
		uint32_t info_size = 0;
		if (!read_u32(part, 0, info_size) || info_size > part.size)
			return false;

		size_t pos = 4 + size_t(info_size);
		uint32_t count = 0;
		if (!read_u32(part, pos, count))
			return false;
		pos += 4;

		if (count != 0)
		{
			uint32_t stride = 0;
			if (count > 4096 || !read_u32(part, pos, stride) || stride < 16)
				return false;
			pos += 4;
			if (pos > part.size || (part.size - pos) / stride < count)
				return false;
			out.resources = part.data + pos;
			out.resource_count = count;
			out.resource_stride = stride;
			pos += size_t(count) * stride;
		}

		// PSVRuntimeInfo3 (52 bytes) stores the entry function name as an offset into the string table at byte 48.
		uint32_t table_size = 0, name_offset = 0;
		if (info_size >= 52 && read_u32(part, pos, table_size) && read_u32(part, 4 + 48, name_offset) &&
			table_size <= part.size - (pos + 4) && name_offset < table_size)
		{
			const char *table = reinterpret_cast<const char *>(part.data + pos + 4);
			const size_t max_length = table_size - name_offset;
			const size_t length = strnlen(table + name_offset, max_length);
			if (length < max_length)
				out.entry = std::string_view(table + name_offset, length);
		}
		return true;
	}

	inline bool binds(const psv0 &psv, resource_type type, uint32_t space, uint32_t reg)
	{
		for (uint32_t i = 0; i < psv.resource_count; ++i)
		{
			const uint8_t *e = psv.resources + size_t(i) * psv.resource_stride;
			uint32_t t, s, lower, upper;
			std::memcpy(&t, e, 4);
			std::memcpy(&s, e + 4, 4);
			std::memcpy(&lower, e + 8, 4);
			std::memcpy(&upper, e + 12, 4);
			if (t == uint32_t(type) && s == space && lower <= reg && reg <= upper)
				return true;
		}
		return false;
	}

	// DXIL resource kind (DXIL::ResourceKind: 2 = Texture2D, 4 = Texture3D, 7 = Texture2DArray...) of the resource bound at
	// that register, from PSV0 version 1 on. Zero when unknown.
	inline uint32_t resource_kind(const psv0 &psv, resource_type type, uint32_t space, uint32_t reg)
	{
		if (psv.resource_stride < 24)
			return 0;
		for (uint32_t i = 0; i < psv.resource_count; ++i)
		{
			const uint8_t *e = psv.resources + size_t(i) * psv.resource_stride;
			uint32_t t, s, lower, upper, kind;
			std::memcpy(&t, e, 4);
			std::memcpy(&s, e + 4, 4);
			std::memcpy(&lower, e + 8, 4);
			std::memcpy(&upper, e + 12, 4);
			std::memcpy(&kind, e + 16, 4);
			if (t == uint32_t(type) && s == space && lower <= reg && reg <= upper)
				return kind;
		}
		return 0;
	}
}
