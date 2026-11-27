#pragma once

#include "canvas.hpp"
#include "desktop_rotation.hpp"

#include <usylibpp/ui/wgc.hpp>

#include <d2d1effectauthor.h>
#include <d2d1effecthelpers.h>
#include <d2d1effects.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace usyseek::ui {

// {6A1E8D20-4C45-4F9A-9D82-2F1E37C0A811}
inline constexpr GUID CLSID_UsyseekLiquidGlass = {
    .Data1=0x6a1e8d20, 
    .Data2=0x4c45, 
    .Data3=0x4f9a, 
    .Data4={0x9d, 0x82, 0x2f, 0x1e, 0x37, 0xc0, 0xa8, 0x11},
};

// {A3F42B91-8D13-4E2F-8A10-914D2C77E502}
inline constexpr GUID GUID_UsyseekLiquidGlassPS = {
    .Data1=0xa3f42b91, 
    .Data2=0x8d13, 
    .Data3=0x4e2f, 
    .Data4={0x8a, 0x10, 0x91, 0x4d, 0x2c, 0x77, 0xe5, 0x02},
};

// {D7C24E18-5B91-4A63-9C4E-318F7D20B645}
inline constexpr GUID CLSID_UsyseekHdrToSdr = {
    .Data1 = 0xd7c24e18,
    .Data2 = 0x5b91,
    .Data3 = 0x4a63,
    .Data4 = {0x9c, 0x4e, 0x31, 0x8f, 0x7d, 0x20, 0xb6, 0x45},
};

// {F2A9106C-3E74-4D8B-B102-8E45C9A3D711}
inline constexpr GUID GUID_UsyseekHdrToSdrPS = {
    .Data1 = 0xf2a9106c,
    .Data2 = 0x3e74,
    .Data3 = 0x4d8b,
    .Data4 = {0xb1, 0x02, 0x8e, 0x45, 0xc9, 0xa3, 0xd7, 0x11},
};

/// Constant buffer matching `cbuffer HdrToSdrParams : register(b0)` in the HDR conversion shader.
struct alignas(16) HdrToSdrParams {
    float mode = 0.0f;            // 0 = FP16 scRGB linear, 1 = R10G10B10A2 HDR10 PQ BT.2020, 2 = R10G10B10A2 10-bit sRGB
    float sdr_white_scale = 1.0f; // sdr_white_nits / 80.0 (linear scRGB white reference)
    float max_nits = 1000.0f;
    float reserved = 0.0f;
};
static_assert(sizeof(HdrToSdrParams) == 16);

/// Constant buffer matching `cbuffer LiquidGlassParams : register(b0)` in the D2D shader.
struct alignas(16) LiquidGlassShaderParams {
    std::array<float, 4> panel_rect{};     // left, top, width, height (DIPs)
    std::array<float, 4> src_bounds{};     // minX, minY, maxX, maxY (DIPs)
    std::array<float, 4> tint_color{};     // r, g, b, a
    std::array<float, 4> refract_params{}; // refThickness, refFactor, refDispersion, dispersionRange
    std::array<float, 4> fresnel_params{}; // fresnelRange, fresnelHardness, fresnelFactor, magnification
    std::array<float, 4> glare_params{};   // glareRange, glareHardness, glareFactor, glareAngleDeg
    std::array<float, 4> shape_params{};   // glareConvergence, glareOppositeFactor, cornerRadiusDip, roundnessN
    std::array<float, 4> misc_params{};    // dpr (overridden from the effect context), forceOpaque, edgeAaPx, shapeMarginDip
    std::array<float, 4> extra_params{};   // opacity, centeredEdge, rimInsetDip, reserved
};
static_assert(sizeof(LiquidGlassShaderParams) == 144);

/// High-level material configuration for one Liquid Glass panel.
struct GlassStyle {
    float blur_std_dev = 5.5f;
    float corner_radius = 12.0f;
    float roundness = 4.6f;
    float margin_dip = 0.5f;
    float ref_thickness = 26.0f;
    float ref_factor = 1.45f;
    float ref_dispersion = 4.5f;
    float dispersion_range = 0.65f;
    float fresnel_range = 36.0f;
    float fresnel_hardness = 10.0f;
    float fresnel_factor = 28.0f;
    float magnification = 1.03f;
    float glare_range = 34.0f;
    float glare_hardness = 14.0f;
    float glare_factor = 68.0f;
    float glare_convergence = 55.0f;
    float glare_opposite_factor = 72.0f;
    float glare_angle_deg = -45.0f;
    D2D1_COLOR_F tint{};
    bool force_opaque = false;
    float opacity = 1.0f;
    /// Centre the edge antialiasing on the outline (like D2D geometry) instead of letting it bleed outward.
    bool centered_edge = false;
    /// Evaluate the refraction slope at least this far (DIPs) inside the rim. 0 keeps the full rim refraction;
    /// raise it on small elements whose rim would otherwise reach far outside their own bounds and pick up
    /// unrelated colours (e.g. a thin coloured line along the bottom edge).
    float rim_inset_dip = 0.0f;
};

namespace detail {

// HLSL shader implementing the LiquidGlassWinUI material algorithms (superellipse SDF,
// analytical gradient normal, Snell's-law refraction, 7-tap spectral dispersion,
// Fresnel rim, and directional specular glare). Parameters are in DIPs; scene positions arrive in pixels.
inline constexpr const char* LIQUID_GLASS_HLSL = R"hlsl(
#define PI 3.14159265359

Texture2D<float4> InputTexture0 : register(t0);
SamplerState InputSampler0 : register(s0);

cbuffer LiquidGlassParams : register(b0)
{
    float4 panelRect;     // x, y, w, h (DIPs)
    float4 srcBounds;     // minX, minY, maxX, maxY (DIPs)
    float4 tintColor;     // r, g, b, a
    float4 refractParams; // x=refThickness, y=refFactor, z=refDispersion, w=dispersionRange
    float4 fresnelParams; // x=fresnelRange, y=fresnelHardness, z=fresnelFactor, w=magnification
    float4 glareParams;   // x=glareRange, y=glareHardness, z=glareFactor, w=glareAngleDeg
    float4 shapeParams;   // x=glareConvergence, y=glareOppositeFactor, z=cornerRadiusDip, w=roundnessN
    float4 miscParams;    // x=dpr, y=forceOpaque, z=edgeAaPx, w=shapeMarginDip
    float4 extraParams;   // x=opacity, y=centeredEdge, z=rimInsetDip, w=reserved
};

float superellipseCornerSDF(float2 p, float r, float n)
{
    p = abs(p);
    if (n <= 2.01)
    {
        return length(p) - r;
    }
    return pow(pow(max(p.x, 1e-6), n) + pow(max(p.y, 1e-6), n), 1.0 / n) - r;
}

float RoundedRectSDF(float2 p, float2 center, float2 half0, float cr, float n)
{
    p -= center;
    cr = min(cr, min(half0.x, half0.y));
    float2 d = abs(p) - half0;
    if (d.x > -cr && d.y > -cr)
    {
        float2 cornerCenter = sign(p) * (half0 - float2(cr, cr));
        return superellipseCornerSDF(p - cornerCenter, cr, n);
    }
    else
    {
        return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
    }
}

float2 RoundedRectUnitNormal(float2 fragCoord, float2 center, float2 halfPx, float cr, float n, float dpr)
{
    float2 p = fragCoord - center;
    cr = min(cr, min(halfPx.x, halfPx.y));
    float2 sgn = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
    float2 ap = abs(p);
    float2 inner = max(halfPx - cr, 0.0);

    // Smoothly round the inner reference box with radius k so the normal field is C1 continuous
    // everywhere inside the panel when cr is small, while keeping innerSm at the exact corner center
    // when cr is already large (e.g. collapsed pill with cr >= 24 DIPs).
    float k = min(max(24.0 * dpr - cr, 0.0), min(inner.x, inner.y));
    float2 innerSm = max(inner - k, 0.0);
    float2 q = max(ap - innerSm, 0.0);
    float2 g = (n <= 2.01) ? q : float2(q.x > 1e-6 ? pow(q.x, n - 1.0) : 0.0, q.y > 1e-6 ? pow(q.y, n - 1.0) : 0.0);
    float gLen = length(g);
    float2 local;
    if (gLen > 1e-6)
    {
        local = g / gLen;
    }
    else
    {
        float2 d = ap - halfPx;
        local = (d.x > d.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    }
    return sgn * local;
}

// D2D runs effect shaders in pixel space: SCENE_POSITION is in physical pixels and uv0.zw is the uv step per
// pixel. Everything in the constant buffer is in DIPs, so positions are converted once via sceneDip = px / dpr.
float4 SampleAtDip(float2 targetDip, float2 sceneDip, float4 uv0, float dpr, float forceOpaque)
{
    float2 pad = 0.5 / max(dpr, 1.0);
    float2 lo = srcBounds.xy + pad;
    float2 hi = max(lo, srcBounds.zw - pad);
    float2 clampedDip = clamp(targetDip, lo, hi);
    float2 sampleUv = uv0.xy + uv0.zw * ((clampedDip - sceneDip) * dpr);
    float4 c = InputTexture0.SampleLevel(InputSampler0, sampleUv, 0.0);
    if (forceOpaque > 0.5)
    {
        c.a = 1.0;
    }
    return c;
}

float EdgeSlopeFactor(float depthPx, float thicknessPx, float ior)
{
    if (depthPx >= thicknessPx || thicknessPx <= 0.001)
    {
        return 0.0;
    }
    float s = saturate(1.0 - depthPx / thicknessPx);
    float s2 = s * s;
    float thetaI = asin(s2);
    float thetaT = asin(clamp((1.0 / max(ior, 1.001)) * s2, -0.999, 0.999));
    return -tan(thetaT - thetaI);
}

float EaseRimDepth(float depthPx, float easePx)
{
    float d = max(depthPx, 0.0);
    float w = max(easePx, 1e-3);
    return d < w ? (d * d) / (2.0 * w) : d - 0.5 * w;
}

float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float dpr = max(miscParams.x, 0.25);
    float forceOpaque = miscParams.y;
    float2 sceneDip = scenePos.xy / dpr;
    float2 res = max(panelRect.zw * dpr, float2(2.0, 2.0));
    float2 fragCoord = (sceneDip - panelRect.xy) * dpr;
    float2 center = res * 0.5;
    float margin = max(miscParams.w * dpr, 0.0);
    float2 halfPx = max(res * 0.5 - margin, 1.0);
    float cr = min(max(shapeParams.z * dpr, 0.0), min(halfPx.x, halfPx.y));
    float n = clamp(shapeParams.w, 2.0, 8.0);

    float distPx = RoundedRectSDF(fragCoord, center, halfPx, cr, n);
    float merged = distPx / res.y;

    float2 dCorner = abs(fragCoord - center) - halfPx;
    float cornerProx = cr > 1.0
        ? saturate((dCorner.x + cr) / cr) * saturate((dCorner.y + cr) / cr)
        : 0.0;
    float aaWidthPx = lerp(1.25, 2.25, cornerProx);
    // extraParams.y > 0.5 centres the edge ramp on the shape outline (50% coverage at distPx == 0), matching how
    // D2D antialiases fill_round/stroke_round. Otherwise coverage is full at the outline and bleeds aaWidthPx out.
    float aaBiasPx = extraParams.y > 0.5 ? aaWidthPx * 0.5 : 0.0;
    float glassThresholdPx = max(2.5, aaWidthPx * 2.0);

    float4 outColor = float4(0.0, 0.0, 0.0, 0.0);
    if (distPx < glassThresholdPx)
    {
        float refThickness = max(refractParams.x * dpr, 1.0);
        float refFactor = max(refractParams.y, 1.001);
        float refDispersion = refractParams.z;
        float dispersionRange = max(refractParams.w, 0.001);

        float nmerged = -distPx;
        // The slope profile is near-vertical at the rim: near depth 0 it behaves like A - B*sqrt(depth), whose
        // derivative is infinite, so neighbouring rim pixels would sample backdrop texels far apart and alias into
        // grain. Rather than clamping the depth (which also throws away the strongest rim refraction), ease the
        // depth quadratically over the first rimEasePx: s = d^2 / (2w) for d < w, d - w/2 beyond (C1 at d == w).
        // sqrt(d^2 / 2w) is linear in d, so the slope becomes finite (~B / sqrt(2w)) while the full rim magnitude
        // EdgeSlopeFactor(0) is kept. extraParams.z optionally starts the profile further inside the rim (DIPs),
        // trading rim reach for less sampling from outside small elements.
        float rimEasePx = min(2.0 * dpr, refThickness * 0.5);
        float rimInsetPx = max(extraParams.z, 0.0) * dpr;
        float edgeFactor = EdgeSlopeFactor(rimInsetPx + EaseRimDepth(nmerged, rimEasePx), refThickness, refFactor);
        float ef1 = EdgeSlopeFactor(rimInsetPx + EaseRimDepth(nmerged + 1.0, rimEasePx), refThickness, refFactor);

        float2 unitN = RoundedRectUnitNormal(fragCoord, center, halfPx, cr, n, dpr);
        float nLen = length(unitN);
        float2 safeN = nLen > 1e-6 ? (unitN / nLen) : float2(0.0, 0.0);

        // Refraction offset in DIPs (matching LiquidGlass.hlsl magnitude in pixels)
        float2 offDip = -safeN * (edgeFactor * 70.710678) / dpr;
        float2 offSpreadDip = safeN * (abs(edgeFactor - ef1) * 70.710678) / dpr;

        float edgeProx = 1.0 - saturate(abs(nmerged) / refThickness);
        float minSpreadDip = (0.25 / dpr) * edgeProx;
        float osLen = length(offSpreadDip);
        if (osLen < minSpreadDip)
        {
            float2 spreadDir = osLen > 1e-8 ? (offSpreadDip / osLen) : safeN;
            offSpreadDip = spreadDir * minSpreadDip;
        }

        float2 panelCenterDip = panelRect.xy + panelRect.zw * 0.5;
        float mag = max(fresnelParams.w, 1.0);
        float2 zoomedDip = panelCenterDip + (sceneDip - panelCenterDip) / mag;

        float3 accRgb = 0.0;
        float accA = 0.0;
        [unroll]
        for (int ti = 0; ti < 8; ti++)
        {
            float f = ((float)ti / 7.0) - 0.5;
            float4 tap = SampleAtDip(zoomedDip + offDip + offSpreadDip * f, sceneDip, uv0, dpr, forceOpaque);
            accRgb += tap.rgb;
            accA += tap.a;
        }
        float4 blurredPixel = float4(accRgb / 8.0, accA / 8.0);
        // Per-channel premultiplied alpha. Dispersion builds each channel from different taps, so each channel
        // must be unpremultiplied by the alpha mixed with the same weights; dividing all channels by one shared
        // alpha pushes single channels far above 1 near transparent backdrop texels (bright saturated pixels).
        float3 chanA = blurredPixel.aaa;

        float depthInZone = nmerged / refThickness;
        float tt = saturate(depthInZone / dispersionRange);
        float attenuation = 1.0 - smoothstep(0.0, 1.0, tt);
        float2 centeredCoord = fragCoord - center;
        float posFactor = (centeredCoord.x * centeredCoord.y) / max(halfPx.x * halfPx.y, 1e-6);
        float dispAmount = refDispersion * attenuation * posFactor;

        if (abs(dispAmount) >= 1e-5)
        {
            float2 dispDip = offDip * dispAmount;
            float maxHalfDip = 6.0;
            float dLen = length(dispDip);
            dispDip *= min(1.0, maxHalfDip / max(dLen, 1e-6));
            float2 baseDip = zoomedDip + offDip;

            float4 sRed    = SampleAtDip(baseDip + dispDip,               sceneDip, uv0, dpr, forceOpaque);
            float4 sOrange = SampleAtDip(baseDip + dispDip * (2.0 / 3.0), sceneDip, uv0, dpr, forceOpaque);
            float4 sYellow = SampleAtDip(baseDip + dispDip * (1.0 / 3.0), sceneDip, uv0, dpr, forceOpaque);
            float4 sCyan   = SampleAtDip(baseDip - dispDip * (1.0 / 3.0), sceneDip, uv0, dpr, forceOpaque);
            float4 sBlue   = SampleAtDip(baseDip - dispDip * (2.0 / 3.0), sceneDip, uv0, dpr, forceOpaque);
            float4 sPurple = SampleAtDip(baseDip - dispDip,               sceneDip, uv0, dpr, forceOpaque);

            float3 disp;
            disp.r = (sRed.r + sOrange.r + sYellow.r) / 3.5 + sPurple.r / 7.0;
            disp.g = sOrange.g / 7.0 + (sYellow.g + blurredPixel.g + sCyan.g) / 3.5;
            disp.b = (blurredPixel.b + sCyan.b + sBlue.b + sPurple.b) / 4.0;
            blurredPixel.rgb = disp;
            chanA.r = (sRed.a + sOrange.a + sYellow.a) / 3.5 + sPurple.a / 7.0;
            chanA.g = sOrange.a / 7.0 + (sYellow.a + blurredPixel.a + sCyan.a) / 3.5;
            chanA.b = (blurredPixel.a + sCyan.a + sBlue.a + sPurple.a) / 4.0;
        }

        // The alpha floor keeps 8-bit quantisation noise in near-transparent texels (e.g. rgb 2/255 over a 1/255)
        // from being amplified into a fully saturated colour; such texels fade to black instead.
        blurredPixel.rgb = saturate(blurredPixel.rgb / max(chanA, 0.04));

        float3 tintRgb = tintColor.rgb;
        float tintA = tintColor.a;
        outColor = lerp(blurredPixel, float4(tintRgb, 1.0), tintA * 0.85);

        // Fresnel rim lighting
        {
            float fresnelRange = max(fresnelParams.x, 0.0001);
            float fresnelHardness = fresnelParams.y / 100.0;
            float fresnelFactorN = fresnelParams.z / 100.0;
            float fresnelRatio = 500.0 / fresnelRange;
            float fresnelK = min(fresnelRatio * fresnelRatio, 3000.0 * dpr);
            float ffBase = saturate(1.0 + merged * res.y / (1500.0 * dpr) * fresnelK + fresnelHardness);
            float ff2 = ffBase * ffBase;
            float fresnelFactor = ff2 * ff2 * ffBase;
            float3 fresnelBase = lerp(float3(1.0, 1.0, 1.0), tintRgb, tintA * 0.5);
            float lighten = clamp(0.2 * fresnelFactor * fresnelFactorN, 0.0, 1.0);
            float3 fresnelTint = lerp(fresnelBase, float3(1.0, 1.0, 1.0), lighten);
            float fresnelCoverage = smoothstep(0.0, 2.0, fresnelParams.x * dpr);
            outColor = lerp(outColor, float4(fresnelTint, 1.0), fresnelFactor * fresnelFactorN * 0.7 * fresnelCoverage);
        }

        // Directional specular glare
        {
            float glareRange = max(glareParams.x, 0.0001);
            float glareHardness = glareParams.y / 100.0;
            float glareFactorN = glareParams.z / 100.0;
            float glareAngleNorm = glareParams.w * PI / 180.0;
            float glareConvergenceN = shapeParams.x / 100.0;
            float glareOppositeN = shapeParams.y / 100.0;

            float glareRatio = 500.0 / glareRange;
            float glareK = min(glareRatio * glareRatio, 3000.0 * dpr);
            float gfBase = saturate(1.0 + merged * res.y / (1500.0 * dpr) * glareK + glareHardness);
            float gf2 = gfBase * gfBase;
            float glareGeoFactor = gf2 * gf2 * gfBase;

            float nAngle = nLen > 1e-6 ? atan2(safeN.y, safeN.x) : 0.0;
            if (nAngle < 0.0) nAngle += 2.0 * PI;
            float glareAngle = (nAngle - PI / 4.0 + glareAngleNorm) * 2.0;

            float glareFarside = ((glareAngle > 1.5 * PI && glareAngle < 3.5 * PI) || (glareAngle < -0.5 * PI)) ? 1.0 : 0.0;
            float glareAngleFactor = (0.5 + sin(glareAngle) * 0.5) * (glareFarside > 0.5 ? 1.2 * glareOppositeN : 1.2) * glareFactorN;
            glareAngleFactor = glareAngleFactor > 0.0
                ? saturate(pow(max(glareAngleFactor, 1e-6), max(0.1 + glareConvergenceN * 2.0, 0.05)))
                : 0.0;

            float g = glareAngleFactor * glareGeoFactor;
            float3 glareBase = lerp(blurredPixel.rgb, tintRgb, tintA * 0.5);
            float3 glareColor = lerp(glareBase, float3(1.0, 1.0, 1.0), clamp(g, 0.0, 1.0));
            float glareCoverage = smoothstep(0.0, 2.0, glareParams.x * dpr);
            outColor = lerp(outColor, float4(glareColor, 1.0), saturate(g) * glareCoverage);
        }
    }

    float alpha = (1.0 - saturate((distPx + aaBiasPx) / aaWidthPx)) * saturate(extraParams.x);
    return float4(outColor.rgb * alpha, alpha);
}
)hlsl";

