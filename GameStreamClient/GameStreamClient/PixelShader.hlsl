Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);

struct PixelInputType
{
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 main(PixelInputType input) : SV_TARGET
{
    // Clamp coordinates to prevent out-of-bounds drawing
    int x = clamp((int) (input.tex.x * 1920.0f), 0, 1919);
    int y = clamp((int) (input.tex.y * 1080.0f), 0, 1079);

    // 1. Sample Y (Brightness)
    // The Y plane is stored exactly in the top 1080 rows
    float luma = shaderTexture.Load(int3(x, y, 0)).r;

    // 2. Sample U and V (Color)
    // The UV plane is half the height, starts at row 1080, and is interleaved (U, V, U, V...)
    int uv_y = 1080 + (y / 2);
    int u_x = (x / 2) * 2; // U is always on an even pixel
    int v_x = u_x + 1; // V is always on the odd pixel next to it

    float u = shaderTexture.Load(int3(u_x, uv_y, 0)).r - 0.5f;
    float v = shaderTexture.Load(int3(v_x, uv_y, 0)).r - 0.5f;

    // 3. Mathematical YUV to RGB Conversion
    float r = luma + 1.402f * v;
    float g = luma - 0.344f * u - 0.714f * v;
    float b = luma + 1.772f * u;

    return float4(r, g, b, 1.0f);
}