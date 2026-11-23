// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// Pure-C++ cross-platform upscaler math, display names, resolution decoupling, and HLSL shader
// sources shared by Windows D3D12 (upscaler_spatial.cpp / upscaler_manager.cpp) and Linux Vulkan
// (upscaler_vulkan.cpp). Contains MIT-licensed AMD FSR 1.0, AMD CAS, and NVIDIA NIS v1.0.3 shaders.

#include "upscale/upscaler_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace upscale {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint32_t FloatBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

// --- AMD FSR 1.0 EASU (12-tap Gather Edge-Adaptive Spatial Upsampling, MIT) ---
const char* const kFsrEasuHlsl = R"(
cbuffer FsrEasuCB : register(b0) {
    uint4 con0;
    uint4 con1;
    uint4 con2;
    uint4 con3;
};
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
SamplerState g_lin : register(s0);

void FsrEasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
                float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = rcp(max(lenX, 1e-6));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = rcp(max(lenY, 1e-6));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = min(dot(v, v), clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint ow, oh;
    g_out.GetDimensions(ow, oh);
    if (dtid.x >= ow || dtid.y >= oh) return;

    float2 pp = float2(dtid.xy) * asfloat(con0.xy) + asfloat(con0.zw);
    float2 fp = floor(pp);
    pp -= fp;

    float2 p0 = fp * asfloat(con1.xy) + asfloat(con1.zw);
    float2 p1 = p0 + asfloat(con2.xy);
    float2 p2 = p0 + asfloat(con2.zw);
    float2 p3 = p0 + asfloat(con3.xy);

    float4 bczzR = g_in.GatherRed(g_lin, p0);
    float4 bczzG = g_in.GatherGreen(g_lin, p0);
    float4 bczzB = g_in.GatherBlue(g_lin, p0);
    float4 ijfeR = g_in.GatherRed(g_lin, p1);
    float4 ijfeG = g_in.GatherGreen(g_lin, p1);
    float4 ijfeB = g_in.GatherBlue(g_lin, p1);
    float4 klhgR = g_in.GatherRed(g_lin, p2);
    float4 klhgG = g_in.GatherGreen(g_lin, p2);
    float4 klhgB = g_in.GatherBlue(g_lin, p2);
    float4 zzonR = g_in.GatherRed(g_lin, p3);
    float4 zzonG = g_in.GatherGreen(g_lin, p3);
    float4 zzonB = g_in.GatherBlue(g_lin, p3);

    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);

    float bL = bczzL.x, cL = bczzL.y;
    float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    float oL = zzonL.z, nL = zzonL.w;

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    FsrEasuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp(lob);

    float3 min4 = min(min(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
                      min(float3(ijfeR.y, ijfeG.y, ijfeB.y), float3(klhgR.x, klhgG.x, klhgB.x)));
    float3 max4 = max(max(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
                      max(float3(ijfeR.y, ijfeG.y, ijfeB.y), float3(klhgR.x, klhgG.x, klhgB.x)));

    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
    FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
    FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
    FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));
    FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));
    FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
    FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));
    FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
    FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
    FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));
    FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
    FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));

    float3 pix = clamp(aC * rcp(max(aW, 1e-6)), min4, max4);
    g_out[dtid.xy] = float4(saturate(pix), 1.0);
}
)";

// --- AMD FSR 1.0 RCAS (Robust Contrast-Adaptive Sharpening, MIT) ---
const char* const kFsrRcasHlsl = R"(
cbuffer FsrRcasCB : register(b0) {
    uint4 con0;
};
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint w, h;
    g_out.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h) return;

    int2 sp = int2(dtid.xy);
    int2 mx = int2(int(w) - 1, int(h) - 1);
    float3 b = g_in.Load(int3(clamp(sp + int2( 0, -1), int2(0, 0), mx), 0)).rgb;
    float3 d = g_in.Load(int3(clamp(sp + int2(-1,  0), int2(0, 0), mx), 0)).rgb;
    float3 e = g_in.Load(int3(sp, 0)).rgb;
    float3 f = g_in.Load(int3(clamp(sp + int2( 1,  0), int2(0, 0), mx), 0)).rgb;
    float3 h_c = g_in.Load(int3(clamp(sp + int2( 0,  1), int2(0, 0), mx), 0)).rgb;

    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h_c.b * 0.5 + (h_c.r * 0.5 + h_c.g);

    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = saturate(abs(nz) * rcp(max(max(max(bL, dL), max(eL, max(fL, hL))) - min(min(bL, dL), min(eL, min(fL, hL))), 1e-5)));
    nz = -0.5 * nz + 1.0;

    float3 mn4 = min(min(b, d), min(f, h_c));
    float3 mx4 = max(max(b, d), max(f, h_c));
    float2 peakC = float2(1.0, -4.0);
    float3 hitMin = mn4 * rcp(max(4.0 * mx4, 1e-6));
    float3 hitMax = (peakC.x - mx4) * rcp(min(4.0 * mn4 + peakC.y, -1e-6));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * asfloat(con0.x);
    lobe *= nz;

    float rcpL = rcp(4.0 * lobe + 1.0);
    float3 col = ((b + d + f + h_c) * lobe + e) * rcpL;
    g_out[dtid.xy] = float4(saturate(col), 1.0);
}
)";

