#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 3), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1), visibility = SHADER_VISIBILITY_ALL), " \
    "StaticSampler(s0, " \
        "filter = FILTER_MIN_MAG_MIP_LINEAR, " \
        "addressU = TEXTURE_ADDRESS_CLAMP, " \
        "addressV = TEXTURE_ADDRESS_CLAMP, " \
        "addressW = TEXTURE_ADDRESS_CLAMP, " \
        "visibility = SHADER_VISIBILITY_ALL)"

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8

#define FLAGS_RAW_SOURCE_BLIT (1 << 0)
#define FLAGS_SCALE_SRC (1 << 1)
#define FLAGS_DEBUG (1 << 16)
#define FLAGS_DEBUG_MODE_MASK (0xFF << 16)
#define FLAGS_DEBUG_SKIP_SIGNAL (2 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DENOISER_OUTPUT (3 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_FUSED_LIGHTING (4 << 17 | FLAGS_DEBUG)

Texture2D<half4> InDenoisedRadiance : register(t0);
Texture2D<half4> InFusedAlbedo : register(t1);
Texture2D<half4> InSkipSignal : register(t2);
RWTexture2D<half4> OutColor : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer CB_Comp : register(b0)
{
    float4 DstTexSize;
    uint Flags;
    float Padding;
    float2 SrcTexSize; // Active source subrect for debug blits; allocation may be larger.
}

bool IsSet(uint mask) { return (Flags & mask) == mask; }
uint GetDebugMode() { return (Flags & FLAGS_DEBUG_MODE_MASK); }

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 px = id.xy;
    if (px.x >= DstTexSize.x || px.y >= DstTexSize.y)
        return;

    [branch]
    if (IsSet(FLAGS_RAW_SOURCE_BLIT))
    {
        [branch]
        if (IsSet(FLAGS_SCALE_SRC))
        {
            uint width, height;
            InDenoisedRadiance.GetDimensions(width, height);
            const float2 uv = (float2(px) + 0.5f) * DstTexSize.zw;
            const float2 srcPixel = clamp(uv * SrcTexSize, 0.5f, SrcTexSize - 0.5f);
            OutColor[px] = InDenoisedRadiance.SampleLevel(LinearSampler, srcPixel / float2(width, height), 0);
        }
        else
            OutColor[px] = InDenoisedRadiance[px];
        return;
    }

    const float3 denoisedRadiance = InDenoisedRadiance[px].rgb;
    const float3 denoisedColor = denoisedRadiance * InFusedAlbedo[px].rgb;
    const float3 preservedLighting = InSkipSignal[px].rgb;

    [branch]
    if (IsSet(FLAGS_DEBUG))
    {
        switch (GetDebugMode())
        {
            case FLAGS_DEBUG_SKIP_SIGNAL:
                OutColor[px] = float4(preservedLighting, 1.0f);
                break;
            case FLAGS_DEBUG_FUSED_LIGHTING:
                OutColor[px] = float4(denoisedColor, 1.0f);
                break;
            default:
                OutColor[px] = float4(denoisedRadiance, 1.0f);
                break;
        }
        return;
    }

    // Native fused composition. Do not mix raw brightness back into the denoiser result,
    // invent a particle layer, or clamp finite source values carried by the signed residual.
    OutColor[px] = float4(denoisedColor + preservedLighting, 1.0f);
}
