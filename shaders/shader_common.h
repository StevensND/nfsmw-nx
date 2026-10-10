#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// NFSMW: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTES_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation, and the library has 160 of
// them for 435 samples.
#define SPEC_CONSTANT_INV_TAMANO_TEX    (1 << 9)
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets: nine
// samples per pixel. In p_000101 (the smoke and wheel spray, which are full-screen rectangles) there
// are eleven, and that single draw is 21 % of the scene. With this bit the eight outer offsets are
// set to zero: the nine samples become identical, DXC merges them into one and the shadow goes from
// 3x3 filtered to a single texel. Edge smoothness is lost.
#define SPEC_CONSTANT_PCF_BARATO        (1 << 15)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing. In p_000131 (the
 * distant world, 45 % of the scene's fragments) that was 12 branches for 1 sample and 14 operations.
 * And it was not needed at all: the app already knows the function when it creates the pipeline (it
 * comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
/*
 * The radial blur of the final composition.
 *
 * p_000139 is the race VisualTreatment: one full-screen quad, exactly 1280x720, no overdraw, with
 * twelve samples. It takes 2.8-3.2 ms real, 72-80 % of all post-processing.
 *
 * Of those twelve, seven are offset DIFFUSEMAP taps (plus one HEIGHTMAP tap that only feeds the
 * factor): the radial speed blur. They are blended with `r5*factor + r3`, where r3 is the center tap,
 * so with factor 0 the output is exactly the center tap and the eight samples are dead: DXC and the
 * driver remove them when specializing.
 *
 * The edge blur when accelerating and with NOS is lost. The image is sharper.
 */
#define SPEC_CONSTANT_SIN_DESENFOQUE    (1 << 19)
/*
 * The world shadow map as the minimum of two textures (nfsmw_nativo_sombra_minimo).
 *
 * The game resolves the same 1600x1600 map twice: without cars (sampled by the car body) and with the
 * cars drawn on top (sampled by the world). With this option the renderer draws the cars onto a
 * render target cleared to 1.0, without copying the world underneath, and the shaders that sample the
 * map with cars take the minimum of the two textures: with the game's LESS/LEQUAL depth test,
 * min(world, cars) is exactly what drawing the cars on top produces. The second texture arrives in the
 * 3D index word of the same register (the shadow map is 2D: that word is unused). With the bit off the
 * code is that of tfetch2DSombra.
 */
#define SPEC_CONSTANT_SOMBRA_MINIMO     (1 << 23)
/*
 * The app knows the library has tfetch2DSombraMin because this constant appears in the SPIR-V of the
 * shaders that use it (OpConstant) and nowhere else. The specialization constant never reaches this
 * value (bits 24-30).
 */
#define NFSMW_MARCA_SOMBRA_MINIMO       0x5E3B1A84u
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

/*
 * NFSMW_DEKO3D: the same HLSL compiled for deko3d (DXC with -DNFSMW_DEKO3D, without -fvk-invert-y), whose
 * SPIR-V nfsmw_dk_glsl.cpp turns into GLSL for uam. uam's GLSL front end has no buffer references and
 * deko3d has no bindless heaps, so in that mode:
 *   - the constants only come from the three UBOs (no push constants, no RawBufferLoad);
 *   - each Xenos sampler register R is one texture at binding R and one sampler at binding R, declared by
 *     the translator; the 2D ones also get their pair (tfetch2DSombraMin) at binding 16 + R;
 *   - the helpers below take the texture and the sampler instead of their heap indices.
 * Without the define the preprocessed text is the same as before it existed. docs/HowToDeko3d.md, 3.1.
 */
#ifndef NFSMW_DEKO3D
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#endif

// NFSMW: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct NfsmwBloqueVs { float4 v[256]; };
struct NfsmwBloquePs { float4 v[224]; };
#ifdef NFSMW_DEKO3D
// The 1/size pairs of the 16 sampler registers end at byte 360 + 16 * 8 = 488, past the 23 float4 below: NVK reads
// beyond the declared array, Mesa's GLSL front end in uam rejects a constant index past it. 31 float4 = 496 bytes,
// the size the deko3d renderer binds.
struct NfsmwBloqueCompartidas { float4 v[31]; };
#else
struct NfsmwBloqueCompartidas { float4 v[23]; };
#endif
[[vk::binding(0, 4)]] ConstantBuffer<NfsmwBloqueVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<NfsmwBloquePs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<NfsmwBloqueCompartidas> g_UboCompartidas;
#ifndef NFSMW_DEKO3D
#define NFSMW_UBO ((g_SpecConstants & SPEC_CONSTANT_CONSTANTES_UBO) != 0)
#endif
#define NFSMW_COMPARTIDA_UINT(B)  asuint(g_UboCompartidas.v[(B) / 16][((B) % 16) / 4])
#define NFSMW_COMPARTIDA_FLOAT(B) g_UboCompartidas.v[(B) / 16][((B) % 16) / 4]

#ifdef NFSMW_DEKO3D
// B is always a constant here: uam loses a dynamic component index into a UBO vector (HowToDeko3d.md,
// section 6, item 12), and g_InputRemap is only called with constant locations.
#define g_Booleans                 NFSMW_COMPARTIDA_UINT(256)
#define g_SwappedTexcoords         NFSMW_COMPARTIDA_UINT(260)
#define g_HalfPixelOffset          float2(NFSMW_COMPARTIDA_FLOAT(264), NFSMW_COMPARTIDA_FLOAT(268))
#define g_AlphaThreshold           NFSMW_COMPARTIDA_FLOAT(272)
#define g_AlphaFunction            NFSMW_COMPARTIDA_UINT(276)
#define g_NdcScale                 float2(NFSMW_COMPARTIDA_FLOAT(280), NFSMW_COMPARTIDA_FLOAT(284))
#define g_NdcOffset                float2(NFSMW_COMPARTIDA_FLOAT(288), NFSMW_COMPARTIDA_FLOAT(292))
#define g_InputRemap(LOC)          NFSMW_COMPARTIDA_UINT(296 + (LOC) * 4)
#else
#define g_Booleans                 (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
#define g_SwappedTexcoords         (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(264), NFSMW_COMPARTIDA_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (NFSMW_UBO ? NFSMW_COMPARTIDA_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// NFSMW: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// NFSMW: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(280), NFSMW_COMPARTIDA_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(288), NFSMW_COMPARTIDA_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// NFSMW: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
#define g_InputRemap(LOC)          (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(296 + (LOC) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))
#endif

// deko3d: there is no specialization at bind time; nfsmw_dk_glsl.cpp gives this constant its value before
// uam compiles, and one DKSH is made per (shader, value).
[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF

#endif

#ifndef NFSMW_DEKO3D
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
#endif

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

#ifdef NFSMW_DEKO3D
// The same helpers with the register's texture and sampler (the translator's NAME_Texture2DDescriptorIndex and
// NAME_SamplerDescriptorIndex are those objects in this mode). Same arithmetic as the heap versions.
float4 tfetch2D(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset, float2 invSize)
{
    float2 desplazamiento;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        desplazamiento = offset * invSize;
    else
        desplazamiento = offset / getTexture2DDimensions(texture);
    return texture.Sample(samplerState, texCoord + desplazamiento);
}

float2 getWeights2D(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset)
{
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}
#else
float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    float2 desplazamiento;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        desplazamiento = offset * invSize;
    else
        desplazamiento = offset / getTexture2DDimensions(texture);
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + desplazamiento);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}
#endif

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