/// Compiles the Liquid Glass pixel shader once and caches its raw bytecode (no COM object outlives `wWinMain`).
inline std::span<const BYTE> liquid_glass_ps_bytecode() noexcept {
    static std::mutex mtx;
    static std::vector<BYTE> bytecode;
    const std::scoped_lock lk{mtx};
    if (!bytecode.empty()) return bytecode;

    wil::com_ptr<ID3DBlob> code;
    wil::com_ptr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        LIQUID_GLASS_HLSL,
        std::strlen(LIQUID_GLASS_HLSL),
        "LiquidGlass.hlsl",
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &err);
    if (FAILED(hr) || !code || code->GetBufferSize() == 0) {
        if (err) {
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        }
        return {};
    }
    try {
        const auto* bytes = static_cast<const BYTE*>(code->GetBufferPointer());
        bytecode.assign(bytes, bytes + code->GetBufferSize());
    } catch (...) {
        bytecode.clear();
        return {};
    }
    return bytecode;
}

// HLSL shader converting native HDR / WCG desktop duplication frames (FP16 scRGB linear or R10G10B10A2
// HDR10 PQ BT.2020 / 10-bit SDR) into white-level-accurate sRGB BGRA8. SDR pixels [0, 1] pass through
// bit-exact so the glass matches the surrounding desktop seamlessly; HDR peaks > 1.0 roll off smoothly.
inline constexpr const char* HDR_TO_SDR_HLSL = R"hlsl(
Texture2D<float4> InputTexture0 : register(t0);
SamplerState InputSampler0 : register(s0);

cbuffer HdrToSdrParams : register(b0)
{
    float mode;          // 0 = FP16 scRGB, 1 = R10G10B10A2 PQ BT.2020, 2 = R10G10B10A2 10-bit sRGB
    float sdrWhiteScale; // sdr_white_nits / 80.0
    float maxNits;
    float reserved;
};

float3 LinearToSrgb(float3 x)
{
    x = saturate(x);
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(max(x, 1e-6), 1.0 / 2.4) - 0.055;
    return (x <= 0.0031308) ? lo : hi;
}

float3 PqToNits(float3 v)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    v = saturate(v);
    float3 vp = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(vp - c1, 0.0);
    float3 den = max(c2 - c3 * vp, 1e-6);
    return pow(num / den, 1.0 / m1) * 10000.0;
}

float3 Bt2020ToBt709(float3 c)
{
    return float3(
         1.6604910 * c.r - 0.5876411 * c.g - 0.0728499 * c.b,
        -0.1245505 * c.r + 1.1328999 * c.g - 0.0083494 * c.b,
        -0.0181508 * c.r - 0.1005789 * c.g + 1.1187297 * c.b
    );
}

float3 TonemapNormalizedLinear(float3 lin)
{
    float luma = max(dot(lin, float3(0.2126, 0.7152, 0.0722)), 0.0);
    float mn = min(lin.r, min(lin.g, lin.b));
    if (mn < 0.0 && luma > 1e-6)
    {
        float t = luma / (luma - mn);
        lin = lerp(luma.xxx, lin, t);
    }
    lin = max(lin, 0.0);

    float peak = max(lin.r, max(lin.g, lin.b));
    if (peak > 1.0)
    {
        float3 base = lin / peak;
        float excess = peak - 1.0;
        float desat = excess / (excess + 1.5);
        lin = lerp(base, float3(1.0, 1.0, 1.0), desat);
    }
    return lin;
}

float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float3 raw = InputTexture0.SampleLevel(InputSampler0, uv0.xy, 0.0).rgb;
    if (mode > 1.5)
    {
        return float4(saturate(raw), 1.0);
    }

    float scale = max(sdrWhiteScale, 1.0);
    float3 lin;
    if (mode > 0.5)
    {
        float3 bt2020 = PqToNits(raw) / (scale * 80.0);
        lin = Bt2020ToBt709(bt2020);
    }
    else
    {
        float3 safeRaw = (raw == raw) ? clamp(raw, -100.0, 1000.0) : float3(0.0, 0.0, 0.0);
        lin = safeRaw / scale;
    }

    return float4(LinearToSrgb(TonemapNormalizedLinear(lin)), 1.0);
}
)hlsl";

inline std::span<const BYTE> hdr_to_sdr_ps_bytecode() noexcept {
    static std::mutex mtx;
    static std::vector<BYTE> bytecode;
    const std::scoped_lock lk{mtx};
    if (!bytecode.empty()) return bytecode;

    wil::com_ptr<ID3DBlob> code;
    wil::com_ptr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        HDR_TO_SDR_HLSL,
        std::strlen(HDR_TO_SDR_HLSL),
        "HdrToSdr.hlsl",
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &code,
        &err);
    if (FAILED(hr) || !code || code->GetBufferSize() == 0) {
        if (err) {
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        }
        return {};
    }
    try {
        const auto* bytes = static_cast<const BYTE*>(code->GetBufferPointer());
        bytecode.assign(bytes, bytes + code->GetBufferSize());
    } catch (...) {
        bytecode.clear();
        return {};
    }
    return bytecode;
}

inline D2D1_RECT_L expand_rect_safe(const D2D1_RECT_L& r, LONG pad) noexcept {
    const auto clamp_add = [](LONG v, LONG delta) -> LONG {
        const long long s = static_cast<long long>(v) + static_cast<long long>(delta);
        return static_cast<LONG>(std::clamp<long long>(s, LONG_MIN, LONG_MAX));
    };
    return D2D1::RectL(
        clamp_add(r.left, -pad),
        clamp_add(r.top, -pad),
        clamp_add(r.right, pad),
        clamp_add(r.bottom, pad));
}

/// True when the calling thread's desktop is the active input desktop (false during UAC Secure Desktop or lock screen).
[[nodiscard]] inline bool is_input_desktop_accessible() noexcept {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    const auto close_input = wil::scope_exit([&] { CloseDesktop(input); });
    HDESK cur = GetThreadDesktop(GetCurrentThreadId());
    if (!cur) return true;
    std::array<wchar_t, 64> input_name{};
    std::array<wchar_t, 64> cur_name{};
    DWORD n1 = 0;
    DWORD n2 = 0;
    if (!GetUserObjectInformationW(input, UOI_NAME, input_name.data(),
                                   static_cast<DWORD>(input_name.size() * sizeof(wchar_t)), &n1) ||
        !GetUserObjectInformationW(cur, UOI_NAME, cur_name.data(),
                                   static_cast<DWORD>(cur_name.size() * sizeof(wchar_t)), &n2)) {
        return true;
    }
    input_name.back() = L'\0';
    cur_name.back() = L'\0';
    return _wcsicmp(input_name.data(), cur_name.data()) == 0;
}

} // namespace detail

/// Native Direct2D custom effect implementation running the Liquid Glass pixel shader.
class LiquidGlassEffectImpl final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    LiquidGlassEffectImpl(const LiquidGlassEffectImpl&) = delete;
    LiquidGlassEffectImpl& operator=(const LiquidGlassEffectImpl&) = delete;
    LiquidGlassEffectImpl(LiquidGlassEffectImpl&&) = delete;
    LiquidGlassEffectImpl& operator=(LiquidGlassEffectImpl&&) = delete;

    static bool register_once(Gfx& gfx) noexcept {
        ID2D1Factory1* factory = gfx.d2d_factory();
        if (!factory) return false;
        if (gfx.liquid_glass_registered()) return true;

        static constexpr PCWSTR xml = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='UsyseekLiquidGlass'/>
  <Property name='Author' type='string' value='usyseek'/>
  <Property name='Category' type='string' value='Stylize'/>
  <Property name='Description' type='string' value='Refractive liquid glass material'/>
  <Inputs>
    <Input name='Source'/>
  </Inputs>
  <Property name='Params' type='blob'>
    <Property name='DisplayName' type='string' value='Params'/>
  </Property>
</Effect>)";

        const std::array<D2D1_PROPERTY_BINDING, 1> bindings = {{
            D2D1_BLOB_TYPE_BINDING(L"Params", &LiquidGlassEffectImpl::set_params, &LiquidGlassEffectImpl::get_params),
        },};
        const HRESULT hr = factory->RegisterEffectFromString(
            CLSID_UsyseekLiquidGlass,
            xml,
            bindings.data(),
            static_cast<UINT32>(bindings.size()),
            &LiquidGlassEffectImpl::create_effect);
        if (SUCCEEDED(hr)) {
            gfx.mark_liquid_glass_registered();
            return true;
        }
        return false;
    }

    static HRESULT CALLBACK create_effect(IUnknown** out) noexcept {
        if (!out) return E_POINTER;
        auto* p = new (std::nothrow) LiquidGlassEffectImpl();
        if (!p) return E_OUTOFMEMORY;
        *out = static_cast<ID2D1EffectImpl*>(p);
        return S_OK;
    }

    HRESULT set_params(const BYTE* data, UINT32 size) noexcept {
        if (!data || size != params_.size()) return E_INVALIDARG;
        std::memcpy(params_.data(), data, params_.size());
        return S_OK;
    }

    HRESULT get_params(BYTE* data, UINT32 size, UINT32* actual) const noexcept {
        if (actual) *actual = static_cast<UINT32>(params_.size());
        if (!data) return S_OK;
        if (size < params_.size()) return E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(data, params_.data(), params_.size());
        return S_OK;
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1EffectImpl)) {
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        } else if (riid == __uuidof(ID2D1DrawTransform)) {
            *ppv = static_cast<ID2D1DrawTransform*>(this);
        } else if (riid == __uuidof(ID2D1Transform)) {
            *ppv = static_cast<ID2D1Transform*>(this);
        } else if (riid == __uuidof(ID2D1TransformNode)) {
            *ppv = static_cast<ID2D1TransformNode*>(this);
        }
        if (*ppv) {
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() noexcept override {
        const auto r = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    // ID2D1EffectImpl
    IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) noexcept override {
        if (!ctx || !graph) return E_INVALIDARG;
        ctx_ = ctx;
        if (!ctx->IsShaderLoaded(GUID_UsyseekLiquidGlassPS)) {
            const std::span<const BYTE> ps = detail::liquid_glass_ps_bytecode();
            if (ps.empty()) return E_FAIL;
            const HRESULT hr = ctx->LoadPixelShader(
                GUID_UsyseekLiquidGlassPS,
                ps.data(),
                static_cast<UINT32>(ps.size()));
            if (FAILED(hr)) return hr;
        }
        return graph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) noexcept override {
        if (!draw_info_) return E_UNEXPECTED;
        LiquidGlassShaderParams p{};
        std::memcpy(&p, params_.data(), params_.size());
        if (ctx_) {
            float dpi_x = 96.0f;
            float dpi_y = 96.0f;
            ctx_->GetDpi(&dpi_x, &dpi_y);
            if (dpi_x > 0.0f) p.misc_params[0] = dpi_x / 96.0f; // DIP -> pixel scale of this render
        }
        return draw_info_->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&p), sizeof(p));
    }

    IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) noexcept override { return E_NOTIMPL; }

    // ID2D1DrawTransform
    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) noexcept override {
        draw_info_ = info;
        if (!draw_info_) return E_INVALIDARG;
        const HRESULT hr = draw_info_->SetPixelShader(GUID_UsyseekLiquidGlassPS);
        if (FAILED(hr)) return hr;
        const D2D1_INPUT_DESCRIPTION desc{
            .filter = D2D1_FILTER_MIN_MAG_MIP_LINEAR,
            .levelOfDetailCount = 0,
        };
        return draw_info_->SetInputDescription(0, desc);
    }

    // ID2D1Transform
    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* output_rect, D2D1_RECT_L* input_rects, UINT32 count) const noexcept override {
        if (!output_rect || !input_rects || count != 1) return E_INVALIDARG;
        input_rects[0] = detail::expand_rect_safe(*output_rect, sample_pad_px());
        return S_OK;
    }

    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* input_rects, const D2D1_RECT_L*, UINT32 count,
                                             D2D1_RECT_L* output_rect, D2D1_RECT_L* opaque_sub_rect) noexcept override {
        if (!input_rects || !output_rect || !opaque_sub_rect || count != 1) return E_INVALIDARG;
        *output_rect = input_rects[0];
        *opaque_sub_rect = D2D1::RectL(0, 0, 0, 0);
        return S_OK;
    }

    IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L invalid_input, D2D1_RECT_L* invalid_output) const noexcept override {
        if (!invalid_output) return E_INVALIDARG;
        *invalid_output = detail::expand_rect_safe(invalid_input, sample_pad_px());
        return S_OK;
    }

    // ID2D1TransformNode
    IFACEMETHODIMP_(UINT32) GetInputCount() const noexcept override { return 1; }

