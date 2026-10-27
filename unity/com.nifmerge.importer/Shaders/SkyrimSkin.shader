// Skyrim skin, opaque or Cutout (_ALPHATEST_ON). The lighting is in SkyrimSkin.cginc.
Shader "NifMerge/Skyrim Skin"
{
    Properties
    {
        _Color ("Colour", Color) = (1, 1, 1, 1)
        _MainTex ("Diffuse", 2D) = "white" {}
        [Normal] _BumpMap ("Normal Map", 2D) = "bump" {}
        _DetailAlbedoMap ("FaceGen Tint (x2)", 2D) = "grey" {}
        _SpecMap ("Specular Mask", 2D) = "white" {}
        [HDR] _SkyrimSpecColor ("Specular Colour x Strength", Color) = (0, 0, 0, 1)
        _Shininess ("Glossiness (Blinn-Phong exponent)", Float) = 30
        _EmissionMap ("Emission", 2D) = "white" {}
        [HDR] _EmissionColor ("Emission Colour", Color) = (0, 0, 0, 1)
        _Cutoff ("Alpha Cutoff", Range(0, 1)) = 0.5
        [HideInInspector] _Mode ("Rendering Mode", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" }
        CGPROGRAM
        #pragma target 3.0
        #pragma surface SkyrimSkinSurf SkyrimSkin fullforwardshadows addshadow
        #pragma shader_feature_local _ALPHATEST_ON
        #pragma shader_feature_local _DETAIL_MULX2
        #pragma shader_feature_local _SPECMAP_ALPHA
        #pragma shader_feature_local _EMISSION
        #include "SkyrimSkin.cginc"
        ENDCG
    }
    FallBack "Diffuse"
}
