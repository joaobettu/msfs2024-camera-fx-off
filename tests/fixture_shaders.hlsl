// Stand-ins for the MSFS 2024 shaders the add-on recognizes: same entry points, same signature names, same t8 binding.
// Each draws a distinct solid color so the fixture can tell from the pixels which one ran.

struct BillboardVertex
{
	float4 position : SV_Position;
	nointerpolation float4 light_pos : LightPos;
	nointerpolation float4 color : Color;
#ifdef WITH_FLARE_UV
	noperspective float2 flare_uv : FlareUV;
	nointerpolation float energy : EnergyConservation;
#endif
};

float2 fullscreen_uv(uint id)
{
	return float2((id << 1) & 2, id & 2);
}

BillboardVertex vh_billboard_lights(uint id : SV_VertexID)
{
	const float2 uv = fullscreen_uv(id);
	BillboardVertex o;
	o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
#ifdef WITH_FLARE_UV
	o.light_pos = float4(1, 0, 0, 1);
	o.color = float4(1, 0, 0, 1);
	o.flare_uv = uv;
	o.energy = 1;
#else
	o.light_pos = float4(0, 0, 0, 1);
	o.color = float4(0, 1, 0, 1);
#endif
	return o;
}

#if defined(WITH_FLARE_UV) || defined(MACRO_LIGHTS)
// The light shape, like the game's: the glow in .x and the star rays in .y, the rays scaled by an angular term. The
// fixture binds a 4x4 texture here, gray but for its last column, which is black. The pixel the fixture reads samples
// (0.66, 0.66), in the gray: the glow alone gives 0.25 there. A glow shrunk to 50% or less samples the black column.
// Both stand-ins also add the disc that far lights get, a quarter of (1 - saturate(r^2))^3: r^2 is 0.28125 at that pixel,
// which gives about 0.093, and 0.031 with the disc shrunk to 75%.
Texture2D<float4> light_shape : register(t8);
SamplerState light_shape_sampler : register(s0);

float disc(float2 r)
{
	const float e = 1 - saturate(dot(r, r));
	return e * e * e * 0.25;
}
#endif

float4 ph_billboard_lights(BillboardVertex i) : SV_Target
{
#ifdef WITH_FLARE_UV
	// The star rays add about 0.45 at that pixel.
	const float4 shape = light_shape.Sample(light_shape_sampler, i.flare_uv * 0.4 + 0.51);
	const float angular = abs(dot(normalize(i.light_pos.xy), normalize(i.flare_uv))) * 0.7 + 0.4;
	return float4(shape.x * 0.5 + shape.y * angular + disc(i.flare_uv), 0, 0, i.energy);
#else
	return i.color;
#endif
}

#ifdef MACRO_LIGHTS
// Aircraft lights: the rays scaled by the exposure (here from the visibility) times an angular term, 0.6 * sin^2 + 0.4.
struct MacroVertex
{
	float4 position : SV_Position;
	nointerpolation float3 light_pos : LIGHTPOS;
	nointerpolation float visibility : VISIBILITY;
	float2 uv : UV;
};

MacroVertex vh_macro_lights(uint id : SV_VertexID)
{
	const float2 uv = fullscreen_uv(id);
	MacroVertex o;
	o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.light_pos = float3(1, 0, 0);
	o.visibility = 0.1;
	o.uv = uv * 0.4 + 0.51;
	return o;
}

float4 ph_macro_lights(MacroVertex i) : SV_Target
{
	// The star rays add about 0.23 at the pixel the fixture reads.
	const float4 shape = light_shape.Sample(light_shape_sampler, i.uv);
	const float c = dot(normalize(i.uv - 0.5), normalize(i.light_pos.xy));
	const float angular = (1 - c * c) * 0.6 + 0.4;
	return float4(shape.x * 0.5 + i.visibility * 6.666667 * angular * shape.y + disc((i.uv - 0.51) * 2.5), 0, 0, 1);
}
#endif

float4 vh_fullscreen(uint id : SV_VertexID) : SV_Position
{
	return float4(fullscreen_uv(id) * float2(2, -2) + float2(-1, 1), 0, 1);
}

#ifdef FLARE_QUAD
// The vertex shader of the lens flare quads, like the game's screen-space sprite permutations of vh_base: the corner's
// offset from the quad center in TEXCOORD1.xy, times the element's size (0.9 here), so the quad covers 90% of the
// viewport around its center.
struct FlareQuadInput
{
	float4 position : POSITION;
	float2 uv : TEXCOORD0;
	float3 corner : TEXCOORD1;
	uint instance : SV_InstanceID;
};

float4 vh_base(FlareQuadInput i) : SV_Position
{
	return float4(i.corner.xy * 0.9, i.position.z, i.position.w);
}
#else
// Material shaders, as the game uses for transparent geometry: a strip whose first triangle already covers the target.
float4 vh_base(uint id : SV_VertexID) : SV_Position
{
	const float2 corner = float2((id & 1) ? 3 : -1, (id & 2) ? 3 : -1);
	return float4(corner, 0, 1);
}
#endif

// Three lens flare elements with shaders the add-on does not know, so it tells them apart by draw order:
// the default one is drawn first (glow), ALT second (star rays), ALT2 after them (ghosts).
float4 ph_base(float4 position : SV_Position) : SV_Target
{
#if defined(ALT)
	return float4(0.25, 1, 0.5, 1);
#elif defined(ALT2)
	return float4(0.5, 0.25, 1, 1);
#else
	return float4(1, 0.5, 0.25, 1);
#endif
}

// Vertex stage for the game's real ph_tonemapping, whose input signature is TEXCOORD0 (register 0), then SV_Position.
struct TonemapVertex
{
	float4 tex : TEXCOORD0;
	float4 position : SV_Position;
};

TonemapVertex vh_fullscreen_tex(uint id : SV_VertexID)
{
	const float2 uv = fullscreen_uv(id);
	TonemapVertex o;
	o.tex = float4(uv, 0, 0);
	o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}

#ifdef WITH_BLOOM
// Compiled for shader model 6.6, like the game's, which binds resources the way the add-on looks for the lens dirt.
Texture2D<float4> lens_dirt : register(t8);
SamplerState lens_dirt_sampler : register(s0);
#endif

// With bloom, like the game's: the lens dirt sampled once makes a factor, by which one branch adds the bloom to the scene
// color and the other (never taken here) blends the scene color toward the bloom; what the bloom adds is then added to
// the scene color. At the pixel the fixture reads, the dirt is gray (about 0.5, alpha 1): the scene color is (0, 0, 1)
// and the bloom about (2, 0, -1), so the bloom adds about (1, 0, -0.5) times its strength. Every value comes from the
// texture, so that the compiler keeps the additions. Without bloom: the scene color alone.
float4 ph_tonemapping(float4 position : SV_Position) : SV_Target
{
#ifdef WITH_BLOOM
	const float4 dirt = lens_dirt.Sample(lens_dirt_sampler, position.xy / 4);
	const float3 scene = max(float3(dirt.a - 1, dirt.a - 1, dirt.a), 0);
	const float3 bloom = float3(dirt.r * 4, dirt.g - 0.5, dirt.b * -2);
	float3 added;
	[branch] if (dirt.a < -1)
		added = saturate(dirt.rgb) * (bloom - scene);
	else
		added = dirt.rgb * bloom;
	return float4(added + scene, 1);
#else
	return float4(0, 0, 1, 1);
#endif
}
