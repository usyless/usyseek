#pragma once

#include "canvas.hpp"

#include <d2d1effectauthor.h>
#include <d2d1effecthelpers.h>
#include <d2d1effects.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace usyseek::ui {

// {6A1E8D20-4C45-4F9A-9D82-2F1E37C0A811}
inline constexpr GUID CLSID_UsyseekLiquidGlass = {
    0x6a1e8d20, 0x4c45, 0x4f9a, {0x9d, 0x82, 0x2f, 0x1e, 0x37, 0xc0, 0xa8, 0x11}};

// {A3F42B91-8D13-4E2F-8A10-914D2C77E502}
inline constexpr GUID GUID_UsyseekLiquidGlassPS = {
    0xa3f42b91, 0x8d13, 0x4e2f, {0x8a, 0x10, 0x91, 0x4d, 0x2c, 0x77, 0xe5, 0x02}};

/// Constant buffer matching `cbuffer LiquidGlassParams : register(b0)` in the D2D shader.
struct alignas(16) LiquidGlassShaderParams {
    float panel_rect[4]{};     // left, top, width, height (DIPs)
    float src_bounds[4]{};     // minX, minY, maxX, maxY (DIPs)
    float tint_color[4]{};     // r, g, b, a
    float refract_params[4]{}; // refThickness, refFactor, refDispersion, dispersionRange
    float fresnel_params[4]{}; // fresnelRange, fresnelHardness, fresnelFactor, magnification
    float glare_params[4]{};   // glareRange, glareHardness, glareFactor, glareAngleDeg
    float shape_params[4]{};   // glareConvergence, glareOppositeFactor, cornerRadiusDip, roundnessN
    float misc_params[4]{};    // dpr, forceOpaque, edgeAaPx, shapeMarginDip
};
static_assert(sizeof(LiquidGlassShaderParams) == 128);

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
};

namespace detail {

// HLSL shader implementing the LiquidGlassWinUI material algorithms (superellipse SDF,
// analytical gradient normal, Snell's-law refraction, 7-tap spectral dispersion,
// Fresnel rim, and directional specular glare) in Direct2D DIP/scene coordinates.
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
};