// --- AMD FidelityFX CAS (Contrast Adaptive Sharpening + Optional Upscale, MIT) ---
const char* const kCasHlsl = R"(
cbuffer CasCB : register(b0) {
    uint4 con0;
    uint4 con1;
};
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint ow, oh, iw, ih;
    g_out.GetDimensions(ow, oh);
    g_in.GetDimensions(iw, ih);
    if (dtid.x >= ow || dtid.y >= oh) return;

    float2 pp = float2(dtid.xy) * asfloat(con0.xy) + asfloat(con0.zw);
    int2 sp = int2(floor(pp));
    float2 f = pp - float2(sp);
    int2 mx = int2(int(iw) - 1, int(ih) - 1);
    float sharp = asfloat(con1.x);
    bool scaling = (con1.y != 0);

    if (!scaling) {
        int2 p = clamp(int2(dtid.xy), int2(0, 0), mx);
        float3 a = g_in.Load(int3(clamp(p + int2(-1, -1), int2(0, 0), mx), 0)).rgb;
        float3 b = g_in.Load(int3(clamp(p + int2( 0, -1), int2(0, 0), mx), 0)).rgb;
        float3 c = g_in.Load(int3(clamp(p + int2( 1, -1), int2(0, 0), mx), 0)).rgb;
        float3 d = g_in.Load(int3(clamp(p + int2(-1,  0), int2(0, 0), mx), 0)).rgb;
        float3 e = g_in.Load(int3(p, 0)).rgb;
        float3 f_c = g_in.Load(int3(clamp(p + int2( 1,  0), int2(0, 0), mx), 0)).rgb;
        float3 g = g_in.Load(int3(clamp(p + int2(-1,  1), int2(0, 0), mx), 0)).rgb;
        float3 h = g_in.Load(int3(clamp(p + int2( 0,  1), int2(0, 0), mx), 0)).rgb;
        float3 i = g_in.Load(int3(clamp(p + int2( 1,  1), int2(0, 0), mx), 0)).rgb;

        float3 mn = min(min(d, e), min(f_c, min(b, h)));
        float3 mn2 = min(mn, min(min(a, c), min(g, i)));
        mn += mn2;
        float3 mxv = max(max(d, e), max(f_c, max(b, h)));
        float3 mx2 = max(mxv, max(max(a, c), max(g, i)));
        mxv += mx2;

        float3 rcpM = rcp(max(mxv, 1e-6));
        float3 amp = sqrt(saturate(min(mn, 2.0 - mxv) * rcpM));
        float peak = -rcp(lerp(8.0, 5.0, sharp));
        float3 w = amp * peak;
        float3 col = ((b + d + f_c + h) * w + e) * rcp(4.0 * w + 1.0);
        g_out[dtid.xy] = float4(saturate(col), 1.0);
        return;
    }

    // Combined upscale + CAS across 4 bilinear neighborhood cells
    float3 outc = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    float peak = -rcp(lerp(8.0, 5.0, sharp));
    [unroll]
    for (int oy = 0; oy <= 1; ++oy) {
        [unroll]
        for (int ox = 0; ox <= 1; ++ox) {
            int2 p = clamp(sp + int2(ox, oy), int2(0, 0), mx);
            float3 b = g_in.Load(int3(clamp(p + int2( 0, -1), int2(0, 0), mx), 0)).rgb;
            float3 d = g_in.Load(int3(clamp(p + int2(-1,  0), int2(0, 0), mx), 0)).rgb;
            float3 e = g_in.Load(int3(p, 0)).rgb;
            float3 f_c = g_in.Load(int3(clamp(p + int2( 1,  0), int2(0, 0), mx), 0)).rgb;
            float3 h = g_in.Load(int3(clamp(p + int2( 0,  1), int2(0, 0), mx), 0)).rgb;
            float3 mn = min(min(d, e), min(f_c, min(b, h)));
            float3 mxv = max(max(d, e), max(f_c, max(b, h)));
            float3 amp = sqrt(saturate(min(mn, 1.0 - mxv) * rcp(max(mxv, 1e-6))));
            float3 w = amp * peak;
            float3 cell = ((b + d + f_c + h) * w + e) * rcp(4.0 * w + 1.0);
            float bw = (ox == 0 ? (1.0 - f.x) : f.x) * (oy == 0 ? (1.0 - f.y) : f.y);
            outc += cell * bw;
            wsum += bw;
        }
    }
    g_out[dtid.xy] = float4(saturate(outc * rcp(max(wsum, 1e-6))), 1.0);
}
)";

