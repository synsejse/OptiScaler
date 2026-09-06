// Diagnostic shader-value capture, not an image-processing pass.
// FP16/UNORM inputs are expanded to FP32; no filtering, clamping or channel omission.
#define MainRS "RootFlags(0), RootConstants(num32BitConstants=2, b0), DescriptorTable(SRV(t0, numDescriptors=1)), UAV(u0)"
Texture2D<float4> Input : register(t0);
RWStructuredBuffer<float4> Output : register(u0);
cbuffer Dimensions : register(b0) { uint Width; uint Height; }
[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < Width && id.y < Height)
        Output[id.y * Width + id.x] = Input.Load(int3(id.xy, 0));
}