float superellipseCornerSDF(float2 p, float r, float n)
{
    p = abs(p);
    return pow(pow(p.x, n) + pow(p.y, n), 1.0 / n) - r;
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
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

float2 RoundedRectUnitNormal(float2 fragCoord, float2 center, float2 halfPx, float cr, float n)
{
    float2 p = fragCoord - center;
    cr = min(cr, min(halfPx.x, halfPx.y));
    float2 sgn = sign(p);
    float2 ap = abs(p);
    float2 inner = max(halfPx - cr, 0.0);

    // Smoothly round the inner reference box with radius k so the normal field is C1 continuous
    // everywhere inside the panel, completely eliminating the 45-degree ridge bisector at the corners.
    float k = min(max(cr * 1.25, 18.0), min(inner.x, inner.y));
    float2 innerSm = max(inner - k, 0.0);
    float2 q = max(ap - innerSm, 0.0);
    float2 g = pow(q, float2(n - 1.0, n - 1.0));
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

float4 SampleAtDip(float2 targetDip, float4 scenePos, float4 uv0, float dpr, float forceOpaque)
{
    float2 pad = 0.5 / max(dpr, 1.0);
    float2 lo = srcBounds.xy + pad;
    float2 hi = max(lo, srcBounds.zw - pad);
    float2 clampedDip = clamp(targetDip, lo, hi);
    float2 sampleUv = uv0.xy + uv0.zw * (clampedDip - scenePos.xy);
    float4 c = InputTexture0.Sample(InputSampler0, sampleUv);
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
    float xR = 1.0 - depthPx / thicknessPx;
    float thetaI = asin(pow(saturate(xR), 2.0));
    float thetaT = asin(clamp((1.0 / max(ior, 1.001)) * sin(thetaI), -0.999, 0.999));
    return -tan(thetaT - thetaI);
}

float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float dpr = max(miscParams.x, 1.0);
    float forceOpaque = miscParams.y;
    float2 res = max(panelRect.zw * dpr, float2(2.0, 2.0));
    float2 fragCoord = (scenePos.xy - panelRect.xy) * dpr;
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
    float glassThresholdPx = max(2.5, aaWidthPx * 2.0);

    float4 outColor = float4(0.0, 0.0, 0.0, 0.0);
    if (distPx < glassThresholdPx)
    {
        float refThickness = max(refractParams.x * dpr, 1.0);
        float refFactor = max(refractParams.y, 1.001);
        float refDispersion = refractParams.z;
        float dispersionRange = max(refractParams.w, 0.001);

        float nmerged = -distPx;
        float edgeFactor = EdgeSlopeFactor(nmerged, refThickness, refFactor);
        float ef1 = EdgeSlopeFactor(nmerged + 1.0, refThickness, refFactor);

        float2 unitN = RoundedRectUnitNormal(fragCoord, center, halfPx, cr, n);
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
        float2 zoomedDip = panelCenterDip + (scenePos.xy - panelCenterDip) / mag;

        float3 accRgb = 0.0;
        float accA = 0.0;
        [unroll]
        for (int ti = 0; ti < 8; ti++)
        {
            float f = ((float)ti / 7.0) - 0.5;
            float4 tap = SampleAtDip(zoomedDip + offDip + offSpreadDip * f, scenePos, uv0, dpr, forceOpaque);
            accRgb += tap.rgb;
            accA += tap.a;
        }
        float4 blurredPixel = float4(accRgb / 8.0, accA / 8.0);

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

            float4 sRed    = SampleAtDip(baseDip + dispDip,               scenePos, uv0, dpr, forceOpaque);
            float4 sOrange = SampleAtDip(baseDip + dispDip * (2.0 / 3.0), scenePos, uv0, dpr, forceOpaque);
            float4 sYellow = SampleAtDip(baseDip + dispDip * (1.0 / 3.0), scenePos, uv0, dpr, forceOpaque);
            float4 sCyan   = SampleAtDip(baseDip - dispDip * (1.0 / 3.0), scenePos, uv0, dpr, forceOpaque);
            float4 sBlue   = SampleAtDip(baseDip - dispDip * (2.0 / 3.0), scenePos, uv0, dpr, forceOpaque);
            float4 sPurple = SampleAtDip(baseDip - dispDip,               scenePos, uv0, dpr, forceOpaque);

            float3 disp;
            disp.r = (sRed.r + sOrange.r + sYellow.r) / 3.5 + sPurple.r / 7.0;
            disp.g = sOrange.g / 7.0 + (sYellow.g + blurredPixel.g + sCyan.g) / 3.5;
            disp.b = (blurredPixel.b + sCyan.b + sBlue.b + sPurple.b) / 4.0;
            blurredPixel.rgb = disp;
        }

        if (blurredPixel.a > 0.001)
        {
            blurredPixel.rgb /= blurredPixel.a;
        }
        else
        {
            blurredPixel.rgb = float3(0.0, 0.0, 0.0);
        }

        float3 tintRgb = tintColor.rgb;
        float tintA = tintColor.a;
        outColor = lerp(blurredPixel, float4(tintRgb, 1.0), tintA * 0.85);

        // Fresnel rim lighting
        {
            float fresnelRange = max(fresnelParams.x, 0.0001);
            float fresnelHardness = fresnelParams.y / 100.0;
            float fresnelFactorN = fresnelParams.z / 100.0;
            float fresnelK = min(pow(500.0 / fresnelRange, 2.0), 3000.0 * dpr);
            float ffBase = 1.0 + merged * res.y / (1500.0 * dpr) * fresnelK + fresnelHardness;
            float fresnelFactor = clamp(pow(max(ffBase, 0.0), 5.0), 0.0, 1.0);
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

            float glareK = min(pow(500.0 / glareRange, 2.0), 3000.0 * dpr);
            float glareGeoFactor = clamp(pow(max(1.0 + merged * res.y / (1500.0 * dpr) * glareK + glareHardness, 0.0), 5.0), 0.0, 1.0);

            float nAngle = atan2(safeN.y, safeN.x);
            if (nAngle < 0.0) nAngle += 2.0 * PI;
            float glareAngle = (nAngle - PI / 4.0 + glareAngleNorm) * 2.0;

            float glareFarside = ((glareAngle > 1.5 * PI && glareAngle < 3.5 * PI) || (glareAngle < -0.5 * PI)) ? 1.0 : 0.0;
            float glareAngleFactor = (0.5 + sin(glareAngle) * 0.5) * (glareFarside > 0.5 ? 1.2 * glareOppositeN : 1.2) * glareFactorN;
            glareAngleFactor = clamp(pow(max(glareAngleFactor, 0.0), 0.1 + glareConvergenceN * 2.0), 0.0, 1.0);

            float g = glareAngleFactor * glareGeoFactor;
            float3 glareBase = lerp(blurredPixel.rgb, tintRgb, tintA * 0.5);
            float3 glareColor = lerp(glareBase, float3(1.0, 1.0, 1.0), clamp(g, 0.0, 1.0));
            float glareCoverage = smoothstep(0.0, 2.0, glareParams.x * dpr);
            outColor = lerp(outColor, float4(glareColor, 1.0), saturate(g) * glareCoverage);
        }
    }

    float alpha = 1.0 - saturate(distPx / aaWidthPx);
    return float4(outColor.rgb * alpha, alpha);
}
)hlsl";

