// Recognizes the MSFS 2024 shaders this add-on acts on, by entry point, signature and bindings rather than by hash,
// so a sim update that recompiles them keeps matching as long as Asobo keeps the names.
// The parts of the lens flare are the exception: they share their shaders' names, so they are told apart by digest,
// with the order the engine draws them in as a fallback.
#pragma once

#include "dxbc.hpp"

namespace camfx
{
	enum class pso_kind : uint8_t
	{
		none,
		// Tonemapping permutation that adds bloom (scaled by the lens dirt texture at t8).
		tonemap_bloom,
		// Generic material shaders. The lens flare elements (sun glow, star rays, ghosts) are quads drawn with these right
		// after upscaling, which the add-on tells apart by draw and render target (see the draw filter).
		material,
		// The glow sprite of a light: the billboard light pixel shader that reads 'FlareUV' (city and airport lights), and the
		// macro light pixel shader that reads 'UV' (aircraft lights). Their light shape texture holds the glow and the star rays.
		light_glow,
		// The flare sprite over aircraft lights, drawn after upscaling with a ph_base material shader: a star over a dim round
		// glow. Told apart from the thousands of other ph_base shaders by its resources and inputs (see is_light_flare).
		light_flare,
	};

	constexpr size_t pso_kind_count = 5;

	inline const char *kind_name(pso_kind kind)
	{
		switch (kind)
		{
		case pso_kind::tonemap_bloom: return "tonemap_bloom";
		case pso_kind::material: return "material";
		case pso_kind::light_glow: return "light_glow";
		case pso_kind::light_flare: return "light_flare";
		default: return "none";
		}
	}

	inline std::string_view entry_point(const void *code, size_t code_size)
	{
		dxbc::psv0 psv;
		if (!dxbc::parse_psv0(dxbc::find_part(code, code_size, dxbc::fourcc('P', 'S', 'V', '0')), psv))
			return {};
		return psv.entry;
	}

	// The light flare ph_base shaders bind exactly two constant buffers (b0, b1), a sampler (s0), a 2D texture at t0 (the
	// flare), a texture at t1 and a 3D texture at t2, and read TEXSPACE_POS and PREVIOUS_POSITION: in the shader packages of
	// October 2026, before and after Sim Update 7, only the two light flare permutations of their 3078 and 3082 ph_base
	// shaders do.
	inline bool is_light_flare(const void *code, size_t code_size, const dxbc::psv0 &psv)
	{
		using dxbc::resource_type;
		if (psv.resource_count != 6 || !dxbc::binds(psv, resource_type::cbv, 0, 0) || !dxbc::binds(psv, resource_type::cbv, 0, 1) ||
			!dxbc::binds(psv, resource_type::sampler, 0, 0) || !dxbc::binds(psv, resource_type::srv_typed, 0, 0) ||
			!dxbc::binds(psv, resource_type::srv_typed, 0, 1) || !dxbc::binds(psv, resource_type::srv_typed, 0, 2))
			return false;
		if (dxbc::resource_kind(psv, resource_type::srv_typed, 0, 0) != 2 || dxbc::resource_kind(psv, resource_type::srv_typed, 0, 2) != 4)
			return false;
		const dxbc::span inputs = dxbc::find_part(code, code_size, dxbc::fourcc('I', 'S', 'G', '1'));
		return dxbc::contains_string(inputs, "TEXSPACE_POS") && dxbc::contains_string(inputs, "PREVIOUS_POSITION");
	}

	inline pso_kind classify_pixel_shader(const void *code, size_t code_size)
	{
		dxbc::psv0 psv;
		if (!dxbc::parse_psv0(dxbc::find_part(code, code_size, dxbc::fourcc('P', 'S', 'V', '0')), psv))
			return pso_kind::none;
		// All 384 bloom permutations bind the lens dirt texture at t8 (space 0); the 384 without bloom don't.
		if (psv.entry == "ph_tonemapping")
			return dxbc::binds(psv, dxbc::resource_type::srv_typed, 0, 8) ? pso_kind::tonemap_bloom : pso_kind::none;
		if (psv.entry == "ph_billboard_lights")
			return dxbc::contains_string(dxbc::find_part(code, code_size, dxbc::fourcc('I', 'S', 'G', '1')), "FlareUV") ? pso_kind::light_glow : pso_kind::none;
		// The other ph_macro_lights permutations light the scene around the lights (they read a cone and a direction instead).
		if (psv.entry == "ph_macro_lights")
			return dxbc::contains_string(dxbc::find_part(code, code_size, dxbc::fourcc('I', 'S', 'G', '1')), "UV") ? pso_kind::light_glow : pso_kind::none;
		if (psv.entry == "ph_base")
			return is_light_flare(code, code_size, psv) ? pso_kind::light_flare : pso_kind::none;
		return pso_kind::none;
	}

	// vs and ps may be null when the pipeline lacks that stage.
	inline pso_kind classify_pipeline(const void *vs, size_t vs_size, const void *ps, size_t ps_size)
	{
		if (ps != nullptr)
		{
			const pso_kind kind = classify_pixel_shader(ps, ps_size);
			if (kind != pso_kind::none)
				return kind;
		}
		if (vs != nullptr && ps != nullptr && entry_point(vs, vs_size) == "vh_base" && entry_point(ps, ps_size) == "ph_base")
			return pso_kind::material;
		return pso_kind::none;
	}

	// The first 4 bytes of the container digest, read as in the "#1a2b3c4d" labels: 0x1a2b3c4d. Zero if there is none.
	inline uint32_t digest_prefix(const void *code, size_t code_size)
	{
		uint8_t digest[16];
		if (!dxbc::digest(code, code_size, digest))
			return 0;
		return (uint32_t(digest[0]) << 24) | (uint32_t(digest[1]) << 16) | (uint32_t(digest[2]) << 8) | uint32_t(digest[3]);
	}

	// Name of a lens flare element: the digest prefixes of its vertex and pixel shaders.
	inline uint64_t flare_tag(uint32_t vs_prefix, uint32_t ps_prefix)
	{
		return (uint64_t(vs_prefix) << 32) | ps_prefix;
	}

	enum class flare_part : uint8_t
	{
		glow,   // the glow around the sun
		star,   // the star rays on the sun
		ghosts, // the lens reflections along the line through the sun and the screen center
	};

	constexpr size_t flare_part_count = 3;

	inline const char *part_name(flare_part part)
	{
		switch (part)
		{
		case flare_part::glow: return "glow";
		case flare_part::star: return "star rays";
		default: return "ghosts";
		}
	}

	// The lens flare elements in the shader package SHADERSPAK.d5d2118d, each one identified by keeping it alone in game.
	// Returns false for other shaders.
	inline bool known_flare_part(uint64_t tag, flare_part &part)
	{
		switch (tag)
		{
		case 0xaafcc02e'7b83c99bull:
			part = flare_part::glow;
			return true;
		case 0xaafcc02e'7f937554ull:
			part = flare_part::star;
			return true;
		case 0x9bee5ecc'2e20deeaull: // 18 quads
		case 0x9bee5ecc'85a3db94ull: // 3 quads
		case 0xaafcc02e'10d0a87full: // 1 quad
			part = flare_part::ghosts;
			return true;
		default:
			return false;
		}
	}

	// The engine draws the 24 quads of the lens flare in one go after binding the HDR target: the glow first, then the star
	// rays, then the ghosts. Used for elements whose shaders are not in the table above (after a sim update).
	inline flare_part flare_part_by_order(uint32_t index)
	{
		return index == 0 ? flare_part::glow : index == 1 ? flare_part::star : flare_part::ghosts;
	}
}