private:
    LiquidGlassEffectImpl() = default;
    ~LiquidGlassEffectImpl() = default;

    /// Conservative upper bound (in physical pixels) on how far the shader can sample from `SCENE_POSITION`
    /// due to Snell refraction, spread taps, chromatic dispersion, and lens magnification.
    [[nodiscard]] LONG sample_pad_px() const noexcept {
        LiquidGlassShaderParams p{};
        std::memcpy(&p, params_.data(), params_.size());
        float dpr = std::max(p.misc_params[0], 0.25f);
        if (ctx_) {
            float dpi_x = 96.0f;
            float dpi_y = 96.0f;
            ctx_->GetDpi(&dpi_x, &dpi_y);
            if (dpi_x > 0.0f) dpr = std::max(dpi_x / 96.0f, 0.25f);
        }
        const float ior = std::clamp(p.refract_params[1], 1.001f, 3.0f);
        const float max_ref_px = std::sqrt(std::max((ior * ior) - 1.0f, 0.0f)) * 70.710678f * 1.5f;
        const float max_disp_px = 6.0f * dpr;
        const float mag = std::max(p.fresnel_params[3], 1.0f);
        const float panel_extent_px = std::max(p.panel_rect[2], p.panel_rect[3]) * dpr;
        const float max_mag_px = 0.5f * std::max(panel_extent_px, 0.0f) * (1.0f - (1.0f / mag));
        const float total = std::ceil(max_ref_px + max_disp_px + max_mag_px + 8.0f);
        if (!std::isfinite(total)) return 160;
        return std::clamp<LONG>(static_cast<LONG>(total), 32, 512);
    }

    volatile long refs_ = 1;
    wil::com_ptr<ID2D1EffectContext> ctx_;
    wil::com_ptr<ID2D1DrawInfo> draw_info_;
    // Raw bytes (not the 16-byte-aligned struct) so the COM object needs no over-alignment padding.
    std::array<std::byte, sizeof(LiquidGlassShaderParams)> params_{};
};

/// Native Direct2D custom effect converting HDR/WCG (FP16 scRGB or R10G10B10A2) textures into SDR BGRA8.
class HdrToSdrEffectImpl final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    HdrToSdrEffectImpl(const HdrToSdrEffectImpl&) = delete;
    HdrToSdrEffectImpl& operator=(const HdrToSdrEffectImpl&) = delete;
    HdrToSdrEffectImpl(HdrToSdrEffectImpl&&) = delete;
    HdrToSdrEffectImpl& operator=(HdrToSdrEffectImpl&&) = delete;

    static bool register_once(Gfx& gfx) noexcept {
        ID2D1Factory1* factory = gfx.d2d_factory();
        if (!factory) return false;
        if (gfx.is_effect_registered(CLSID_UsyseekHdrToSdr)) return true;

        static constexpr PCWSTR xml = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='UsyseekHdrToSdr'/>
  <Property name='Author' type='string' value='usyseek'/>
  <Property name='Category' type='string' value='Color'/>
  <Property name='Description' type='string' value='HDR scRGB/PQ to sRGB converter'/>
  <Inputs>
    <Input name='Source'/>
  </Inputs>
  <Property name='Params' type='blob'>
    <Property name='DisplayName' type='string' value='Params'/>
  </Property>
</Effect>)";

        const std::array<D2D1_PROPERTY_BINDING, 1> bindings = {{
            D2D1_BLOB_TYPE_BINDING(L"Params", &HdrToSdrEffectImpl::set_params, &HdrToSdrEffectImpl::get_params),
        },};
        const HRESULT hr = factory->RegisterEffectFromString(
            CLSID_UsyseekHdrToSdr,
            xml,
            bindings.data(),
            static_cast<UINT32>(bindings.size()),
            &HdrToSdrEffectImpl::create_effect);
        if (SUCCEEDED(hr)) {
            gfx.mark_effect_registered(CLSID_UsyseekHdrToSdr);
            return true;
        }
        return false;
    }

    static HRESULT CALLBACK create_effect(IUnknown** out) noexcept {
        if (!out) return E_POINTER;
        auto* p = new (std::nothrow) HdrToSdrEffectImpl();
        if (!p) return E_OUTOFMEMORY;
        *out = static_cast<ID2D1EffectImpl*>(p);
        return S_OK;
    }

    HRESULT set_params(const BYTE* data, UINT32 size) noexcept {
        if (!data || size != params_.size()) return E_INVALIDARG;
        std::memcpy(params_.data(), data, params_.size());
        return S_OK;
    }

    HRESULT get_params(BYTE* data, UINT32 size, UINT32* actual) const noexcept {
        if (actual) *actual = static_cast<UINT32>(params_.size());
        if (!data) return S_OK;
        if (size < params_.size()) return E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(data, params_.data(), params_.size());
        return S_OK;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1EffectImpl)) {
            *ppv = static_cast<ID2D1EffectImpl*>(this);
        } else if (riid == __uuidof(ID2D1DrawTransform)) {
            *ppv = static_cast<ID2D1DrawTransform*>(this);
        } else if (riid == __uuidof(ID2D1Transform)) {
            *ppv = static_cast<ID2D1Transform*>(this);
        } else if (riid == __uuidof(ID2D1TransformNode)) {
            *ppv = static_cast<ID2D1TransformNode*>(this);
        }
        if (*ppv) {
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() noexcept override {
        const auto r = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) noexcept override {
        if (!ctx || !graph) return E_INVALIDARG;
        if (!ctx->IsShaderLoaded(GUID_UsyseekHdrToSdrPS)) {
            const std::span<const BYTE> ps = detail::hdr_to_sdr_ps_bytecode();
            if (ps.empty()) return E_FAIL;
            const HRESULT hr = ctx->LoadPixelShader(
                GUID_UsyseekHdrToSdrPS,
                ps.data(),
                static_cast<UINT32>(ps.size()));
            if (FAILED(hr)) return hr;
        }
        return graph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) noexcept override {
        if (!draw_info_) return E_UNEXPECTED;
        return draw_info_->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(params_.data()),
                                                        static_cast<UINT32>(params_.size()));
    }

    IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) noexcept override { return E_NOTIMPL; }

    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) noexcept override {
        draw_info_ = info;
        if (!draw_info_) return E_INVALIDARG;
        const HRESULT hr = draw_info_->SetPixelShader(GUID_UsyseekHdrToSdrPS);
        if (FAILED(hr)) return hr;
        (void)draw_info_->SetOutputBuffer(D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_CHANNEL_DEPTH_4);
        const D2D1_INPUT_DESCRIPTION desc{
            .filter = D2D1_FILTER_MIN_MAG_MIP_POINT,
            .levelOfDetailCount = 0,
        };
        return draw_info_->SetInputDescription(0, desc);
    }

    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* output_rect, D2D1_RECT_L* input_rects, UINT32 count) const noexcept override {
        if (!output_rect || !input_rects || count != 1) return E_INVALIDARG;
        input_rects[0] = *output_rect;
        return S_OK;
    }

    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* input_rects, const D2D1_RECT_L*, UINT32 count,
                                             D2D1_RECT_L* output_rect, D2D1_RECT_L* opaque_sub_rect) noexcept override {
        if (!input_rects || !output_rect || !opaque_sub_rect || count != 1) return E_INVALIDARG;
        *output_rect = input_rects[0];
        *opaque_sub_rect = input_rects[0];
        return S_OK;
    }

    IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L invalid_input, D2D1_RECT_L* invalid_output) const noexcept override {
        if (!invalid_output) return E_INVALIDARG;
        *invalid_output = invalid_input;
        return S_OK;
    }

    IFACEMETHODIMP_(UINT32) GetInputCount() const noexcept override { return 1; }

private:
    HdrToSdrEffectImpl() = default;
    ~HdrToSdrEffectImpl() = default;

    volatile long refs_ = 1;
    wil::com_ptr<ID2D1DrawInfo> draw_info_;
    std::array<std::byte, sizeof(HdrToSdrParams)> params_{};
};

/// Captures the live monitor desktops on the GPU via DXGI 1.5/1.6 Output Duplication (`DuplicateOutput1` with
/// native HDR/WCG support), falling back to shared Windows.Graphics.Capture (`usylibpp::ui::wgc`) per monitor
/// when DXGI duplication is unavailable (e.g. cross-adapter / hybrid-GPU displays).
/// Shared across all windows on the same D3D11 device so duplication is only acquired once per monitor.
class DesktopDuplicator {
public:
    DesktopDuplicator() = default;
    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;
    DesktopDuplicator(DesktopDuplicator&&) = delete;
    DesktopDuplicator& operator=(DesktopDuplicator&&) = delete;
    ~DesktopDuplicator() { reset(); }

    enum class CaptureBackend : uint8_t { None, Dxgi, Wgc };

    struct FrameDelta {
        uint64_t seq = 0;
        bool dirty_all = false;
        std::vector<RECT> dirty_rects; // virtual-desktop coordinates
    };

    struct OutputState {
        static constexpr size_t MAX_HISTORY = 16;
        HMONITOR mon = nullptr;
        RECT mon_rect{}; // virtual-desktop coordinates (already rotated)
        DesktopRotation rotation = DesktopRotation::Identity;
        CaptureBackend backend = CaptureBackend::None;
        usylibpp::ui::MonitorColorInfo color_info{};
        ULONGLONG color_refresh_tick = 0;
        wil::com_ptr<IDXGIOutputDuplication> dupl;
        usylibpp::ui::wgc::MonitorCaptureSession wgc_session;
        wil::com_ptr<ID3D11Texture2D> desktop_tex; // always desktop-oriented BGRA8, `tex_w` x `tex_h` == monitor size
        bool desktop_is_rt = false;
        uint64_t tex_id = 0;
        UINT tex_w = 0;
        UINT tex_h = 0;
        // Rotated SDR outputs (or HDR fallback when direct DXGI surface wrapping fails): holds native-oriented frame.
        wil::com_ptr<ID3D11Texture2D> native_tex;
        DXGI_FORMAT native_fmt = DXGI_FORMAT_UNKNOWN;
        wil::com_ptr<ID2D1Bitmap1> native_bmp;
        wil::com_ptr<ID2D1Bitmap1> desktop_target_bmp;
        bool has_frame = false;
        bool needs_full_copy = true;
        uint64_t frame_seq = 0;
        ULONGLONG retry_after_tick = 0;

        [[nodiscard]] long desk_w() const noexcept { return mon_rect.right - mon_rect.left; }
        [[nodiscard]] long desk_h() const noexcept { return mon_rect.bottom - mon_rect.top; }

        /// Frame history, oldest first. Fixed ring: slots (and their rect vectors' capacity) are reused.
        [[nodiscard]] size_t history_size() const noexcept { return history_len_; }
        [[nodiscard]] const FrameDelta& history_at(size_t i) const noexcept { return history_[(history_head_ + i) % MAX_HISTORY]; }
        /// Swaps `delta` into the newest slot; `delta` gets the evicted slot back for reuse.
        void push_history(FrameDelta& delta) noexcept {
            size_t idx = 0;
            if (history_len_ < MAX_HISTORY) {
                idx = (history_head_ + history_len_) % MAX_HISTORY;
                ++history_len_;
            } else {
                idx = history_head_;
                history_head_ = (history_head_ + 1) % MAX_HISTORY;
            }
            std::swap(history_[idx], delta);
        }
        void clear_history() noexcept { history_head_ = history_len_ = 0; }

    private:
        std::array<FrameDelta, MAX_HISTORY> history_{};
        size_t history_head_ = 0;
        size_t history_len_ = 0;

    public:

        /// Part of `screen_rc` (virtual-desktop coordinates) on this output, in `desktop_tex` coordinates.
        [[nodiscard]] IntRect tex_rect_for(const RECT& screen_rc) const noexcept {
            return int_rect_intersect(
                IntRect{.left = screen_rc.left - mon_rect.left, .top = screen_rc.top - mon_rect.top,
                        .right = screen_rc.right - mon_rect.left, .bottom = screen_rc.bottom - mon_rect.top,},
                IntRect{.left = 0, .top = 0, .right = static_cast<long>(tex_w), .bottom = static_cast<long>(tex_h)});
        }
    };

    void reset() noexcept {
        if (hdr_fx_) hdr_fx_->SetInput(0, nullptr);
        for (const auto& o : outputs_) o->wgc_session.stop();
        outputs_.clear();
        hdr_fx_.reset();
        conv_dc_.reset();
        wgc_dev_.reset();
        epoch_ = 0;
    }

    /// Marks a glass window move-size loop until the matching `end_drag`. During one, Windows can draw the cursor
    /// into duplicated frames, so captures keep updating live but never copy the pixels around the cursor; those
    /// keep their last cursor-free contents and are refreshed once the cursor moves off them (that repaint shows up
    /// as a dirty rect). Every output is refreshed first so monitors the window is dragged onto are already current.
    void begin_drag(Gfx& gfx) {
        if (drag_depth_++ > 0 || !gfx.ready()) return;
        sync_epoch(gfx);
        has_last_cursor_ = false;
        const std::optional<RECT> mask = cursor_mask();
        const ULONGLONG now = GetTickCount64();
        mons_scratch_.clear();
        EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&mons_scratch_));
        for (const HMONITOR mon : mons_scratch_) (void)poll_output(gfx, mon, now, mask ? &*mask : nullptr);
    }

    void end_drag() noexcept {
        if (drag_depth_ <= 0) return;
        --drag_depth_;
        if (drag_depth_ == 0) {
            has_last_cursor_ = false;
            // Repair any cursor-masked pixels skipped during the drag on the next acquired frame.
            for (const auto& o : outputs_) o->needs_full_copy = true;
        }
    }

    /// Polls every output that `screen_rc` overlaps (without blocking, or with a brief wait on initial acquire).
    /// Fills `out` with their states and returns true only when every overlapped output has a valid frame.
    bool poll_rect(Gfx& gfx, const RECT& screen_rc, ULONGLONG now_tick, std::vector<const OutputState*>& out) {
        out.clear();
        if (!gfx.ready()) return false;
        sync_epoch(gfx);

        mons_scratch_.clear();
        EnumDisplayMonitors(nullptr, &screen_rc, &collect_monitor, reinterpret_cast<LPARAM>(&mons_scratch_));
        if (mons_scratch_.empty()) {
            if (HMONITOR nearest = MonitorFromRect(&screen_rc, MONITOR_DEFAULTTONEAREST)) {
                mons_scratch_.push_back(nearest);
            }
        }

        const std::optional<RECT> mask = drag_depth_ > 0 ? cursor_mask() : std::nullopt;
        bool all_ok = true;
        for (const HMONITOR mon : mons_scratch_) {
            if (const OutputState* st = poll_output(gfx, mon, now_tick, mask ? &*mask : nullptr)) {
                out.push_back(st);
            } else {
                all_ok = false;
            }
        }
        return all_ok && !out.empty();
    }

