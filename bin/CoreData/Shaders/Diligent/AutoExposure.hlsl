#include "Uniforms.hlsl"
#include "Transform.hlsl"
#include "Samplers.hlsl"
#include "ScreenPos.hlsl"
#include "PostProcess.hlsl"

#if !defined(D3D11) && !defined(DILIGENT)
uniform float cAutoExposureAdaptRate;
uniform float2 cAutoExposureLumRange;
uniform float cAutoExposureMiddleGrey;
uniform float2 cHDR128Offsets;
uniform float2 cLum64Offsets;
uniform float2 cLum16Offsets;
uniform float2 cLum4Offsets;
uniform float2 cHDR128InvSize;
uniform float2 cLum64InvSize;
uniform float2 cLum16InvSize;
uniform float2 cLum4InvSize;
#else
#ifdef COMPILEVS
cbuffer AutoExposure : register(b3)
{
    float2 cHDR128Offsets;
    float2 cLum64Offsets;
    float2 cLum16Offsets;
    float2 cLum4Offsets;
}
#else
cbuffer AutoExposure : register(b3)
{
    float cAutoExposureAdaptRate;
    float cAutoExposureMiddleGrey;
    float2 cAutoExposureLumRange;
    float2 cHDR128InvSize;
    float2 cLum64InvSize;
    float2 cLum16InvSize;
    float2 cLum4InvSize;
}
#endif
#endif

#if !defined(D3D11) && !defined(DILIGENT)
float GatherAvgLum(sampler2D texSampler, float2 texCoord, float2 texelSize)
#else
float GatherAvgLum(Texture2D tex, SamplerState texSampler, float2 texCoord, float2 texelSize)
#endif
{
    float lumAvg = 0.0;
    #if !defined(D3D11) && !defined(DILIGENT)
    lumAvg += tex2D(texSampler, texCoord + float2(0.0, 0.0) * texelSize).r;
    lumAvg += tex2D(texSampler, texCoord + float2(0.0, 2.0) * texelSize).r;
    lumAvg += tex2D(texSampler, texCoord + float2(2.0, 2.0) * texelSize).r;
    lumAvg += tex2D(texSampler, texCoord + float2(2.0, 0.0) * texelSize).r;
    #else
    lumAvg += tex.Sample(texSampler, texCoord + float2(0.0, 0.0) * texelSize).r;
    lumAvg += tex.Sample(texSampler, texCoord + float2(0.0, 2.0) * texelSize).r;
    lumAvg += tex.Sample(texSampler, texCoord + float2(2.0, 2.0) * texelSize).r;
    lumAvg += tex.Sample(texSampler, texCoord + float2(2.0, 0.0) * texelSize).r;
    #endif
    return lumAvg / 4.0;
}

void VS(float4 iPos : POSITION,
    out float2 oTexCoord : TEXCOORD0,
    out float2 oScreenPos : TEXCOORD1,
    out float4 oPos : OUTPOSITION)
{
    float4x3 modelMatrix = iModelMatrix;
    float3 worldPos = GetWorldPos(modelMatrix);
    oPos = GetClipPos(worldPos);

    oTexCoord = GetQuadTexCoord(oPos);

    #ifdef LUMINANCE64
    oTexCoord = GetQuadTexCoord(oPos) + cHDR128Offsets;
    #endif

    #ifdef LUMINANCE16
    oTexCoord = GetQuadTexCoord(oPos) + cLum64Offsets;
    #endif

    #ifdef LUMINANCE4
    oTexCoord = GetQuadTexCoord(oPos) + cLum16Offsets;
    #endif

    #ifdef LUMINANCE1
    oTexCoord = GetQuadTexCoord(oPos) + cLum4Offsets;
    #endif

    oScreenPos = GetScreenPosPreDiv(oPos);
}

void PS(float2 iTexCoord : TEXCOORD0,
    float2 iScreenPos : TEXCOORD1,
    out float4 oColor : OUTCOLOR0)
{
    #ifdef LUMINANCE64
    float logLumSum = 0.0;
    logLumSum += log(dot(Sample2D(DiffMap, iTexCoord + float2(0.0, 0.0) * cHDR128InvSize).rgb, LumWeights) + 1e-5);
    logLumSum += log(dot(Sample2D(DiffMap, iTexCoord + float2(0.0, 2.0) * cHDR128InvSize).rgb, LumWeights) + 1e-5);
    logLumSum += log(dot(Sample2D(DiffMap, iTexCoord + float2(2.0, 2.0) * cHDR128InvSize).rgb, LumWeights) + 1e-5);
    logLumSum += log(dot(Sample2D(DiffMap, iTexCoord + float2(2.0, 0.0) * cHDR128InvSize).rgb, LumWeights) + 1e-5);
    oColor = logLumSum;
    #endif

    #ifdef LUMINANCE16
    #if !defined(D3D11) && !defined(DILIGENT)
    oColor = GatherAvgLum(sDiffMap, iTexCoord, cLum64InvSize);
    #else
    #ifdef DILIGENT
    oColor = GatherAvgLum(tDiffMap, tDiffMap_sampler, iTexCoord, cLum64InvSize);
    #else
    oColor = GatherAvgLum(tDiffMap, sDiffMap, iTexCoord, cLum64InvSize);
    #endif
    #endif
    #endif

    #ifdef LUMINANCE4
    #if !defined(D3D11) && !defined(DILIGENT)
    oColor = GatherAvgLum(sDiffMap, iTexCoord, cLum16InvSize);
    #else
    #ifdef DILIGENT
    oColor = GatherAvgLum(tDiffMap, tDiffMap_sampler, iTexCoord, cLum16InvSize);
    #else
    oColor = GatherAvgLum(tDiffMap, sDiffMap, iTexCoord, cLum16InvSize);
    #endif
    #endif
    #endif

    #ifdef LUMINANCE1
    #if !defined(D3D11) && !defined(DILIGENT)
    oColor = exp(GatherAvgLum(sDiffMap, iTexCoord, cLum4InvSize) / 16.0);
    #else
    #ifdef DILIGENT
    oColor = exp(GatherAvgLum(tDiffMap, tDiffMap_sampler, iTexCoord, cLum4InvSize) / 16.0);
    #else
    oColor = exp(GatherAvgLum(tDiffMap, sDiffMap, iTexCoord, cLum4InvSize) / 16.0);
    #endif
    #endif
    #endif

    #ifdef ADAPTLUMINANCE
    float adaptedLum = Sample2D(DiffMap, iTexCoord).r;
    float lum = clamp(Sample2D(NormalMap, iTexCoord).r, cAutoExposureLumRange.x, cAutoExposureLumRange.y);
    oColor = adaptedLum + (lum - adaptedLum) * (1.0 - exp(-cDeltaTimePS * cAutoExposureAdaptRate));
    #endif

    #ifdef EXPOSE
    float3 color = Sample2D(DiffMap, iScreenPos).rgb;
    float adaptedLum = Sample2D(NormalMap, iTexCoord).r;
    oColor = float4(color * (cAutoExposureMiddleGrey / adaptedLum), 1.0);
    #endif
}