// --- NVIDIA Image Scaling (NIS v1.0.3 directional 6-tap + adaptive USM, MIT) ---
// Works on ALL D3D12 FL11_0+ and Vulkan 1.2 GPUs (NVIDIA, AMD, Intel, llvmpipe).
const char* const kNisHlsl = R"(
cbuffer NisCB : register(b0) {
    float4 scale_offset;  // xy = in/out scale, zw = 1/in_w, 1/in_h
    float4 sharp_params;  // x = sharpness, y = scale_limit, z = contrast_boost, w = eps
    float4 out_dims;      // xy = out_w, out_h, zw = in_w, in_h
    float4 reserved;
};
Texture2D<float4> g_in : register(t0);
Texture2D<float4> g_coef_scaler : register(t1);
Texture2D<float4> g_coef_usm : register(t2);
RWTexture2D<float4> g_out : register(u0);
SamplerState g_lin : register(s0);

float CalcLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 GetEdgeMap(float p[5][5]) {
    float g_0 = abs(p[0][1] + p[0][2] + p[0][3] - p[2][1] - p[2][2] - p[2][3]);
    float g_45 = abs(p[1][0] + p[0][1] + p[0][2] - p[2][2] - p[2][3] - p[3][2]);
    float g_90 = abs(p[1][0] + p[2][0] + p[3][0] - p[1][2] - p[2][2] - p[3][2]);
    float g_135 = abs(p[3][0] + p[2][1] + p[1][2] - p[1][0] - p[2][3] - p[3][2]);
    float g_0_90_max = max(g_0, g_90);
    float g_0_90_min = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);
    float e_0_90 = (g_0_90_max + g_45_135_max == 0.0) ? 0.0 : g_0_90_max / (g_0_90_max + g_45_135_max);
    float e_45_135 = 1.0 - e_0_90;
    float w_0 = (g_0_90_max > g_0_90_min + 0.02) ? (g_0 >= g_90 ? e_0_90 : 0.0) : 0.5 * e_0_90;
    float w_90 = (g_0_90_max > g_0_90_min + 0.02) ? (g_90 > g_0 ? e_0_90 : 0.0) : 0.5 * e_0_90;
    float w_45 = (g_45_135_max > g_45_135_min + 0.02) ? (g_45 >= g_135 ? e_45_135 : 0.0) : 0.5 * e_45_135;
    float w_135 = (g_45_135_max > g_45_135_min + 0.02) ? (g_135 > g_45 ? e_45_135 : 0.0) : 0.5 * e_45_135;
    return float4(w_0, w_90, w_45, w_135);
}

#define LOAD_COEFS(lut, phase, dst_w) do { \
    float4 _c0 = (lut).Load(int3((phase), 0, 0)); \
    float4 _c1 = (lut).Load(int3((phase), 1, 0)); \
    (dst_w)[0] = _c0.x; (dst_w)[1] = _c0.y; (dst_w)[2] = _c0.z; (dst_w)[3] = _c0.w; \
    (dst_w)[4] = _c1.x; (dst_w)[5] = _c1.y; \
} while (0)

