#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include <optional>
#include <string>
#include <string_view>

// Touch light, after Liquid Glass's press glow. No pane is lit from a fixed
// corner by it: light enters glass where it is pressed (mouse button or touch
// screen). It starts under the pointer, fills a small pressed control, glows
// around the press point on a big pane, follows the pointer while held, and
// spreads onto nearby glass (other windows and panels too, press_spill) as it
// fades after release. Clicks on anything that is not glass do nothing.
// plugin:hyprglass:press_glow: 0 = off, 1 = subtle (Apple 26.1+), 2 = bright (26.0).
namespace TouchLight {

struct SLight {
    Vector2D posGlobal;          // logical, global layout coordinates
    float    strength    = 0.f;  // 0 = no light; springs toward 1 while held, may overshoot a little
    float    spread      = 0.f;  // the glow's reach as a share of its full radius (above 1 while it fades)
    float    spillRadius = 0.f;  // logical px the light reaches on panes that were not pressed
    float    wash        = 0.f;  // gain of the flat fill of a small pressed control
    float    radial      = 0.f;  // gain of the glow around the press point
    float    spill       = 0.f;  // share of that glow other panes catch
};

void init();
void shutdown();

// The light for the frame being rendered now.
[[nodiscard]] SLight current();

// Was the light pressed on this window / layer? It stays theirs while it fades.
[[nodiscard]] bool pressedOn(const PHLWINDOW& window);
[[nodiscard]] bool pressedOn(const PHLLS& layer);

// 0 = dark, 1 = held, 2 = fading. Folded into the render fingerprint (main.cpp)
// so glass stacked above lit panes resamples when the light comes and goes,
// and never keeps a lit rim in its cached backdrop.
[[nodiscard]] int phase();

// `hyprctl hyprglass touch X Y [hold_ms]`, `touch-hold X Y`, `touch-release`,
// `touch-path X1 Y1 X2 Y2 ms`, `touch-probe X Y`. nullopt when `request` is not
// a touch command.
[[nodiscard]] std::optional<std::string> handleHyprctl(std::string_view request, bool json);

} // namespace TouchLight
