// Recognizes the MSFS 2024 shaders this add-on acts on, by entry point and signature rather than by hash,
// so a sim update that recompiles them keeps matching as long as Asobo keeps the names.
#pragma once

#include "dxbc.hpp"

namespace camfx
{
	enum class pso_kind : uint8_t
	{
		none,
		// Billboard light variant that outputs 'FlareUV': the glow sprite that is the light itself seen from a distance
		// (city, airport and aircraft lights). Only hidden by a diagnostic option.
		light_glow,
		// Billboard light variant without 'FlareUV': the light point. Only hidden by a diagnostic option.
		light_point,
		// Tonemapping permutation that adds bloom (scaled by the lens dirt texture at t8).
		tonemap_bloom,
		// Generic material shaders. The lens flare elements (sun ghosts and glare) are quads drawn with these right after
		// upscaling, which the add-on tells apart by draw and render target (see the draw filter), not by shader.
		material,
	};

	constexpr size_t pso_kind_count = 5;

	inline const char *kind_name(pso_kind kind)
	{
		switch (kind)
		{
		case pso_kind::light_glow: return "light_glow";
		case pso_kind::light_point: return "light_point";
		case pso_kind::tonemap_bloom: return "tonemap_bloom";
		case pso_kind::material: return "material";
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

	inline pso_kind classify_vertex_shader(const void *code, size_t code_size)
	{
		if (entry_point(code, code_size) != "vh_billboard_lights")
			return pso_kind::none;
		return dxbc::contains_string(dxbc::find_part(code, code_size, dxbc::fourcc('O', 'S', 'G', '1')), "FlareUV") ? pso_kind::light_glow : pso_kind::light_point;
	}

	inline pso_kind classify_pixel_shader(const void *code, size_t code_size)
	{
		dxbc::psv0 psv;
		if (!dxbc::parse_psv0(dxbc::find_part(code, code_size, dxbc::fourcc('P', 'S', 'V', '0')), psv) || psv.entry != "ph_tonemapping")
			return pso_kind::none;
		// All 384 bloom permutations bind the lens dirt texture at t8 (space 0); the 384 without bloom don't.
		return dxbc::binds(psv, dxbc::resource_type::srv_typed, 0, 8) ? pso_kind::tonemap_bloom : pso_kind::none;
	}

	// vs and ps may be null when the pipeline lacks that stage.
	inline pso_kind classify_pipeline(const void *vs, size_t vs_size, const void *ps, size_t ps_size)
	{
		if (vs != nullptr)
		{
			const pso_kind kind = classify_vertex_shader(vs, vs_size);
			if (kind != pso_kind::none)
				return kind;
		}
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
}