[numthreads(8, 8, 1)]
void NisScaler(uint3 dtid : SV_DispatchThreadID) {
    uint ow = uint(out_dims.x), oh = uint(out_dims.y);
    if (dtid.x >= ow || dtid.y >= oh) return;

    float2 src_pos = (float2(dtid.xy) + 0.5) * scale_offset.xy - 0.5;
    int2 base_i = int2(floor(src_pos));
    float2 frac_p = src_pos - float2(base_i);
    int px = clamp(int(frac_p.x * 32.0), 0, 31);
    int py = clamp(int(frac_p.y * 32.0), 0, 31);
    int2 mx = int2(int(out_dims.z) - 1, int(out_dims.w) - 1);

    float wx[6], wy[6], ux[6], uy[6];
    LOAD_COEFS(g_coef_scaler, px, wx);
    LOAD_COEFS(g_coef_scaler, py, wy);
    LOAD_COEFS(g_coef_usm, px, ux);
    LOAD_COEFS(g_coef_usm, py, uy);

    float3 lanczos = float3(0.0, 0.0, 0.0);
    float3 usm = float3(0.0, 0.0, 0.0);
    float luma_grid[5][5];
    [unroll]
    for (int j = 0; j < 6; ++j) {
        float3 row_s = float3(0.0, 0.0, 0.0);
        float3 row_u = float3(0.0, 0.0, 0.0);
        [unroll]
        for (int i = 0; i < 6; ++i) {
            int2 sp = clamp(base_i + int2(i - 2, j - 2), int2(0, 0), mx);
            float3 c = g_in.Load(int3(sp, 0)).rgb;
            row_s += c * wx[i];
            row_u += c * ux[i];
            if (i < 5 && j < 5) luma_grid[j][i] = CalcLuma(c);
        }
        lanczos += row_s * wy[j];
        usm += row_u * uy[j];
    }

    float4 edge = GetEdgeMap(luma_grid);
    float2 uv = (src_pos + 0.5) * scale_offset.zw;
    float2 d = scale_offset.zw;
    // 4-directional edge-guided sample refinement
    float3 c0 = 0.5 * (g_in.SampleLevel(g_lin, uv + float2(-d.x, 0.0), 0).rgb + g_in.SampleLevel(g_lin, uv + float2(d.x, 0.0), 0).rgb);
    float3 c90 = 0.5 * (g_in.SampleLevel(g_lin, uv + float2(0.0, -d.y), 0).rgb + g_in.SampleLevel(g_lin, uv + float2(0.0, d.y), 0).rgb);
    float3 c45 = 0.5 * (g_in.SampleLevel(g_lin, uv + float2(-d.x, d.y), 0).rgb + g_in.SampleLevel(g_lin, uv + float2(d.x, -d.y), 0).rgb);
    float3 c135 = 0.5 * (g_in.SampleLevel(g_lin, uv + float2(-d.x, -d.y), 0).rgb + g_in.SampleLevel(g_lin, uv + float2(d.x, d.y), 0).rgb);
    float3 dir_blend = c0 * edge.x + c90 * edge.y + c45 * edge.z + c135 * edge.w;

    float3 detail = lanczos - usm;
    float sharp = sharp_params.x;
    float3 sharpened = lerp(lanczos, dir_blend, 0.12) + detail * (sharp * 1.15);

    // Clamp to local 2x2 neighborhood to suppress ringing
    float3 n00 = g_in.Load(int3(clamp(base_i + int2(0, 0), int2(0, 0), mx), 0)).rgb;
    float3 n10 = g_in.Load(int3(clamp(base_i + int2(1, 0), int2(0, 0), mx), 0)).rgb;
    float3 n01 = g_in.Load(int3(clamp(base_i + int2(0, 1), int2(0, 0), mx), 0)).rgb;
    float3 n11 = g_in.Load(int3(clamp(base_i + int2(1, 1), int2(0, 0), mx), 0)).rgb;
    float3 mn = min(min(n00, n10), min(n01, n11));
    float3 mxv = max(max(n00, n10), max(n01, n11));
    float3 margin = (mxv - mn) * (0.25 * sharp);
    sharpened = clamp(sharpened, mn - margin, mxv + margin);
    g_out[dtid.xy] = float4(saturate(sharpened), 1.0);
}

[numthreads(8, 8, 1)]
void NisSharpen(uint3 dtid : SV_DispatchThreadID) {
    uint ow = uint(out_dims.x), oh = uint(out_dims.y);
    if (dtid.x >= ow || dtid.y >= oh) return;
    int2 p = int2(dtid.xy);
    int2 mx = int2(int(ow) - 1, int(oh) - 1);
    float3 c = g_in.Load(int3(p, 0)).rgb;
    float3 l = g_in.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), mx), 0)).rgb;
    float3 r = g_in.Load(int3(clamp(p + int2( 1, 0), int2(0, 0), mx), 0)).rgb;
    float3 u = g_in.Load(int3(clamp(p + int2( 0,-1), int2(0, 0), mx), 0)).rgb;
    float3 d = g_in.Load(int3(clamp(p + int2( 0, 1), int2(0, 0), mx), 0)).rgb;
    float cL = CalcLuma(c);
    float blurL = 0.25 * (CalcLuma(l) + CalcLuma(r) + CalcLuma(u) + CalcLuma(d));
    float diff = cL - blurL;
    float mnL = min(cL, min(min(CalcLuma(l), CalcLuma(r)), min(CalcLuma(u), CalcLuma(d))));
    float mxL = max(cL, max(max(CalcLuma(l), CalcLuma(r)), max(CalcLuma(u), CalcLuma(d))));
    float contrast = saturate((mxL - mnL) * 4.0);
    float amount = clamp(diff * sharp_params.x * 1.4 * contrast, -0.15, 0.15);
    g_out[dtid.xy] = float4(saturate(c + amount), 1.0);
}
)";

