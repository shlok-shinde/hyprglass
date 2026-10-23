#include "GlassRenderer.hpp"
#include "BuiltInPresets.hpp"
#include "Diagnostics.hpp"
#include "Frost.hpp"
#include "Globals.hpp"
#include "Touch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

namespace GlassRenderer {

static GLuint fbId(const SP<Render::IFramebuffer>& framebuffer) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(framebuffer.get())->getFBID();
}

static void uploadThemeUniforms(const SResolveContext& ctx, const SGlassProgram& program) {
    const auto& uniforms    = program.uniforms;
    const auto& glassShader = program.shader;
    const auto& defaults = ctx.isDark ? DARK_THEME_DEFAULTS : LIGHT_THEME_DEFAULTS;

    glassShader->setUniformFloat(SHADER_BRIGHTNESS, resolvePresetFloat(ctx, &SPresetValues::brightness, &SOverridableConfig::brightness, defaults.brightness));
    glassShader->setUniformFloat(SHADER_CONTRAST,   resolvePresetFloat(ctx, &SPresetValues::contrast, &SOverridableConfig::contrast, defaults.contrast));
    glUniform1f(uniforms.saturation,                 resolvePresetFloat(ctx, &SPresetValues::saturation, &SOverridableConfig::saturation, defaults.saturation));
    glassShader->setUniformFloat(SHADER_VIBRANCY,   resolvePresetFloat(ctx, &SPresetValues::vibrancy, &SOverridableConfig::vibrancy, defaults.vibrancy));
    glUniform1f(uniforms.vibrancyDarkness,           resolvePresetFloat(ctx, &SPresetValues::vibrancyDarkness, &SOverridableConfig::vibrancyDarkness, defaults.vibrancyDarkness));

    glUniform1f(uniforms.adaptiveDim,   resolvePresetFloat(ctx, &SPresetValues::adaptiveDim, &SOverridableConfig::adaptiveDim, defaults.adaptiveDim));
    glUniform1f(uniforms.adaptiveBoost, resolvePresetFloat(ctx, &SPresetValues::adaptiveBoost, &SOverridableConfig::adaptiveBoost, defaults.adaptiveBoost));
}

CBox SSampleMap::toSample(const CBox& framebufferBox) const {
    return CBox{(framebufferBox.x - srcX0) * scaleX, (framebufferBox.y - srcY0) * scaleY,
                framebufferBox.width * scaleX, framebufferBox.height * scaleY};
}

SSampleMap sampleMapFor(const CBox& box, int downscale) {
    SSampleMap map;
    map.fullWidth  = static_cast<int>(box.width) + 2 * SAMPLE_PADDING_PX;
    map.fullHeight = static_cast<int>(box.height) + 2 * SAMPLE_PADDING_PX;

    // Reduced resolution when blur is strong enough to hide it.
    // Weak blur at half-res shows pixelation.
    map.width  = std::max(1, map.fullWidth / downscale);
    map.height = std::max(1, map.fullHeight / downscale);

    map.srcX0  = static_cast<int>(box.x) - SAMPLE_PADDING_PX;
    map.srcY0  = static_cast<int>(box.y) - SAMPLE_PADDING_PX;
    map.srcX1  = static_cast<int>(box.x + box.width) + SAMPLE_PADDING_PX;
    map.srcY1  = static_cast<int>(box.y + box.height) + SAMPLE_PADDING_PX;
    map.scaleX = static_cast<float>(map.width) / map.fullWidth;
    map.scaleY = static_cast<float>(map.height) / map.fullHeight;
    return map;
}

bool damageCoversBox(const CBox& box, const SP<Render::IFramebuffer>& framebuffer, const CRegion& damage,
                     const PHLMONITOR& monitor) {
    if (!framebuffer)
        return false;

    // Hyprland clips element damage to the monitor, and what a blit reads beyond the
    // framebuffer is cleared padding: neither can be missing from the damage.
    const double x1 = std::max(box.x, 0.0);
    const double y1 = std::max(box.y, 0.0);
    const double x2 = std::min(box.x + box.w, framebuffer->m_size.x);
    const double y2 = std::min(box.y + box.h, framebuffer->m_size.y);
    if (x2 <= x1 || y2 <= y1)
        return true;

    // Render damage is in the rotated monitor space while the box is in unrotated
    // framebuffer pixels: transform it like Hyprland does for the output damage.
    CRegion framebufferDamage = damage.copy();
    if (monitor)
        framebufferDamage.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                                    monitor->m_transformedSize.x, monitor->m_transformedSize.y);

    return CRegion(CBox{x1, y1, x2 - x1, y2 - y1}).subtract(framebufferDamage).empty();
}

bool sampleRegionCovered(const CBox& box, const SP<Render::IFramebuffer>& source, const CRegion& damage,
                         const PHLMONITOR& monitor) {
    // The exact source rect the blit reads.
    const auto map = sampleMapFor(box, 1);
    return damageCoversBox(CBox{static_cast<double>(map.srcX0), static_cast<double>(map.srcY0), static_cast<double>(map.srcX1 - map.srcX0),
                                static_cast<double>(map.srcY1 - map.srcY0)},
                           source, damage, monitor);
}

SFoldedBlur foldBlurPasses(float radius, int iterations) noexcept {
    const float totalRadius      = radius * std::sqrt(static_cast<float>(iterations));
    const float cappedRatio      = totalRadius / BLUR_SHADER_TAP_CAP;
    const int   foldedIterations = std::max(1, static_cast<int>(std::ceil(cappedRatio * cappedRatio)));

    if (foldedIterations >= iterations)
        return {radius, iterations};

    return {totalRadius / std::sqrt(static_cast<float>(foldedIterations)), foldedIterations};
}

void sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int downscale) {
    if (!sourceFramebuffer)
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::SampleBackground);

    const int  pad = SAMPLE_PADDING_PX;
    const auto map = sampleMapFor(box, downscale);

    int fullWidth  = map.fullWidth;
    int fullHeight = map.fullHeight;

    // Full-res source pixels this call blits, before any downscale — what the
    // GPU actually reads off the source framebuffer, not the (possibly
    // half-res) destination the sample FBO ends up holding.
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordSampledPixels(monitor->m_id, static_cast<double>(fullWidth) * static_cast<double>(fullHeight));

    int sampleWidth  = map.width;
    int sampleHeight = map.height;

    if (!sampleFramebuffer)
        sampleFramebuffer = g_pHyprRenderer->createFB("hyprglass-sample");

    // the format follows the monitor framebuffer, which changes with cm/bitdepth
    if (sampleFramebuffer->m_size.x != sampleWidth || sampleFramebuffer->m_size.y != sampleHeight ||
        sampleFramebuffer->m_drmFormat != sourceFramebuffer->m_drmFormat)
        sampleFramebuffer->alloc(sampleWidth, sampleHeight, sourceFramebuffer->m_drmFormat);

    int srcX0 = map.srcX0;
    int srcX1 = map.srcX1;
    int srcY0 = map.srcY0;
    int srcY1 = map.srcY1;

    // Clamp source coordinates to framebuffer bounds to avoid reading black/undefined pixels
    int framebufferWidth  = static_cast<int>(sourceFramebuffer->m_size.x);
    int framebufferHeight = static_cast<int>(sourceFramebuffer->m_size.y);

    // Destination coords in downscaled FBO space
    int dstX0 = 0, dstY0 = 0, dstX1 = sampleWidth, dstY1 = sampleHeight;

    // Scale destination adjustments proportionally for the downscaled FBO
    const float xScale = map.scaleX;
    const float yScale = map.scaleY;

    // Tracks whether any clamp shrank the destination rect below the full FBO,
    // which is the only case that can leave uninitialized texels after the blit.
    bool destinationClamped = false;

    if (srcX0 < 0) { dstX0 += static_cast<int>(-srcX0 * xScale); srcX0 = 0; destinationClamped = true; }
    if (srcY0 < 0) { dstY0 += static_cast<int>(-srcY0 * yScale); srcY0 = 0; destinationClamped = true; }
    if (srcX1 > framebufferWidth)  { dstX1 -= static_cast<int>((srcX1 - framebufferWidth) * xScale);  srcX1 = framebufferWidth; destinationClamped = true; }
    if (srcY1 > framebufferHeight) { dstY1 -= static_cast<int>((srcY1 - framebufferHeight) * yScale); srcY1 = framebufferHeight; destinationClamped = true; }

    // Padding ratio is relative to the logical content area (resolution-independent)
    outPaddingRatio = Vector2D(
        static_cast<double>(pad) / fullWidth,
        static_cast<double>(pad) / fullHeight
    );

    // The render pass scissors each element to its damage region.
    // That scissor state leaks here and clips glBlitFramebuffer on the
    // DRAW framebuffer, causing partial writes and stale noise artifacts.
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    // the tracker skips glDisable when it believes the test is already off
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glDisable(GL_SCISSOR_TEST);
        Diagnostics::recordStateDesync("scissor on before the background blit");
    }

    // Clear the sample FBO before blitting only when the blit destination
    // doesn't cover the whole FBO. Clamped regions (near monitor edges)
    // would otherwise leave uninitialized GPU memory (pink artifacts) outside
    // the blit; a full-rect blit overwrites every texel, so the clear is redundant.
    if (destinationClamped) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
                      dstX0, dstY0, dstX1, dstY1,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void blendOwnContent(SP<Render::IFramebuffer>& sampleFramebuffer, PHLWINDOW window, PHLMONITOR monitor,
                      const CBox& box, int downscale, float amount, float cornerRadius, float roundingPower) {
    if (amount <= 0.0f || !sampleFramebuffer || !window || !monitor)
        return;

    // The sample framebuffer holds already-rotated framebuffer-space pixels, while a
    // texture drawn under RPT_EXPORT lands axis-aligned: on a 90/270 output the self
    // image would come out rotated. Skipping keeps those outputs at today's look.
    if (monitor->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        return;

    // Hyprland renders a transformed window through a redirected pass and blits the
    // result; our untransformed copy would not match what the user sees.
    if (!window->m_transformers.empty())
        return;

    const auto hlSurface = window->wlSurface();
    const auto root      = hlSurface ? hlSurface->resource() : nullptr;
    if (!root)
        return;

    const auto     map    = sampleMapFor(box, downscale);
    const Vector2D fbSize = sampleFramebuffer->m_size;

    // The map only describes this framebuffer if sampleBackground() sized it from the
    // same box; otherwise boxes would project against one size and rasterise into another.
    if (fbSize.x != map.width || fbSize.y != map.height)
        return;

    auto& renderData = g_pHyprRenderer->m_renderData;

    // Leave the caller the state sampleBackground leaves: scissor off, viewport from
    // the re-bound framebuffer's own size (monitor sizes are wrong here, #41).
    // Declared before the FB guard so it runs after that framebuffer is back, on the
    // throwing path too.
    const Hyprutils::Utils::CScopeGuard restoreGLState([&] {
        g_pHyprOpenGL->scissor(nullptr);
        if (const auto& restored = renderData.currentFB)
            g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(restored->m_size.x), static_cast<int>(restored->m_size.y));
    });

    auto guard = g_pHyprRenderer->bindTempFB(sampleFramebuffer);

    const auto  savedProjection      = renderData.projectionType;
    const auto  savedFbSize          = renderData.fbSize;
    const auto  savedRenderModif     = renderData.renderModif;
    const auto  savedWindow          = renderData.currentWindow;
    const auto  savedSurface         = renderData.surface;
    const auto  savedClipBox         = renderData.clipBox;
    const auto  savedUVTopLeft       = renderData.primarySurfaceUVTopLeft;
    const auto  savedUVBottomRight   = renderData.primarySurfaceUVBottomRight;
    const bool  savedTransformDamage = renderData.transformDamage;

    // draw() allocates a pass element per surface, so the restores must survive a
    // throw: every later element would otherwise project through the sample size.
    // Only render data, no GL state, so running after the FB rebind below is safe.
    const Hyprutils::Utils::CScopeGuard restoreRenderData([&] {
        renderData.primarySurfaceUVBottomRight = savedUVBottomRight;
        renderData.primarySurfaceUVTopLeft     = savedUVTopLeft;
        renderData.clipBox                     = savedClipBox;
        renderData.surface                     = savedSurface;
        renderData.currentWindow               = savedWindow;
        renderData.renderModif                 = savedRenderModif;
        renderData.transformDamage             = savedTransformDamage;
        // before setProjectionType: RPT_FB recomputes the projection from fbSize
        renderData.fbSize = savedFbSize;
        g_pHyprRenderer->setProjectionType(savedProjection);
    });

    renderData.fbSize = fbSize;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    // our boxes and damage rects are sample-framebuffer pixels, not monitor space
    renderData.transformDamage = false;
    // an overview plugin's modifier would otherwise be applied a second time
    renderData.renderModif = {};
    // a monitor-space clip would scissor the injection to the wrong rectangle
    renderData.clipBox = {};
    // Only for the rgbx shader variant, the one thing the texture draw cannot be told
    // any other way. It also lets dim_inactive and the not-responding tint into the
    // self image of those windows: accepted, there is no allowDim on this draw path.
    if (window->m_ruleApplicator && window->m_ruleApplicator->RGBX().valueOrDefault())
        renderData.currentWindow = window;

    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(fbSize.x), static_cast<int>(fbSize.y));
    // Premultiplied source-over. Not restored afterwards: m_blend is private, and every
    // element boundary already leaves blending on with this same function.
    g_pHyprRenderer->blend(true);

    // Must be non-empty: renderTextureInternal drops empty damage, and an empty
    // region here makes Hyprland substitute its own, which is in monitor space.
    const CRegion fullDamage = CBox{0, 0, fbSize.x, fbSize.y};

    root->breadthfirst(
        [&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
            const auto texture = surface->m_current.texture;
            // renderTextureInternal asserts on both, and an assert kills the compositor
            if (!texture || !texture->ok())
                return;
            if (surface->m_current.size.x < 1 || surface->m_current.size.y < 1)
                return;

            const bool mainSurface = surface == root;

            CBox       framebufferBox = box;
            if (!mainSurface) {
                framebufferBox = CBox{box.x + offset.x * monitor->m_scale, box.y + offset.y * monitor->m_scale,
                                      surface->m_current.size.x * monitor->m_scale, surface->m_current.size.y * monitor->m_scale};
                framebufferBox.round();
            }

            // Viewporter source crop, the one surface-state correction video players
            // need. Small and misaligned surfaces keep their uncorrected placement.
            // Each surface also blends at the same alpha, so where a subsurface covers
            // the main one the desktop share is (1-amount)^2: visible as a ghost of the
            // main surface at mid values, gone at 1.0. Fixing it needs a scratch target.
            const auto& viewport   = surface->m_current.viewport;
            const auto& bufferSize = surface->m_current.bufferSize;
            bool        customUV   = false;
            if (viewport.hasSource && bufferSize.x > 0 && bufferSize.y > 0) {
                const Vector2D uvTopLeft{viewport.source.x / bufferSize.x, viewport.source.y / bufferSize.y};
                const Vector2D uvBottomRight{(viewport.source.x + viewport.source.width) / bufferSize.x,
                                             (viewport.source.y + viewport.source.height) / bufferSize.y};
                if (uvBottomRight.x > 0.00001 && uvBottomRight.y > 0.00001) {
                    renderData.primarySurfaceUVTopLeft     = uvTopLeft;
                    renderData.primarySurfaceUVBottomRight = uvBottomRight;
                    customUV                               = true;
                }
            }
            if (!customUV) {
                renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
            }

            g_pHyprRenderer->draw(CTexPassElement::SRenderData{
                .tex           = texture,
                .box           = map.toSample(framebufferBox),
                .a             = amount,
                .damage        = fullDamage,
                .round         = mainSurface ? static_cast<int>(cornerRadius * map.scaleX) : 0,
                .roundingPower = roundingPower,
                .allowCustomUV = customUV,
                .surface       = surface,
            });

            // The texture path tracks no buffer of its own, and the window's real draw
            // may be discarded as occluded, releasing the buffer we just read.
            if (surface->m_current.buffer && !surface->m_current.buffer->isSynchronous())
                g_pHyprRenderer->m_usedAsyncBuffers.emplace_back(surface->m_current.buffer);
        },
        nullptr);

    guard.reset();
}