inline wil::com_ptr<ID3DBlob> compile_liquid_glass_ps() {
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
    if (FAILED(hr)) {
        if (err) {
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        }
        return nullptr;
    }
    return code;
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

} // namespace detail

/// Native Direct2D custom effect implementation running the Liquid Glass pixel shader.
class LiquidGlassEffectImpl final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static bool register_once(ID2D1Factory1* factory) {
        if (!factory) return false;
        static std::mutex mtx;
        static std::vector<ID2D1Factory1*> registered;
        std::scoped_lock lk{mtx};
        if (std::ranges::find(registered, factory) != registered.end()) return true;

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

        const D2D1_PROPERTY_BINDING bindings[] = {
            D2D1_BLOB_TYPE_BINDING(L"Params", &LiquidGlassEffectImpl::set_params, &LiquidGlassEffectImpl::get_params),
        };
        const HRESULT hr = factory->RegisterEffectFromString(
            CLSID_UsyseekLiquidGlass,
            xml,
            bindings,
            static_cast<UINT32>(std::size(bindings)),
            &LiquidGlassEffectImpl::create_effect);
        if (SUCCEEDED(hr)) {
            registered.push_back(factory);
            return true;
        }
        return false;
    }

    static HRESULT CALLBACK create_effect(IUnknown** out) {
        if (!out) return E_POINTER;
        auto* p = new (std::nothrow) LiquidGlassEffectImpl();
        if (!p) return E_OUTOFMEMORY;
        *out = static_cast<ID2D1EffectImpl*>(p);
        return S_OK;
    }

    HRESULT set_params(const BYTE* data, UINT32 size) {
        if (!data || size != sizeof(params_)) return E_INVALIDARG;
        std::memcpy(&params_, data, sizeof(params_));
        return S_OK;
    }

    HRESULT get_params(BYTE* data, UINT32 size, UINT32* actual) const {
        if (actual) *actual = sizeof(params_);
        if (!data) return S_OK;
        if (size < sizeof(params_)) return E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(data, &params_, sizeof(params_));
        return S_OK;
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
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

    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (r == 0) delete this;
        return r;
    }

    // ID2D1EffectImpl
    IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) override {
        if (!ctx || !graph) return E_INVALIDARG;
        if (!ctx->IsShaderLoaded(GUID_UsyseekLiquidGlassPS)) {
            static const wil::com_ptr<ID3DBlob> ps = detail::compile_liquid_glass_ps();
            if (!ps) return E_FAIL;
            const HRESULT hr = ctx->LoadPixelShader(
                GUID_UsyseekLiquidGlassPS,
                static_cast<const BYTE*>(ps->GetBufferPointer()),
                static_cast<UINT32>(ps->GetBufferSize()));
            if (FAILED(hr)) return hr;
        }
        return graph->SetSingleTransformNode(this);
    }

    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) noexcept override {
        if (!draw_info_) return E_UNEXPECTED;
        return draw_info_->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&params_), sizeof(params_));
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
        input_rects[0] = detail::expand_rect_safe(*output_rect, 72);
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
        *invalid_output = detail::expand_rect_safe(invalid_input, 72);
        return S_OK;
    }

    // ID2D1TransformNode
    IFACEMETHODIMP_(UINT32) GetInputCount() const noexcept override { return 1; }

private:
    LiquidGlassEffectImpl() = default;
    ~LiquidGlassEffectImpl() = default;

    volatile long refs_ = 1;
    wil::com_ptr<ID2D1DrawInfo> draw_info_;
    LiquidGlassShaderParams params_{};
};

/// Captures the live monitor desktop on the GPU via DXGI Output Duplication.
/// Shared across all windows on the same D3D11 device so `DuplicateOutput` is only
/// acquired once per monitor (DXGI rejects a second duplication on the same device & output).
class DesktopDuplicator {
public:
    struct OutputState {
        HMONITOR mon = nullptr;
        RECT mon_rect{};
        wil::com_ptr<IDXGIOutputDuplication> dupl;
        wil::com_ptr<ID3D11Texture2D> desktop_tex;
        uint64_t tex_id = 0;
        UINT tex_w = 0;
        UINT tex_h = 0;
        bool has_frame = false;
        uint64_t frame_seq = 0;
        bool dirty_all = false;
        std::vector<RECT> dirty_rects;
        ULONGLONG retry_after_tick = 0;
    };

    void reset() {
        outputs_.clear();
        epoch_ = 0;
    }