private:
    /// Above this many changed rects a whole-frame copy is cheaper than individual region copies.
    static constexpr size_t MAX_REGION_COPIES = 48;

    void sync_epoch(Gfx& gfx) noexcept {
        if (epoch_ != gfx.epoch()) {
            if (hdr_fx_) hdr_fx_->SetInput(0, nullptr);
            for (const auto& o : outputs_) o->wgc_session.stop();
            outputs_.clear();
            hdr_fx_.reset();
            conv_dc_.reset();
            wgc_dev_.reset();
            epoch_ = gfx.epoch();
        }
    }

    static BOOL CALLBACK collect_monitor(HMONITOR m, HDC /*dc*/, LPRECT /*rc*/, LPARAM lp) noexcept {
        try {
            reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
        } catch (...) {
            return FALSE;
        }
        return TRUE;
    }

    /// Screen-space area the cursor may have been drawn into since the last poll (previous and current cursor
    /// boxes, padded for the drop shadow), or nullopt when the cursor is hidden.
    std::optional<RECT> cursor_mask() noexcept {
        CURSORINFO ci{.cbSize = sizeof(CURSORINFO)};
        if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0 || !ci.hCursor) {
            has_last_cursor_ = false;
            return std::nullopt;
        }
        if (ci.hCursor != cursor_handle_) {
            ICONINFO ii{};
            if (!GetIconInfo(ci.hCursor, &ii)) {
                has_last_cursor_ = false;
                return std::nullopt;
            }
            BITMAP bm{};
            const HBITMAP shape = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
            const bool ok = shape && GetObjectW(shape, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0;
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (!ok) {
                has_last_cursor_ = false;
                return std::nullopt;
            }
            cursor_handle_ = ci.hCursor;
            cursor_hotspot_ = {.x = static_cast<LONG>(ii.xHotspot), .y = static_cast<LONG>(ii.yHotspot)};
            // Monochrome cursors stack the AND and XOR masks vertically in one bitmap.
            cursor_size_ = {.cx = bm.bmWidth, .cy = ii.hbmColor ? bm.bmHeight : std::max<LONG>(1, bm.bmHeight / 2)};
        }
        const RECT cur{
            .left = ci.ptScreenPos.x - cursor_hotspot_.x,
            .top = ci.ptScreenPos.y - cursor_hotspot_.y,
            .right = ci.ptScreenPos.x - cursor_hotspot_.x + cursor_size_.cx,
            .bottom = ci.ptScreenPos.y - cursor_hotspot_.y + cursor_size_.cy,
        };
        RECT mask = cur;
        if (has_last_cursor_) UnionRect(&mask, &cur, &last_cursor_);
        last_cursor_ = cur;
        has_last_cursor_ = true;
        const LONG pad = std::max<LONG>(8, std::max(cursor_size_.cx, cursor_size_.cy) / 4);
        InflateRect(&mask, pad, pad);
        return mask;
    }

    /// Texture-space bounds of an output's duplication frames (the panel's native orientation).
    [[nodiscard]] static IntRect native_bounds(const OutputState& st) noexcept {
        const bool swap = st.rotation == DesktopRotation::Rot90 || st.rotation == DesktopRotation::Rot270;
        return {.left = 0, .top = 0, .right = static_cast<long>(swap ? st.tex_h : st.tex_w),
                .bottom = static_cast<long>(swap ? st.tex_w : st.tex_h),};
    }

    /// `screen_rc` mapped into an output's native texture space (empty when it misses the output).
    [[nodiscard]] static IntRect screen_to_native(const OutputState& st, const RECT& screen_rc) noexcept {
        const IntRect local = st.tex_rect_for(screen_rc);
        if (int_rect_empty(local)) return {};
        return int_rect_intersect(
            desktop_to_tex(local, st.rotation, static_cast<long>(st.tex_w), static_cast<long>(st.tex_h)),
            native_bounds(st));
    }

    static void copy_region(Gfx& gfx, ID3D11Texture2D* dst, ID3D11Texture2D* src, const IntRect& r) noexcept {
        if (int_rect_empty(r) || r.left < 0 || r.top < 0) return;
        const D3D11_BOX box{.left = static_cast<UINT>(r.left), .top = static_cast<UINT>(r.top), .front = 0,
                            .right = static_cast<UINT>(r.right), .bottom = static_cast<UINT>(r.bottom), .back = 1,};
        gfx.d3d_context()->CopySubresourceRegion(dst, 0, box.left, box.top, 0, src, 0, &box);
    }

    /// `mask` (screen space, may be null) is left uncopied whenever the output already holds a frame.
    const OutputState* poll_output(Gfx& gfx, HMONITOR mon, ULONGLONG now_tick, const RECT* mask) {
        OutputState* st = find_or_create(mon);
        if (!ensure_duplication(gfx, *st, now_tick)) return st->has_frame ? st : nullptr;
        if (st->backend == CaptureBackend::Wgc) {
            return poll_output_wgc(gfx, *st);
        }

        for (int attempt = 0; attempt < 4; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            wil::com_ptr<IDXGIResource> res;
            const UINT timeout_ms = st->has_frame ? 0u : 25u;
            const HRESULT hr = st->dupl->AcquireNextFrame(timeout_ms, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) break;
            if (FAILED(hr)) {
                drop_duplication(*st);
                st->retry_after_tick = now_tick + ((hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) ? 100u : 250u);
                return st->has_frame ? st : nullptr;
            }

            const auto release_frame = wil::scope_exit([&] { st->dupl->ReleaseFrame(); });
            // The initial DXGI handshake frame (and pointer-only updates) have LastPresentTime == 0: skip them.
            if (info.LastPresentTime.QuadPart == 0) continue;

            const auto tex = res.try_query<ID3D11Texture2D>();
            if (!tex) break;

            D3D11_TEXTURE2D_DESC sd{};
            tex->GetDesc(&sd);
            const bool is_sdr_bgra = (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            const bool is_hdr_fmt = (sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
            if (sd.Width == 0 || sd.Height == 0 || (!is_sdr_bgra && !is_hdr_fmt)) break;

            if (is_hdr_fmt && now_tick >= st->color_refresh_tick) {
                const float fresh_scale = usylibpp::ui::query_monitor_sdr_white_scale(st->mon);
                if (std::fabs(fresh_scale - st->color_info.sdr_white_scale) > 1e-3f) {
                    st->color_info.sdr_white_scale = fresh_scale;
                    st->color_info.sdr_white_nits = fresh_scale * 80.0f;
                    st->needs_full_copy = true;
                }
                st->color_refresh_tick = now_tick + 2000u;
            }

            const bool rotated = st->rotation != DesktopRotation::Identity;
            const bool swap = st->rotation == DesktopRotation::Rot90 || st->rotation == DesktopRotation::Rot270;
            const UINT want_w = swap ? sd.Height : sd.Width;
            const UINT want_h = swap ? sd.Width : sd.Height;
            const bool need_rt = rotated || is_hdr_fmt;
            const bool need_native_sdr = rotated && is_sdr_bgra;
            if (!ensure_desktop_surface(gfx, *st, want_w, want_h, need_rt, need_native_sdr, sd.Width, sd.Height, next_tex_id_)) {
                break;
            }

            FrameDelta& delta = scratch_delta_;
            delta.seq = 0;
            const bool have_meta = collect_frame_rects(*st, info.TotalMetadataBufferSize, delta);
            delta.dirty_all = !st->has_frame || st->needs_full_copy || !have_meta;
            const bool full = delta.dirty_all || copy_rects_.size() > MAX_REGION_COPIES;

            if (is_sdr_bgra) {
                // Zero-overhead SDR fast path: direct GPU copy (plus D2D rotation only when rotated).
                ID3D11Texture2D* dst = rotated ? st->native_tex.get() : st->desktop_tex.get();
                const IntRect masked = (mask && st->has_frame) ? screen_to_native(*st, *mask) : IntRect{};
                if (full && int_rect_empty(masked)) {
                    gfx.d3d_context()->CopyResource(dst, tex.get());
                } else {
                    if (full) {
                        const IntRect b = native_bounds(*st);
                        copy_rects_.assign(1, RECT{.left = b.left, .top = b.top, .right = b.right, .bottom = b.bottom});
                    }
                    // Only changed regions differ from the previous frame already held in `dst`.
                    std::array<IntRect, 4> pieces{};
                    for (const RECT& r : copy_rects_) {
                        const IntRect ir{.left = r.left, .top = r.top, .right = r.right, .bottom = r.bottom};
                        const size_t n = int_rect_subtract(ir, masked, pieces);
                        for (size_t i = 0; i < n; ++i) copy_region(gfx, dst, tex.get(), pieces[i]);
                    }
                }
                if (rotated && !rotate_into_desktop(*st, full ? nullptr : &delta.dirty_rects)) {
                    st->has_frame = false; // desktop_tex is now out of step with native_tex: force a full copy next time
                    st->needs_full_copy = true;
                    break;
                }
            } else {
                // Native HDR / 10-bit path: single-pass GPU tone-map (+ rotation & dirty/mask clipping) into BGRA8.
                if (!convert_hdr_into_desktop(gfx, *st, tex.get(), sd,
                                              full ? nullptr : &delta.dirty_rects,
                                              (mask && st->has_frame) ? mask : nullptr)) {
                    st->has_frame = false;
                    st->needs_full_copy = true;
                    break;
                }
            }

            st->has_frame = true;
            st->needs_full_copy = false;
            ++st->frame_seq;
            delta.seq = st->frame_seq;
            st->push_history(delta);
            break;
        }
        return st->has_frame ? st : nullptr;
    }

    const OutputState* poll_output_wgc(Gfx& gfx, OutputState& st) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(st.mon, &mi)) {
            drop_duplication(st);
            return st.has_frame ? &st : nullptr;
        }
        st.mon_rect = mi.rcMonitor;

        if (!st.has_frame) {
            if (HANDLE ev = st.wgc_session.event_handle()) {
                (void)WaitForSingleObject(ev, 25u);
            }
        }

        bool dirty_valid = false;
        (void)st.wgc_session.consume_latest_frame_with_dirty(
            [&](ID3D11Texture2D* tex, ABI::Windows::Graphics::SizeInt32 content_sz) noexcept {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                const UINT want_w = (content_sz.Width > 0) ? std::min<UINT>(td.Width, static_cast<UINT>(content_sz.Width)) : td.Width;
                const UINT want_h = (content_sz.Height > 0) ? std::min<UINT>(td.Height, static_cast<UINT>(content_sz.Height)) : td.Height;
                if (want_w == 0 || want_h == 0) return;
                if (!ensure_desktop_surface(gfx, st, want_w, want_h, false, false, want_w, want_h, next_tex_id_)) {
                    return;
                }

                FrameDelta& delta = scratch_delta_;
                delta.seq = 0;
                delta.dirty_rects.clear();

                const bool use_dirty = dirty_valid && st.has_frame && !st.needs_full_copy &&
                                       wgc_dirty_scratch_.size() <= MAX_REGION_COPIES;
                if (use_dirty) {
                    const IntRect bounds{.left = 0, .top = 0, .right = static_cast<long>(want_w), .bottom = static_cast<long>(want_h)};
                    try {
                        for (const RECT& r : wgc_dirty_scratch_) {
                            const IntRect clamped = int_rect_intersect(
                                IntRect{.left = r.left, .top = r.top, .right = r.right, .bottom = r.bottom}, bounds);
                            if (int_rect_empty(clamped)) continue;
                            copy_region(gfx, st.desktop_tex.get(), tex, clamped);
                            delta.dirty_rects.push_back(RECT{
                                .left = clamped.left + st.mon_rect.left,
                                .top = clamped.top + st.mon_rect.top,
                                .right = clamped.right + st.mon_rect.left,
                                .bottom = clamped.bottom + st.mon_rect.top,
                            });
                        }
                    } catch (...) {
                        delta.dirty_rects.clear();
                    }
                    delta.dirty_all = delta.dirty_rects.empty();
                    if (delta.dirty_all) {
                        usylibpp::ui::wgc::gpu_copy_clamped(gfx.d3d_context(), st.desktop_tex.get(), st.tex_w, st.tex_h,
                                                            0, 0, tex, content_sz, 0, 0, want_w, want_h);
                    }
                } else {
                    usylibpp::ui::wgc::gpu_copy_clamped(gfx.d3d_context(), st.desktop_tex.get(), st.tex_w, st.tex_h,
                                                        0, 0, tex, content_sz, 0, 0, want_w, want_h);
                    delta.dirty_all = true;
                }

                st.has_frame = true;
                st.needs_full_copy = false;
                ++st.frame_seq;
                delta.seq = st.frame_seq;
                st.push_history(delta);
            },
            wgc_dirty_scratch_, dirty_valid);

        return st.has_frame ? &st : nullptr;
    }

    /// Reads the frame's move/dirty rects into `copy_rects_` (texture space, clamped) and `delta.dirty_rects`
    /// (virtual-desktop space). Returns false when the frame carries no usable metadata.
    bool collect_frame_rects(const OutputState& st, UINT meta_bytes, FrameDelta& delta) {
        delta.dirty_rects.clear();
        copy_rects_.clear();
        if (meta_bytes == 0) return false;
        metadata_buf_.resize(meta_bytes);
        const IntRect nb = native_bounds(st);
        const RECT bounds{.left = nb.left, .top = nb.top, .right = nb.right, .bottom = nb.bottom};
        const auto add = [&](const RECT& r) {
            RECT c{};
            if (!IntersectRect(&c, &r, &bounds)) return;
            copy_rects_.push_back(c);
            delta.dirty_rects.push_back(tex_to_screen(st, c));
        };

        bool any_meta = false;
        UINT move_bytes = 0;
        const HRESULT move_hr = st.dupl->GetFrameMoveRects(
            meta_bytes, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_buf_.data()), &move_bytes);
        if (FAILED(move_hr)) return false;
        if (move_bytes > 0) {
            const std::span moves{reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metadata_buf_.data()),
                                  move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT)};
            for (const auto& m : moves) add(m.DestinationRect);
            any_meta = true;
        }
        UINT dirty_bytes = 0;
        const HRESULT dirty_hr = st.dupl->GetFrameDirtyRects(
            meta_bytes, reinterpret_cast<RECT*>(metadata_buf_.data()), &dirty_bytes);
        if (FAILED(dirty_hr)) return false;
        if (dirty_bytes > 0) {
            const std::span rects{reinterpret_cast<const RECT*>(metadata_buf_.data()), dirty_bytes / sizeof(RECT)};
            for (const RECT& r : rects) add(r);
            any_meta = true;
        }
        return any_meta;
    }

    /// Duplication metadata rects are in texture space; map them to virtual-desktop coordinates.
    [[nodiscard]] static RECT tex_to_screen(const OutputState& st, const RECT& r) noexcept {
        const IntRect d = tex_to_desktop(IntRect{.left = r.left, .top = r.top, .right = r.right, .bottom = r.bottom},
                                         st.rotation, static_cast<long>(st.tex_w), static_cast<long>(st.tex_h));
        return RECT{.left = d.left + st.mon_rect.left, .top = d.top + st.mon_rect.top,
                    .right = d.right + st.mon_rect.left, .bottom = d.bottom + st.mon_rect.top,};
    }

    [[nodiscard]] static DesktopRotation to_rotation(DXGI_MODE_ROTATION r) noexcept {
        switch (r) {
            case DXGI_MODE_ROTATION_ROTATE90: return DesktopRotation::Rot90;
            case DXGI_MODE_ROTATION_ROTATE180: return DesktopRotation::Rot180;
            case DXGI_MODE_ROTATION_ROTATE270: return DesktopRotation::Rot270;
            default: return DesktopRotation::Identity;
        }
    }

    OutputState* find_or_create(HMONITOR mon) {
        for (const auto& o : outputs_) {
            if (o->mon == mon) return o.get();
        }
        auto st = std::make_unique<OutputState>();
        st->mon = mon;
        outputs_.push_back(std::move(st));
        return outputs_.back().get();
    }

    bool ensure_conv_dc(Gfx& gfx) noexcept {
        if (conv_dc_) return true;
        if (!gfx.d2d_device()) return false;
        if (FAILED(gfx.d2d_device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &conv_dc_)) || !conv_dc_) {
            return false;
        }
        conv_dc_->SetDpi(96.0f, 96.0f);
        return true;
    }

    /// Draws the native-orientation SDR frame into the desktop-oriented texture, limited to the bounding box of
    /// `dirty` (virtual-desktop rects) when given.
    bool rotate_into_desktop(OutputState& st, const std::vector<RECT>* dirty) noexcept {
        if (!conv_dc_ || !st.native_bmp || !st.desktop_target_bmp) return false;
        std::optional<D2D1_RECT_F> clip;
        if (dirty) {
            if (dirty->empty()) return true;
            RECT bb = dirty->front();
            for (const RECT& r : *dirty) UnionRect(&bb, &bb, &r);
            clip = D2D1::RectF(static_cast<float>(bb.left - st.mon_rect.left), static_cast<float>(bb.top - st.mon_rect.top),
                               static_cast<float>(bb.right - st.mon_rect.left), static_cast<float>(bb.bottom - st.mon_rect.top));
        }
        const Affine2D m = tex_to_desktop_xform(st.rotation, static_cast<long>(st.tex_w), static_cast<long>(st.tex_h), 0.0f, 0.0f);
        const D2D1_SIZE_U native = st.native_bmp->GetPixelSize();
        const D2D1_RECT_F src = D2D1::RectF(0.0f, 0.0f, static_cast<float>(native.width), static_cast<float>(native.height));
        conv_dc_->SetTarget(st.desktop_target_bmp.get());
        conv_dc_->BeginDraw();
        conv_dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        if (clip) conv_dc_->PushAxisAlignedClip(*clip, D2D1_ANTIALIAS_MODE_ALIASED);
        conv_dc_->SetTransform(D2D1::Matrix3x2F(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy));
        conv_dc_->DrawBitmap(st.native_bmp.get(), &src, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, nullptr);
        conv_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (clip) conv_dc_->PopAxisAlignedClip();
        const HRESULT hr = conv_dc_->EndDraw();
        conv_dc_->SetTarget(nullptr);
        return SUCCEEDED(hr);
    }

    /// Converts a native HDR / 10-bit DXGI duplication frame (FP16 scRGB or R10G10B10A2) directly into
    /// `st.desktop_tex` (BGRA8) in one GPU pass, handling rotation, dirty rects, and drag cursor masking.
    bool convert_hdr_into_desktop(Gfx& gfx, OutputState& st, ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd,
                                  const std::vector<RECT>* dirty, const RECT* mask) noexcept {
        if (!ensure_conv_dc(gfx) || !st.desktop_target_bmp) return false;
        if (!hdr_fx_) {
            if (!HdrToSdrEffectImpl::register_once(gfx)) return false;
            if (FAILED(conv_dc_->CreateEffect(CLSID_UsyseekHdrToSdr, &hdr_fx_)) || !hdr_fx_) return false;
        }

        // Fast zero-copy path: wrap the acquired DXGI surface directly as a D2D bitmap.
        wil::com_ptr<ID2D1Bitmap1> transient_bmp;
        ID2D1Bitmap1* src_bmp = nullptr;
        if ((sd.BindFlags & static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE)) != 0u) {
            if (const auto surf = wil::try_com_query<IDXGISurface>(src)) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(sd.Format, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
                if (SUCCEEDED(conv_dc_->CreateBitmapFromDxgiSurface(surf.get(), &props, &transient_bmp)) && transient_bmp) {
                    src_bmp = transient_bmp.get();
                }
            }
        }
        // Fallback when a driver omits D3D11_BIND_SHADER_RESOURCE on acquired frames: copy to `native_tex`.
        if (!src_bmp) {
            if (!ensure_native_surface(gfx, st, sd.Width, sd.Height, sd.Format)) return false;
            gfx.d3d_context()->CopyResource(st.native_tex.get(), src);
            src_bmp = st.native_bmp.get();
        }
        if (!src_bmp) return false;

        const HdrToSdrParams params{
            .mode = (sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 0.0f : (st.color_info.is_hdr ? 1.0f : 2.0f),
            .sdr_white_scale = st.color_info.sdr_white_scale,
            .max_nits = st.color_info.max_luminance,
            .reserved = 0.0f,
        };
        if (FAILED(hdr_fx_->SetValue(0, reinterpret_cast<const BYTE*>(&params), sizeof(params)))) return false;

        hdr_fx_->SetInput(0, src_bmp);
        const auto detach = wil::scope_exit([&] { hdr_fx_->SetInput(0, nullptr); });

        const Affine2D m = tex_to_desktop_xform(st.rotation, static_cast<long>(st.tex_w), static_cast<long>(st.tex_h), 0.0f, 0.0f);
        const D2D1_MATRIX_3X2_F rot_xf = D2D1::Matrix3x2F(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy);
        const IntRect full_local{.left = 0, .top = 0, .right = static_cast<long>(st.tex_w), .bottom = static_cast<long>(st.tex_h)};
        const IntRect masked_local = mask ? st.tex_rect_for(*mask) : IntRect{};

        conv_dc_->SetTarget(st.desktop_target_bmp.get());
        conv_dc_->BeginDraw();
        conv_dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);

        const auto draw_piece = [&](const IntRect& piece) noexcept {
            if (int_rect_empty(piece)) return;
            conv_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
            const D2D1_RECT_F clip = D2D1::RectF(
                static_cast<float>(piece.left), static_cast<float>(piece.top),
                static_cast<float>(piece.right), static_cast<float>(piece.bottom));
            conv_dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            conv_dc_->SetTransform(rot_xf);
            conv_dc_->DrawImage(hdr_fx_.get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            conv_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
            conv_dc_->PopAxisAlignedClip();
        };

        if (!dirty && int_rect_empty(masked_local)) {
            conv_dc_->SetTransform(rot_xf);
            conv_dc_->DrawImage(hdr_fx_.get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            conv_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        } else if (!dirty) {
            std::array<IntRect, 4> pieces{};
            const size_t n = int_rect_subtract(full_local, masked_local, pieces);
            for (size_t i = 0; i < n; ++i) draw_piece(pieces[i]);
        } else {
            std::array<IntRect, 4> pieces{};
            for (const RECT& r : *dirty) {
                const IntRect local = st.tex_rect_for(r);
                if (int_rect_empty(local)) continue;
                const size_t n = int_rect_subtract(local, masked_local, pieces);
                for (size_t i = 0; i < n; ++i) draw_piece(pieces[i]);
            }
        }

        const HRESULT hr = conv_dc_->EndDraw();
        conv_dc_->SetTarget(nullptr);
        return SUCCEEDED(hr);
    }

    /// Releases a lost duplication/WGC session while retaining `desktop_tex` (if any) so glass windows keep their
    /// last valid backdrop across transient desktop switches (e.g. UAC Secure Desktop) instead of flashing.
    static void drop_duplication(OutputState& st) noexcept {
        st.dupl.reset();
        st.wgc_session.stop();
        st.backend = CaptureBackend::None;
        st.needs_full_copy = true;
        st.clear_history();
    }

    bool try_start_wgc(Gfx& gfx, OutputState& st) noexcept {
        if (!gfx.d3d() || !st.mon) return false;
        if (!ro_scope_) {
            ro_scope_.emplace(RO_INIT_SINGLETHREADED);
        }
        if (!ro_scope_->ok()) return false;
        if (!wgc_dev_.winrt_dev && !wgc_dev_.init(gfx.d3d(), false)) return false;

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(st.mon, &mi)) return false;

        if (!st.wgc_session.start(
                wgc_dev_.winrt_dev.get(),
                st.mon,
                /*include_cursor=*/false,
                /*num_buffers=*/2,
                /*min_update_interval_100ns=*/0,
                ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                /*report_dirty_regions=*/true)) {
            return false;
        }
        st.backend = CaptureBackend::Wgc;
        st.mon_rect = mi.rcMonitor;
        st.rotation = DesktopRotation::Identity; // WGC delivers pre-rotated desktop frames
        st.needs_full_copy = true;
        st.clear_history();
        return true;
    }

    bool ensure_duplication(Gfx& gfx, OutputState& st, ULONGLONG now_tick) noexcept {
        if (st.backend == CaptureBackend::Dxgi && st.dupl) return true;
        if (st.backend == CaptureBackend::Wgc && st.wgc_session.active()) return true;
        if (now_tick < st.retry_after_tick) return false;
        if (!detail::is_input_desktop_accessible()) {
            st.retry_after_tick = now_tick + 100;
            return false;
        }

        const auto try_adapter = [&](IDXGIAdapter* adapter) noexcept -> bool {
            if (!adapter) return false;
            static constexpr std::array<DXGI_FORMAT, 3> supported_formats = {
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R10G10B10A2_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM,
            };
            for (UINT i = 0;; ++i) {
                wil::com_ptr<IDXGIOutput> out;
                if (FAILED(adapter->EnumOutputs(i, &out)) || !out) break;
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(out->GetDesc(&desc)) && desc.Monitor == st.mon) {
                    wil::com_ptr<IDXGIOutputDuplication> dupl;
                    if (const auto out5 = out.try_query<IDXGIOutput5>()) {
                        (void)out5->DuplicateOutput1(gfx.d3d(), 0, static_cast<UINT>(supported_formats.size()),
                                                     supported_formats.data(), &dupl);
                    }
                    if (!dupl) {
                        if (const auto out1 = out.try_query<IDXGIOutput1>()) {
                            (void)out1->DuplicateOutput(gfx.d3d(), &dupl);
                        }
                    }
                    if (dupl) {
                        DXGI_OUTDUPL_DESC dd{};
                        dupl->GetDesc(&dd);
                        st.backend = CaptureBackend::Dxgi;
                        st.dupl = std::move(dupl);
                        st.mon_rect = desc.DesktopCoordinates;
                        st.rotation = to_rotation(dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED ? dd.Rotation : desc.Rotation);
                        st.color_info = usylibpp::ui::query_monitor_color_info(st.mon, out.get());
                        st.color_refresh_tick = now_tick + 2000u;
                        st.needs_full_copy = true;
                        st.clear_history();
                        return true;
                    }
                    break;
                }
            }
            return false;
        };

        // Tier 1: DXGI 1.5/1.6 Output Duplication (native HDR/WCG, lowest latency, dirty rects).
        if (const auto fresh_adapter = gfx.current_adapter()) {
            if (try_adapter(fresh_adapter.get())) return true;
        }
        wil::com_ptr<IDXGIAdapter> adapter;
        if (SUCCEEDED(gfx.dxgi_device()->GetAdapter(&adapter)) && try_adapter(adapter.get())) {
            return true;
        }

        // Tier 2: Shared Windows.Graphics.Capture fallback (cross-adapter / hybrid-GPU / duplicator slot exhaustion).
        if (try_start_wgc(gfx, st)) {
            return true;
        }

        // Tier 3 (GDI BitBlt) is handled by `LiquidGlassWindow::draw_desktop_glass` when polling returns false.
        st.retry_after_tick = now_tick + 250;
        return false;
    }

    bool ensure_desktop_surface(Gfx& gfx, OutputState& st, UINT want_w, UINT want_h, bool need_rt,
                                bool need_native_sdr, UINT native_w, UINT native_h, uint64_t& next_tex_id) noexcept {
        if (want_w == 0 || want_h == 0) return false;
        const bool rt_ok = need_rt ? (st.desktop_is_rt && st.desktop_target_bmp != nullptr) : !st.desktop_is_rt;
        const bool native_ok = need_native_sdr
            ? (st.native_tex && st.native_bmp && st.native_fmt == DXGI_FORMAT_B8G8R8A8_UNORM)
            : true;
        if (st.desktop_tex && st.tex_w == want_w && st.tex_h == want_h && rt_ok && native_ok) {
            return true;
        }

        st.native_bmp.reset();
        st.desktop_target_bmp.reset();
        st.native_tex.reset();
        st.native_fmt = DXGI_FORMAT_UNKNOWN;
        st.desktop_tex.reset();
        st.desktop_is_rt = false;
        st.tex_id = 0;
        st.tex_w = st.tex_h = 0;
        st.has_frame = false; // a fresh texture is empty until this frame lands in it

        D3D11_TEXTURE2D_DESC td{};
        td.Width = want_w;
        td.Height = want_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE) |
                       (need_rt ? static_cast<UINT>(D3D11_BIND_RENDER_TARGET) : 0u);
        if (FAILED(gfx.d3d()->CreateTexture2D(&td, nullptr, &st.desktop_tex))) return false;
        st.desktop_is_rt = need_rt;

        if (need_rt) {
            if (!ensure_conv_dc(gfx)) {
                st.desktop_tex.reset();
                st.desktop_is_rt = false;
                return false;
            }
            const auto desk_surf = st.desktop_tex.try_query<IDXGISurface>();
            const auto target_props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            if (!desk_surf || FAILED(conv_dc_->CreateBitmapFromDxgiSurface(desk_surf.get(), &target_props, &st.desktop_target_bmp))) {
                st.desktop_target_bmp.reset();
                st.desktop_tex.reset();
                st.desktop_is_rt = false;
                return false;
            }
        }

        if (need_native_sdr && !ensure_native_surface(gfx, st, native_w, native_h, DXGI_FORMAT_B8G8R8A8_UNORM)) {
            st.native_bmp.reset();
            st.desktop_target_bmp.reset();
            st.native_tex.reset();
            st.native_fmt = DXGI_FORMAT_UNKNOWN;
            st.desktop_tex.reset();
            st.desktop_is_rt = false;
            return false;
        }

        st.tex_id = ++next_tex_id;
        st.tex_w = want_w;
        st.tex_h = want_h;
        return true;
    }

    bool ensure_native_surface(Gfx& gfx, OutputState& st, UINT w, UINT h, DXGI_FORMAT fmt) noexcept {
        if (!ensure_conv_dc(gfx)) return false;
        if (st.native_tex && st.native_bmp && st.native_fmt == fmt) {
            D3D11_TEXTURE2D_DESC cur{};
            st.native_tex->GetDesc(&cur);
            if (cur.Width == w && cur.Height == h) return true;
        }
        st.native_bmp.reset();
        st.native_tex.reset();
        st.native_fmt = DXGI_FORMAT_UNKNOWN;

        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = w;
        nd.Height = h;
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Format = fmt;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gfx.d3d()->CreateTexture2D(&nd, nullptr, &st.native_tex))) return false;
        const auto native_surf = st.native_tex.try_query<IDXGISurface>();
        if (!native_surf) {
            st.native_tex.reset();
            return false;
        }
        const auto native_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(fmt, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(conv_dc_->CreateBitmapFromDxgiSurface(native_surf.get(), &native_props, &st.native_bmp))) {
            st.native_bmp.reset();
            st.native_tex.reset();
            return false;
        }
        st.native_fmt = fmt;
        return true;
    }

    // Destruction order matters: `outputs_` (WGC sessions) -> `wgc_dev_` -> `ro_scope_` (RoUninitialize).
    std::optional<usylibpp::ui::wgc::RoScope> ro_scope_;
    usylibpp::ui::wgc::WgcDevice wgc_dev_;
    std::vector<std::unique_ptr<OutputState>> outputs_; // stable addresses: windows hold pointers across a frame
    std::vector<HMONITOR> mons_scratch_;
    std::vector<RECT> copy_rects_;        // texture-space regions to copy for the frame being acquired
    std::vector<RECT> wgc_dirty_scratch_; // monitor-local dirty rects accumulated from Win11 24H2+ WGC frames
    FrameDelta scratch_delta_;            // swapped with the evicted history slot, so rect storage is recycled
    int drag_depth_ = 0;
    HCURSOR cursor_handle_ = nullptr;
    POINT cursor_hotspot_{};
    SIZE cursor_size_{};
    RECT last_cursor_{};
    bool has_last_cursor_ = false;
    wil::com_ptr<ID2D1DeviceContext> conv_dc_; // lazily created only for rotated or HDR outputs
    wil::com_ptr<ID2D1Effect> hdr_fx_;         // lazily created only for HDR/10-bit outputs
    uint64_t epoch_ = 0;
    uint64_t next_tex_id_ = 0;
    std::vector<uint8_t> metadata_buf_;
};