void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, float radius, int iterations,
                    SP<Render::IFramebuffer> callerFramebuffer, SP<Render::IFramebuffer>* temporaryFramebuffer) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!sampleFramebuffer || !callerFramebuffer || radius <= 0.0f || iterations <= 0 || !shaderManager.isInitialized())
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::BlurBackground);

    // Two ping-pong passes (horizontal, vertical) per iteration.
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordBlurPasses(monitor->m_id, static_cast<uint64_t>(iterations) * 2);

    int width  = static_cast<int>(sampleFramebuffer->m_size.x);
    int height = static_cast<int>(sampleFramebuffer->m_size.y);

    auto& blurTempFramebuffer = temporaryFramebuffer ? *temporaryFramebuffer : g_pGlobalState->blurTempFramebuffer;
    if (!blurTempFramebuffer)
        blurTempFramebuffer = g_pHyprRenderer->createFB("hyprglass-blur-temp");

    if (blurTempFramebuffer->m_size.x != width || blurTempFramebuffer->m_size.y != height ||
        blurTempFramebuffer->m_drmFormat != sampleFramebuffer->m_drmFormat)
        blurTempFramebuffer->alloc(width, height, sampleFramebuffer->m_drmFormat);

    // An allocation that failed (out of GPU memory) leaves no texture to ping-pong through.
    if (!blurTempFramebuffer->isAllocated() || !blurTempFramebuffer->getTexture() || !sampleFramebuffer->getTexture())
        return;

    // Fullscreen quad projection: maps VAO positions [0,1] to clip space [-1,1]
    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    const auto& blurUniforms = shaderManager.blurUniforms;

    // Each pass overwrites its whole target: blending would mix in the temp FBO's
    // previous contents, a leaked scissor or stencil would leave texels unwritten.
    g_pHyprRenderer->blend(false);
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glDisable(GL_SCISSOR_TEST);
        Diagnostics::recordStateDesync("scissor on before the blur passes");
    }
    if (glIsEnabled(GL_STENCIL_TEST)) {
        glDisable(GL_STENCIL_TEST);
        Diagnostics::recordStateDesync("stencil on before the blur passes");
    }

    auto shader = g_pHyprOpenGL->useShader(shaderManager.blurShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1f(blurUniforms.radius, radius);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);

    // Ping-pong at full resolution: sampleFramebuffer ↔ blurTempFramebuffer
    for (int iteration = 0; iteration < iterations; iteration++) {
        // Horizontal pass: sampleFramebuffer → blurTempFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(blurTempFramebuffer));
        sampleFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 1.0f / width, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Vertical pass: blurTempFramebuffer → sampleFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        blurTempFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 0.0f, 1.0f / height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Restore caller's GL state without querying (avoids pipeline stalls).
    // The viewport must match the re-bound framebuffer's own size: monitor
    // sizes are wrong here on 90°/270° monitors, where m_transformedSize is
    // swapped relative to the framebuffer's native orientation (#41).
    g_pHyprRenderer->blend(true); // Hyprland's state at every element boundary
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0,
        static_cast<int>(callerFramebuffer->m_size.x),
        static_cast<int>(callerFramebuffer->m_size.y));
}