    /// Polls DXGI Output Duplication without blocking (or with a brief wait on initial acquire).
    /// Returns the monitor state when valid (or nullptr if duplication is currently unavailable).
    const OutputState* poll_monitor(Gfx& gfx, HWND hwnd, ULONGLONG now_tick) {
        if (!gfx.ready() || !hwnd) return nullptr;
        if (epoch_ != gfx.epoch()) {
            outputs_.clear();
            epoch_ = gfx.epoch();
        }

        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        OutputState* st = find_or_create(mon);
        if (!ensure_duplication(gfx, *st, now_tick)) return st->has_frame ? st : nullptr;

        for (int attempt = 0; attempt < 4; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            wil::com_ptr<IDXGIResource> res;
            const UINT timeout_ms = st->has_frame ? 0u : 25u;
            const HRESULT hr = st->dupl->AcquireNextFrame(timeout_ms, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) break;
            if (FAILED(hr)) {
                if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                    reset_output(*st);
                    st->retry_after_tick = now_tick + 400;
                }
                return st->has_frame ? st : nullptr;
            }

            const auto release_frame = wil::scope_exit([&] { st->dupl->ReleaseFrame(); });
            // The initial DXGI handshake frame (and pointer-only updates) have LastPresentTime == 0: skip them.
            if (info.LastPresentTime.QuadPart == 0) continue;

            st->dirty_rects.clear();
            st->dirty_all = !st->has_frame;
            const UINT dirty_bytes = info.TotalMetadataBufferSize;
            if (dirty_bytes > 0) {
                metadata_buf_.resize(dirty_bytes);
                UINT needed = 0;
                if (SUCCEEDED(st->dupl->GetFrameDirtyRects(dirty_bytes, reinterpret_cast<RECT*>(metadata_buf_.data()), &needed)) && needed > 0) {
                    const UINT n = needed / sizeof(RECT);
                    const auto* rects = reinterpret_cast<const RECT*>(metadata_buf_.data());
                    st->dirty_rects.reserve(n);
                    for (UINT i = 0; i < n; ++i) {
                        RECT r = rects[i];
                        OffsetRect(&r, st->mon_rect.left, st->mon_rect.top);
                        st->dirty_rects.push_back(r);
                    }
                } else {
                    st->dirty_all = true;
                }
            } else {
                st->dirty_all = true;
            }

            if (auto tex = res.try_query<ID3D11Texture2D>()) {
                if (ensure_desktop_surface(gfx, *st, tex.get(), next_tex_id_)) {
                    gfx.d3d_context()->CopyResource(st->desktop_tex.get(), tex.get());
                    st->has_frame = true;
                    ++st->frame_seq;
                }
            }
            break;
        }
        return st->has_frame ? st : nullptr;
    }

private:
    OutputState* find_or_create(HMONITOR mon) {
        for (auto& o : outputs_) {
            if (o.mon == mon) return &o;
        }
        outputs_.push_back(OutputState{.mon = mon});
        return &outputs_.back();
    }

    static void reset_output(OutputState& st) {
        st.dupl.reset();
        st.desktop_tex.reset();
        st.tex_id = 0;
        st.mon_rect = {};
        st.tex_w = st.tex_h = 0;
        st.has_frame = false;
        st.dirty_all = false;
        st.dirty_rects.clear();
    }

    static bool ensure_duplication(Gfx& gfx, OutputState& st, ULONGLONG now_tick) {
        if (st.dupl) return true;
        if (now_tick < st.retry_after_tick) return false;

        reset_output(st);
        wil::com_ptr<IDXGIAdapter> adapter;
        if (FAILED(gfx.dxgi_device()->GetAdapter(&adapter)) || !adapter) {
            st.retry_after_tick = now_tick + 1000;
            return false;
        }
        for (UINT i = 0;; ++i) {
            wil::com_ptr<IDXGIOutput> out;
            if (adapter->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(out->GetDesc(&desc)) && desc.Monitor == st.mon) {
                if (auto out1 = out.try_query<IDXGIOutput1>()) {
                    if (SUCCEEDED(out1->DuplicateOutput(gfx.d3d(), &st.dupl))) {
                        st.mon_rect = desc.DesktopCoordinates;
                        st.has_frame = false;
                        return true;
                    }
                }
                break;
            }
        }
        st.retry_after_tick = now_tick + 1000;
        return false;
    }

    static bool ensure_desktop_surface(Gfx& gfx, OutputState& st, ID3D11Texture2D* src, uint64_t& next_tex_id) {
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (st.desktop_tex && st.tex_w == sd.Width && st.tex_h == sd.Height) return true;

        st.desktop_tex.reset();
        st.tex_id = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = sd.Width;
        td.Height = sd.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gfx.d3d()->CreateTexture2D(&td, nullptr, &st.desktop_tex))) return false;
        st.tex_id = ++next_tex_id;
        st.tex_w = sd.Width;
        st.tex_h = sd.Height;
        return true;
    }

    std::vector<OutputState> outputs_;
    uint64_t epoch_ = 0;
    uint64_t next_tex_id_ = 0;
    std::vector<uint8_t> metadata_buf_;
};