/// Per-window Liquid Glass rendering helper (manages blur + shader effects, backdrop copy, and in-app scene snapshot).
class LiquidGlassWindow {
public:
    LiquidGlassWindow() = default;
    LiquidGlassWindow(const LiquidGlassWindow&) = delete;
    LiquidGlassWindow& operator=(const LiquidGlassWindow&) = delete;
    LiquidGlassWindow(LiquidGlassWindow&&) = delete;
    LiquidGlassWindow& operator=(LiquidGlassWindow&&) = delete;
    ~LiquidGlassWindow() { release(); }

    void release() noexcept {
        detach_fx_inputs(desktop_fx_);
        for (auto& fx : lens_pool_) detach_fx_inputs(fx);
        views_.clear();
        outs_.clear();
        std::vector<uint8_t>{}.swap(black_px_);
        win_bmp_[0].reset();
        win_bmp_[1].reset();
        win_bmp_idx_ = 0;
        cached_glass_bmp_.reset();
        cached_glass_valid_ = false;
        scene_bmp_.reset();
        scene_prev_target_.reset();
        scene_saved_clip_.reset();
        desktop_fx_ = {};
        lens_pool_.clear();
        lens_cursor_ = 0;
        last_probe_valid_ = false;
        last_probe_origin_ = {.x = LONG_MIN, .y = LONG_MIN};
        last_probe_pw_ = last_probe_ph_ = 0;
        backdrop_ver_ = 0;
        gdi_origin_ = {.x = LONG_MIN, .y = LONG_MIN};
        gdi_pw_ = gdi_ph_ = 0;
        last_rendered_origin_ = {.x = LONG_MIN, .y = LONG_MIN};
        origin_override_.reset();
        seeded_gdi_ = false;
        scene_active_ = false;
        scene_ready_ = false;
        end_drag();
        sizemove_ = false;
        win_bmp_w_ = win_bmp_h_ = 0;
        cached_glass_w_ = cached_glass_h_ = 0;
        scene_w_ = scene_h_ = 0;
        epoch_ = 0;
        scene_epoch_ = 0;
        win_bmp_epoch_ = 0;
        cached_glass_epoch_ = 0;
    }

