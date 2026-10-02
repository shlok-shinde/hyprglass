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
 * 4. Subtle center dome lens magnification
 * 5. Frosted tint (brightness boost + desaturation)
 * 6. Configurable color tint overlay
 * 7. Bevel (thin lit line at the edge)
 * 8. Fresnel edge glow (white or tinted by the background)
 * 9. Specular highlight (top)
 * 10. Inner shadow (bottom rim)
 */

uniform sampler2D tex;
uniform vec2 fullSize;
uniform vec2 invFullSize;      // = 1.0 / fullSize, hoisted out of the per-pixel divisions below
uniform vec4 radii;            // per-corner radius: top-left, top-right, bottom-right, bottom-left
uniform vec2 uvPadding;

uniform float refractionStrength;
uniform float chromaticAberration;
uniform float fresnelStrength;
uniform float specularStrength;
uniform float glassOpacity;
uniform float edgeThickness;
uniform float invBezelWidthPx; // = 1.0 / (edgeThickness * minDim), hoisted per-draw
uniform vec3 tintColor;
uniform float tintAlpha;
uniform float lensDistortion;
uniform float lensMaxPx;       // = lensDistortion * minDim * 0.006, hoisted per-draw
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
uniform float bevelAngle;
uniform float bevelShadow;
uniform float specularAngle;

uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;
uniform int maskMode;          // 0 = alpha threshold, 1 = protocol region
uniform int regionRectCount;   // 0..16
uniform vec4 regionRects[16];  // box-local pixels: xy = offset from box top-left, zw = size

// Subsurface item glass only: the rounded-box SDF (getCornerSDF below) is
// evaluated over this sub-rect of the drawn box instead of the full box —
// the item's blur-region extents, which can be smaller than its own surface
// box (e.g. a capsule pill inside a wider hit-test area). Box-local pixels,
// same space as regionRects. Windows and alpha-mask layers pass offset (0,0)
// and size == fullSize, so the SDF is unchanged from the old whole-box math.
uniform vec2 glassBoxOffsetPx;
uniform vec2 glassBoxSizePx;

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
    return texture(tex, clamp(tuv, 0.001, 0.999));
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
// 1.0 - uv.y), negative x is the left half. cornerRadii is (top-left,
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
// LIGHT
// ============================================================================

// light direction for a clockwise angle in degrees, 0 = from the top (screen y grows downward)
vec2 lightDir(float angleDeg) {
    float a = radians(angleDeg);
    return vec2(sin(a), -cos(a));
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
            coverage = smoothstep(maskAlphaThreshold * 0.6, maskAlphaThreshold * 1.4 + 0.004, surfacePixel.a);
            // Not glass here: hand the shell's own pixel (a drop shadow, say) through untouched.
            if (coverage < 0.002) { fragColor = surfacePixel; return; }
        }
    }
    bool fieldShape = hasMask && maskMode == 0 && useField == 1;

    float cornerSdf    = getCornerSDF(uv);
    float cornerAlpha  = 1.0 - smoothstep(-1.5, 0.5, cornerSdf);

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

    // subtle dome in the flat interior
    vec2 domeUV = vec2(0.0);
    if (lensDistortion > 0.001) {
        vec2 c = (uv - 0.5) * 2.0;
        vec2 dGrad = vec2(-4.0 * c.x * (1.0 - c.y * c.y), -4.0 * c.y * (1.0 - c.x * c.x));
        domeUV = dGrad * lensMaxPx * smoothstep(0.0, 1.0, t) * invFullSize;
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

    // ========================================
    // TONE (dimming layer / adaptivity)
    // ========================================
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, saturation);

    float lumCurve = smoothstep(0.25, 0.55, lum);
    color *= brightness * (1.0 - adaptiveDim * lumCurve);
    color += vec3(adaptiveBoost * (1.0 - lumCurve) * 0.5);
    color = mix(vec3(0.5), color, contrast);

    float currentLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float darkFactor = 1.0 - vibrancyDarkness * (1.0 - lum);
    color = mix(vec3(currentLum), color, 1.0 + vibrancy * sat * darkFactor);

    color = mix(color, tintColor, tintAlpha);

    // ========================================
    // INNER GLOW — light scattered inside the curved rim
    // ========================================
    float rimFall = 1.0 - smoothstep(0.0, 1.0, t);
    if (fresnelStrength > 0.001) {
        vec3 glow = vec3(1.0);
        if (fresnelColorAlpha > 0.001) glow = mix(glow, fresnelColor, fresnelColorAlpha);
        if (fresnelTint > 0.001) {
            float maxC = max(max(color.r, color.g), color.b);
            glow = mix(glow, maxC > 0.001 ? color / maxC : vec3(1.0), fresnelTint);
        }
        color += glow * rimFall * rimFall * fresnelStrength * 0.10;
    }

    // ========================================
    // RIM LIGHT — a hairline that is brightest where the outline faces the key
    // light and, weaker, on the opposite side where that light leaves the slab.
    // The two flanks in between stay nearly dark, which is what gives the
    // material its characteristic diagonal sparkle.
    // ========================================
    if (bevelStrength > 0.001) {
        vec2  L      = lightDir(bevelAngle);
        float ndl    = dot(nOut, L);
        float lit    = pow(max(ndl, 0.0), 1.6);
        float back   = pow(max(-ndl, 0.0), 2.4) * 0.6;
        float facing = clamp(lit + back + 0.10, 0.0, 1.0);

        float lineW  = max(0.9 * px, 0.75);
        float line   = exp(-(depthPx * depthPx) / (2.0 * lineW * lineW));
        if (fieldShape) {
            // The field is too soft to place a hairline: probe the real outline instead.
            vec2 stepOut = nOut * invFullSize * lineW;
            line = clamp(1.0 - 0.65 * maskCoverage(uv + stepOut * 1.2) - 0.35 * maskCoverage(uv + stepOut * 2.4), 0.0, 1.0);
        }
        float bandW  = max(bevelSize * px, 1.0);
        float band   = exp(-depthPx / bandW) * 0.28;

        vec3 rimLight = vec3(1.0);
        if (bevelColorAlpha > 0.001) rimLight = mix(rimLight, bevelColor, bevelColorAlpha);
        if (bevelTint > 0.001) {
            float maxC = max(max(color.r, color.g), color.b);
            rimLight = mix(rimLight, maxC > 0.001 ? color / maxC : vec3(1.0), bevelTint);
        }
        color = mix(color, rimLight, clamp((line + band) * facing * bevelStrength, 0.0, 1.0));

        // the unlit flanks read slightly darker than the pane
        if (bevelShadow > 0.001)
            color *= 1.0 - bevelShadow * band * 1.6 * (1.0 - clamp(lit + back, 0.0, 1.0));
    }

    // ========================================
    // SHEEN — broad, soft light across the bezel on the lit side
    // ========================================
    if (specularStrength > 0.001) {
        vec2  L     = lightDir(specularAngle);
        float specT = clamp(0.5 + dot(uv - 0.5, L), 0.0, 1.0);
        color += vec3(1.0, 0.99, 0.97) * specT * specT * rimFall * specularStrength * 0.10;
    }

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
};
