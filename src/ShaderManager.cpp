#include "ShaderManager.hpp"
#include "Globals.hpp"
#include "Shaders.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>

std::string CShaderManager::loadShaderSource(const char* fileName) {
    if (SHADERS.contains(fileName))
        return SHADERS.at(fileName);

    const std::string message = std::format("[{}] Failed to load shader: {}", PLUGIN_NAME, fileName);
    HyprlandAPI::addNotification(PHANDLE, message, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error(message);
}

bool CShaderManager::compileGlassShader() {
    if (!glassShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("liquidglass.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile glass shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = glassShader->program();

    glassUniforms.refractionStrength  = glGetUniformLocation(program, "refractionStrength");
    glassUniforms.chromaticAberration = glGetUniformLocation(program, "chromaticAberration");
    glassUniforms.fresnelStrength     = glGetUniformLocation(program, "fresnelStrength");
    glassUniforms.specularStrength    = glGetUniformLocation(program, "specularStrength");
    glassUniforms.glassOpacity        = glGetUniformLocation(program, "glassOpacity");
    glassUniforms.edgeThickness       = glGetUniformLocation(program, "edgeThickness");
    glassUniforms.invBezelWidthPx     = glGetUniformLocation(program, "invBezelWidthPx");
    glassUniforms.uvPadding           = glGetUniformLocation(program, "uvPadding");
    glassUniforms.tintColor           = glGetUniformLocation(program, "tintColor");
    glassUniforms.tintAlpha           = glGetUniformLocation(program, "tintAlpha");
    glassUniforms.lensDistortion      = glGetUniformLocation(program, "lensDistortion");
    glassUniforms.lensMaxPx           = glGetUniformLocation(program, "lensMaxPx");
    glassUniforms.saturation          = glGetUniformLocation(program, "saturation");
    glassUniforms.vibrancyDarkness    = glGetUniformLocation(program, "vibrancyDarkness");
    glassUniforms.adaptiveDim         = glGetUniformLocation(program, "adaptiveDim");
    glassUniforms.adaptiveBoost       = glGetUniformLocation(program, "adaptiveBoost");
    glassUniforms.refractionFlow      = glGetUniformLocation(program, "refractionFlow");
    glassUniforms.refractionSpread    = glGetUniformLocation(program, "refractionSpread");
    glassUniforms.fresnelTint         = glGetUniformLocation(program, "fresnelTint");
    glassUniforms.bevelStrength       = glGetUniformLocation(program, "bevelStrength");
    glassUniforms.bevelSize           = glGetUniformLocation(program, "bevelSize");
    glassUniforms.monitorScale        = glGetUniformLocation(program, "monitorScale");
    glassUniforms.fresnelColor        = glGetUniformLocation(program, "fresnelColor");
    glassUniforms.fresnelColorAlpha   = glGetUniformLocation(program, "fresnelColorAlpha");
    glassUniforms.bevelColor          = glGetUniformLocation(program, "bevelColor");
    glassUniforms.bevelColorAlpha     = glGetUniformLocation(program, "bevelColorAlpha");
    glassUniforms.bevelTint           = glGetUniformLocation(program, "bevelTint");
    glassUniforms.bevelAngle          = glGetUniformLocation(program, "bevelAngle");
    glassUniforms.bevelShadow         = glGetUniformLocation(program, "bevelShadow");
    glassUniforms.specularAngle       = glGetUniformLocation(program, "specularAngle");
    glassUniforms.invFullSize         = glGetUniformLocation(program, "invFullSize");
    glassUniforms.invRoundingPower    = glGetUniformLocation(program, "invRoundingPower");
    glassUniforms.radii               = glGetUniformLocation(program, "radii");
    glassUniforms.maskTex             = glGetUniformLocation(program, "maskTex");
    glassUniforms.useMask             = glGetUniformLocation(program, "useMask");
    glassUniforms.maskUVOffset        = glGetUniformLocation(program, "maskUVOffset");
    glassUniforms.maskUVScale         = glGetUniformLocation(program, "maskUVScale");
    glassUniforms.maskAlphaThreshold  = glGetUniformLocation(program, "maskAlphaThreshold");
    glassUniforms.maskMode            = glGetUniformLocation(program, "maskMode");
    glassUniforms.regionRectCount     = glGetUniformLocation(program, "regionRectCount");
    glassUniforms.regionRects         = glGetUniformLocation(program, "regionRects[0]");
    if (glassUniforms.regionRects == -1)
        glassUniforms.regionRects = glGetUniformLocation(program, "regionRects");
    glassUniforms.sampleUVOffset      = glGetUniformLocation(program, "sampleUVOffset");
    glassUniforms.sampleUVScale       = glGetUniformLocation(program, "sampleUVScale");
    glassUniforms.glassBoxOffsetPx    = glGetUniformLocation(program, "glassBoxOffsetPx");
    glassUniforms.glassBoxSizePx      = glGetUniformLocation(program, "glassBoxSizePx");
    glassUniforms.fieldTex            = glGetUniformLocation(program, "fieldTex");
    glassUniforms.useField            = glGetUniformLocation(program, "useField");
    glassUniforms.fieldUVOffset       = glGetUniformLocation(program, "fieldUVOffset");
    glassUniforms.fieldUVScale        = glGetUniformLocation(program, "fieldUVScale");
    glassUniforms.fieldSigmaPx        = glGetUniformLocation(program, "fieldSigmaPx");
    glassUniforms.pressPosPx          = glGetUniformLocation(program, "pressPosPx");
    glassUniforms.pressGlow           = glGetUniformLocation(program, "pressGlow");
    glassUniforms.pressRadiusPx       = glGetUniformLocation(program, "pressRadiusPx");
    glassUniforms.tinted              = glGetUniformLocation(program, "tinted");
    glassUniforms.tintedColor         = glGetUniformLocation(program, "tintedColor");
    glassUniforms.rimLevel            = glGetUniformLocation(program, "rimLevel");

    return true;
}

bool CShaderManager::compileBlurShader() {
    if (!blurShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("gaussianblur.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile blur shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = blurShader->program();

    blurUniforms.direction = glGetUniformLocation(program, "direction");
    blurUniforms.radius    = glGetUniformLocation(program, "blurRadius");

    return true;
}

bool CShaderManager::compileFieldShader() {
    if (!fieldShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("maskfield.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile mask field shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = fieldShader->program();

    fieldUniforms.direction = glGetUniformLocation(program, "direction");
    fieldUniforms.sigma     = glGetUniformLocation(program, "sigma");
    fieldUniforms.binarize  = glGetUniformLocation(program, "binarize");
    fieldUniforms.encode    = glGetUniformLocation(program, "encode");
    fieldUniforms.threshold = glGetUniformLocation(program, "threshold");
    fieldUniforms.uvOffset  = glGetUniformLocation(program, "uvOffset");
    fieldUniforms.uvScale   = glGetUniformLocation(program, "uvScale");
    fieldUniforms.uvClamp   = glGetUniformLocation(program, "uvClamp");

    return true;
}

bool CShaderManager::compileGenieShader() {
    if (!genieShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("genie.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile genie shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = genieShader->program();

    genieUniforms.fbSize   = glGetUniformLocation(program, "fbSize");
    genieUniforms.srcBox   = glGetUniformLocation(program, "srcBox");
    genieUniforms.target   = glGetUniformLocation(program, "target");
    genieUniforms.progress = glGetUniformLocation(program, "progress");

    return true;
}

void CShaderManager::initializeIfNeeded() {
    if (m_initialized)
        return;

    if (!compileGlassShader())
        return;

    if (!compileBlurShader())
        return;

    if (!compileFieldShader())
        return;

    if (!compileGenieShader())
        return;

    m_initialized = true;
}

void CShaderManager::destroy() noexcept {
    glassShader->destroy();
    blurShader->destroy();
    fieldShader->destroy();
    genieShader->destroy();
    m_initialized = false;
}
