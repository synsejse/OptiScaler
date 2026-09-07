// Owned RGB-only adapter. Compile both entry points as SM5 DXBC with native
// Windows FXC; validate the exact generated PS's sample-frequency reflection
// before compiling the runtime consumer. Do not strip reflection metadata.
Texture2D<float4> Input : register(t0);
struct VertexOutput
{
    // DXBC links stages by register as well as semantic: PS consumes TEXCOORD0
    // as v0, so keep UV first (o0). Native build validation checks exact linkage.
    float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    VertexOutput output;
    output.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = p;
    return output;
}
float4 PSMain(sample float2 uv : TEXCOORD0) : SV_Target0
{
    // SV_Position may describe a coarse VRS region even for sample-frequency
    // execution. Use the actual sample-interpolated coordinate instead. With
    // W=1 at every vertex and an exact full viewport, this maps each fine
    // sample to its own texel; the admitted source/target extents are equal.
    uint width, height;
    Input.GetDimensions(width, height);
    return float4(Input.Load(int3(uint2(uv * float2(width, height)), 0)).rgb, 0);
}
