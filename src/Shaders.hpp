#pragma once

#include <unordered_map>
#include <string>

inline const std::unordered_map<std::string, const char*> SHADERS = {
    {"liquidglass.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Liquid Glass fragment shader — squircle-bezel refraction (nothing-liquid fork)
 *
 * The window is modeled as a thick convex glass slab:
 *   - Center: flat surface → clean frosted blur, no distortion
 *   - Edges: curved surface → refraction pulls in content from beyond
 *     the window boundary, creating natural color bleeding
 *
 * Rendering layers:
 * 1. Edge refraction via smooth outward direction (optionally along the edges,
 *    optionally rim-only) + exponential proximity
 * 2. Chromatic aberration (per-channel refraction scale)
 * 3. Edge raw-texture blend for vivid color pickup
 * 4. Body lens: the whole pane is a shallow convex lens
 * 5. Frosted tint (brightness boost + desaturation)
 * 6. Configurable color tint overlay, then frosted grain
 * 7. Rim: a crisp one-pixel edge, nearly invisible unless it has something
 *    bright to reflect (no key light, no lit corner)
 * 8. Fresnel edge glow (white or tinted by the background)
 * 9. Touch light: lights the glass around a press point, nowhere else
 */

uniform sampler2D tex;
uniform vec2 fullSize;
uniform vec2 invFullSize;      // = 1.0 / fullSize, hoisted out of the per-pixel divisions below
uniform vec4 radii;            // per-corner radius: top-left, top-right, bottom-right, bottom-left
uniform vec2 uvPadding;

uniform float refractionStrength;
uniform float noiseStrength;
uniform float chromaticAberration;
uniform float fresnelStrength;
uniform float specularStrength;
uniform float glassOpacity;
uniform float edgeThickness;
uniform float invBezelWidthPx; // = 1.0 / (edgeThickness * minDim), hoisted per-draw (minDim: the glass box's smaller side)
uniform vec3 tintColor;
uniform float tintAlpha;
uniform float lensDistortion;
uniform float lensMaxPx;       // unused (the old dome); see bodyLensPx
uniform float bodyLensPx;      // body lens: how far (px) the backdrop is pulled in at the rim
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float vibrancy;
uniform float vibrancyDarkness;
uniform float adaptiveDim;
uniform float adaptiveBoost;
uniform float roundingPower;
uniform float invRoundingPower; // = 1.0 / roundingPower, hoisted per-draw
uniform float refractionFlow;
uniform float refractionSpread;
uniform float fresnelTint;
uniform float bevelStrength;
uniform float bevelSize;
uniform float monitorScale;
uniform vec3 fresnelColor;
uniform float fresnelColorAlpha;
uniform vec3 bevelColor;
uniform float bevelColorAlpha;
uniform float bevelTint;
uniform float bevelAngle;      // unused: the rim has no key light
uniform float bevelShadow;     // shell panels: drop shadow just outside their shape
uniform float specularAngle;   // unused: light only comes from touches

uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;
uniform float maskFeather;     // alpha mask: glass fades in over this alpha range above the threshold (0 = this fork's own soft edge); not maskCoverage(), the field function
uniform int maskMode;          // 0 = alpha threshold, 1 = protocol region
uniform int regionRectCount;   // 0..64
uniform vec4 regionRects[64];  // box-local pixels: xy = offset from box top-left, zw = size

// The glass shape: the outline, bezel, lens, refraction centre and lighting
// all follow this sub-rect of the drawn box. Smaller than the drawn box only
// for subsurface items (e.g. a capsule pill inside a wider hit-test area) and
// layers with a fixed shape. Box-local pixels, same space as regionRects.
// Windows and other layers pass offset (0,0) and size == fullSize.
uniform vec2 glassBoxOffsetPx;
uniform vec2 glassBoxSizePx;
// glassUV = uv * glassUVScale - glassUVOffset: 0..1 across the glass box.
// = fullSize / glassBoxSizePx and glassBoxOffsetPx / glassBoxSizePx, hoisted
// per-draw; exactly (1,1) and (0,0) when the glass box is the whole box, so
// glassUV == uv bit for bit for windows and layers.
uniform vec2 glassUVScale;
uniform vec2 glassUVOffset;

// Maps this fragment's own box UV into the sample texture's normalized space
// before uvPadding is applied. Identity (offset 0, scale 1) unless the sample
// texture covers a smaller area than this box — PROTOCOL_REGION layers only,
// where the background is only ever sampled/blurred inside the blur region's
// bounding box (see GlassRenderer::sampleBackground callers in GlassLayerSurface.cpp).
uniform vec2 sampleUVOffset;
uniform vec2 sampleUVScale;

// Alpha-mask layers: a gaussian-blurred copy of the layer's own coverage
// (maskfield.frag). Its level sets stand in for the signed distance to the
// visible shape, so every pill, card and dock a shell draws gets its own bezel
// instead of sharing the layer's bounding box.
uniform sampler2D fieldTex;
uniform int   useField;
uniform vec2  fieldUVOffset;   // box UV -> field texture UV
uniform vec2  fieldUVScale;
uniform float fieldSigmaPx;    // sigma the field was blurred with, framebuffer px

// Touch light (Touch.hpp). The material has no light source of its own; light
// enters where it is pressed. Box-local framebuffer px; pressGlow 0 = none.
uniform vec2  pressPosPx;
uniform float pressGlow;
uniform float pressRadiusPx;

// Clear / Tinted (plugin:hyprglass:tinted): tinted panes lean toward
// tintedColor, the theme's surface colour (their backdrop is also blurred more).
uniform float tinted;
uniform vec3  tintedColor;
// Scales the rim's edge highlights (plugin:hyprglass:edge_highlight).
uniform float rimLevel;

#ifdef HYPRGLASS_FEATURE_LENS_EDGE
uniform float lensHeightPx;          // edge_height in framebuffer px, > 0
uniform float lensReflection;        // fresnel_reflection, 0..1
uniform float lensReflectionReachPx; // farthest the rim mirrors outside the glass, inside the sample padding

const float LENS_IOR = 1.5;
// refractive index spread per unit of chromatic_aberration: red below LENS_IOR, blue above
const float LENS_DISPERSION = 0.25;
// refraction_strength that bends by exactly what Snell's law gives
const float LENS_REFERENCE_STRENGTH = 0.6;
#endif

#ifdef HYPRGLASS_FEATURE_FOCUS_LIGHT
uniform float focusLight;           // focus_light times the focus fade, > 0
uniform vec3  focusLightColor;
uniform float focusLightColorAlpha;

// Logical px, so the light looks the same on every monitor scale.
const float FOCUS_LIGHT_LINE_DEPTH = 1.0;  // the bright line peaks this far in from the edge
const float FOCUS_LIGHT_LINE_WIDTH = 1.0;
const float FOCUS_LIGHT_SHEEN_FALLOFF = 4.0; // the soft sheen inside it fades out over ~3 of these
const float FOCUS_LIGHT_SHEEN = 0.3;       // sheen brightness, relative to the line
const float FOCUS_LIGHT_FLOOR = 0.5;       // the side facing away from bevel_angle keeps this much
const float FOCUS_LIGHT_REACH = 24.0;      // deeper, the light is under a third of a colour step: skipped
#endif

#ifdef HYPRGLASS_FEATURE_FROST
uniform sampler2D frostTex;  // tex blurred further, at half its size: same UVs as tex
uniform float frostAmount;   // frost times its ease, > 0

const float FROST_SATURATION_LIFT = 0.35; // frosted glass reads slightly more colourful ...
const float FROST_DIM = 0.10;             // ... and slightly deeper, at full amount
#endif

#ifdef HYPRGLASS_FEATURE_CONTOUR
// Layers with mask_mode = contour: the edge follows the outline the app draws, read
// from a distance field (Contour.hpp) instead of the layer's rectangle.
uniform highp sampler2D contourTex; // r: signed distance to the outline in px (negative inside), gb: its gradient
uniform vec2 contourUVScale;      // box UV -> field UV: the field covers whole texels, the box need not
uniform float contourReferencePx; // the bezel is a fraction of this, not of the layer's short side
uniform float contourReachPx;     // where the field stops; the edge effects fade out before it
#endif

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// ============================================================================
// TEXTURE SAMPLING (window UV -> padded texture UV)
// ============================================================================

// Box UV -> sample-texture-local UV (undoes sampleUVOffset/uvScale's
// shrink before the padding remap below sees it).
vec2 toSampleBoxUV(vec2 wuv) {
    return (wuv - sampleUVOffset) / sampleUVScale;
}

vec2 toTexUV(vec2 wuv) {
    return wuv * (1.0 - 2.0 * uvPadding) + uvPadding;
}

vec4 sampleBlurred(vec2 wuv) {
    vec2 tuv = toTexUV(toSampleBoxUV(wuv));
#ifdef HYPRGLASS_FEATURE_FROST
    tuv = clamp(tuv, 0.001, 0.999);
    return mix(texture(tex, tuv), texture(frostTex, tuv), frostAmount);
#else
    return texture(tex, clamp(tuv, 0.001, 0.999));
#endif
}

// hash12 from "Hash without Sine" (Dave Hoskins), tuned for integer pixel coordinates
float pixelHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// ============================================================================
// SDF
// ============================================================================

float lpNorm(vec2 v, float p, float invP) {
    // Exact identity: pow(x^2+y^2, 0.5) == length(v) when p == 2.0 (the
    // Hyprland default). Native sqrt is a single correctly-rounded hardware
    // op vs. two pow()s (exp2/log2-based) + a third pow() for the outer root.
    if (p == 2.0) return length(v);
    return pow(pow(abs(v.x), p) + pow(abs(v.y), p), invP);
}

// Quadrant lookup for the per-corner radius. p is measured from the box
// center (as getRoundedBoxSDF/getBevelSDF compute it below): negative y is
// the top half (v_texcoord grows downward — see the specular highlight's
// 1.0 - glassUV.y), negative x is the left half. cornerRadii is (top-left,
// top-right, bottom-right, bottom-left).
float pickCornerRadius(vec2 p, vec4 cornerRadii) {
    float top    = p.x < 0.0 ? cornerRadii.x : cornerRadii.y;
    float bottom = p.x < 0.0 ? cornerRadii.w : cornerRadii.z;
    return p.y < 0.0 ? top : bottom;
}

// posPx/boxSizePx: pixel-space position relative to (and size of) the box the
// SDF is measured against — the glass box (see glassBoxOffsetPx/SizePx above),
// not necessarily the fragment's full drawn box.
float getRoundedBoxSDF(vec2 posPx, vec2 boxSizePx, vec4 cornerRadii) {
    vec2 p = posPx - boxSizePx * 0.5;
    vec2 halfSize = boxSizePx * 0.5;
    float r = pickCornerRadius(p, cornerRadii);
    float clampedR = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + clampedR;
    return min(max(q.x, q.y), 0.0) + lpNorm(max(q, 0.0), roundingPower, invRoundingPower) - clampedR;
}

float getCornerSDF(vec2 uv) {
    vec2 boxLocalPx = uv * fullSize - glassBoxOffsetPx;
    return getRoundedBoxSDF(boxLocalPx, glassBoxSizePx, radii);
}

// Edge distance for the bevel, crease-free. Inside the window, getCornerSDF is
// max(q.x, q.y) - r: the distance to whichever edge is nearer. That has a crease
// along each corner's 45-degree diagonal, so the edge refraction (and anything
// else driven by edgeProximity) shows a straight seam in all four corners.
//
// Here the contour lines are rounded rectangles that match the window outline
// exactly at the edge (depth 0) and get rounder going inward: at depth d the box
// is inset by d and its corner radius is r + d. The radius only grows, so the
// top and side bevels always blend round the corner. Per pixel the depth is a
// closed-form quadratic in the corner zone:
//   |a + 2d| = r + d,  a = |p| - halfSize + r   =>   7d^2 + (4(ax+ay) - 2r) d + |a|^2 - r^2 = 0
// and the plain straight-edge distance elsewhere. Past depth (h - r) / 2, with h
// the smaller half-size, the radius would outgrow the inset box: from there the
// contours are stadiums, i.e. the plain distance to the box rounded by h (so a
// capsule gets its exact distance everywhere).
float getBevelSDF(vec2 uv) {
    vec2  H = glassBoxSizePx * 0.5;
    float h = min(H.x, H.y);
    vec2  signedP = uv * fullSize - glassBoxOffsetPx - H;
    float r = min(pickCornerRadius(signedP, radii), h);
    vec2  p = abs(signedP);
    vec2  a = p - H + r;
    float d = min(r - a.x, r - a.y);                      // straight-edge depth
    float B = 4.0 * (a.x + a.y) - 2.0 * r;
    float C = dot(a, a) - r * r;
    float disc = B * B - 28.0 * C;
    if (disc >= 0.0) {
        float dc = (-B + sqrt(disc)) / 14.0;
        if (a.x + 2.0 * dc >= 0.0 && a.y + 2.0 * dc >= 0.0)
            d = dc;                                        // in a corner zone: the rounded contour
    }
    vec2 s = p - H + h;
    return -max(d, h - length(max(s, 0.0)) - min(max(s.x, s.y), 0.0));
}

// ============================================================================
// SHAPE FROM COVERAGE (alpha-mask layers)
// ============================================================================

float maskCoverage(vec2 uv) {
    float a = texture(maskTex, clamp(uv * maskUVScale + maskUVOffset, 0.001, 0.999)).a;
    return smoothstep(maskAlphaThreshold * 0.6, maskAlphaThreshold * 1.4 + 0.004, a);
}

float fieldCoverage(vec2 uv) {
    float e = texture(fieldTex, uv * fieldUVScale + fieldUVOffset).r;
    float k = 1.0 - e;                 // stored as 1 - sqrt(1 - b): precision where b -> 1
    return 1.0 - k * k;
}

// Inverse normal CDF (Abramowitz-Stegun 26.2.23). A straight edge blurred with
// sigma s reads Phi(d / s) at depth d, so d = s * probit(coverage).
float probit(float p) {
    p = clamp(p, 0.002, 0.998);
    float q  = p < 0.5 ? p : 1.0 - p;
    float tt = sqrt(-2.0 * log(q));
    float z  = tt - (2.515517 + 0.802853 * tt + 0.010328 * tt * tt) /
                    (1.0 + 1.432788 * tt + 0.189269 * tt * tt + 0.001308 * tt * tt * tt);
    return p < 0.5 ? -z : z;
}

float fieldDepthPx(vec2 uv) {
    return fieldSigmaPx * probit(fieldCoverage(uv));
}

vec2 fieldInwardNormal(vec2 uv) {
    vec2 h = invFullSize * 3.0;
    vec2 g = vec2(
        fieldCoverage(uv + vec2(h.x, 0.0)) - fieldCoverage(uv - vec2(h.x, 0.0)),
        fieldCoverage(uv + vec2(0.0, h.y)) - fieldCoverage(uv - vec2(0.0, h.y))
    );
    float len = length(g);
    // fade out, rather than snap to zero, where the field goes flat
    return len > 1e-7 ? (g / len) * smoothstep(0.0, 0.002, len) : vec2(0.0);
}

// ============================================================================
// GLASS GEOMETRY
//
// The pane is a slab whose rim is a convex squircle bezel, the profile Apple
// uses for Liquid Glass: flat in the interior, rolling over to a vertical wall
// at the outline.   h(t) = T * (1 - (1 - t)^4)^(1/4),   t = depth / bezelWidth
// ============================================================================

// Depth inside the outline in pixels (positive inside), crease-free where possible.
float edgeDepthPx(vec2 uv) {
    return -(roundingPower == 2.0 ? getBevelSDF(uv) : getCornerSDF(uv));
}

// Unit vector pointing INTO the pane, perpendicular to the nearest outline.
vec2 inwardNormal(vec2 uv) {
    vec2 h = invFullSize;
    vec2 grad = vec2(
        edgeDepthPx(uv + vec2(h.x, 0.0)) - edgeDepthPx(uv - vec2(h.x, 0.0)),
        edgeDepthPx(uv + vec2(0.0, h.y)) - edgeDepthPx(uv - vec2(0.0, h.y))
    );
    float len = length(grad);
    if (len > 1e-5) return grad / len;
    vec2 toCenter = (vec2(0.5) - uv) * fullSize;
    float l2 = length(toCenter);
    return l2 > 0.1 ? toCenter / l2 : vec2(0.0);
}

float squircleHeight(float t) {
    float u = 1.0 - t;
    return pow(max(1.0 - u * u * u * u, 0.0), 0.25);
}

// d(height)/dt of the profile above
float squircleSlope(float t) {
    float u = 1.0 - t;
    float g = max(1.0 - u * u * u * u, 1e-4);
    return u * u * u * pow(g, -0.75);
}

// Snell's law for a ray travelling straight into the screen and hitting the
// bezel at normalized depth t. Returns how far (px) the ray has drifted toward
// the pane's interior by the time it reaches the background plane.
float refractShiftPx(float t, float thicknessPx, float bezelPx, float ior) {
    float slope  = squircleSlope(t) * thicknessPx / bezelPx;   // dh/dx of the surface
    float thetaI = atan(slope);                                 // angle of incidence
    float thetaR = asin(clamp(sin(thetaI) / ior, -1.0, 1.0));   // n1 sin(i) = n2 sin(r), n1 = 1
    float h      = squircleHeight(t) * thicknessPx;             // glass under this point
    return h * tan(thetaI - thetaR);
}

#ifdef HYPRGLASS_FEATURE_LENS_EDGE
// ============================================================================
// LENS EDGE
// The rim (the edge width) is a convex glass wall edge_height tall: a quarter
// ellipse across its width, vertical at the edge and flat from its inner side
// on, so the centre is not bent at all. A vertical eye ray refracts where it
// enters the rim (Snell's law) and runs down through the glass to the backdrop.
// ============================================================================

// The edge distance main() uses (bevelSdf), so the rim normal has no crease either.
float lensEdgeSDF(vec2 uv) {
    return roundingPower == 2.0 ? getBevelSDF(uv) : getCornerSDF(uv);
}

// Inward distance, in px, between where the eye ray enters the glass and where
// it meets the backdrop: n is the surface normal as (inward, up), surfacePx the
// glass height there. gain scales it, capPx bounds it smoothly.
float lensBendPx(vec2 n, float surfacePx, float ior, float gain, float capPx) {
    vec2 ray = refract(vec2(0.0, -1.0), n, 1.0 / ior);
    float runPx = surfacePx * ray.x / max(-ray.y, 1e-3);
    return capPx * tanh(clamp(gain * runPx / capPx, -8.0, 8.0));
}

// At a square or tight corner the rim normal turns all the way round within a
// few pixels, so every ray there bends toward one point (a dark curl) and the
// reflection swirls. Fades both out near a corner whose radius is under the rim width.
float lensCornerFade(vec2 uv, float rimPx) {
    vec2  halfBox    = glassBoxSizePx * 0.5;
    vec2  fromCenter = uv * fullSize - glassBoxOffsetPx - halfBox;
    float radius     = min(pickCornerRadius(fromCenter, radii), min(halfBox.x, halfBox.y));
    float toCorner   = length(max(halfBox - abs(fromCenter), 0.0));
    return mix(smoothstep(0.0, rimPx, toCorner), 1.0, clamp(radius / rimPx, 0.0, 1.0));
}
#endif

// ============================================================================
// MAIN
// ============================================================================

void main() {
    vec2 uv = v_texcoord;

    // Layers only: sample the temp FBO to get the rendered surface pixel.
    vec4 surfacePixel = vec4(0.0);
    float coverage = 1.0;
    bool hasMask = (useMask == 1);
    if (hasMask) {
        vec2 maskUV = uv * maskUVScale + maskUVOffset;
        surfacePixel = texture(maskTex, clamp(maskUV, 0.001, 0.999));

        if (maskMode == 1) {
            vec2 pixelPos = uv * fullSize;
            bool insideRegion = false;
            for (int i = 0; i < regionRectCount; i++) {
                vec4 r = regionRects[i];
                if (pixelPos.x >= r.x && pixelPos.y >= r.y &&
                    pixelPos.x <= r.x + r.z && pixelPos.y <= r.y + r.w) {
                    insideRegion = true;
                    break;
                }
            }
            if (!insideRegion) { fragColor = surfacePixel; return; } // premultiplied, output as-is
        } else {
            if (maskFeather > 0.0) {
                // A soft edge set for this layer (layers mask_feather): the glass fades
                // in over that alpha range above the threshold. Not smoothstep(): it is
                // undefined when threshold + feather rounds to threshold in float.
                float t = clamp((surfacePixel.a - maskAlphaThreshold) / maskFeather, 0.0, 1.0);
                coverage = surfacePixel.a < maskAlphaThreshold ? 0.0 : t * t * (3.0 - 2.0 * t);
            } else
                coverage = smoothstep(maskAlphaThreshold * 0.6, maskAlphaThreshold * 1.4 + 0.004, surfacePixel.a);
            // Not glass here: hand the shell's own pixel through, over a soft
            // shadow that the pane casts (it is what sets a pane apart from a
            // bright backdrop). The coverage field reaches a little past the shape.
            if (coverage < 0.002) {
                float shadowA = 0.0;
                if (useField == 1 && bevelShadow > 0.001) {
                    float f = fieldCoverage(uv - vec2(0.0, 3.0 * max(monitorScale, 0.5)) * invFullSize);
                    shadowA = bevelShadow * pow(clamp(f * 2.0, 0.0, 1.0), 1.6);
                }
                fragColor = surfacePixel + (1.0 - surfacePixel.a) * vec4(0.0, 0.0, 0.0, shadowA);
                return;
            }
        }
    }
    bool fieldShape = hasMask && maskMode == 0 && useField == 1;

#ifdef HYPRGLASS_FEATURE_CONTOUR
    // The traced outline replaces the layer box. The mask above already limits the glass
    // to the visible content, so the outline only shapes the edge effects, not the cut.
    vec4  contourSample  = texture(contourTex, uv * contourUVScale);
    float cornerSdf      = contourSample.r;
    float cornerAlpha    = 1.0;
    float contourGradLen = length(contourSample.gb);
    // zero where the field is flat (deep inside, or past its reach): no edge there
    vec2  contourOutward = contourGradLen > 0.05 ? contourSample.gb / contourGradLen : vec2(0.0);
#else
    float cornerSdf    = getCornerSDF(uv);
    float cornerAlpha  = 1.0 - smoothstep(-1.5, 0.5, cornerSdf);
#endif

    if (maskMode == 1) {
        if (cornerAlpha < 0.001) {
            fragColor = surfacePixel;
            return;
        }
    } else {
        if (cornerSdf > 0.0) discard;
        if (cornerAlpha < 0.001) discard;
    }

    float px      = max(monitorScale, 0.5);                       // one logical pixel
    float minDim  = min(glassBoxSizePx.x, glassBoxSizePx.y);

    // Bezel width is an absolute size (edge_thickness 0.06 = 24 logical px), so a
    // window gets a rim while a small pill, whose half-height is under that, is
    // lens all the way through: the size-dependent look of the real material.
    float bezelPx = clamp(edgeThickness * 400.0 * px, 1.0, max(0.5 * minDim, 1.0));
    float depthPx;
    vec2  nIn;
    if (fieldShape) {
        // The visible shape, not the layer box, is the pane. Thin shapes never
        // reach full coverage in the field, which leaves them curved across.
        bezelPx = max(edgeThickness * 400.0 * px, 1.0);
        depthPx = max(fieldDepthPx(uv), 0.0);
        nIn     = fieldInwardNormal(uv);
    } else {
        depthPx = max(edgeDepthPx(uv), 0.0);
        nIn     = inwardNormal(uv);
    }
    float t       = clamp(depthPx / bezelPx, 0.004, 1.0);
    vec2  nOut    = -nIn;

    // ========================================
    // REFRACTION — lensing ramps in with the pane's own alpha, so a pane
    // materializes by bending light rather than by cross-fading.
    // ========================================
    float materialize = smoothstep(0.0, 1.0, clamp(glassOpacity, 0.0, 1.0));
    float thicknessPx = refractionStrength * bezelPx * materialize;

    const float IOR = 1.5;
    float dispersion = chromaticAberration * 0.12;
    float shiftG = refractShiftPx(t, thicknessPx, bezelPx, IOR);
    vec2 offG = nIn * shiftG * invFullSize;

    // BODY LENS — the pane is not flat-topped: it is a shallow convex lens
    // across its whole face, so the refraction reads everywhere, not only on
    // the bezel. What is behind is magnified a little at the centre and pulled
    // in harder toward the rim, where the bezel takes over.
    vec2 domeUV = vec2(0.0);
    if (bodyLensPx > 0.01) {
        vec2  halfBox = max(0.5 * glassBoxSizePx, vec2(1.0));
        vec2  q       = (uv * fullSize - glassBoxOffsetPx - halfBox) / halfBox;   // -1..1 across the pane
        float r2      = clamp(0.5 * dot(q, q), 0.0, 1.0);
        domeUV = -q * bodyLensPx * (0.45 + 0.55 * r2) * materialize * invFullSize;
    }

    vec3 color;
    if (dispersion > 0.0005 && t < 0.999) {
        float shiftR = refractShiftPx(t, thicknessPx, bezelPx, IOR - dispersion);
        float shiftB = refractShiftPx(t, thicknessPx, bezelPx, IOR + dispersion);
        color.r = sampleBlurred(uv + nIn * shiftR * invFullSize + domeUV).r;
        color.g = sampleBlurred(uv + offG + domeUV).g;
        color.b = sampleBlurred(uv + nIn * shiftB * invFullSize + domeUV).b;
    } else {
        color = sampleBlurred(uv + offG + domeUV).rgb;
    }

#ifdef HYPRGLASS_FEATURE_FROST
    // Frosted glass (frost): the backdrop was blurred further above; it also gets
    // a little more colour and depth, before the theme's tone mapping below.
    {
        float frostLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = max(mix(vec3(frostLum), color, 1.0 + FROST_SATURATION_LIFT * frostAmount), 0.0) * (1.0 - FROST_DIM * frostAmount);
    }
#endif

    // ========================================
    // TONE (dimming layer / adaptivity)
    // ========================================
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, saturation);

    // Adaptive tone. Dark mode: a bright backdrop is pulled down toward a
    // ceiling, so the shell's white text stays readable on any wallpaper or
    // window; a dark one is left as it is. adaptive_dim = how hard (1 = flat
    // at the ceiling).
    const float CEILING = 0.30;
    float toned    = lum - max(lum - CEILING, 0.0) * adaptiveDim;
    color *= brightness * (lum > 1e-4 ? toned / lum : 1.0);
    // Light mode, the mirror: a dark backdrop is lifted toward a floor by
    // mixing in white (milky, like Tahoe's light glass), so dark text stays
    // readable; a bright one is left as it is. adaptive_boost = how hard
    // (1 = nothing darker than the floor).
    const float FLOOR = 0.74;
    if (adaptiveBoost > 0.001) {
        float l      = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float lifted = l + max(FLOOR - l, 0.0) * adaptiveBoost;
        color = mix(color, vec3(1.0), clamp((lifted - l) / max(1.0 - l, 1e-3), 0.0, 1.0));
    }
    color = mix(vec3(0.5), color, contrast);

    float currentLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float darkFactor = 1.0 - vibrancyDarkness * (1.0 - lum);
    color = mix(vec3(currentLum), color, 1.0 + vibrancy * sat * darkFactor);

    color = mix(color, tintColor, tintAlpha);

    // Tinted: more opaque, so what sits on the glass gets more contrast.
    if (tinted > 0.001)
        color = mix(color, tintedColor, tinted * 0.62);

    // grain-free copy for the rim tints, so the grain does not speckle their hue
    vec3 tintSource = color;

    // ========================================
    // FROSTED GRAIN
    // Hashed on box-local pixels so the grain moves with the glass and keeps
    // its size on any window; after the tint so the tint does not fade it.
    // ========================================
    if (noiseStrength > 0.001)
        color += (pixelHash(floor(uv * fullSize)) - 0.5) * noiseStrength * 0.1;

    // What the edge reflects: the backdrop just outside it. Bright content
    // there lights the rim; over a dark backdrop the rim all but disappears.
    vec3  envC = sampleBlurred(uv + nOut * invFullSize * 5.0 * px).rgb;
    float env  = smoothstep(0.30, 0.95, dot(envC, vec3(0.2126, 0.7152, 0.0722)));

    // ========================================
    // INNER GLOW — light scattered inside the curved rim
    // ========================================
    float rimFall = 1.0 - smoothstep(0.0, 1.0, t);
    if (fresnelStrength > 0.001) {
        vec3 glow = vec3(1.0);
        if (fresnelColorAlpha > 0.001) glow = mix(glow, fresnelColor, fresnelColorAlpha);
        if (fresnelTint > 0.001) {
            float maxC = max(max(tintSource.r, tintSource.g), tintSource.b);
            glow = mix(glow, maxC > 0.001 ? tintSource / maxC : vec3(1.0), fresnelTint);
        }
        color += glow * rimFall * rimFall * fresnelStrength * 0.10 * (0.3 + 0.7 * env);
    }

    // ========================================
    // TOUCH LIGHT — where the glass is pressed, light gets into it: every rim
    // within reach of the press point catches it, and small controls glow
    // through. specular_strength scales it.
    // ========================================
    float touch = 0.0;   // reaches the rims of everything nearby
    float spot  = 0.0;   // the glow right under the press
    if (pressGlow > 0.001) {
        vec2  dp = uv * fullSize - pressPosPx;
        float r2 = dot(dp, dp) / max(pressRadiusPx * pressRadiusPx, 1.0);
        touch = pressGlow * specularStrength * exp(-2.0 * r2);
        spot  = pressGlow * specularStrength * exp(-5.0 * r2);
    }

    // ========================================
    // RIM — a crisp one-pixel edge where the slab ends, the same on every
    // side (there is no key light): a quiet line on its own, brighter where
    // the edge reflects something bright, and where it is touched.
    // ========================================
    if (bevelStrength > 0.001 || touch > 0.001) {
        float line;
        if (fieldShape) {
            // the pane's outermost pixel row: inside, with a neighbour outside
            vec2  o     = invFullSize;
            float inner = min(min(maskCoverage(uv + vec2(o.x, 0.0)), maskCoverage(uv - vec2(o.x, 0.0))),
                              min(maskCoverage(uv + vec2(0.0, o.y)), maskCoverage(uv - vec2(0.0, o.y))));
            line = clamp(coverage - inner, 0.0, 1.0);
        } else {
            line = 1.0 - smoothstep(0.0, 1.25, depthPx);
        }
        float bandW = max(bevelSize * px, 1.0);
        float band  = exp(-depthPx / bandW) * 0.28;   // only the touch light uses it

        vec3 rimLight = vec3(1.0);
        if (bevelColorAlpha > 0.001) rimLight = mix(rimLight, bevelColor, bevelColorAlpha);
        if (bevelTint > 0.001) {
            float maxC = max(max(tintSource.r, tintSource.g), tintSource.b);
            rimLight = mix(rimLight, maxC > 0.001 ? tintSource / maxC : vec3(1.0), bevelTint);
        }
        float rim = line * bevelStrength * rimLevel * (0.42 + 0.58 * env)
                  + (line * 1.6 + band * 3.0) * touch;
        color = mix(color, rimLight, clamp(rim, 0.0, 1.0));
    }

    // the pressed spot fills with soft light (a small control fills entirely)
    if (spot > 0.001)
        color += vec3(1.0) * spot * 0.16;

    color = clamp(color, 0.0, 1.0);
    float glassA = clamp(glassOpacity * cornerAlpha * coverage, 0.0, 1.0);

    if (hasMask) {
        float surfA = surfacePixel.a;
        vec3 surfRGB = surfA > 0.001 ? surfacePixel.rgb / surfA : vec3(0.0);

        float compA = surfA + glassA * (1.0 - surfA);
        vec3 compRGB = compA > 0.001
            ? (surfRGB * surfA + color * glassA * (1.0 - surfA)) / compA
            : vec3(0.0);

        fragColor = vec4(compRGB * compA, compA);
    } else {
        fragColor = vec4(color * glassA, glassA);
    }
}
)GLSL"},

    {"maskfield.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Builds the coverage field liquidglass.frag reads its bezel from: the layer's
 * alpha, thresholded to "glass / not glass" and gaussian-blurred. Run twice
 * (horizontal with binarize = 1, then vertical with encode = 1).
 */

uniform sampler2D tex;
uniform vec2  direction;   // one tap step along the blur axis, in tex UV
uniform float sigma;       // in tap steps
uniform int   binarize;    // 1: tex is the layer's rendered surface, read alpha and threshold it
uniform int   encode;      // 1: write 1 - sqrt(1 - b)
uniform float threshold;
uniform vec2  uvOffset;    // output UV -> tex UV
uniform vec2  uvScale;
uniform vec4  uvClamp;     // tex-UV rect that holds the layer; outside it nothing is covered

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

float tap(vec2 uv) {
    if (binarize == 1) {
        if (uv.x < uvClamp.x || uv.y < uvClamp.y || uv.x > uvClamp.z || uv.y > uvClamp.w)
            return 0.0;
        return smoothstep(threshold * 0.6, threshold * 1.4 + 0.004, texture(tex, uv).a);
    }
    return texture(tex, uv).r;
}

void main() {
    vec2  uv   = v_texcoord * uvScale + uvOffset;
    float inv  = -0.5 / (sigma * sigma);
    float sum  = tap(uv);
    float wsum = 1.0;
    int   n    = int(min(ceil(sigma * 3.0), 48.0));
    for (int i = 1; i <= n; i++) {
        float x = float(i);
        float w = exp(x * x * inv);
        sum  += (tap(uv + direction * x) + tap(uv - direction * x)) * w;
        wsum += 2.0 * w;
    }
    float b = sum / wsum;
    if (encode == 1)
        b = 1.0 - sqrt(max(1.0 - b, 0.0));
    fragColor = vec4(b, b, b, 1.0);
}
)GLSL"},

    {"genie.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Magic lamp. Inverse-maps every output pixel back into the window:
 *   - the window's two sides bend into a funnel that ends at the target
 *   - the window then slides down that funnel, squeezed to its width
 * progress 0 is the window at rest, 1 is the window gone into the target.
 */

uniform sampler2D tex;
uniform vec2  fbSize;     // framebuffer pixels
uniform vec4  srcBox;     // window: x, y, w, h
uniform vec3  target;     // x0, x1, y of the line the window vanishes into
uniform float progress;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2  P  = v_texcoord * fbSize;
    float x0 = srcBox.x, x1 = srcBox.x + srcBox.z;
    float y0 = srcBox.y, y1 = srcBox.y + srcBox.w;
    float H  = srcBox.w;

    // Measure along the axis from the window's far edge (s = 0) to the target (s = S).
    float dir  = target.z >= 0.5 * (y0 + y1) ? 1.0 : -1.0;
    float yFar = dir > 0.0 ? y0 : y1;
    float S    = max(abs(target.z - yFar), H + 1.0);
    float s    = (P.y - yFar) * dir;

    if (s < 0.0 || s > S) { fragColor = vec4(0.0); return; }

    float bend  = smoothstep(0.0, 0.40, progress);          // the funnel forms first...
    float slide = smoothstep(0.08, 1.0, progress);          // ...then the window pours down it

    float f     = smoothstep(0.0, 1.0, s / S);
    float left  = mix(x0, target.x, f * bend);
    float right = mix(x1, target.y, f * bend);
    float width = max(right - left, 0.5);
    float u     = (P.x - left) / width;
    if (u < 0.0 || u > 1.0) { fragColor = vec4(0.0); return; }

    float sSrc = s - slide * S;
    if (sSrc < 0.0 || sSrc > H) { fragColor = vec4(0.0); return; }

    vec2 src = vec2(mix(x0, x1, u), yFar + dir * sSrc);
    vec4 c   = texture(tex, src / fbSize);

    float side = smoothstep(0.0, 1.5 / width, u) * smoothstep(0.0, 1.5 / width, 1.0 - u);
    float sink = 1.0 - bend * smoothstep(S - max(0.05 * S, 10.0), S, s);   // dissolve into the target
    fragColor = c * side * sink;
}
)GLSL"},

    {"gaussianblur.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 direction; // (1.0/width, 0.0) for horizontal, (0.0, 1.0/height) for vertical
uniform float blurRadius; // kernel radius in pixels

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    // Compute sigma from radius (covers ~3 sigma)
    float sigma = max(blurRadius / 3.0, 0.001);
    float invSigma2 = -0.5 / (sigma * sigma);

    int samples = min(int(ceil(blurRadius)), 8);

    // Center tap, clamped: an out-of-range texel from a float framebuffer would otherwise dominate the kernel
    float w0 = 1.0;
    vec4 result = clamp(texture(tex, v_texcoord), 0.0, 1.0) * w0;
    float totalWeight = w0;

    // Linear sampling: pair adjacent taps (i, i+1) into a single bilinear fetch.
    // The interpolated offset between two texels yields their weighted average
    // in one texture() call, halving the total tap count.
    for (int i = 1; i <= samples; i += 2) {
        float x1 = float(i);
        float x2 = float(i + 1);
        float w1 = exp(x1 * x1 * invSigma2);
        float w2 = (i + 1 <= samples) ? exp(x2 * x2 * invSigma2) : 0.0;
        float wSum = w1 + w2;
        if (wSum < 0.0001) continue;

        // Offset biased toward the heavier weight
        float offset = (x1 * w1 + x2 * w2) / wSum;

        result += clamp(texture(tex, v_texcoord + direction * offset), 0.0, 1.0) * wSum;
        result += clamp(texture(tex, v_texcoord - direction * offset), 0.0, 1.0) * wSum;
        totalWeight += 2.0 * wSum;
    }

    fragColor = result / totalWeight;
}
)GLSL"},
    // ----------------------------------------------------------------------------
    // Contour field (mask_mode = contour, see Contour.hpp). Six passes at a fraction of the
    // layer's size build a signed distance field of the outline the app draws; they run only
    // when the layer's content changes. Texel (i, j) is framebuffer row j, like everywhere else.
    // ----------------------------------------------------------------------------

    // Pass 1: which texels are inside the outline. The result is the start of a distance
    // transform: per channel the squared distance to the nearest texel of the other kind
    // (r: for an inside texel, to an outside one; g: the opposite), 0 on texels of that
    // other kind and FAR where nothing is known yet.
    {"contourseed.frag", R"GLSL(
#version 300 es
precision highp float;
precision highp sampler2D;

uniform sampler2D tex;       // the layer's own render, alpha is the mask
uniform vec2 boxOffsetPx;    // the layer box in that framebuffer
uniform vec2 boxSizePx;
uniform vec2 sourceSizePx;
uniform float divisor;       // framebuffer px per texel: 1, 2, 4 or 8
uniform float threshold;     // alpha at which the app's content counts

layout(location = 0) out vec4 fragColor;

const float FAR = 16384.0;   // above any squared distance within reach, and exact in half floats (Contour.hpp)

void main() {
    vec2 texel = floor(gl_FragCoord.xy);

    // Mean alpha over the texel's block. A tap on the shared corner of 2x2 pixels already
    // averages them in hardware, so one tap per 2 px, and only the block's own pixels.
    float taps    = max(divisor * 0.5, 1.0);
    float stepPx  = divisor > 1.5 ? 2.0 : 1.0;
    float startPx = divisor > 1.5 ? 1.0 : 0.5;
    float sum = 0.0;
    for (float j = 0.0; j < taps; j += 1.0) {
        for (float i = 0.0; i < taps; i += 1.0) {
            vec2 p = texel * divisor + vec2(startPx + i * stepPx, startPx + j * stepPx);
            // never past the box: outside it the layer's render is not cleared, so it holds old frames
            p = clamp(p, vec2(0.5), boxSizePx - 0.5);
            // past the monitor the layer is cut off: carry its edge on
            vec2 q = clamp(boxOffsetPx + p, vec2(0.5), sourceSizePx - 0.5);
            sum += texture(tex, q / sourceSizePx).a;
        }
    }

    bool inside = sum / (taps * taps) >= threshold;
    fragColor = vec4(inside ? FAR : 0.0, inside ? 0.0 : FAR, 0.0, 1.0);
}
)GLSL"},

    // Passes 2 and 3: the exact Euclidean distance transform, one axis at a time (squared
    // distance = min over the row of (offset^2 + what the previous axis found)), looking at
    // most reachTexels away. Texels past the box count as outside, so a layer's own edge is an
    // outline. The last pass turns both distances into one signed distance in framebuffer px,
    // half a texel in from each class's centre so that zero is the outline itself.
    {"contouredt.frag", R"GLSL(
#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

uniform sampler2D tex;
uniform vec2 fieldSize;       // texels
uniform vec2 direction;       // (1, 0) or (0, 1)
uniform int reachTexels;
uniform int resolveSigned;    // 1 on the last pass
uniform float texelPx;        // framebuffer px per texel
uniform float reachPx;        // the signed distance is clamped to +-this

layout(location = 0) out vec4 fragColor;

const float FAR = 16384.0;

vec2 known(vec2 p) {
    if (p.x < 0.0 || p.y < 0.0 || p.x >= fieldSize.x || p.y >= fieldSize.y)
        return vec2(0.0, FAR);
    return texelFetch(tex, ivec2(p), 0).rg;
}

void main() {
    vec2 p = floor(gl_FragCoord.xy);
    vec2 best = known(p);
    for (int k = 1; k <= reachTexels; k++) {
        float offsetSq = float(k * k);
        // nothing farther can beat what is found
        if (offsetSq >= max(best.x, best.y)) break;
        vec2 step = direction * float(k);
        best = min(best, min(known(p + step), known(p - step)) + offsetSq);
    }

    if (resolveSigned == 1) {
        // an outside texel has best.x == 0: its own squared distance to "outside" is nothing
        float signedPx = best.x > 0.0 ? -(sqrt(best.x) - 0.5) * texelPx
                                      :  (sqrt(best.y) - 0.5) * texelPx;
        fragColor = vec4(clamp(signedPx, -reachPx, reachPx), 0.0, 0.0, 1.0);
    } else {
        fragColor = vec4(best, 0.0, 1.0);
    }
}
)GLSL"},

    // Passes 4 and 5: a Gaussian along one axis on the signed distance. The distance is linear
    // through the outline, so this leaves the outline where it is and only rounds the creases
    // where distances from two edges meet at a corner.
    {"contoursmooth.frag", R"GLSL(
#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

uniform sampler2D tex;
uniform vec2 fieldSize;
uniform vec2 direction;
uniform int radius;           // taps each side
uniform float negHalfInvSigmaSq;

layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(fieldSize) - 1;
    ivec2 axis = ivec2(direction);
    float sum = 0.0;
    float weights = 0.0;
    for (int k = -radius; k <= radius; k++) {
        float w = exp(float(k * k) * negHalfInvSigmaSq);
        sum += w * texelFetch(tex, clamp(p + axis * k, ivec2(0), last), 0).r;
        weights += w;
    }
    fragColor = vec4(sum / weights, 0.0, 0.0, 1.0);
}
)GLSL"},

    // Pass 6: the field the glass shader reads. r: the signed distance, gb: its gradient
    // (outward, about unit length), so the shader needs one fetch for both.
    {"contourfield.frag", R"GLSL(
#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;

uniform sampler2D tex;
uniform vec2 fieldSize;
uniform float texelPx;

layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(fieldSize) - 1;
    float centre = texelFetch(tex, p, 0).r;
    float left   = texelFetch(tex, clamp(p + ivec2(-1, 0), ivec2(0), last), 0).r;
    float right  = texelFetch(tex, clamp(p + ivec2(1, 0), ivec2(0), last), 0).r;
    float above  = texelFetch(tex, clamp(p + ivec2(0, -1), ivec2(0), last), 0).r;
    float below  = texelFetch(tex, clamp(p + ivec2(0, 1), ivec2(0), last), 0).r;
    fragColor = vec4(centre, vec2(right - left, below - above) / (2.0 * texelPx), 1.0);
}
)GLSL"},
};
