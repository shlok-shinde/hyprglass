#pragma once

#include <hyprland/src/helpers/math/Math.hpp>

#include <optional>
#include <string>
#include <string_view>

// Touch light. Liquid Glass has no fixed light source: no pane is lit from a
// corner. Light shows up where the material is touched. Pressing a mouse button
// on glass (a glassed window or shell panel) lights it from the press point:
// the rim of every pane within reach catches the light and small controls glow
// through. The light swells while the button is held, follows the pointer, and
// fades out after release. Clicks on anything that is not glass do nothing.
namespace TouchLight {

struct SLight {
    Vector2D posGlobal;      // logical, global layout coordinates
    float    strength = 0.f; // 0..1, 0 = no light
    float    radius   = 0.f; // logical px
};

void init();
void shutdown();

// The light for the frame being rendered now.
[[nodiscard]] SLight current();

// `hyprctl hyprglass touch X Y [hold_ms]`, `touch-hold X Y`, `touch-release`.
// nullopt when `request` is not a touch command.
[[nodiscard]] std::optional<std::string> handleHyprctl(std::string_view request, bool json);

} // namespace TouchLight