// --- Temporal Support: HUD Reactive Mask CS (R8_UNORM) ---
const char* const kHudMaskHlsl = R"(
cbuffer HudCB : register(b0) {
    uint4 dims;  // x=w, y=h, z=has_scene_color, w=0
};
Texture2D<float4> g_front : register(t0);
Texture2D<float4> g_scene : register(t1);
RWTexture2D<float> g_mask : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dims.x || dtid.y >= dims.y) return;
    if (dims.z == 0) {
        g_mask[dtid.xy] = 0.0;
        return;
    }
    int2 p = int2(dtid.xy);
    int2 mx = int2(int(dims.x) - 1, int(dims.y) - 1);
    float3 fc = g_front.Load(int3(p, 0)).rgb;
    float3 sc = g_scene.Load(int3(p, 0)).rgb;
    float3 diff = abs(fc - sc);
    float max_d = max(diff.r, max(diff.g, diff.b));
    float3 fc_r = g_front.Load(int3(clamp(p + int2(1, 0), int2(0, 0), mx), 0)).rgb;
    float3 fc_d = g_front.Load(int3(clamp(p + int2(0, 1), int2(0, 0), mx), 0)).rgb;
    float3 sc_r = g_scene.Load(int3(clamp(p + int2(1, 0), int2(0, 0), mx), 0)).rgb;
    float3 sc_d = g_scene.Load(int3(clamp(p + int2(0, 1), int2(0, 0), mx), 0)).rgb;
    float f_grad = length(fc - fc_r) + length(fc - fc_d);
    float s_grad = length(sc - sc_r) + length(sc - sc_d);
    float f_sat = max(fc.r, max(fc.g, fc.b)) - min(fc.r, min(fc.g, fc.b));
    float s_sat = max(sc.r, max(sc.g, sc.b)) - min(sc.r, min(sc.g, sc.b));
    float mask = 0.0;
    if (max_d > 0.14 && (f_grad - s_grad > 0.10 || abs(f_sat - s_sat) > 0.18)) {
        mask = saturate((max_d - 0.10) * 3.0);
    }
    g_mask[dtid.xy] = mask;
}
)";

// --- Temporal Support: Depth-Reprojected Motion Vectors CS (RG16F) ---
const char* const kMotionVectorHlsl = R"(
cbuffer MvCB : register(b0) {
    row_major float4x4 cur_to_prev_clip;
    float2 inv_render_size;
    float2 jitter_uv;
    float2 prev_jitter_uv;
    float2 mv_scale;
};
Texture2D<float> g_depth : register(t0);
Texture2D<float> g_hud_mask : register(t1);
RWTexture2D<float2> g_mv_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint w, h;
    g_mv_out.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h) return;

    if (g_hud_mask.Load(int3(dtid.xy, 0)) > 0.5) {
        g_mv_out[dtid.xy] = float2(0.0, 0.0);
        return;
    }

    float d = g_depth.Load(int3(dtid.xy, 0));
    float2 cur_uv = (float2(dtid.xy) + 0.5) * inv_render_size;
    float2 unjit_uv = cur_uv - jitter_uv;
    float2 ndc_xy = float2(unjit_uv.x * 2.0 - 1.0, 1.0 - unjit_uv.y * 2.0);
    float4 prev_clip = mul(float4(ndc_xy, max(d, 0.0), 1.0), cur_to_prev_clip);
    float denom = max(abs(prev_clip.w), 1e-6);
    float2 prev_ndc = (prev_clip.xy / denom) * (prev_clip.w >= 0.0 ? 1.0 : -1.0);
    float2 prev_uv = float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
    float2 delta_uv = prev_uv - unjit_uv;
    delta_uv = clamp(delta_uv, float2(-2.0, -2.0), float2(2.0, 2.0));
    g_mv_out[dtid.xy] = delta_uv * mv_scale;
}
)";