    /// Drops monitor views and cached desktop glass without disrupting reusable effects or an active drag.
    void invalidate_backdrop() noexcept {
        outs_.clear();
        views_.clear();
        cached_glass_valid_ = false;
        last_probe_valid_ = false;
        seeded_gdi_ = false;
        gdi_pw_ = gdi_ph_ = 0;
        ++backdrop_ver_;
    }

    void set_origin_override(std::optional<POINT> pt) noexcept { origin_override_ = pt; }
    /// Enters/leaves a window move-size loop; while in one the shared duplicator masks the cursor out of captures.
    void set_sizemove(bool on, DesktopDuplicator& dupl, Gfx& gfx) {
        if (on == sizemove_) return;
        sizemove_ = on;
        if (on) {
            dupl.begin_drag(gfx);
            drag_dupl_ = &dupl;
        } else {
            end_drag();
        }
    }
    [[nodiscard]] POINT last_rendered_origin() const noexcept { return last_rendered_origin_; }
    [[nodiscard]] bool has_cached_desktop_glass() const noexcept { return cached_glass_valid_ && cached_glass_bmp_ != nullptr; }

    bool poll_desktop(DesktopDuplicator& dupl, Gfx& gfx, HWND hwnd) {
        if (!hwnd) return false;
        RECT crc{};
        if (!GetClientRect(hwnd, &crc) || crc.right <= crc.left || crc.bottom <= crc.top) return false;
        const UINT pw = static_cast<UINT>(crc.right - crc.left);
        const UINT ph = static_cast<UINT>(crc.bottom - crc.top);
        POINT origin{.x = 0, .y = 0};
        if (!ClientToScreen(hwnd, &origin)) return false;
        const auto clear_outs = wil::scope_exit([&] { outs_.clear(); });
        const auto [ok, changed] = sync_dupl_state(dupl, gfx, hwnd, pw, ph, origin, !sizemove_);
        return ok && changed;
    }

    /// Draws the live monitor desktop behind `hwnd` through the Liquid Glass shader across `panel_rect`.
    /// Returns false if Desktop Duplication or shader compilation is unavailable.
    bool draw_desktop_glass(DesktopDuplicator& dupl, Canvas& cv, WindowTarget& target, Gfx& gfx, HWND hwnd,
                            D2D1_RECT_F panel_rect, const GlassStyle& style) {
        if (!target.ready() || !hwnd) return false;
        const auto clear_outs = wil::scope_exit([&] { outs_.clear(); });

        const UINT pw = target.pixel_width();
        const UINT ph = target.pixel_height();
        if (pw == 0 || ph == 0) return false;

        POINT origin{.x = 0, .y = 0};
        if (origin_override_) {
            origin = *origin_override_;
        } else if (!ClientToScreen(hwnd, &origin)) {
            return false;
        }
        last_rendered_origin_ = origin;

        const bool allow_probe = !sizemove_ && !origin_override_ && last_probe_valid_ &&
                                 last_probe_origin_.x == origin.x && last_probe_origin_.y == origin.y &&
                                 last_probe_pw_ == pw && last_probe_ph_ == ph;
        const auto [use_dupl, _] = sync_dupl_state(dupl, gfx, hwnd, pw, ph, origin, allow_probe);

        GlassStyle s = style;
        s.force_opaque = true;

        const D2D1_SIZE_F sz = target.size();
        const D2D1_RECT_F full_dest = D2D1::RectF(0.0f, 0.0f, sz.width, sz.height);

        ID2D1Bitmap1* gdi_bmp = nullptr;
        if (!use_dupl) {
            if (detail::is_input_desktop_accessible()) {
                gdi_bmp = capture_gdi_backdrop(target, origin, pw, ph);
            }
            if (!gdi_bmp) {
                if (cached_glass_valid_ && cached_glass_bmp_ &&
                    cached_key_.pw == pw && cached_key_.ph == ph &&
                    cached_key_.dpi == target.dpi() && cached_key_.epoch == target.target_epoch()) {
                    cv.dc()->DrawBitmap(cached_glass_bmp_.get(), &full_dest, 1.0f,
                                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &full_dest, nullptr);
                    return true;
                }
                return false;
            }
        }

        const GlassCacheKey key = make_cache_key(pw, ph, target.dpi(), target.target_epoch(), origin, panel_rect, s, use_dupl, backdrop_ver_);
        if (cached_glass_valid_ && cached_glass_bmp_ && cached_key_ == key) {
            cv.dc()->DrawBitmap(cached_glass_bmp_.get(), &full_dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &full_dest, nullptr);
            return true;
        }

        ID2D1Bitmap1* src_bmp = nullptr;
        if (use_dupl) {
            src_bmp = composite_outputs(gfx, target, origin, pw, ph);
            if (!src_bmp) return false;
        } else {
            src_bmp = gdi_bmp;
        }

        if (!ensure_cached_glass_bmp(target, pw, ph)) {
            if (!ensure_effects(target, gfx)) return false;
            return draw_bitmap_glass_with(
                cv, target, src_bmp, full_dest,
                panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), s, desktop_fx_);
        }

        wil::com_ptr<ID2D1Image> prev_target;
        cv.dc()->GetTarget(&prev_target);
        D2D1_MATRIX_3X2_F prev_xf{};
        cv.dc()->GetTransform(&prev_xf);
        // When `scene_active_` is true, `begin_scene` has already popped `cv.dirty_clip()` off `dc`.
        const auto dc_clip = scene_active_ ? std::nullopt : cv.dirty_clip();
        if (dc_clip) cv.dc()->PopAxisAlignedClip();
        bool ok = false;
        {
            const auto restore_dc = wil::scope_exit([&] {
                detach_fx_inputs(desktop_fx_);
                cv.dc()->SetTarget(prev_target.get());
                cv.dc()->SetTransform(prev_xf);
                if (dc_clip) {
                    cv.dc()->PushAxisAlignedClip(*dc_clip, D2D1_ANTIALIAS_MODE_ALIASED);
                }
            });
            cv.dc()->SetTransform(D2D1::Matrix3x2F::Identity());
            cv.dc()->SetTarget(cached_glass_bmp_.get());
            cv.dc()->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            ok = ensure_effects(target, gfx) && draw_bitmap_glass_with(
                cv, target, src_bmp, full_dest,
                panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), s, desktop_fx_);
            // Materialize effect draws into `cached_glass_bmp_` before switching targets and reading it as an effect input.
            if (FAILED(cv.dc()->Flush())) ok = false;
        }
        if (!ok) {
            cached_glass_valid_ = false;
            return false;
        }

        cached_key_ = key;
        cached_glass_valid_ = true;
        cv.dc()->DrawBitmap(cached_glass_bmp_.get(), &full_dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &full_dest, nullptr);
        return true;
    }

    /// Redirects `target.dc()` into an offscreen `scene_bmp_` so floating glass panels
    /// can sample and refract the rendered scene underneath them without copying.
    bool begin_scene(Canvas& cv, WindowTarget& target) noexcept {
        scene_active_ = false;
        scene_ready_ = false;
        scene_prev_target_.reset();
        scene_saved_clip_.reset();
        if (!target.ready() || !target.target_bitmap()) return false;
        const UINT pw = target.pixel_width();
        const UINT ph = target.pixel_height();
        if (pw == 0 || ph == 0) return false;

        const bool same_env = scene_bmp_ && scene_dpi_ == target.dpi() && scene_epoch_ == target.target_epoch();
        const auto [want_w, want_h] = desired_bitmap_size(scene_w_, scene_h_, pw, ph, same_env);
        if (!same_env || scene_w_ != want_w || scene_h_ != want_h) {
            scene_bmp_.reset();
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                target.dpi(), target.dpi());
            if (FAILED(target.dc()->CreateBitmap(D2D1::SizeU(want_w, want_h), nullptr, 0, &props, &scene_bmp_))) {
                scene_w_ = scene_h_ = 0;
                return false;
            }
            scene_w_ = want_w;
            scene_h_ = want_h;
            scene_dpi_ = target.dpi();
            scene_epoch_ = target.target_epoch();
        }
        ID2D1DeviceContext6* dc = target.dc();
        dc->GetTarget(&scene_prev_target_);
        scene_saved_clip_ = cv.dirty_clip();
        if (scene_saved_clip_) dc->PopAxisAlignedClip();
        dc->SetTarget(scene_bmp_.get());
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        scene_active_ = true;
        return true;
    }

    /// Restores `target.target_bitmap()` and blits `scene_bmp_` onto it as the base layer.
    void end_scene(WindowTarget& target) noexcept {
        if (!scene_active_) return;
        scene_active_ = false;
        const wil::com_ptr<ID2D1Image> prev_target = std::move(scene_prev_target_);
        const std::optional<D2D1_RECT_F> saved_clip = std::exchange(scene_saved_clip_, std::nullopt);
        if (!target.ready() || !target.target_bitmap() || !scene_bmp_) return;
        ID2D1DeviceContext6* dc = target.dc();
        // Flush effect draws targeting `scene_bmp_` before downstream `draw_scene_glass` effects bind it as input.
        const bool flush_ok = SUCCEEDED(dc->Flush());
        dc->SetTarget(prev_target ? prev_target.get() : target.target_bitmap());
        if (saved_clip) dc->PushAxisAlignedClip(*saved_clip, D2D1_ANTIALIAS_MODE_ALIASED);
        scene_ready_ = flush_ok;
        const D2D1_SIZE_F sz = target.size();
        const D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, sz.width, sz.height);
        dc->DrawBitmap(scene_bmp_.get(), &full, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &full);
    }

    void end_frame() noexcept {
        detach_fx_inputs(desktop_fx_);
        const size_t used = std::min(lens_cursor_, lens_pool_.size());
        for (size_t i = 0; i < used; ++i) detach_fx_inputs(lens_pool_[i]);
        lens_cursor_ = 0;
        outs_.clear();
        scene_ready_ = false;
    }

    /// Draws a floating Liquid Glass panel refracting the captured in-app scene (`scene_bmp_`).
    bool draw_scene_glass(Canvas& cv, WindowTarget& target, Gfx& gfx,
                          D2D1_RECT_F panel_rect, const GlassStyle& style) {
        if (!scene_ready_ || scene_active_ || !scene_bmp_ || style.opacity <= 0.002f) return false;
        if (!cv.intersects_clip(panel_rect, 3.0f)) return true;
        const D2D1_SIZE_F sz = target.size();
        return draw_bitmap_glass_pooled(
            cv, target, gfx, scene_bmp_.get(), D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
            panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), style);
    }

    /// Draws a Liquid Glass control lens refracting the cached desktop glass backdrop (`cached_glass_bmp_`).
    bool draw_cached_glass(Canvas& cv, WindowTarget& target, Gfx& gfx,
                           D2D1_RECT_F panel_rect, const GlassStyle& style) {
        if (!cached_glass_valid_ || !cached_glass_bmp_ || style.opacity <= 0.002f) return false;
        if (!cv.intersects_clip(panel_rect, 3.0f)) return true;
        const D2D1_SIZE_F sz = target.size();
        return draw_bitmap_glass_pooled(
            cv, target, gfx, cached_glass_bmp_.get(), D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
            panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), style);
    }