#ifdef NFSMW_DEKO3D
// tfetch2DSombra, tfetch2DSombraMin, tfetch3D and tfetchCube of the heap path below, with objects. The pair of
// tfetch2DSombraMin is the register's NAME_Texture3DDescriptorIndex, a 2D texture at binding 16 + R in this
// mode. There is no tfetch2DBicubic: only UNLEASHED_RECOMP emits it.
float4 tfetch2DSombra(Texture2D<float4> texture, SamplerState samplerState, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_BARATO)
        return tfetch2D(texture, samplerState, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(texture, samplerState, texCoord, offset, invSize);
}

float4 tfetch2DSombraMin(Texture2D<float4> texture, Texture2D<float4> pareja, SamplerState samplerState, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == NFSMW_MARCA_SOMBRA_MINIMO)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 valor = tfetch2DSombra(texture, samplerState, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SOMBRA_MINIMO)
        valor = min(valor, tfetch2DSombra(pareja, samplerState, texCoord, offset, invSize));
    return valor;
}

float4 tfetch3D(Texture3D<float4> texture, SamplerState samplerState, float3 texCoord)
{
    return texture.Sample(samplerState, texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(TextureCube<float4> texture, SamplerState samplerState, float3 texCoord, inout CubeMapData cubeMapData)
{
    return texture.Sample(samplerState, cubeMapData.cubeMapDirections[texCoord.z]);
}
#else
// Shadow map sampling. The translator emits tfetch2D for everything; the library rewrite step
// changes the calls whose sampler is SHADOWMAP_SAMPLER to this function, since those are the only
// ones meant to be made cheaper. With the bit set, all the 3x3 samples land on the same texel and
// the compiler keeps only one.
float4 tfetch2DSombra(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_BARATO)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}

// The shadow map with the minimum of its pair (nfsmw_nativo_sombra_minimo). The library step changes
// all shadow map calls to this function and passes it the 3D index of that same register, which is
// where the app puts the pair: the world map (or the texture itself, which gives the same texel, while
// the app is checking). Both are sampled the same way (same point sampler, same coordinates and
// offsets), so the minimum is per texel. The first comparison is the library marker: it is never
// true and the driver removes it when specializing.
float4 tfetch2DSombraMin(uint resourceDescriptorIndex, uint parejaDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == NFSMW_MARCA_SOMBRA_MINIMO)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 valor = tfetch2DSombra(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SOMBRA_MINIMO)
        valor = min(valor, tfetch2DSombra(parejaDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return valor;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}
#endif

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
#ifdef NFSMW_DEKO3D
    // 32 bits: a 64-bit shift needs int64, which uam does not take. semanticIndex is below 16.
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
#else
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
#endif
}

// NFSMW: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// NFSMW: Xenos alpha test with its comparison function. Positive if the pixel passes.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

#ifndef NFSMW_DEKO3D
float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}
#endif

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