bool buildMaskField(SP<Render::IFramebuffer>& fieldFramebuffer, SMaskInfo& mask, const Vector2D& maskTextureSize,
                    const CBox& box, float sigmaPx, SP<Render::IFramebuffer> callerFramebuffer) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized() || !callerFramebuffer || mask.textureId == 0 || sigmaPx < 0.5f ||
        box.w < 1.0 || box.h < 1.0 || maskTextureSize.x < 1.0 || maskTextureSize.y < 1.0)
        return false;

    // Room around the box for the blur to fall off into "not covered".
    const int padPx      = static_cast<int>(std::ceil(sigmaPx * 3.0f));
    const int fullWidth  = static_cast<int>(box.w) + 2 * padPx;
    const int fullHeight = static_cast<int>(box.h) + 2 * padPx;
    const int width      = std::max(1, fullWidth / FIELD_DOWNSCALE);
    const int height     = std::max(1, fullHeight / FIELD_DOWNSCALE);

    // Half-float: at 8 bits the field has plateaus, and the bezel normal is its gradient.
    static DRMFormat fieldFormat = DRM_FORMAT_ABGR16161616F;
    const auto ensure = [&](SP<Render::IFramebuffer>& framebuffer, const char* name) {
        if (!framebuffer)
            framebuffer = g_pHyprRenderer->createFB(name);
        if (framebuffer->m_size.x == width && framebuffer->m_size.y == height && framebuffer->m_drmFormat == fieldFormat)
            return true;
        if (framebuffer->alloc(width, height, fieldFormat))
            return true;
        if (fieldFormat == DRM_FORMAT_ARGB8888)
            return false;
        fieldFormat = DRM_FORMAT_ARGB8888; // no renderable half-float on this GPU
        return framebuffer->alloc(width, height, fieldFormat);
    };

    auto& scratch = g_pGlobalState->fieldTempFramebuffer;
    if (!ensure(scratch, "hyprglass-field-temp") || !ensure(fieldFramebuffer, "hyprglass-field"))
        return false;

    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    const auto& uniforms = shaderManager.fieldUniforms;

    g_pHyprRenderer->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    if (glIsEnabled(GL_SCISSOR_TEST))
        glDisable(GL_SCISSOR_TEST);
    if (glIsEnabled(GL_STENCIL_TEST))
        glDisable(GL_STENCIL_TEST);

    auto shader = g_pHyprOpenGL->useShader(shaderManager.fieldShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);

    const float stepPx = static_cast<float>(FIELD_DOWNSCALE);
    glUniform1f(uniforms.sigma, sigmaPx / stepPx);
    glUniform1f(uniforms.threshold, mask.alphaThreshold);

    // Horizontal: layer surface (monitor-sized texture) -> scratch, thresholding as it goes.
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(scratch));
    glBindTexture(mask.target, mask.textureId);
    glUniform1i(uniforms.binarize, 1);
    glUniform1i(uniforms.encode, 0);
    glUniform2f(uniforms.uvScale, static_cast<float>(fullWidth / maskTextureSize.x), static_cast<float>(fullHeight / maskTextureSize.y));
    glUniform2f(uniforms.uvOffset, static_cast<float>((box.x - padPx) / maskTextureSize.x), static_cast<float>((box.y - padPx) / maskTextureSize.y));
    glUniform4f(uniforms.uvClamp,
        static_cast<float>(box.x / maskTextureSize.x), static_cast<float>(box.y / maskTextureSize.y),
        static_cast<float>((box.x + box.w) / maskTextureSize.x), static_cast<float>((box.y + box.h) / maskTextureSize.y));
    glUniform2f(uniforms.direction, stepPx / static_cast<float>(maskTextureSize.x), 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Vertical: scratch -> field.
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(fieldFramebuffer));
    scratch->getTexture()->bind();
    glUniform1i(uniforms.binarize, 0);
    glUniform1i(uniforms.encode, 1);
    glUniform2f(uniforms.uvScale, 1.0f, 1.0f);
    glUniform2f(uniforms.uvOffset, 0.0f, 0.0f);
    glUniform2f(uniforms.direction, 0.0f, 1.0f / static_cast<float>(height));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0,
        static_cast<int>(callerFramebuffer->m_size.x),
        static_cast<int>(callerFramebuffer->m_size.y));

    mask.fieldTextureId = fieldFramebuffer->getTexture()->m_texID;
    mask.fieldUVScale   = Vector2D(box.w / fullWidth, box.h / fullHeight);
    mask.fieldUVOffset  = Vector2D(static_cast<double>(padPx) / fullWidth, static_cast<double>(padPx) / fullHeight);
    mask.fieldSigmaPx   = sigmaPx;
    return true;
}

