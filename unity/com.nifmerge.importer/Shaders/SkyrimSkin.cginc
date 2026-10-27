// Skyrim's BSLightingShader skin lighting (SkinTint and FaceGen shader types) for the Built-in pipeline.
// Per light, as the game computes it:
//   diffuse  = albedo * saturate(N.L) * light colour
//   specular = pow(saturate(N.H), glossiness) * specular colour * strength * mask * light colour
// The specular term is added on top of the diffuse with no energy conservation and no N.L factor.
// The light colour carries the shadow. The mask is the _s map's red channel on model-space slots
// (_SPECMAP_ALPHA off) or the normal map's alpha on tangent-space slots (_SPECMAP_ALPHA on).
// Ambient light is diffuse only: Skyrim gives skin no reflections.
#ifndef NIFMERGE_SKYRIM_SKIN_INCLUDED
#define NIFMERGE_SKYRIM_SKIN_INCLUDED

#include "UnityPBSLighting.cginc"

sampler2D _MainTex;
sampler2D _BumpMap;
sampler2D _DetailAlbedoMap;
sampler2D _SpecMap;
sampler2D _EmissionMap;
fixed4 _Color;
half4 _SkyrimSpecColor;
half _Shininess;
half4 _EmissionColor;
half _Cutoff;

struct Input
{
    float2 uv_MainTex;
};

struct SurfaceOutputSkyrimSkin
{
    fixed3 Albedo;
    fixed3 Normal;
    half3 Emission;
    fixed Alpha;
    half SpecMask;
};

// Every map uses the diffuse's UVs, transform included, as Skyrim does.
void SkyrimSkinSurf(Input IN, inout SurfaceOutputSkyrimSkin o)
{
    float2 uv = IN.uv_MainTex;
    fixed4 c = tex2D(_MainTex, uv) * _Color;
#if defined(_DETAIL_MULX2)
    c.rgb *= tex2D(_DetailAlbedoMap, uv).rgb * unity_ColorSpaceDouble.rgb;
#endif
    o.Albedo = c.rgb;
    o.Alpha = c.a;
#if defined(_ALPHATEST_ON)
    clip(o.Alpha - _Cutoff);
#endif
    o.Normal = UnpackNormal(tex2D(_BumpMap, uv));
    half4 mask = tex2D(_SpecMap, uv);
#if defined(_SPECMAP_ALPHA)
    o.SpecMask = mask.a;
#else
    o.SpecMask = mask.r;
#endif
#if defined(_EMISSION)
    o.Emission = tex2D(_EmissionMap, uv).rgb * _EmissionColor.rgb;
#else
    o.Emission = 0;
#endif
}

half4 LightingSkyrimSkin(SurfaceOutputSkyrimSkin s, half3 viewDir, UnityGI gi)
{
    half3 n = normalize(s.Normal);
    half3 l = gi.light.dir;
    half nl = saturate(dot(n, l));
    half nh = saturate(dot(n, normalize(l + viewDir)));
    half spec = pow(max(nh, 1e-4), _Shininess) * s.SpecMask;
    half4 c;
    c.rgb = (s.Albedo * nl + _SkyrimSpecColor.rgb * spec) * gi.light.color;
#if defined(UNITY_LIGHT_FUNCTION_APPLY_INDIRECT)
    c.rgb += s.Albedo * gi.indirect.diffuse;
#endif
    c.a = s.Alpha;
    return c;
}

inline void LightingSkyrimSkin_GI(SurfaceOutputSkyrimSkin s, UnityGIInput data, inout UnityGI gi)
{
    gi = UnityGlobalIllumination(data, 1.0, s.Normal);
}

#endif