private:
    void end_drag() noexcept {
        if (drag_dupl_) drag_dupl_->end_drag();
        drag_dupl_ = nullptr;
    }

    struct GlassCacheKey {
        UINT pw = 0;
        UINT ph = 0;
        float dpi = 0.0f;
        uint64_t epoch = 0;
        LONG origin_x = LONG_MIN;
        LONG origin_y = LONG_MIN;
        D2D1_RECT_F panel_rect{};
        float blur_std_dev = 0.0f;
        float corner_radius = 0.0f;
        float roundness = 0.0f;
        float margin_dip = 0.0f;
        float ref_thickness = 0.0f;
        float ref_factor = 0.0f;
        float ref_dispersion = 0.0f;
        float dispersion_range = 0.0f;
        float fresnel_range = 0.0f;
        float fresnel_hardness = 0.0f;
        float fresnel_factor = 0.0f;
        float magnification = 0.0f;
        float glare_range = 0.0f;
        float glare_hardness = 0.0f;
        float glare_factor = 0.0f;
        float glare_convergence = 0.0f;
        float glare_opposite_factor = 0.0f;
        float glare_angle_deg = 0.0f;
        D2D1_COLOR_F tint{};
        bool force_opaque = false;
        float opacity = 1.0f;
        bool centered_edge = false;
        float rim_inset_dip = 0.0f;
        bool use_dupl = false;
        uint64_t backdrop_ver = 0;

        [[nodiscard]] bool operator==(const GlassCacheKey& o) const noexcept {
            return pw == o.pw && ph == o.ph && dpi == o.dpi && epoch == o.epoch &&
                   origin_x == o.origin_x && origin_y == o.origin_y &&
                   panel_rect.left == o.panel_rect.left && panel_rect.top == o.panel_rect.top &&
                   panel_rect.right == o.panel_rect.right && panel_rect.bottom == o.panel_rect.bottom &&
                   blur_std_dev == o.blur_std_dev && corner_radius == o.corner_radius &&
                   roundness == o.roundness && margin_dip == o.margin_dip &&
                   ref_thickness == o.ref_thickness && ref_factor == o.ref_factor &&
                   ref_dispersion == o.ref_dispersion && dispersion_range == o.dispersion_range &&
                   fresnel_range == o.fresnel_range && fresnel_hardness == o.fresnel_hardness &&
                   fresnel_factor == o.fresnel_factor && magnification == o.magnification &&
                   glare_range == o.glare_range && glare_hardness == o.glare_hardness &&
                   glare_factor == o.glare_factor && glare_convergence == o.glare_convergence &&
                   glare_opposite_factor == o.glare_opposite_factor && glare_angle_deg == o.glare_angle_deg &&
                   tint.r == o.tint.r && tint.g == o.tint.g && tint.b == o.tint.b && tint.a == o.tint.a &&
                   force_opaque == o.force_opaque && opacity == o.opacity &&
                   centered_edge == o.centered_edge && rim_inset_dip == o.rim_inset_dip &&
                   use_dupl == o.use_dupl && backdrop_ver == o.backdrop_ver;
        }
    };

    [[nodiscard]] static GlassCacheKey make_cache_key(UINT pw, UINT ph, float dpi, uint64_t epoch, POINT origin,
                                                      D2D1_RECT_F panel_rect, const GlassStyle& s,
                                                      bool use_dupl, uint64_t backdrop_ver) noexcept {
        return GlassCacheKey{
            .pw = pw,
            .ph = ph,
            .dpi = dpi,
            .epoch = epoch,
            .origin_x = origin.x,
            .origin_y = origin.y,
            .panel_rect = panel_rect,
            .blur_std_dev = s.blur_std_dev,
            .corner_radius = s.corner_radius,
            .roundness = s.roundness,
            .margin_dip = s.margin_dip,
            .ref_thickness = s.ref_thickness,
            .ref_factor = s.ref_factor,
            .ref_dispersion = s.ref_dispersion,
            .dispersion_range = s.dispersion_range,
            .fresnel_range = s.fresnel_range,
            .fresnel_hardness = s.fresnel_hardness,
            .fresnel_factor = s.fresnel_factor,
            .magnification = s.magnification,
            .glare_range = s.glare_range,
            .glare_hardness = s.glare_hardness,
            .glare_factor = s.glare_factor,
            .glare_convergence = s.glare_convergence,
            .glare_opposite_factor = s.glare_opposite_factor,
            .glare_angle_deg = s.glare_angle_deg,
            .tint = s.tint,
            .force_opaque = s.force_opaque,
            .opacity = s.opacity,
            .centered_edge = s.centered_edge,
            .rim_inset_dip = s.rim_inset_dip,
            .use_dupl = use_dupl,
            .backdrop_ver = backdrop_ver,
        };
    }

    /// What this window last saw of one duplicated output, plus its self-repaint probe baseline.
    struct OutputView {
        HMONITOR mon = nullptr;
        uint64_t seen_tex_id = 0;
        uint64_t seen_seq = 0;
        wil::com_ptr<ID2D1Bitmap1> bmp; // D2D view of the output's desktop texture
        uint64_t bmp_tex_id = 0;
        uint64_t bmp_epoch = 0;
        wil::com_ptr<ID3D11Texture2D> probe_tex;
        UINT probe_tex_w = 0, probe_tex_h = 0;
        uint64_t probe_epoch = 0;
        std::vector<uint8_t> probe_px;
        IntRect probe_rc{};
        uint64_t probe_src_tex = 0;
        uint64_t probe_seq = 0;
        bool probe_valid = false;
    };

    [[nodiscard]] static RECT screen_rect(POINT origin, UINT pw, UINT ph) noexcept {
        return RECT{.left = origin.x, .top = origin.y, .right = origin.x + static_cast<LONG>(pw), .bottom = origin.y + static_cast<LONG>(ph)};
    }

    /// Polls every output under the window into `outs_` (with `views_` kept parallel to it).
    /// Returns {all outputs available, backdrop changed}.
    std::pair<bool, bool> sync_dupl_state(DesktopDuplicator& dupl, Gfx& gfx, HWND hwnd, UINT pw, UINT ph, POINT origin, bool allow_probe) {
        const RECT rc = screen_rect(origin, pw, ph);
        if (!dupl.poll_rect(gfx, rc, GetTickCount64(), outs_)) {
            outs_.clear();
            return {false, false};
        }

        // Re-align views with outputs (keeping state for monitors still covered) only when the monitor list changed.
        bool aligned = views_.size() == outs_.size();
        for (size_t i = 0; aligned && i < outs_.size(); ++i) aligned = views_[i].mon == outs_[i]->mon;
        bool set_changed = !aligned;
        if (aligned) {
            for (size_t i = 0; i < outs_.size(); ++i) {
                if (views_[i].seen_tex_id != outs_[i]->tex_id) set_changed = true;
            }
        } else {
            views_scratch_.clear();
            for (const auto* st : outs_) {
                const auto it = std::ranges::find(views_, st->mon, &OutputView::mon);
                if (it == views_.end()) {
                    views_scratch_.emplace_back().mon = st->mon;
                } else {
                    views_scratch_.push_back(std::move(*it));
                }
            }
            views_.swap(views_scratch_);
            views_scratch_.clear();
        }

        bool any_new_frame = set_changed;
        for (size_t i = 0; i < outs_.size() && !any_new_frame; ++i) {
            if (views_[i].seen_seq != outs_[i]->frame_seq) any_new_frame = true;
        }
        if (!any_new_frame) return {true, false};

        bool need_check = set_changed;
        if (!need_check) {
            RECT win_rc = rc;
            RECT full_win_rc{};
            if (GetWindowRect(hwnd, &full_win_rc)) UnionRect(&win_rc, &win_rc, &full_win_rc);
            for (size_t i = 0; i < outs_.size() && !need_check; ++i) {
                need_check = output_touched(*outs_[i], views_[i].seen_seq, win_rc);
            }
        }

        for (size_t i = 0; i < outs_.size(); ++i) {
            views_[i].seen_tex_id = outs_[i]->tex_id;
            views_[i].seen_seq = outs_[i]->frame_seq;
        }
        if (!need_check) return {true, false};

        if (allow_probe && !probe_outputs(gfx, origin, pw, ph) && !set_changed) {
            return {true, false};
        }
        ++backdrop_ver_;
        return {true, true};
    }

    /// True when frames newer than `seen_seq` may have changed pixels inside `win_rc`.
    [[nodiscard]] static bool output_touched(const DesktopDuplicator::OutputState& st, uint64_t seen_seq, const RECT& win_rc) noexcept {
        if (st.frame_seq == seen_seq) return false;
        if (seen_seq == 0 || st.frame_seq < seen_seq || st.history_size() == 0 || st.history_at(0).seq > seen_seq + 1) return true;
        for (size_t h = 0; h < st.history_size(); ++h) {
            const auto& delta = st.history_at(h);
            if (delta.seq <= seen_seq) continue;
            if (delta.dirty_all) return true;
            for (const RECT& r : delta.dirty_rects) {
                RECT inter{};
                if (IntersectRect(&inter, &r, &win_rc)) return true;
            }
        }
        return false;
    }

    /// Composites every output under the window into the next `win_bmp_` (black where no monitor is).
    ID2D1Bitmap1* composite_outputs(Gfx& gfx, WindowTarget& target, POINT origin, UINT pw, UINT ph) {
        if (outs_.empty() || outs_.size() != views_.size()) return nullptr;
        const RECT rc = screen_rect(origin, pw, ph);

        uint64_t covered = 0;
        for (size_t i = 0; i < outs_.size(); ++i) {
            const auto& st = *outs_[i];
            OutputView& v = views_[i];
            if (!st.desktop_tex) return nullptr;
            if (!v.bmp || v.bmp_tex_id != st.tex_id || v.bmp_epoch != target.target_epoch()) {
                v.bmp.reset();
                v.bmp_tex_id = 0;
                const auto surf = st.desktop_tex.try_query<IDXGISurface>();
                if (!surf) return nullptr;
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                    target.dpi(), target.dpi());
                if (FAILED(target.dc()->CreateBitmapFromDxgiSurface(surf.get(), &props, &v.bmp))) return nullptr;
                v.bmp_tex_id = st.tex_id;
                v.bmp_epoch = target.target_epoch();
            }
            const IntRect tr = st.tex_rect_for(rc);
            if (!int_rect_empty(tr)) {
                covered += static_cast<uint64_t>(tr.right - tr.left) * static_cast<uint64_t>(tr.bottom - tr.top);
            }
        }
        if (covered == 0) return nullptr;

        if (!ensure_win_bmp(target, pw, ph)) return nullptr;
        win_bmp_idx_ ^= 1u;
        seeded_gdi_ = false;
        ID2D1Bitmap1* dst_bmp = win_bmp_[win_bmp_idx_].get();

        // Parts of the window off every monitor would otherwise keep whatever this buffer held two frames ago.
        if (covered < static_cast<uint64_t>(pw) * ph) {
            constexpr UINT STRIP_ROWS = 64u;
            const UINT strip_h = std::min(ph, STRIP_ROWS);
            const size_t need_bytes = static_cast<size_t>(pw) * strip_h * 4u;
            if (black_px_.size() < need_bytes) black_px_.resize(need_bytes, 0u);
            for (UINT y = 0; y < ph; y += strip_h) {
                const UINT h = std::min(strip_h, ph - y);
                const D2D1_RECT_U band{.left = 0, .top = y, .right = pw, .bottom = y + h};
                if (FAILED(dst_bmp->CopyFromMemory(&band, black_px_.data(), pw * 4u))) return nullptr;
            }
        }

        for (size_t i = 0; i < outs_.size(); ++i) {
            const auto& st = *outs_[i];
            const IntRect tr = st.tex_rect_for(rc);
            if (int_rect_empty(tr)) continue;
            const D2D1_POINT_2U dst_pt{
                .x = static_cast<UINT32>(static_cast<int64_t>(st.mon_rect.left) + tr.left - origin.x),
                .y = static_cast<UINT32>(static_cast<int64_t>(st.mon_rect.top) + tr.top - origin.y),
            };
            const D2D1_RECT_U src_rc{
                .left = static_cast<UINT32>(tr.left),
                .top = static_cast<UINT32>(tr.top),
                .right = static_cast<UINT32>(tr.right),
                .bottom = static_cast<UINT32>(tr.bottom),
            };
            if (FAILED(dst_bmp->CopyFromBitmap(&dst_pt, views_[i].bmp.get(), &src_rc))) return nullptr;
        }

        if (!sizemove_ && !origin_override_) {
            for (size_t i = 0; i < outs_.size(); ++i) {
                if (views_[i].probe_seq != outs_[i]->frame_seq) {
                    (void)probe_outputs(gfx, origin, pw, ph);
                    break;
                }
            }
        }
        return dst_bmp;
    }

    /// Reads back the window's footprint on every output and compares it with the last readback, so repaints of
    /// this (capture-excluded) window that still show up as dirty rects don't trigger endless re-renders.
    bool probe_outputs(Gfx& gfx, POINT origin, UINT pw, UINT ph) {
        const RECT rc = screen_rect(origin, pw, ph);
        bool changed = false;
        for (size_t i = 0; i < outs_.size() && i < views_.size(); ++i) {
            if (probe_output(gfx, views_[i], *outs_[i], rc)) changed = true;
        }
        last_probe_valid_ = true;
        last_probe_origin_ = origin;
        last_probe_pw_ = pw;
        last_probe_ph_ = ph;
        return changed;
    }

    static bool probe_output(Gfx& gfx, OutputView& v, const DesktopDuplicator::OutputState& st, const RECT& rc) {
        if (!st.desktop_tex || !gfx.d3d() || !gfx.d3d_context()) return true;

        const IntRect tr = st.tex_rect_for(rc);
        if (int_rect_empty(tr)) {
            v.probe_valid = false;
            v.probe_seq = st.frame_seq;
            return false;
        }

        const UINT cw = static_cast<UINT>(tr.right - tr.left);
        const UINT ch = static_cast<UINT>(tr.bottom - tr.top);
        if (!v.probe_tex || v.probe_tex_w != cw || v.probe_tex_h != ch || v.probe_epoch != gfx.epoch()) {
            v.probe_tex.reset();
            v.probe_valid = false;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = cw;
            td.Height = ch;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(gfx.d3d()->CreateTexture2D(&td, nullptr, &v.probe_tex))) return true;
            v.probe_tex_w = cw;
            v.probe_tex_h = ch;
            v.probe_epoch = gfx.epoch();
        }

        const D3D11_BOX src_box{
            .left = static_cast<UINT>(tr.left),
            .top = static_cast<UINT>(tr.top),
            .front = 0,
            .right = static_cast<UINT>(tr.right),
            .bottom = static_cast<UINT>(tr.bottom),
            .back = 1,
        };
        gfx.d3d_context()->CopySubresourceRegion(v.probe_tex.get(), 0, 0, 0, 0, st.desktop_tex.get(), 0, &src_box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(gfx.d3d_context()->Map(v.probe_tex.get(), 0, D3D11_MAP_READ, 0, &mapped))) return true;
        const auto unmap = wil::scope_exit([&] { gfx.d3d_context()->Unmap(v.probe_tex.get(), 0); });

        const size_t row_bytes = static_cast<size_t>(cw) * 4;
        if (!mapped.pData || mapped.RowPitch < row_bytes) return true;
        const bool same_meta = v.probe_valid && v.probe_rc == tr && v.probe_src_tex == st.tex_id &&
                               v.probe_px.size() == row_bytes * ch;

        const auto* src_ptr = static_cast<const uint8_t*>(mapped.pData);
        bool changed = !same_meta;
        UINT first_diff_y = 0;
        if (!changed) {
            for (UINT y = 0; y < ch; ++y) {
                if (std::memcmp(src_ptr + (static_cast<size_t>(y) * mapped.RowPitch),
                                v.probe_px.data() + (static_cast<size_t>(y) * row_bytes), row_bytes) != 0) {
                    changed = true;
                    first_diff_y = y;
                    break;
                }
            }
        }
        if (changed) {
            if (!same_meta) v.probe_px.resize(row_bytes * ch);
            for (UINT y = first_diff_y; y < ch; ++y) {
                std::memcpy(v.probe_px.data() + (static_cast<size_t>(y) * row_bytes),
                            src_ptr + (static_cast<size_t>(y) * mapped.RowPitch), row_bytes);
            }
            v.probe_valid = true;
            v.probe_rc = tr;
            v.probe_src_tex = st.tex_id;
        }
        v.probe_seq = st.frame_seq;
        return changed;
    }

    [[nodiscard]] static UINT quantize_extent(UINT v, UINT step = 256u) noexcept {
        return std::max(step, ((v + step - 1u) / step) * step);
    }

    [[nodiscard]] std::pair<UINT, UINT> desired_bitmap_size(UINT cur_w, UINT cur_h, UINT pw, UINT ph, bool same_env) const noexcept {
        if (sizemove_) {
            if (same_env && cur_w >= pw && cur_h >= ph) return {cur_w, cur_h};
            return {
                (same_env && cur_w >= pw) ? cur_w : quantize_extent(pw),
                (same_env && cur_h >= ph) ? cur_h : quantize_extent(ph),
            };
        }
        return {pw, ph};
    }

    bool ensure_cached_glass_bmp(WindowTarget& target, UINT pw, UINT ph) noexcept {
        if (pw == 0 || ph == 0) return false;
        const bool same_env = cached_glass_bmp_ && cached_glass_dpi_ == target.dpi() && cached_glass_epoch_ == target.target_epoch();
        const auto [want_w, want_h] = desired_bitmap_size(cached_glass_w_, cached_glass_h_, pw, ph, same_env);
        if (same_env && cached_glass_w_ == want_w && cached_glass_h_ == want_h) {
            return true;
        }
        cached_glass_bmp_.reset();
        cached_glass_valid_ = false;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            target.dpi(), target.dpi());
        if (FAILED(target.dc()->CreateBitmap(D2D1::SizeU(want_w, want_h), nullptr, 0, &props, &cached_glass_bmp_))) {
            cached_glass_w_ = cached_glass_h_ = 0;
            return false;
        }
        cached_glass_w_ = want_w;
        cached_glass_h_ = want_h;
        cached_glass_dpi_ = target.dpi();
        cached_glass_epoch_ = target.target_epoch();
        return true;
    }

    bool ensure_win_bmp(WindowTarget& target, UINT pw, UINT ph) noexcept {
        if (pw == 0 || ph == 0) return false;
        const bool same_env = win_bmp_[0] && win_bmp_[1] && win_bmp_dpi_ == target.dpi() && win_bmp_epoch_ == target.target_epoch();
        const auto [want_w, want_h] = desired_bitmap_size(win_bmp_w_, win_bmp_h_, pw, ph, same_env);
        if (same_env && win_bmp_w_ == want_w && win_bmp_h_ == want_h) {
            return true;
        }
        win_bmp_[0].reset();
        win_bmp_[1].reset();
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            target.dpi(), target.dpi());
        if (FAILED(target.dc()->CreateBitmap(D2D1::SizeU(want_w, want_h), nullptr, 0, &props, &win_bmp_[0])) ||
            FAILED(target.dc()->CreateBitmap(D2D1::SizeU(want_w, want_h), nullptr, 0, &props, &win_bmp_[1]))) {
            win_bmp_[0].reset();
            win_bmp_[1].reset();
            win_bmp_w_ = win_bmp_h_ = 0;
            return false;
        }
        win_bmp_w_ = want_w;
        win_bmp_h_ = want_h;
        win_bmp_dpi_ = target.dpi();
        win_bmp_epoch_ = target.target_epoch();
        seeded_gdi_ = false;
        gdi_pw_ = gdi_ph_ = 0;
        return true;
    }

    ID2D1Bitmap1* capture_gdi_backdrop(WindowTarget& target, POINT origin, UINT pw, UINT ph) noexcept {
        if (!ensure_win_bmp(target, pw, ph)) return nullptr;
        if (!seeded_gdi_ || origin.x != gdi_origin_.x || origin.y != gdi_origin_.y || pw > gdi_pw_ || ph > gdi_ph_) {
            const uint32_t next_idx = win_bmp_idx_ ^ 1u;
            if (!seed_from_screen_dc(win_bmp_[next_idx].get(), origin, pw, ph)) return nullptr;
            win_bmp_idx_ = next_idx;
            seeded_gdi_ = true;
            gdi_origin_ = origin;
            gdi_pw_ = pw;
            gdi_ph_ = ph;
            ++backdrop_ver_;
        }
        return seeded_gdi_ ? win_bmp_[win_bmp_idx_].get() : nullptr;
    }

    static bool seed_from_screen_dc(ID2D1Bitmap1* dst_bmp, POINT origin, UINT pw, UINT ph) noexcept {
        if (!dst_bmp || pw == 0 || ph == 0 ||
            pw > static_cast<UINT>(INT_MAX / 4) || ph > static_cast<UINT>(INT_MAX)) {
            return false;
        }
        HDC screen_dc = GetDC(nullptr);
        if (!screen_dc) return false;
        const auto release_screen = wil::scope_exit([&] { ReleaseDC(nullptr, screen_dc); });
        HDC mem_dc = CreateCompatibleDC(screen_dc);
        if (!mem_dc) return false;
        const auto release_mem = wil::scope_exit([&] { DeleteDC(mem_dc); });

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(pw);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(ph);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            return false;
        }
        const auto release_dib = wil::scope_exit([&] { DeleteObject(dib); });
        HGDIOBJ prev = SelectObject(mem_dc, dib);
        if (!prev || prev == HGDI_ERROR) return false;
        const BOOL ok = BitBlt(mem_dc, 0, 0, static_cast<int>(pw), static_cast<int>(ph),
                               screen_dc, origin.x, origin.y, SRCCOPY);
        SelectObject(mem_dc, prev);
        if (!ok) return false;
        const D2D1_RECT_U dst_rc{.left = 0, .top = 0, .right = pw, .bottom = ph};
        return SUCCEEDED(dst_bmp->CopyFromMemory(&dst_rc, bits, pw * 4));
    }

    struct LensFx {
        wil::com_ptr<ID2D1Effect> crop_in;
        wil::com_ptr<ID2D1Effect> border;
        wil::com_ptr<ID2D1Effect> crop_pad;
        wil::com_ptr<ID2D1Effect> blur;
        wil::com_ptr<ID2D1Effect> glass;
    };

    static void detach_fx_inputs(LensFx& fx) noexcept {
        if (fx.crop_in) fx.crop_in->SetInput(0, nullptr);
        if (fx.blur) fx.blur->SetInput(0, nullptr);
        if (fx.glass) fx.glass->SetInput(0, nullptr);
    }

    bool ensure_effects(WindowTarget& target, Gfx& gfx) {
        if (desktop_fx_.blur && desktop_fx_.glass && epoch_ == target.target_epoch()) return true;
        detach_fx_inputs(desktop_fx_);
        for (auto& fx : lens_pool_) detach_fx_inputs(fx);
        desktop_fx_ = {};
        lens_pool_.clear();
        lens_cursor_ = 0;
        if (!target.ready() || !gfx.ready()) return false;
        if (!LiquidGlassEffectImpl::register_once(gfx)) return false;
        if (FAILED(target.dc()->CreateEffect(CLSID_D2D1GaussianBlur, &desktop_fx_.blur))) return false;
        if (FAILED(target.dc()->CreateEffect(CLSID_UsyseekLiquidGlass, &desktop_fx_.glass))) {
            desktop_fx_ = {};
            return false;
        }
        epoch_ = target.target_epoch();
        return true;
    }

    bool draw_bitmap_glass_pooled(Canvas& cv, WindowTarget& target, Gfx& gfx, ID2D1Bitmap1* src_bmp,
                                  D2D1_RECT_F src_bounds, D2D1_RECT_F panel_rect, D2D1_POINT_2F target_offset,
                                  const GlassStyle& style) {
        if (!src_bmp || width(panel_rect) <= 2.0f || height(panel_rect) <= 2.0f) return false;
        if (!ensure_effects(target, gfx)) return false;

        if (lens_cursor_ >= lens_pool_.size()) {
            LensFx fx;
            if (FAILED(target.dc()->CreateEffect(CLSID_UsyseekLiquidGlass, &fx.glass))) return false;
            lens_pool_.push_back(std::move(fx));
        }
        LensFx& slot = lens_pool_[lens_cursor_];
        if (style.blur_std_dev > 0.05f && !slot.blur) {
            if (FAILED(target.dc()->CreateEffect(CLSID_D2D1GaussianBlur, &slot.blur))) return false;
        }
        ++lens_cursor_;
        return draw_bitmap_glass_with(cv, target, src_bmp, src_bounds, panel_rect, target_offset, style, slot);
    }

    static bool draw_bitmap_glass_with(Canvas& cv, WindowTarget& target, ID2D1Bitmap1* src_bmp,
                                       D2D1_RECT_F src_bounds, D2D1_RECT_F panel_rect, D2D1_POINT_2F target_offset,
                                       const GlassStyle& style, LensFx& fx) {
        if (!src_bmp || !fx.glass || width(panel_rect) <= 2.0f || height(panel_rect) <= 2.0f) return false;

        const LiquidGlassShaderParams params{
            .panel_rect = {panel_rect.left, panel_rect.top, width(panel_rect), height(panel_rect)},
            .src_bounds = {src_bounds.left, src_bounds.top, src_bounds.right, src_bounds.bottom},
            .tint_color = {style.tint.r, style.tint.g, style.tint.b, style.tint.a},
            .refract_params = {style.ref_thickness, style.ref_factor, style.ref_dispersion, style.dispersion_range},
            .fresnel_params = {style.fresnel_range, style.fresnel_hardness, style.fresnel_factor, style.magnification},
            .glare_params = {style.glare_range, style.glare_hardness, style.glare_factor, style.glare_angle_deg},
            .shape_params = {style.glare_convergence, style.glare_opposite_factor, style.corner_radius, style.roundness},
            .misc_params = {target.scale(), style.force_opaque ? 1.0f : 0.0f, 1.5f, style.margin_dip},
            .extra_params = {std::clamp(style.opacity, 0.0f, 1.0f), style.centered_edge ? 1.0f : 0.0f,
                             std::max(style.rim_inset_dip, 0.0f), 0.0f,},
        };

        if (fx.blur && style.blur_std_dev > 0.05f) {
            const D2D1_SIZE_U bmp_px = src_bmp->GetPixelSize();
            const bool oversized = (bmp_px.width != target.pixel_width() || bmp_px.height != target.pixel_height());
            bool clamped_ok = false;
            if (oversized) {
                if (!fx.crop_in) (void)target.dc()->CreateEffect(CLSID_D2D1Crop, &fx.crop_in);
                if (!fx.border) (void)target.dc()->CreateEffect(CLSID_D2D1Border, &fx.border);
                if (!fx.crop_pad) (void)target.dc()->CreateEffect(CLSID_D2D1Crop, &fx.crop_pad);
                if (fx.crop_in && fx.border && fx.crop_pad) {
                    // Clamp oversized pooled bitmaps to active window bounds before blurring so stale/blank headroom never bleeds in.
                    const float blur_pad = std::ceil(style.blur_std_dev * 3.0f) + 2.0f;
                    fx.crop_in->SetInput(0, nullptr);
                    fx.crop_in->SetInput(0, src_bmp);
                    fx.crop_in->SetValue(D2D1_CROP_PROP_RECT, src_bounds);
                    fx.border->SetInputEffect(0, fx.crop_in.get());
                    fx.border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
                    fx.border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
                    fx.crop_pad->SetInputEffect(0, fx.border.get());
                    fx.crop_pad->SetValue(D2D1_CROP_PROP_RECT, inset(src_bounds, -blur_pad, -blur_pad));
                    fx.blur->SetInput(0, nullptr);
                    fx.blur->SetInputEffect(0, fx.crop_pad.get());
                    clamped_ok = true;
                }
            }
            if (!clamped_ok) {
                if (fx.crop_in) fx.crop_in->SetInput(0, nullptr);
                fx.blur->SetInput(0, nullptr);
                fx.blur->SetInput(0, src_bmp);
            }
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.0f, style.blur_std_dev));
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            fx.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            fx.glass->SetInput(0, nullptr);
            fx.glass->SetInputEffect(0, fx.blur.get());
        } else {
            if (fx.crop_in) fx.crop_in->SetInput(0, nullptr);
            if (fx.blur) fx.blur->SetInput(0, nullptr);
            fx.glass->SetInput(0, nullptr);
            fx.glass->SetInput(0, src_bmp);
        }
        if (FAILED(fx.glass->SetValue(0, reinterpret_cast<const BYTE*>(&params), sizeof(params)))) return false;

        const float pad = (style.margin_dip < 1.5f && !style.force_opaque) ? 2.5f : 0.0f;
        const D2D1_RECT_F draw_rect = (pad > 0.0f) ? inset(panel_rect, -pad, -pad) : panel_rect;
        const D2D1_POINT_2F draw_offset{.x = target_offset.x - pad, .y = target_offset.y - pad};
        cv.dc()->DrawImage(
            fx.glass.get(),
            draw_offset,
            draw_rect,
            D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_OVER);
        return true;
    }

    std::vector<const DesktopDuplicator::OutputState*> outs_; // outputs under the window, valid for this frame
    std::vector<OutputView> views_;                         // parallel to `outs_`
    std::vector<OutputView> views_scratch_;
    std::vector<uint8_t> black_px_;
    std::array<wil::com_ptr<ID2D1Bitmap1>, 2> win_bmp_;
    uint32_t win_bmp_idx_ = 0;
    wil::com_ptr<ID2D1Bitmap1> cached_glass_bmp_;
    GlassCacheKey cached_key_{};
    bool cached_glass_valid_ = false;
    UINT cached_glass_w_ = 0, cached_glass_h_ = 0;
    float cached_glass_dpi_ = 0.0f;
    uint64_t cached_glass_epoch_ = 0;
    wil::com_ptr<ID2D1Bitmap1> scene_bmp_;
    wil::com_ptr<ID2D1Image> scene_prev_target_;
    std::optional<D2D1_RECT_F> scene_saved_clip_;
    LensFx desktop_fx_;
    std::vector<LensFx> lens_pool_;
    size_t lens_cursor_ = 0;
    bool last_probe_valid_ = false;
    POINT last_probe_origin_{.x = LONG_MIN, .y = LONG_MIN};
    UINT last_probe_pw_ = 0, last_probe_ph_ = 0;
    uint64_t backdrop_ver_ = 0;
    UINT win_bmp_w_ = 0, win_bmp_h_ = 0;
    float win_bmp_dpi_ = 0.0f;
    UINT scene_w_ = 0, scene_h_ = 0;
    float scene_dpi_ = 0.0f;
    uint64_t epoch_ = 0;
    uint64_t scene_epoch_ = 0;
    uint64_t win_bmp_epoch_ = 0;
    POINT gdi_origin_{.x = LONG_MIN, .y = LONG_MIN};
    UINT gdi_pw_ = 0, gdi_ph_ = 0;
    POINT last_rendered_origin_{.x = LONG_MIN, .y = LONG_MIN};
    std::optional<POINT> origin_override_;
    bool seeded_gdi_ = false;
    bool scene_active_ = false;
    bool scene_ready_ = false;
    bool sizemove_ = false;
    DesktopDuplicator* drag_dupl_ = nullptr; // set while this window holds a duplicator drag
};