bool buildFrostCopy(const SP<Render::IFramebuffer>& sample, SP<Render::IFramebuffer>& copy, SP<Render::IFramebuffer>& temporary,
                    float radius, int iterations, const SP<Render::IFramebuffer>& callerFramebuffer) {
    if (!sample || !callerFramebuffer || !g_pGlobalState->shaderManager.isInitialized())
        return false;

    const int sampleWidth  = static_cast<int>(sample->m_size.x);
    const int sampleHeight = static_cast<int>(sample->m_size.y);
    const int width        = std::max(1, sampleWidth / 2);
    const int height       = std::max(1, sampleHeight / 2);
    if (sampleWidth < 1 || sampleHeight < 1)
        return false;

    if (!copy)
        copy = g_pHyprRenderer->createFB("hyprglass-frost");
    if (copy->m_size.x != width || copy->m_size.y != height || copy->m_drmFormat != sample->m_drmFormat)
        copy->alloc(width, height, sample->m_drmFormat);

    // Dropped, so the caller sees a missing copy and retries rather than drawing a texture-less one.
    if (!copy->isAllocated() || !copy->getTexture()) {
        copy.reset();
        return false;
    }

    // same as the sample blit: the render pass's damage scissor would clip it
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glDisable(GL_SCISSOR_TEST);
        Diagnostics::recordStateDesync("scissor on before the frost blit");
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sample));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(copy));
    glBlitFramebuffer(0, 0, sampleWidth, sampleHeight, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    blurBackground(copy, radius, iterations, callerFramebuffer, &temporary);

    // blurBackground() returns without binding when it has nothing to blur
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
    return true;
}

// The optional effects (ShaderVariants.hpp) this draw's resolved settings turn on.
// Must stay GLASS_FEATURE_NONE whenever every effect is at its off value, so the
// draw runs the default program.
static GlassFeatureMask glassFeaturesFor(float edgeHeight, float focusLight, bool frost, bool contour) {
    GlassFeatureMask features = GLASS_FEATURE_NONE;
    if (edgeHeight > 0.0f)
        features |= GLASS_FEATURE_LENS_EDGE;
    if (focusLight > 0.0f)
        features |= GLASS_FEATURE_FOCUS_LIGHT;
    if (frost)
        features |= GLASS_FEATURE_FROST;
    if (contour)
        features |= GLASS_FEATURE_CONTOUR;
    return features;
}

