#include "Uniforms.hlsl"
#include "Samplers.hlsl"
#include "Transform.hlsl"

void VS(float4 iPos : POSITION,
    #ifndef NOUV
        float2 iTexCoord : TEXCOORD0,
    #endif
    #ifdef SKINNED
        float4 iBlendWeights : BLENDWEIGHT,
        int4 iBlendIndices : BLENDINDICES,
    #endif
    #ifdef INSTANCED
        #ifdef DILIGENT
            float4 iModelInstanceCol1 : TEXCOORD4,
            float4 iModelInstanceCol2 : TEXCOORD5,
            float4 iModelInstanceCol3 : TEXCOORD6,
        #else
            float4x3 iModelInstance : TEXCOORD4,
        #endif
    #endif
    #if defined(BILLBOARD) || defined(DIRBILLBOARD)
        float2 iSize : TEXCOORD1,
    #endif
    #ifdef VSM_SHADOW
        out float4 oTexCoord : TEXCOORD0,
    #else
        out float2 oTexCoord : TEXCOORD0,
    #endif
    out float4 oPos : OUTPOSITION)
{
    // Define a 0,0 UV coord if not expected from the vertex data
    #ifdef NOUV
    float2 iTexCoord = float2(0.0, 0.0);
    #endif

    float4x3 modelMatrix = iModelMatrix;
    float3 worldPos = GetWorldPos(modelMatrix);
    oPos = GetClipPos(worldPos);
    #ifdef VSM_SHADOW
        oTexCoord = float4(GetTexCoord(iTexCoord), oPos.z, oPos.w);
    #else
        oTexCoord = GetTexCoord(iTexCoord);
    #endif
}

void PS(
    #ifdef VSM_SHADOW
        float4 iTexCoord : TEXCOORD0,
    #else
        float2 iTexCoord : TEXCOORD0,
    #endif
    out float4 oColor : OUTCOLOR0)
{
    #ifdef ALPHAMASK
        float alpha = Sample2D(DiffMap, iTexCoord.xy).a;
        if (alpha < 0.5)
            discard;
    #endif

    #ifdef VSM_SHADOW
        float depth = iTexCoord.z / iTexCoord.w;
        oColor = float4(depth, depth * depth, 1.0, 1.0);
    #else
        oColor = 1.0;
    #endif
}
