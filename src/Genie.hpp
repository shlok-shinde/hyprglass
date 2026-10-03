#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <optional>
#include <string>
#include <string_view>

// Magic-lamp ("genie") minimize and restore.
//
// A window is minimized by pouring it into a target rectangle (a dock icon, or
// the bottom centre of its monitor when no target is given) and then parking it
// on the special workspace `special:minimized`. Restoring reverses both.
//
// The warp is a Hyprland window transformer: Hyprland renders the window, glass
// decoration included, into a scratch framebuffer, hands that to us, and draws
// whatever framebuffer we hand back.
namespace Genie {

inline constexpr auto MINIMIZED_WORKSPACE = "special:minimized";

void init(HANDLE handle);
void shutdown();

// True while a window is mid-animation (or being held invisible right after
// one). Its glass keeps the backdrop it sampled beforehand: the redirected pass
// a transformed window renders into has no desktop behind it to sample.
[[nodiscard]] bool isAnimating(const PHLWINDOW& window);

// `hyprctl hyprglass minimize|restore|minimized|launch ...`. nullopt when
// `request` is not a genie command. `launch <class[,class...]> x y w h` makes
// the next window of that app open out of the rect (its dock icon).
[[nodiscard]] std::optional<std::string> handleHyprctl(std::string_view request, bool json);

} // namespace Genie