/// Per-window Liquid Glass rendering helper (manages blur + shader effects, backdrop copy, and in-app scene snapshot).
class LiquidGlassWindow {
public:
    void release() {
        desktop_bmp_.reset();
        desktop_bmp_tex_id_ = 0;
        win_bmp_[0].reset();
        win_bmp_[1].reset();
        win_bmp_idx_ = 0;
        scene_bmp_.reset();
        blur_effect_.reset();
        glass_effect_.reset();
        scene_blur_effect_.reset();
        scene_glass_effect_.reset();
        last_mon_ = nullptr;
        last_seq_ = 0;
        gdi_origin_ = {LONG_MIN, LONG_MIN};
        last_rendered_origin_ = {LONG_MIN, LONG_MIN};
        origin_override_.reset();
        seeded_gdi_ = false;
        scene_active_ = false;
        epoch_ = 0;
        scene_epoch_ = 0;
        desktop_bmp_epoch_ = 0;
        win_bmp_epoch_ = 0;
    }

    void set_origin_override(std::optional<POINT> pt) noexcept { origin_override_ = pt; }
    [[nodiscard]] POINT last_rendered_origin() const noexcept { return last_rendered_origin_; }

    bool poll_desktop(DesktopDuplicator& dupl, Gfx& gfx, HWND hwnd) {
        const auto* st = dupl.poll_monitor(gfx, hwnd, GetTickCount64());
        if (!st || !st->has_frame) return false;

        const bool mon_changed = (last_mon_ != st->mon);
        if (!mon_changed && last_seq_ == st->frame_seq) return false;

        const bool skipped = (!mon_changed && last_seq_ != 0 && st->frame_seq - last_seq_ > 1);
        last_mon_ = st->mon;
        last_seq_ = st->frame_seq;

        if (mon_changed || skipped || st->dirty_all) return true;

        RECT win_rc{};
        if (!GetWindowRect(hwnd, &win_rc)) return true;
        for (const RECT& r : st->dirty_rects) {
            RECT inter{};
            if (IntersectRect(&inter, &r, &win_rc)) return true;
        }
        return false;
    }