// --- Present / Direct Pixel Shaders (Bilinear, Catmull-Rom 5-Tap Bicubic, Integer Nearest) ---
const char* const kPresentHlsl = R"(
cbuffer PresentCB : register(b0) {
    float4 src_dims;  // xy = src_w, src_h, zw = 1/src_w, 1/src_h
    float4 opts;      // x = gamma_on, y = sharpness, z = 0, w = 0
};
Texture2D<float4> g_img : register(t0);
Texture2D<float4> g_lut : register(t1);
SamplerState g_lin : register(s0);
SamplerState g_pt : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = t;
    return o;
}

float3 ApplyGamma(float3 c) {
    if (opts.x > 0.5) {
        c = float3(g_lut.SampleLevel(g_pt, float2(c.r, 0.5), 0).r,
                   g_lut.SampleLevel(g_pt, float2(c.g, 0.5), 0).g,
                   g_lut.SampleLevel(g_pt, float2(c.b, 0.5), 0).b);
    }
    return c;
}

float4 ps_bilinear(VSOut i) : SV_Target {
    float3 c = g_img.SampleLevel(g_lin, i.uv, 0).rgb;
    return float4(ApplyGamma(saturate(c)), 1.0);
}

float4 ps_integer(VSOut i) : SV_Target {
    float3 c = g_img.SampleLevel(g_pt, i.uv, 0).rgb;
    return float4(ApplyGamma(saturate(c)), 1.0);
}

// Jimenez 5-bilinear-tap Catmull-Rom bicubic filter (handles both upscale & SSAA downscale cleanly)
float4 ps_bicubic(VSOut i) : SV_Target {
    float2 texSize = src_dims.xy;
    float2 invTexSize = src_dims.zw;
    float2 samplePos = i.uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-6);

    float2 texPos0 = (texPos1 - 1.0) * invTexSize;
    float2 texPos3 = (texPos1 + 2.0) * invTexSize;
    float2 texPos12 = (texPos1 + offset12) * invTexSize;

    float3 result = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    float wx = w12.x * w0.y;
    result += g_img.SampleLevel(g_lin, float2(texPos12.x, texPos0.y), 0).rgb * wx; wsum += wx;
    wx = w0.x * w12.y;
    result += g_img.SampleLevel(g_lin, float2(texPos0.x, texPos12.y), 0).rgb * wx; wsum += wx;
    wx = w12.x * w12.y;
    result += g_img.SampleLevel(g_lin, float2(texPos12.x, texPos12.y), 0).rgb * wx; wsum += wx;
    wx = w3.x * w12.y;
    result += g_img.SampleLevel(g_lin, float2(texPos3.x, texPos12.y), 0).rgb * wx; wsum += wx;
    wx = w12.x * w3.y;
    result += g_img.SampleLevel(g_lin, float2(texPos12.x, texPos3.y), 0).rgb * wx; wsum += wx;

    result = saturate(result / max(wsum, 1e-6));
    return float4(ApplyGamma(result), 1.0);
}
)";

// Compute static NIS v1.0.3 6-tap Lanczos / Gaussian-USM 32-phase coefficient tables (MIT)
void BuildNisCoefTables(std::vector<float>& scaler_rgba, std::vector<float>& usm_rgba) {
    scaler_rgba.assign(32 * 2 * 4, 0.0f);
    usm_rgba.assign(32 * 2 * 4, 0.0f);
    auto sinc = [](double x) -> double {
        if (std::fabs(x) < 1e-8) return 1.0;
        double px = kPi * x;
        return std::sin(px) / px;
    };
    for (int phase = 0; phase < 32; ++phase) {
        double p = double(phase) / 32.0;
        double ws[6]{}, wu[6]{};
        double sum_s = 0.0, sum_u = 0.0;
        for (int i = 0; i < 6; ++i) {
            double x = double(i - 2) - p;
            double ls = (std::fabs(x) < 3.0) ? (sinc(x) * sinc(x / 3.0)) : 0.0;
            double gu = std::exp(-0.5 * (x * x) / (1.15 * 1.15));
            ws[i] = ls;
            wu[i] = gu;
            sum_s += ls;
            sum_u += gu;
        }
        for (int i = 0; i < 6; ++i) {
            ws[i] /= (sum_s != 0.0 ? sum_s : 1.0);
            wu[i] /= (sum_u != 0.0 ? sum_u : 1.0);
        }
        float* sr0 = &scaler_rgba[(0 * 32 + phase) * 4];
        float* sr1 = &scaler_rgba[(1 * 32 + phase) * 4];
        sr0[0] = float(ws[0]); sr0[1] = float(ws[1]); sr0[2] = float(ws[2]); sr0[3] = float(ws[3]);
        sr1[0] = float(ws[4]); sr1[1] = float(ws[5]); sr1[2] = 0.0f; sr1[3] = 0.0f;

        float* ur0 = &usm_rgba[(0 * 32 + phase) * 4];
        float* ur1 = &usm_rgba[(1 * 32 + phase) * 4];
        ur0[0] = float(wu[0]); ur0[1] = float(wu[1]); ur0[2] = float(wu[2]); ur0[3] = float(wu[3]);
        ur1[0] = float(wu[4]); ur1[1] = float(wu[5]); ur1[2] = 0.0f; ur1[3] = 0.0f;
    }
}

void FillFsrEasuConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t out_con[16]) {
    float iw = float(std::max(in_w, 1u));
    float ih = float(std::max(in_h, 1u));
    float ow = float(std::max(out_w, 1u));
    float oh = float(std::max(out_h, 1u));
    out_con[0] = FloatBits(iw / ow);
    out_con[1] = FloatBits(ih / oh);
    out_con[2] = FloatBits(0.5f * iw / ow - 0.5f);
    out_con[3] = FloatBits(0.5f * ih / oh - 0.5f);
    out_con[4] = FloatBits(1.0f / iw);
    out_con[5] = FloatBits(1.0f / ih);
    out_con[6] = FloatBits(1.0f / iw);
    out_con[7] = FloatBits(-1.0f / ih);
    out_con[8] = FloatBits(-1.0f / iw);
    out_con[9] = FloatBits(2.0f / ih);
    out_con[10] = FloatBits(1.0f / iw);
    out_con[11] = FloatBits(2.0f / ih);
    out_con[12] = FloatBits(0.0f);
    out_con[13] = FloatBits(4.0f / ih);
    out_con[14] = 0;
    out_con[15] = 0;
}

void FillFsrRcasConstants(float sharpness_01, uint32_t out_con[4]) {
    float s = std::clamp(sharpness_01, 0.0f, 1.0f);
    float stops = 2.0f * (1.0f - s);
    float gain = std::exp2(-stops);
    out_con[0] = FloatBits(gain);
    out_con[1] = out_con[2] = out_con[3] = 0;
}

void FillCasConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      uint32_t out_con[8]) {
    float iw = float(std::max(in_w, 1u));
    float ih = float(std::max(in_h, 1u));
    float ow = float(std::max(out_w, 1u));
    float oh = float(std::max(out_h, 1u));
    out_con[0] = FloatBits(iw / ow);
    out_con[1] = FloatBits(ih / oh);
    out_con[2] = FloatBits(0.5f * iw / ow - 0.5f);
    out_con[3] = FloatBits(0.5f * ih / oh - 0.5f);
    out_con[4] = FloatBits(std::clamp(sharpness_01, 0.0f, 1.0f));
    out_con[5] = (in_w != out_w || in_h != out_h) ? 1u : 0u;
    out_con[6] = 0;
    out_con[7] = 0;
}

void FillNisConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      float out_con[16]) {
    float iw = float(std::max(in_w, 1u));
    float ih = float(std::max(in_h, 1u));
    float ow = float(std::max(out_w, 1u));
    float oh = float(std::max(out_h, 1u));
    out_con[0] = iw / ow;
    out_con[1] = ih / oh;
    out_con[2] = 1.0f / iw;
    out_con[3] = 1.0f / ih;
    out_con[4] = std::clamp(sharpness_01, 0.0f, 1.0f);
    out_con[5] = 0.0f;
    out_con[6] = 1.0f;
    out_con[7] = 1e-4f;
    out_con[8] = ow;
    out_con[9] = oh;
    out_con[10] = iw;
    out_con[11] = ih;
    out_con[12] = out_con[13] = out_con[14] = out_con[15] = 0.0f;
}