[[nodiscard]] inline GlassStyle make_desktop_glass_style(const Theme& t, float corner_radius, bool fullscreen = false) noexcept {
    GlassStyle s;
    s.blur_std_dev = t.glass.blur;
    s.corner_radius = fullscreen ? 0.0f : corner_radius;
    s.roundness = t.pill_radius.has_value() ? 2.0f : t.glass.roundness;
    s.margin_dip = fullscreen ? 0.0f : 0.5f;
    s.ref_thickness = fullscreen ? 14.0f : 18.0f;
    s.ref_factor = t.glass.refraction;
    s.ref_dispersion = t.glass.dispersion;
    s.dispersion_range = 0.70f;
    s.fresnel_range = 28.0f;
    s.fresnel_hardness = 12.0f;
    s.fresnel_factor = (t.dark_mode ? 34.0f : 44.0f) * t.glass.glare_scale;
    s.magnification = 1.035f;
    s.glare_range = 26.0f;
    s.glare_hardness = 18.0f;
    s.glare_factor = (t.dark_mode ? 70.0f : 88.0f) * t.glass.glare_scale;
    s.glare_convergence = 58.0f;
    s.glare_opposite_factor = 72.0f;
    s.glare_angle_deg = t.glass.glare_angle;
    s.tint = with_alpha(t.bg, t.pill_bg_alpha);
    return s;
}

[[nodiscard]] inline GlassStyle make_overlay_glass_style(const Theme& t, float corner_radius, float blur_std = 1.8f) noexcept {
    GlassStyle s;
    s.blur_std_dev = std::clamp(blur_std * (t.glass.blur / 1.4f), 0.0f, 8.0f);
    s.corner_radius = corner_radius;
    // Overlay popups draw circular D2D shadows and borders (`cv.shadow` / `cv.stroke_round`),
    // so the glass SDF must also use circular corners (`roundness = 2.0`) to stay aligned.
    s.roundness = 2.0f;
    s.margin_dip = 0.5f;
    s.ref_thickness = 16.0f;
    s.ref_factor = t.glass.refraction;
    s.ref_dispersion = t.glass.dispersion * (4.5f / 5.5f);
    s.dispersion_range = 0.65f;
    s.fresnel_range = 26.0f;
    s.fresnel_hardness = 12.0f;
    s.fresnel_factor = (t.dark_mode ? 36.0f : 46.0f) * t.glass.glare_scale;
    s.magnification = 1.045f;
    s.glare_range = 26.0f;
    s.glare_hardness = 18.0f;
    s.glare_factor = (t.dark_mode ? 74.0f : 92.0f) * t.glass.glare_scale;
    s.glare_convergence = 60.0f;
    s.glare_opposite_factor = 74.0f;
    s.glare_angle_deg = t.glass.glare_angle;
    s.tint = with_alpha(t.surface_high, t.dark_mode ? 0.28f : 0.36f);
    return s;
}

[[nodiscard]] inline GlassStyle make_control_glass_style(const Theme& t, float corner_radius, D2D1_COLOR_F tint,
                                                        float opacity = 1.0f, float blur_std = 0.0f,
                                                        float thickness = 8.0f, float mag = 1.06f) noexcept {
    GlassStyle s;
    s.blur_std_dev = blur_std;
    s.corner_radius = corner_radius;
    s.roundness = 2.0f;
    s.margin_dip = 0.0f;
    s.ref_thickness = thickness;
    s.ref_factor = 1.0f + ((t.glass.refraction - 1.0f) * (0.48f / 0.56f));
    s.ref_dispersion = t.glass.dispersion * (3.0f / 5.5f);
    s.dispersion_range = 0.65f;
    s.fresnel_range = 13.0f;
    s.fresnel_hardness = 0.0f;
    s.fresnel_factor = (t.dark_mode ? 24.0f : 32.0f) * t.glass.glare_scale;
    s.magnification = mag;
    s.glare_range = 12.0f;
    s.glare_hardness = 0.0f;
    s.glare_factor = (t.dark_mode ? 42.0f : 56.0f) * t.glass.glare_scale;
    s.glare_convergence = 75.0f;
    s.glare_opposite_factor = 48.0f;
    s.glare_angle_deg = t.glass.glare_angle;
    s.tint = tint;
    s.opacity = opacity;
    s.centered_edge = true;
    return s;
}

/// Builds a Liquid Glass style tuned for compact floating tooltips.
[[nodiscard]] inline GlassStyle make_tooltip_glass_style(const Theme& t, float corner_radius, float opacity = 1.0f) noexcept {
    GlassStyle s = make_overlay_glass_style(t, corner_radius, 1.6f);
    s.ref_thickness = 10.0f;
    s.tint = with_alpha(t.surface_high, t.dark_mode ? 0.36f : 0.48f);
    s.opacity = opacity;
    // Tooltips are often ~27 DIPs tall, so inset the rim slope slightly to avoid pulling distant colours into an edge line.
    s.rim_inset_dip = 1.0f;
    return s;
}

/// Draws a floating wrapped tooltip anchored near `anchor`, rendering its surface as live Liquid Glass when active.
inline void draw_tooltip(Canvas& cv, LiquidGlassWindow& glass, WindowTarget& target, Gfx& gfx,
                         D2D1_RECT_F bounds, D2D1_RECT_F anchor, std::wstring_view title,
                         std::wstring_view subtitle = {}, float opacity = 1.0f, float bounds_radius = 0.0f) {
    detail::draw_tooltip_impl(cv, bounds, anchor, title, subtitle, opacity, bounds_radius,
                              [&](D2D1_RECT_F r, const Theme& t, float rad, float op) {
                                  if (!glass.draw_scene_glass(cv, target, gfx, r, make_tooltip_glass_style(t, rad, op))) {
                                      return false;
                                  }
                                  cv.stroke_round(r, rad, with_alpha(t.outline_variant, 0.55f));
                                  return true;
                              });
}

/// Draws a floating wrapped tooltip anchored near `anchor` within `win_sz`, rendering as live Liquid Glass when active.
inline void draw_tooltip(Canvas& cv, LiquidGlassWindow& glass, WindowTarget& target, Gfx& gfx,
                         D2D1_SIZE_F win_sz, D2D1_RECT_F anchor, std::wstring_view title,
                         std::wstring_view subtitle = {}, float opacity = 1.0f) {
    draw_tooltip(cv, glass, target, gfx, D2D1::RectF(0.0f, 0.0f, win_sz.width, win_sz.height), anchor, title, subtitle, opacity, 0.0f);
}

} // namespace usyseek::ui