    /// Draws the live monitor desktop behind `hwnd` through the Liquid Glass shader across `panel_rect`.
    /// Returns false if Desktop Duplication or shader compilation is unavailable.
    bool draw_desktop_glass(DesktopDuplicator& dupl, Canvas& cv, WindowTarget& target, Gfx& gfx, HWND hwnd,
                            D2D1_RECT_F panel_rect, const GlassStyle& style) {
        if (!target.ready() || !hwnd) return false;
        const auto* st = dupl.poll_monitor(gfx, hwnd, GetTickCount64());
        if (st && st->has_frame) {
            last_mon_ = st->mon;
            last_seq_ = st->frame_seq;
        }

        const UINT pw = target.pixel_width();
        const UINT ph = target.pixel_height();
        if (pw == 0 || ph == 0) return false;

        GlassStyle s = style;
        s.force_opaque = true;

        if (st && st->has_frame && st->desktop_tex) {
            if (!desktop_bmp_ || desktop_bmp_tex_id_ != st->tex_id || desktop_bmp_dpi_ != target.dpi() || desktop_bmp_epoch_ != gfx.epoch()) {
                desktop_bmp_.reset();
                desktop_bmp_tex_id_ = 0;
                auto surf = st->desktop_tex.try_query<IDXGISurface>();
                if (!surf) return false;
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                    target.dpi(), target.dpi());
                if (FAILED(target.dc()->CreateBitmapFromDxgiSurface(surf.get(), &props, &desktop_bmp_))) {
                    return false;
                }
                desktop_bmp_tex_id_ = st->tex_id;
                desktop_bmp_dpi_ = target.dpi();
                desktop_bmp_epoch_ = gfx.epoch();
            }

            POINT origin{0, 0};
            if (origin_override_) {
                origin = *origin_override_;
            } else {
                ClientToScreen(hwnd, &origin);
            }
            last_rendered_origin_ = origin;
            const LONG sx = origin.x - st->mon_rect.left;
            const LONG sy = origin.y - st->mon_rect.top;
            const LONG clip_l = std::clamp<LONG>(sx, 0, static_cast<LONG>(st->tex_w));
            const LONG clip_t = std::clamp<LONG>(sy, 0, static_cast<LONG>(st->tex_h));
            const LONG clip_r = std::clamp<LONG>(sx + static_cast<LONG>(pw), 0, static_cast<LONG>(st->tex_w));
            const LONG clip_b = std::clamp<LONG>(sy + static_cast<LONG>(ph), 0, static_cast<LONG>(st->tex_h));
            if (clip_r <= clip_l || clip_b <= clip_t) return false;

            if (!ensure_win_bmp(gfx, target, pw, ph)) return false;
            win_bmp_idx_ ^= 1u;
            ID2D1Bitmap1* dst_bmp = win_bmp_[win_bmp_idx_].get();

            const D2D1_POINT_2U dst_pt{
                .x = static_cast<UINT32>(clip_l - sx),
                .y = static_cast<UINT32>(clip_t - sy),
            };
            const D2D1_RECT_U src_rc{
                .left = static_cast<UINT32>(clip_l),
                .top = static_cast<UINT32>(clip_t),
                .right = static_cast<UINT32>(clip_r),
                .bottom = static_cast<UINT32>(clip_b),
            };
            if (FAILED(dst_bmp->CopyFromBitmap(&dst_pt, desktop_bmp_.get(), &src_rc))) return false;
            const D2D1_SIZE_F sz = target.size();
            return draw_bitmap_glass(
                cv, target, gfx, dst_bmp, D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
                panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), s, false);
        }

        ID2D1Bitmap1* bmp = capture_gdi_backdrop(gfx, target, hwnd, pw, ph);
        if (!bmp) return false;
        const D2D1_SIZE_F sz = target.size();
        return draw_bitmap_glass(
            cv, target, gfx, bmp, D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
            panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), s, false);
    }

    /// Redirects `target.dc()` into an offscreen `scene_bmp_` so floating glass panels
    /// can sample and refract the rendered scene underneath them without copying.
    bool begin_scene(WindowTarget& target, Gfx& gfx) {
        scene_active_ = false;
        if (!target.ready() || !target.target_bitmap()) return false;
        const UINT pw = target.pixel_width();
        const UINT ph = target.pixel_height();
        if (pw == 0 || ph == 0) return false;

        if (!scene_bmp_ || scene_w_ != pw || scene_h_ != ph || scene_dpi_ != target.dpi() || scene_epoch_ != gfx.epoch()) {
            scene_bmp_.reset();
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                target.dpi(), target.dpi());
            if (FAILED(target.dc()->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &props, &scene_bmp_))) {
                return false;
            }
            scene_w_ = pw;
            scene_h_ = ph;
            scene_dpi_ = target.dpi();
            scene_epoch_ = gfx.epoch();
        }
        target.dc()->SetTarget(scene_bmp_.get());
        target.dc()->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        scene_active_ = true;
        return true;
    }

    /// Restores `target.target_bitmap()` and blits `scene_bmp_` onto it as the base layer.
    void end_scene(WindowTarget& target) {
        if (!scene_active_) return;
        scene_active_ = false;
        if (!target.ready() || !target.target_bitmap() || !scene_bmp_) return;
        ID2D1DeviceContext6* dc = target.dc();
        dc->SetTarget(target.target_bitmap());
        dc->DrawBitmap(scene_bmp_.get(), nullptr, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    }

    /// Draws a floating Liquid Glass panel refracting the captured in-app scene (`scene_bmp_`).
    bool draw_scene_glass(Canvas& cv, WindowTarget& target, Gfx& gfx,
                          D2D1_RECT_F panel_rect, const GlassStyle& style) {
        if (!scene_bmp_) return false;
        const D2D1_SIZE_F sz = target.size();
        return draw_bitmap_glass(
            cv, target, gfx, scene_bmp_.get(), D2D1::RectF(0.0f, 0.0f, sz.width, sz.height),
            panel_rect, D2D1::Point2F(panel_rect.left, panel_rect.top), style, true);
    }

private:
    bool ensure_win_bmp(Gfx& gfx, WindowTarget& target, UINT pw, UINT ph) {
        if (pw == 0 || ph == 0) return false;
        if (win_bmp_[0] && win_bmp_[1] && win_bmp_w_ == pw && win_bmp_h_ == ph &&
            win_bmp_dpi_ == target.dpi() && win_bmp_epoch_ == gfx.epoch()) {
            return true;
        }
        win_bmp_[0].reset();
        win_bmp_[1].reset();
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            target.dpi(), target.dpi());
        if (FAILED(target.dc()->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &props, &win_bmp_[0])) ||
            FAILED(target.dc()->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &props, &win_bmp_[1]))) {
            win_bmp_[0].reset();
            win_bmp_[1].reset();
            return false;
        }
        win_bmp_w_ = pw;
        win_bmp_h_ = ph;
        win_bmp_dpi_ = target.dpi();
        win_bmp_epoch_ = gfx.epoch();
        seeded_gdi_ = false;
        return true;
    }

    ID2D1Bitmap1* capture_gdi_backdrop(Gfx& gfx, WindowTarget& target, HWND hwnd, UINT pw, UINT ph) {
        if (!ensure_win_bmp(gfx, target, pw, ph)) return nullptr;
        POINT origin{0, 0};
        if (origin_override_) {
            origin = *origin_override_;
        } else {
            ClientToScreen(hwnd, &origin);
        }
        last_rendered_origin_ = origin;
        if (!seeded_gdi_ || origin.x != gdi_origin_.x || origin.y != gdi_origin_.y) {
            win_bmp_idx_ ^= 1u;
            if (seed_from_screen_dc(win_bmp_[win_bmp_idx_].get(), origin, pw, ph)) {
                seeded_gdi_ = true;
                gdi_origin_ = origin;
            }
        }
        return seeded_gdi_ ? win_bmp_[win_bmp_idx_].get() : nullptr;
    }

    bool seed_from_screen_dc(ID2D1Bitmap1* dst_bmp, POINT origin, UINT pw, UINT ph) {
        if (!dst_bmp || pw == 0 || ph == 0) return false;
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
        const BOOL ok = BitBlt(mem_dc, 0, 0, static_cast<int>(pw), static_cast<int>(ph),
                               screen_dc, origin.x, origin.y, SRCCOPY);
        SelectObject(mem_dc, prev);
        if (!ok) return false;
        const D2D1_RECT_U dst_rc{0, 0, pw, ph};
        return SUCCEEDED(dst_bmp->CopyFromMemory(&dst_rc, bits, pw * 4));
    }

    bool ensure_effects(WindowTarget& target, Gfx& gfx) {
        if (blur_effect_ && glass_effect_ && scene_blur_effect_ && scene_glass_effect_ && epoch_ == gfx.epoch()) return true;
        blur_effect_.reset();
        glass_effect_.reset();
        scene_blur_effect_.reset();
        scene_glass_effect_.reset();
        if (!target.ready() || !gfx.ready()) return false;
        if (!LiquidGlassEffectImpl::register_once(gfx.d2d_factory())) return false;
        if (FAILED(target.dc()->CreateEffect(CLSID_D2D1GaussianBlur, &blur_effect_))) return false;
        if (FAILED(target.dc()->CreateEffect(CLSID_UsyseekLiquidGlass, &glass_effect_))) {
            blur_effect_.reset();
            return false;
        }
        if (FAILED(target.dc()->CreateEffect(CLSID_D2D1GaussianBlur, &scene_blur_effect_)) ||
            FAILED(target.dc()->CreateEffect(CLSID_UsyseekLiquidGlass, &scene_glass_effect_))) {
            blur_effect_.reset();
            glass_effect_.reset();
            scene_blur_effect_.reset();
            scene_glass_effect_.reset();
            return false;
        }
        epoch_ = gfx.epoch();
        return true;
    }

    bool draw_bitmap_glass(Canvas& cv, WindowTarget& target, Gfx& gfx, ID2D1Bitmap1* src_bmp,
                           D2D1_RECT_F src_bounds, D2D1_RECT_F panel_rect, D2D1_POINT_2F target_offset,
                           const GlassStyle& style, bool use_scene_effect) {
        if (!src_bmp || width(panel_rect) <= 2.0f || height(panel_rect) <= 2.0f) return false;
        if (!ensure_effects(target, gfx)) return false;

        ID2D1Effect* blur_fx = use_scene_effect ? scene_blur_effect_.get() : blur_effect_.get();
        ID2D1Effect* glass_fx = use_scene_effect ? scene_glass_effect_.get() : glass_effect_.get();

        const LiquidGlassShaderParams params{
            .panel_rect = {panel_rect.left, panel_rect.top, width(panel_rect), height(panel_rect)},
            .src_bounds = {src_bounds.left, src_bounds.top, src_bounds.right, src_bounds.bottom},
            .tint_color = {style.tint.r, style.tint.g, style.tint.b, style.tint.a},
            .refract_params = {style.ref_thickness, style.ref_factor, style.ref_dispersion, style.dispersion_range},
            .fresnel_params = {style.fresnel_range, style.fresnel_hardness, style.fresnel_factor, style.magnification},
            .glare_params = {style.glare_range, style.glare_hardness, style.glare_factor, style.glare_angle_deg},
            .shape_params = {style.glare_convergence, style.glare_opposite_factor, style.corner_radius, style.roundness},
            .misc_params = {target.scale(), style.force_opaque ? 1.0f : 0.0f, 1.5f, style.margin_dip},
        };

        blur_fx->SetInput(0, nullptr);
        blur_fx->SetInput(0, src_bmp);
        blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.0f, style.blur_std_dev));
        blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        blur_fx->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);

        glass_fx->SetInputEffect(0, blur_fx);
        if (FAILED(glass_fx->SetValue(0, reinterpret_cast<const BYTE*>(&params), sizeof(params)))) return false;

        cv.dc()->DrawImage(
            glass_fx,
            target_offset,
            panel_rect,
            D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_OVER);
        return true;
    }

    wil::com_ptr<ID2D1Bitmap1> desktop_bmp_;
    uint64_t desktop_bmp_tex_id_ = 0;
    float desktop_bmp_dpi_ = 0.0f;
    std::array<wil::com_ptr<ID2D1Bitmap1>, 2> win_bmp_;
    uint32_t win_bmp_idx_ = 0;
    wil::com_ptr<ID2D1Bitmap1> scene_bmp_;
    wil::com_ptr<ID2D1Effect> blur_effect_;
    wil::com_ptr<ID2D1Effect> glass_effect_;
    wil::com_ptr<ID2D1Effect> scene_blur_effect_;
    wil::com_ptr<ID2D1Effect> scene_glass_effect_;
    HMONITOR last_mon_ = nullptr;
    uint64_t last_seq_ = 0;
    UINT win_bmp_w_ = 0, win_bmp_h_ = 0;
    float win_bmp_dpi_ = 0.0f;
    UINT scene_w_ = 0, scene_h_ = 0;
    float scene_dpi_ = 0.0f;
    uint64_t epoch_ = 0;
    uint64_t scene_epoch_ = 0;
    uint64_t desktop_bmp_epoch_ = 0;
    uint64_t win_bmp_epoch_ = 0;
    POINT gdi_origin_{LONG_MIN, LONG_MIN};
    POINT last_rendered_origin_{LONG_MIN, LONG_MIN};
    std::optional<POINT> origin_override_;
    bool seeded_gdi_ = false;
    bool scene_active_ = false;
};