static constexpr int FROST_TEXTURE_UNIT   = 2; // 0 is the sample, 1 the layer surface mask
static constexpr int CONTOUR_TEXTURE_UNIT = 3; // layers only, so never at the same time as frost

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, const std::array<float, 4>& radii, float roundingPower,
                       const Vector2D& paddingRatio, const SResolveContext& resolveContext,
                       const SMaskInfo* mask, float focusLight, const SFrostInput* frost) {
    if (!sampleFramebuffer || !targetFramebuffer)
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::ApplyGlassEffect);

    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordGlassPixels(monitor->m_id, rawBox.w * rawBox.h);

    const float edgeHeightValue =
        std::min(resolvePresetFloat(resolveContext, &SPresetValues::edgeHeight, &SOverridableConfig::edgeHeight), EDGE_HEIGHT_MAX);
    // windows only: a masked draw is a layer or an item
    const float focusLightValue = !mask && focusLight > 0.0f ? std::min(focusLight, 1.0f) : 0.0f;
    // windows only, like the light
    const bool  frosted         = !mask && frost && frost->copy && frost->copy->getTexture() && frost->amount > Frost::MINIMUM_AMOUNT;
    // layers only too, and only when the layer has a field to draw
    const bool  wantsContour    = mask && mask->contourTextureId != 0 && mask->contourReferencePx > 0.0f;
    const auto  features        = glassFeaturesFor(edgeHeightValue, focusLightValue, frosted, wantsContour);

    // Before any GL binding: a variant's first use compiles it, which rebinds the VAO and array buffer.
    const auto& program  = g_pGlobalState->shaderManager.glassProgram(features);
    const auto& uniforms = program.uniforms;
    // A variant that failed to compile falls back to the default program: the layer then keeps its rectangle.
    const bool  contoured = wantsContour && (program.features & GLASS_FEATURE_CONTOUR);

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));

    Mat3x3 glMatrix = g_pHyprRenderer->projectBoxToTarget(rawBox, transform);
    auto texture    = sampleFramebuffer->getTexture();

    glMatrix.transpose();

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    glActiveTexture(GL_TEXTURE0);
    texture->bind();

    if (frosted) {
        glActiveTexture(GL_TEXTURE0 + FROST_TEXTURE_UNIT);
        frost->copy->getTexture()->bind();
        glActiveTexture(GL_TEXTURE0);
    }

    if (contoured) {
        glActiveTexture(GL_TEXTURE0 + CONTOUR_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D, mask->contourTextureId);
        glActiveTexture(GL_TEXTURE0);
    }

    // Layers only: bind the temp FBO texture (rendered surface) on texture unit 1.
    // The shader samples it to mask glass to visible content and composite surface on top.
    // Windows pass mask=nullptr so this block is skipped.
    if (mask && mask->textureId != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(mask->target, mask->textureId);
        if (mask->fieldTextureId != 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, mask->fieldTextureId);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    auto shader = g_pHyprOpenGL->useShader(program.shader);

    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);

    const auto fullSize = Vector2D(transformedBox.width, transformedBox.height);
    shader->setUniformFloat2(SHADER_FULL_SIZE,
        static_cast<float>(fullSize.x), static_cast<float>(fullSize.y));

    // a rounding_power window rule bypasses Hyprland's [2, 10] clamp; <= 0 is NaN in the SDF
    const float safeRoundingPower = std::max(roundingPower, 1.0f);

    // The glass shape within the drawn box (see SMaskInfo::glassBoxSizePx): only
    // subsurface items and shaped layers set one; the sentinel (negative) means the whole box.
    // Bezel, lens and refraction are sized and centred on it, not on the drawn box.
    Vector2D glassBoxOffsetPx{0.0, 0.0};
    Vector2D glassBoxSizePx = fullSize;
    if (mask && !contoured && mask->glassBoxSizePx.x >= 0.0 && mask->glassBoxSizePx.y >= 0.0) {
        glassBoxOffsetPx = mask->glassBoxOffsetPx;
        glassBoxSizePx   = mask->glassBoxSizePx;
    }
    glUniform2f(uniforms.glassBoxOffsetPx, static_cast<float>(glassBoxOffsetPx.x), static_cast<float>(glassBoxOffsetPx.y));
    glUniform2f(uniforms.glassBoxSizePx, static_cast<float>(glassBoxSizePx.x), static_cast<float>(glassBoxSizePx.y));
    // x / x is exactly 1 and 0 / x exactly 0, so the whole-box case maps uv to itself bit for bit
    glUniform2f(uniforms.glassUVScale,
        static_cast<float>(glassBoxSizePx.x > 0.0 ? fullSize.x / glassBoxSizePx.x : 1.0),
        static_cast<float>(glassBoxSizePx.y > 0.0 ? fullSize.y / glassBoxSizePx.y : 1.0));
    glUniform2f(uniforms.glassUVOffset,
        static_cast<float>(glassBoxSizePx.x > 0.0 ? glassBoxOffsetPx.x / glassBoxSizePx.x : 0.0),
        static_cast<float>(glassBoxSizePx.y > 0.0 ? glassBoxOffsetPx.y / glassBoxSizePx.y : 0.0));

    // Reciprocals computed once per draw instead of once per pixel in the shader.
    // a contoured layer's edge is measured against a card's size, not the layer's
    const float minDimensionPx = contoured ? mask->contourReferencePx : static_cast<float>(std::min(glassBoxSizePx.x, glassBoxSizePx.y));
    glUniform2f(uniforms.invFullSize,
        1.0f / static_cast<float>(fullSize.x), 1.0f / static_cast<float>(fullSize.y));
    glUniform1f(uniforms.invRoundingPower, 1.0f / safeRoundingPower);

    const auto  monitor            = g_pHyprRenderer->m_renderData.pMonitor.lock();
    const float monitorScale       = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    const float edgeThicknessValue = effectiveEdgeThickness(
        resolvePresetFloat(resolveContext, &SPresetValues::edgeThickness, &SOverridableConfig::edgeThickness),
        resolvePresetFloat(resolveContext, &SPresetValues::edgeThicknessPx, &SOverridableConfig::edgeThicknessPx),
        monitorScale, minDimensionPx);
    const float lensDistortionValue = resolvePresetFloat(resolveContext, &SPresetValues::lensDistortion, &SOverridableConfig::lensDistortion);
    // Epsilon guard matches the shader's own degenerate-size behaviour (bezelWidthPx == 0 would divide by zero).
    glUniform1f(uniforms.invBezelWidthPx, 1.0f / std::max(edgeThicknessValue * minDimensionPx, 1e-4f));
    glUniform1f(uniforms.lensMaxPx, lensDistortionValue * minDimensionPx * 0.006f);

    // inf * 0 at a square lens corner would give NaN offsets
    const float refractionStrengthValue = resolvePresetFloat(resolveContext, &SPresetValues::refractionStrength, &SOverridableConfig::refractionStrength);
    glUniform1f(uniforms.refractionStrength,  std::isfinite(refractionStrengthValue) ? refractionStrengthValue : 0.0f);
    glUniform1f(uniforms.noiseStrength,       resolvePresetFloat(resolveContext, &SPresetValues::noiseStrength, &SOverridableConfig::noiseStrength));
    glUniform1f(uniforms.chromaticAberration, resolvePresetFloat(resolveContext, &SPresetValues::chromaticAberration, &SOverridableConfig::chromaticAberration));
    glUniform1f(uniforms.fresnelStrength,     resolvePresetFloat(resolveContext, &SPresetValues::fresnelStrength, &SOverridableConfig::fresnelStrength));
    glUniform1f(uniforms.specularStrength,    resolvePresetFloat(resolveContext, &SPresetValues::specularStrength, &SOverridableConfig::specularStrength));
    glUniform1f(uniforms.specularAngle,       resolvePresetFloat(resolveContext, &SPresetValues::specularAngle, &SOverridableConfig::specularAngle));
    glUniform1f(uniforms.glassOpacity,        resolvePresetFloat(resolveContext, &SPresetValues::glassOpacity, &SOverridableConfig::glassOpacity) * alpha);
    glUniform1f(uniforms.edgeThickness,       edgeThicknessValue);
    glUniform1f(uniforms.lensDistortion,      lensDistortionValue);
    glUniform1f(uniforms.refractionFlow,      resolvePresetFloat(resolveContext, &SPresetValues::refractionFlow, &SOverridableConfig::refractionFlow));
    glUniform1f(uniforms.refractionSpread,    resolvePresetFloat(resolveContext, &SPresetValues::refractionSpread, &SOverridableConfig::refractionSpread));
    glUniform1f(uniforms.fresnelTint,         resolvePresetFloat(resolveContext, &SPresetValues::fresnelTint, &SOverridableConfig::fresnelTint));
    glUniform1f(uniforms.bevelStrength,       resolvePresetFloat(resolveContext, &SPresetValues::bevelStrength, &SOverridableConfig::bevelStrength));
    glUniform1f(uniforms.bevelSize,           resolvePresetFloat(resolveContext, &SPresetValues::bevelSize, &SOverridableConfig::bevelSize));
    glUniform1f(uniforms.bevelTint,           resolvePresetFloat(resolveContext, &SPresetValues::bevelTint, &SOverridableConfig::bevelTint));
    glUniform1f(uniforms.bevelAngle,          resolvePresetFloat(resolveContext, &SPresetValues::bevelAngle, &SOverridableConfig::bevelAngle));
    glUniform1f(uniforms.bevelShadow,         resolvePresetFloat(resolveContext, &SPresetValues::bevelShadow, &SOverridableConfig::bevelShadow));

    // bevel width is defined in logical px; scale it to framebuffer px per monitor
    glUniform1f(uniforms.monitorScale, monitorScale);

    if (features & GLASS_FEATURE_LENS_EDGE) {
        const float reflection =
            resolvePresetFloat(resolveContext, &SPresetValues::fresnelReflection, &SOverridableConfig::fresnelReflection);
        glUniform1f(uniforms.lensHeightPx, edgeHeightValue * monitorScale);
        glUniform1f(uniforms.lensReflection, reflection > 0.0f ? std::min(reflection, 1.0f) : 0.0f);
        glUniform1f(uniforms.lensReflectionReachPx, LENS_REFLECTION_REACH_PX);
    }

    if (features & GLASS_FEATURE_FOCUS_LIGHT) {
        const int64_t focusLightColorValue =
            resolvePresetInt(resolveContext, &SPresetValues::focusLightColor, &SOverridableConfig::focusLightColor);
        glUniform1f(uniforms.focusLight, focusLightValue);
        glUniform3f(uniforms.focusLightColor,
            static_cast<float>((focusLightColorValue >> 24) & 0xFF) / 255.0f,
            static_cast<float>((focusLightColorValue >> 16) & 0xFF) / 255.0f,
            static_cast<float>((focusLightColorValue >> 8) & 0xFF) / 255.0f);
        glUniform1f(uniforms.focusLightColorAlpha,
            static_cast<float>(focusLightColorValue & 0xFF) / 255.0f);
    }

    if (features & GLASS_FEATURE_FROST) {
        glUniform1i(uniforms.frostTex, FROST_TEXTURE_UNIT);
        glUniform1f(uniforms.frostAmount, std::min(frost->amount, 1.0f));
    }

    if (contoured) {
        glUniform1i(uniforms.contourTex, CONTOUR_TEXTURE_UNIT);
        glUniform2f(uniforms.contourUVScale, static_cast<float>(mask->contourUVScale.x), static_cast<float>(mask->contourUVScale.y));
        glUniform1f(uniforms.contourReferencePx, mask->contourReferencePx);
        glUniform1f(uniforms.contourReachPx, mask->contourReachPx);
    }

    uploadThemeUniforms(resolveContext, program);

    // Clear / Tinted: tinted panes lean toward the theme's own surface colour.
    glUniform1f(uniforms.tinted, tintedAmount());
    if (resolveContext.isDark)
        glUniform3f(uniforms.tintedColor, 0.075f, 0.075f, 0.085f);
    else
        glUniform3f(uniforms.tintedColor, 0.93f, 0.93f, 0.95f);
    const auto& cfg = g_pGlobalState->config;
    glUniform1f(uniforms.rimLevel, cfg.edgeHighlight ? std::max(static_cast<float>(**cfg.edgeHighlight), 0.0f) : 1.0f);

    // Body lens: how far (px) what is behind a pane is pulled in at its rim.
    // Grows with the pane, capped. Shell layers that cover the whole monitor
    // (overview) hold several separate panes: no single lens fits them.
    float bodyLensPx = lensDistortionValue * std::min(0.14f * minDimensionPx, 130.0f * (monitor ? static_cast<float>(monitor->m_scale) : 1.0f));
    if (cfg.lens)
        bodyLensPx *= std::max(static_cast<float>(**cfg.lens), 0.0f);
    if (mask && monitor && rawBox.w >= 0.9 * monitor->m_pixelSize.x && rawBox.h >= 0.9 * monitor->m_pixelSize.y)
        bodyLensPx = 0.0f;
    glUniform1f(uniforms.bodyLensPx, bodyLensPx);

    // Touch light: the press point in this box's own pixels. Rotated/flipped
    // monitors don't get it (rawBox and the box-local frame differ there).
    const auto light = TouchLight::current();
    if (light.strength > 0.f && monitor && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL) {
        const Vector2D local = (light.posGlobal - monitor->m_position) * monitor->m_scale;
        glUniform2f(uniforms.pressPosPx, static_cast<float>(local.x - rawBox.x), static_cast<float>(local.y - rawBox.y));
        glUniform1f(uniforms.pressGlow, light.strength);
        glUniform1f(uniforms.pressRadiusPx, light.radius * monitor->m_scale);
    } else
        glUniform1f(uniforms.pressGlow, 0.0f);

    const int64_t tintColorValue = resolvePresetInt(resolveContext, &SPresetValues::tintColor, &SOverridableConfig::tintColor);
    glUniform3f(uniforms.tintColor,
        static_cast<float>((tintColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 8) & 0xFF) / 255.0f);
    glUniform1f(uniforms.tintAlpha,
        static_cast<float>(tintColorValue & 0xFF) / 255.0f);

    const int64_t fresnelColorValue = resolvePresetInt(resolveContext, &SPresetValues::fresnelColor, &SOverridableConfig::fresnelColor);
    glUniform3f(uniforms.fresnelColor,
        static_cast<float>((fresnelColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((fresnelColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((fresnelColorValue >> 8) & 0xFF) / 255.0f);
    glUniform1f(uniforms.fresnelColorAlpha,
        static_cast<float>(fresnelColorValue & 0xFF) / 255.0f);

    const int64_t bevelColorValue = resolvePresetInt(resolveContext, &SPresetValues::bevelColor, &SOverridableConfig::bevelColor);
    glUniform3f(uniforms.bevelColor,
        static_cast<float>((bevelColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((bevelColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((bevelColorValue >> 8) & 0xFF) / 255.0f);
    glUniform1f(uniforms.bevelColorAlpha,
        static_cast<float>(bevelColorValue & 0xFF) / 255.0f);

    glUniform2f(uniforms.uvPadding,
        static_cast<float>(paddingRatio.x),
        static_cast<float>(paddingRatio.y));

    // Layers only: enable mask and provide UV mapping from the glass quad into
    // the monitor-sized temp FBO. Windows use useMask=0 (no masking).
    if (mask && mask->textureId != 0) {
        glUniform1i(uniforms.useMask, 1);
        glUniform1i(uniforms.maskTex, 1);
        glUniform2f(uniforms.maskUVOffset,
            static_cast<float>(mask->uvOffset.x),
            static_cast<float>(mask->uvOffset.y));
        glUniform2f(uniforms.maskUVScale,
            static_cast<float>(mask->uvScale.x),
            static_cast<float>(mask->uvScale.y));
        glUniform1f(uniforms.maskAlphaThreshold, mask->alphaThreshold);
        glUniform1f(uniforms.maskCoverage, mask->coverage);
        glUniform1i(uniforms.maskMode, mask->maskMode);
        glUniform1i(uniforms.regionRectCount, mask->regionRectCount);
        if (mask->regionRectCount > 0)
            glUniform4fv(uniforms.regionRects, mask->regionRectCount,
                         reinterpret_cast<const float*>(mask->regionRects.data()));
        glUniform2f(uniforms.sampleUVOffset,
            static_cast<float>(mask->sampleUVOffset.x), static_cast<float>(mask->sampleUVOffset.y));
        glUniform2f(uniforms.sampleUVScale,
            static_cast<float>(mask->sampleUVScale.x), static_cast<float>(mask->sampleUVScale.y));
        glUniform1i(uniforms.fieldTex, 2);
        glUniform1i(uniforms.useField, mask->fieldTextureId != 0 ? 1 : 0);
        glUniform2f(uniforms.fieldUVOffset,
            static_cast<float>(mask->fieldUVOffset.x), static_cast<float>(mask->fieldUVOffset.y));
        glUniform2f(uniforms.fieldUVScale,
            static_cast<float>(mask->fieldUVScale.x), static_cast<float>(mask->fieldUVScale.y));
        glUniform1f(uniforms.fieldSigmaPx, mask->fieldSigmaPx);
    } else {
        glUniform1i(uniforms.useField, 0);
        glUniform1i(uniforms.useMask, 0);
        glUniform1f(uniforms.maskAlphaThreshold, 0.001f);
        glUniform1f(uniforms.maskCoverage, 0.0f);
        glUniform1i(uniforms.maskMode, 0);
        glUniform1i(uniforms.regionRectCount, 0);
        // Windows, and layers outside PROTOCOL_REGION, always sample the same
        // box they draw — identity, since this shader program's uniforms
        // persist across draws that don't set them (a prior region-mode
        // layer's non-identity value would otherwise leak into this draw).
        glUniform2f(uniforms.sampleUVOffset, 0.0f, 0.0f);
        glUniform2f(uniforms.sampleUVScale, 1.0f, 1.0f);
    }

    glUniform4f(uniforms.radii, radii[0], radii[1], radii[2], radii[3]);
    shader->setUniformFloat(SHADER_ROUNDING_POWER, safeRoundingPower);

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));

    // Only finalDamage is copied to the screen, and elementDamage (which finalDamage is a
    // subset of) already covers every pixel we're visible at, so scissoring to the damage
    // clips no pixel that would otherwise reach the screen.
    CBox damageExtents = g_pHyprRenderer->m_renderData.damage.copy().intersect(rawBox).getExtents();
    g_pHyprOpenGL->scissor(damageExtents.w > 0 && damageExtents.h > 0 ? damageExtents : rawBox);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_pHyprOpenGL->scissor(nullptr);
}

} // namespace GlassRenderer