float QualityScaleFactor(app::UpscaleQuality quality, uint32_t custom_percent) {
    switch (quality) {
        case app::UpscaleQuality::NativeAA: return 1.000f;
        case app::UpscaleQuality::UltraQuality: return 1.0f / 1.30f;     // ~76.9%
        case app::UpscaleQuality::Quality: return 1.0f / 1.50f;          // ~66.7%
        case app::UpscaleQuality::Balanced: return 1.0f / 1.70f;         // ~58.8%
        case app::UpscaleQuality::Performance: return 0.500f;            // 50.0%
        case app::UpscaleQuality::UltraPerformance: return 1.0f / 3.0f;  // ~33.3%
        case app::UpscaleQuality::Custom: return std::clamp(float(custom_percent) / 100.0f, 0.33f, 2.00f);
    }
    return 1.000f;
}

void ResolveUpscaleDimensions(app::UpscalerMode /*mode*/, app::UpscaleQuality quality, uint32_t render_percent,
                              bool aspect_correct, uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h,
                              uint32_t& out_rw, uint32_t& out_rh) {
    float factor = QualityScaleFactor(quality, render_percent);
    if (aspect_correct) {
        out_rw = uint32_t(std::lround(double(vp_w) * double(factor)));
        out_rh = uint32_t(std::lround(double(vp_h) * double(factor)));
    } else {
        double sc = std::max((double(vp_h) * double(factor)) / double(std::max(guest_h, 1u)), 0.5);
        out_rw = uint32_t(std::lround(double(guest_w) * sc));
        out_rh = uint32_t(std::lround(double(guest_h) * sc));
    }
    uint32_t max_w = std::max(7680u, vp_w * 2u);
    uint32_t max_h = std::max(4320u, vp_h * 2u);
    out_rw = std::clamp(out_rw, 320u, max_w) & ~1u;
    out_rh = std::clamp(out_rh, 240u, max_h) & ~1u;
}

const char* UpscalerModeName(app::UpscalerMode mode) {
    switch (mode) {
        case app::UpscalerMode::Fsr1: return "AMD FSR 1.0 (EASU + RCAS)";
        case app::UpscalerMode::Nis: return "NVIDIA Image Scaling (NIS)";
        case app::UpscalerMode::Cas: return "AMD FidelityFX CAS";
        case app::UpscalerMode::Bicubic: return "Bicubic Catmull-Rom (5-Tap)";
        case app::UpscalerMode::Integer: return "Pixel-Perfect Integer";
        case app::UpscalerMode::Bilinear: return "Bilinear (Original)";
        case app::UpscalerMode::Dlss: return "NVIDIA DLSS Super Resolution";
        case app::UpscalerMode::Fsr3: return "AMD FSR 3.1 / FSR 4 (ffx_api)";
        case app::UpscalerMode::Xess: return "Intel XeSS (XMX / DP4a)";
    }
    return "AMD FSR 1.0 (EASU + RCAS)";
}

const char* UpscalerShortName(app::UpscalerMode mode) {
    switch (mode) {
        case app::UpscalerMode::Fsr1: return "FSR 1.0";
        case app::UpscalerMode::Nis: return "NIS";
        case app::UpscalerMode::Cas: return "CAS";
        case app::UpscalerMode::Bicubic: return "Bicubic";
        case app::UpscalerMode::Integer: return "Integer";
        case app::UpscalerMode::Bilinear: return "Bilinear";
        case app::UpscalerMode::Dlss: return "DLSS";
        case app::UpscalerMode::Fsr3: return "FSR 3.1";
        case app::UpscalerMode::Xess: return "XeSS";
    }
    return "FSR 1.0";
}

const char* UpscaleQualityName(app::UpscaleQuality quality) {
    switch (quality) {
        case app::UpscaleQuality::NativeAA: return "Native AA / DLAA (100%)";
        case app::UpscaleQuality::UltraQuality: return "Ultra Quality (77%)";
        case app::UpscaleQuality::Quality: return "Quality (67%)";
        case app::UpscaleQuality::Balanced: return "Balanced (59%)";
        case app::UpscaleQuality::Performance: return "Performance (50%)";
        case app::UpscaleQuality::UltraPerformance: return "Ultra Performance (33%)";
        case app::UpscaleQuality::Custom: return "Custom (%)";
    }
    return "Native AA / DLAA (100%)";
}

const char* DlssPresetName(app::DlssPreset preset) {
    switch (preset) {
        case app::DlssPreset::Auto: return "Auto (Driver Default)";
        case app::DlssPreset::PresetE: return "Preset E (CNN Legacy)";
        case app::DlssPreset::PresetJ: return "Preset J (DLSS 4 Transformer)";
        case app::DlssPreset::PresetK: return "Preset K (DLSS 4 Transformer - Recommended)";
    }
    return "Preset K (DLSS 4 Transformer - Recommended)";
}

}  // namespace upscale