[[nodiscard]] inline GlassStyle make_desktop_glass_style(const Theme& t, float corner_radius, bool fullscreen = false) noexcept {
    GlassStyle s;
    s.blur_std_dev = 1.4f;
    s.corner_radius = fullscreen ? 0.0f : corner_radius;
    s.roundness = 4.6f;
    s.margin_dip = fullscreen ? 0.0f : 0.5f;
    s.ref_thickness = fullscreen ? 14.0f : 18.0f;
    s.ref_factor = 1.56f;
    s.ref_dispersion = 5.5f;
    s.dispersion_range = 0.70f;
    s.fresnel_range = 28.0f;
    s.fresnel_hardness = 12.0f;
    s.fresnel_factor = t.dark_mode ? 34.0f : 44.0f;
    s.magnification = 1.035f;
    s.glare_range = 26.0f;
    s.glare_hardness = 18.0f;
    s.glare_factor = t.dark_mode ? 70.0f : 88.0f;
    s.glare_convergence = 58.0f;
    s.glare_opposite_factor = 72.0f;
    s.glare_angle_deg = -45.0f;
    s.tint = with_alpha(t.bg, t.pill_bg_alpha);
    return s;
}

[[nodiscard]] inline GlassStyle make_overlay_glass_style(const Theme& t, float corner_radius, float blur_std = 1.8f) noexcept {
    GlassStyle s;
    s.blur_std_dev = blur_std;
    s.corner_radius = corner_radius;
    s.roundness = 4.5f;
    s.margin_dip = 0.5f;
    s.ref_thickness = 16.0f;
    s.ref_factor = 1.56f;
    s.ref_dispersion = 4.5f;
    s.dispersion_range = 0.65f;
    s.fresnel_range = 26.0f;
    s.fresnel_hardness = 12.0f;
    s.fresnel_factor = t.dark_mode ? 36.0f : 46.0f;
    s.magnification = 1.045f;
    s.glare_range = 26.0f;
    s.glare_hardness = 18.0f;
    s.glare_factor = t.dark_mode ? 74.0f : 92.0f;
    s.glare_convergence = 60.0f;
    s.glare_opposite_factor = 74.0f;
    s.glare_angle_deg = -45.0f;
    s.tint = with_alpha(t.surface_high, t.dark_mode ? 0.28f : 0.36f);
    return s;
}

} // namespace usyseek::ui
