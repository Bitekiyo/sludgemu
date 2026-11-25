// Presentation: draws one plane of a console present, scaled into its place in the window.

#define ROOT_SIGNATURE                                                                                 \
    "RootConstants(num32BitConstants = 8, b0),"                                                        \
    "DescriptorTable(SRV(t0)),"                                                                        \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP,"           \
    "              addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP)"

cbuffer Placement : register(b0) {
    float4 dest;   // left, top, right, bottom in clip space
    float4 source; // left, top, right, bottom in texture coordinates
};

Texture2D<float4> plane : register(t0);
SamplerState linear_clamp : register(s0);

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Four vertices, drawn as a triangle strip: the corners of the destination rectangle.
[RootSignature(ROOT_SIGNATURE)]
VertexOut vs_main(uint id : SV_VertexID) {
    float2 corner = float2(id & 1, id >> 1);
    VertexOut o;
    o.position = float4(lerp(dest.xy, dest.zw, corner), 0.0, 1.0);
    o.uv = lerp(source.xy, source.zw, corner);
    return o;
}

[RootSignature(ROOT_SIGNATURE)]
float4 ps_main(VertexOut i) : SV_Target {
    return plane.Sample(linear_clamp, i.uv);
}
