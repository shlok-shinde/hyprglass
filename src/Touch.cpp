#include "Touch.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"
#include "PluginConfig.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <sstream>
#include <vector>

namespace TouchLight {

namespace {
    using Clock = std::chrono::steady_clock;

    // The glow's radius is this many times the pressed element's short side
    // (Kyant's Android Backdrop), the element capped at ELEMENT_CAP: a window
    // or a long bar lights like a control that size, around the press point.
    constexpr float RADIUS_PER_SIDE = 1.5f;
    constexpr float ELEMENT_CAP     = 160.f; // logical px
    // After release the light spreads out as it fades (Flutter's GlassGlow
    // blooms; Apple: it "spreads throughout the element and onto nearby glass")
    constexpr float BLOOM     = 3.f;   // to this many radii...
    constexpr float BLOOM_CAP = 300.f; // ...but no more than this many logical px further
    constexpr float MIN_HOLD_MS = 90.f; // a quick click still lights before it fades
    constexpr int   TICK_MS     = 8;

    // Springs (stiffness, damping ratio) for the light coming on and going out,
    // and the gains of the flat fill and the glow around the press point.
    struct SStyle {
        float pressK, pressZeta, releaseK, releaseZeta, wash, radial;
    };
    constexpr SStyle SUBTLE{300.f, 0.9f, 150.f, 1.0f, 0.05f, 0.10f}; // 26.1+: no flash, ~0.45 s out
    constexpr SStyle BRIGHT{300.f, 0.5f, 300.f, 0.5f, 0.08f, 0.15f}; // 26.0: ~16 % overshoot

    struct SSpring {
        float x = 0.f, v = 0.f;
        void  step(float target, float k, float zeta, float dt) {
            const float c = 2.f * zeta * std::sqrt(k);
            for (float left = dt; left > 1e-6f; left -= 0.002f) { // semi-implicit Euler, 2 ms steps
                const float h = std::min(left, 0.002f);
                v += (k * (target - x) - c * v) * h;
                x += v * h;
            }
        }
        [[nodiscard]] bool settled(float target) const {
            return std::abs(x - target) < 1.f / 512.f && std::abs(v) < 0.02f;
        }
    };

    enum class EFollow : uint8_t {
        NONE,    // `hyprctl hyprglass touch`: stays where it was pressed
        POINTER, // a mouse button: follows the pointer while held
        TOUCH,   // a finger: follows its touch point
        PATH,    // `hyprctl hyprglass touch-path`: moves along a line, then lets go
    };

    struct SHit {
        bool         glass = false;
        PHLWINDOWREF window;
        PHLLSREF     layer;
        float        elementPx = ELEMENT_CAP; // short side of what was hit, logical px
    };

    struct SState {
        bool              active = false; // something is lit, or fading
        bool              held   = false;
        EFollow           follow = EFollow::NONE;
        Vector2D          pos;
        PHLWINDOWREF      window; // what was pressed: it fills with light, the rest catch the spill
        PHLLSREF          layer;
        float             elementPx = ELEMENT_CAP;
        SSpring           light, spread;
        Clock::time_point pressAt, lastStep;
        std::optional<Clock::time_point> releaseAt; // let go at this time (quick clicks, `touch X Y ms`)
        Vector2D          pathFrom, pathTo;
        float             pathMs = 0.f;
        int32_t           touchId = -1;
        PHLMONITORREF     touchMonitor;
        std::vector<PHLMONITORREF> damaged; // monitors the light reached last tick
    } s;

    SP<CEventLoopTimer> s_tick;

    bool enabled() {
        return g_pGlobalState && settingOr(g_pGlobalState->config.pressGlow, 1.f) >= 0.5f;
    }

    const SStyle& style() {
        return g_pGlobalState && settingOr(g_pGlobalState->config.pressGlow, 1.f) >= 1.5f ? BRIGHT : SUBTLE;
    }

    float msSince(Clock::time_point t, Clock::time_point now) {
        return std::chrono::duration<float, std::milli>(now - t).count();
    }

    float fullRadius() {
        return RADIUS_PER_SIDE * std::clamp(s.elementPx, 1.f, ELEMENT_CAP);
    }

    float spreadTarget() {
        if (s.held)
            return 1.f;
        const float r = fullRadius();
        return std::min(BLOOM, (r + BLOOM_CAP) / r);
    }

    float reachPx() {
        return fullRadius() * std::max(s.spread.x, 0.f) + 2.f;
    }

    PHLMONITOR monitorAt(const Vector2D& pos) {
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && CBox{monitor->m_position, monitor->m_size}.containsPoint(pos))
                return monitor;
        return nullptr;
    }

    // Whole monitors, not a box round the light: shell panels that take their
    // shape from their own pixels rebuild it only where damaged, so a box edge
    // across one reads as its outline there (and stays, a bright line, once the
    // light is gone). The light also reaches every pane on the way.
    void damageReach() {
        const float reach = reachPx();
        const CBox  box{s.pos.x - reach, s.pos.y - reach, reach * 2.0, reach * 2.0};
        std::vector<PHLMONITORREF> now;
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && CBox{monitor->m_position, monitor->m_size}.overlaps(box))
                now.emplace_back(monitor);
        for (const auto& ref : s.damaged)
            if (const auto monitor = ref.lock())
                g_pHyprRenderer->damageMonitor(monitor);
        for (const auto& ref : now)
            if (const auto monitor = ref.lock())
                g_pHyprRenderer->damageMonitor(monitor);
        s.damaged = std::move(now);
    }

    void release();

    // Damage added while a frame renders is cleared with that frame, so the
    // next frame is requested from out here (same as the genie's tick).
    void onTick() {
        const auto  now = Clock::now();
        const float dt  = std::clamp(std::chrono::duration<float>(now - s.lastStep).count(), 0.f, 0.05f);
        s.lastStep      = now;

        if (s.held) {
            if (s.follow == EFollow::POINTER)
                s.pos = Pointer::mgr()->position();
            else if (s.follow == EFollow::PATH) {
                const float k = s.pathMs > 0.f ? std::clamp(msSince(s.pressAt, now) / s.pathMs, 0.f, 1.f) : 1.f;
                s.pos         = s.pathFrom + (s.pathTo - s.pathFrom) * k;
                if (k >= 1.f && !s.releaseAt)
                    release();
            }
            if (s.releaseAt && now >= *s.releaseAt) {
                s.releaseAt.reset();
                s.held = false;
            }
        }

        const auto& st   = style();
        const float k    = s.held ? st.pressK : st.releaseK;
        const float zeta = s.held ? st.pressZeta : st.releaseZeta;
        s.light.step(s.held ? 1.f : 0.f, k, zeta, dt);
        s.spread.step(spreadTarget(), k, zeta, dt);
        damageReach();

        if (!s.held && s.light.settled(0.f)) {
            s = {}; // fully faded: the damage above wipes the last of it, go idle
            return;
        }
        s_tick->updateTimeout(std::chrono::milliseconds(TICK_MS));
    }

    void armTick() {
        if (!s_tick) {
            s_tick = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { onTick(); }, nullptr);
            g_pEventLoopManager->addTimer(s_tick);
        }
        if (!s_tick->armed())
            s_tick->updateTimeout(std::chrono::milliseconds(TICK_MS));
    }

    float shortSide(const Vector2D& size) {
        return static_cast<float>(std::max(1.0, std::min(size.x, size.y)));
    }

    // What is under this point, and is it glass? Same stacking order input uses:
    // overlay and top layers, then windows, then bottom and background layers.
    SHit glassAt(const Vector2D& pos) {
        const auto monitor = monitorAt(pos);
        if (!monitor)
            return {};
        const auto hits = Desktop::viewState()->hitTest();

        auto layerHit = [&](int layer, SHit& hit) {
            PHLLS    found;
            Vector2D local;
            if (!hits.layerSurfaceAt(pos, &monitor->m_layerSurfaceLayers[layer], &local, &found) || !found)
                return false;
            hit = {.glass     = layerHasGlass(found),
                   .layer     = found,
                   .elementPx = shortSide(found->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT))};
            return true;
        };

        SHit hit;
        for (int layer : {3 /* overlay */, 2 /* top */})
            if (layerHit(layer, hit))
                return hit;

        if (const auto window = hits.windowAt(pos, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING)) {
            const auto* decoration = glassDecorationFor(window);
            return {.glass     = decoration && decoration->isGlassEnabled(),
                    .window    = window,
                    .elementPx = shortSide(window->getWindowMainSurfaceBox().size())};
        }

        for (int layer : {1 /* bottom */, 0 /* background */})
            if (layerHit(layer, hit))
                return hit;
        return {};
    }

    void press(const Vector2D& pos, EFollow follow, const SHit& hit) {
        if (!enabled())
            return;
        const auto now = Clock::now();
        if (!s.active)
            s.light = {}; // pressed again while fading: the light carries on from where it is
        s.spread    = {}; // ...but grows out of the new press point
        s.active    = true;
        s.held      = true;
        s.follow    = follow;
        s.pos       = pos;
        s.window    = hit.window;
        s.layer     = hit.layer;
        s.elementPx = hit.elementPx;
        s.pressAt   = now;
        s.lastStep  = now;
        s.releaseAt.reset();
        armTick();
    }

    void release() {
        if (!s.held)
            return;
        const auto earliest = s.pressAt + std::chrono::milliseconds(static_cast<int>(MIN_HOLD_MS));
        if (Clock::now() < earliest)
            s.releaseAt = earliest; // onTick lets go then
        else {
            s.releaseAt.reset();
            s.held = false;
        }
        armTick();
    }

    // Touch screens report a point on the output the device is mapped to (or
    // the focused one), from 0 to 1.
    Vector2D touchPos(const PHLMONITOR& monitor, const Vector2D& pos) {
        return monitor->m_position + pos * monitor->m_size;
    }

    PHLMONITOR touchMonitorFor(const SP<ITouch>& device) {
        if (device && !device->m_boundOutput.empty())
            for (const auto& monitor : State::monitorState()->monitors())
                if (monitor && monitor->m_name == device->m_boundOutput)
                    return monitor;
        return Desktop::focusState()->monitor();
    }
} // namespace

void init() {
    auto& listeners = g_pGlobalState->listeners;
    listeners.push_back(Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo&) {
        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto pos = Pointer::mgr()->position();
            if (const auto hit = glassAt(pos); hit.glass)
                press(pos, EFollow::POINTER, hit);
        } else if (s.follow == EFollow::POINTER)
            release();
    }));

    // Only the first finger lights the glass.
    listeners.push_back(Event::bus()->m_events.input.touch.down.listen([](ITouch::SDownEvent event, Event::SCallbackInfo&) {
        if (s.held && s.follow == EFollow::TOUCH)
            return;
        const auto monitor = touchMonitorFor(event.device);
        if (!monitor)
            return;
        const auto pos = touchPos(monitor, event.pos);
        if (const auto hit = glassAt(pos); hit.glass) {
            press(pos, EFollow::TOUCH, hit);
            s.touchId      = event.touchID;
            s.touchMonitor = monitor;
        }
    }));
    listeners.push_back(Event::bus()->m_events.input.touch.motion.listen([](ITouch::SMotionEvent event, Event::SCallbackInfo&) {
        if (!s.held || s.follow != EFollow::TOUCH || event.touchID != s.touchId)
            return;
        if (const auto monitor = s.touchMonitor.lock())
            s.pos = touchPos(monitor, event.pos);
    }));
    const auto lift = [](int32_t touchID) {
        if (s.follow == EFollow::TOUCH && touchID == s.touchId) {
            release();
            s.touchId = -1;
        }
    };
    listeners.push_back(Event::bus()->m_events.input.touch.up.listen([lift](ITouch::SUpEvent event, Event::SCallbackInfo&) { lift(event.touchID); }));
    listeners.push_back(Event::bus()->m_events.input.touch.cancel.listen([lift](ITouch::SCancelEvent event, Event::SCallbackInfo&) { lift(event.touchID); }));
}

void shutdown() {
    if (s_tick) {
        g_pEventLoopManager->removeTimer(s_tick);
        s_tick.reset();
    }
    s = {};
}

SLight current() {
    if (!s.active || !enabled())
        return {};
    const float strength = std::max(s.light.x, 0.f);
    if (strength <= 1.f / 512.f)
        return {};
    const auto& st = style();
    return {
        .posGlobal   = s.pos,
        .strength    = strength,
        .spread      = std::max(s.spread.x, 0.f),
        .spillRadius = fullRadius() * std::max(s.spread.x, 0.f),
        .wash        = st.wash, // added as is: the glow is not a specular highlight
        .radial      = st.radial,
        .spill       = std::clamp(settingOr(g_pGlobalState->config.pressSpill, 0.5f), 0.f, 1.f),
    };
}

bool pressedOn(const PHLWINDOW& window) {
    return s.active && window && s.window.lock() == window;
}

bool pressedOn(const PHLLS& layer) {
    return s.active && layer && s.layer.lock() == layer;
}

int phase() {
    if (!s.active)
        return 0;
    return s.held ? 1 : 2;
}

std::optional<std::string> handleHyprctl(std::string_view request, bool) {
    std::string        word;
    std::istringstream in{std::string(request)};
    in >> word;
    if (word != "touch" && word != "touch-hold" && word != "touch-release" && word != "touch-probe" && word != "touch-path")
        return std::nullopt;

    if (word == "touch-probe") { // would a press here light anything?
        double x = 0, y = 0;
        if (!(in >> x >> y))
            return "usage: touch-probe X Y\n";
        return glassAt(Vector2D{x, y}).glass ? "glass\n" : "not glass\n";
    }

    if (word == "touch-release") {
        release();
        return "ok\n";
    }

    if (!enabled())
        return "press_glow is off\n";

    if (word == "touch-path") { // a held press dragged along a line, then let go
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        float  ms = 0;
        if (!(in >> x1 >> y1 >> x2 >> y2 >> ms))
            return "usage: touch-path X1 Y1 X2 Y2 ms\n";
        press(Vector2D{x1, y1}, EFollow::PATH, glassAt(Vector2D{x1, y1}));
        s.pathFrom = Vector2D{x1, y1};
        s.pathTo   = Vector2D{x2, y2};
        s.pathMs   = std::max(ms, 0.f);
        return "ok\n";
    }

    double x = 0, y = 0;
    if (!(in >> x >> y))
        return std::format("usage: {} X Y{}\n", word, word == "touch" ? " [hold_ms]" : "");
    press(Vector2D{x, y}, EFollow::NONE, glassAt(Vector2D{x, y}));

    if (word == "touch") {
        int holdMs = 120;
        in >> holdMs;
        s.releaseAt = s.pressAt + std::chrono::milliseconds(std::max(holdMs, static_cast<int>(MIN_HOLD_MS)));
    }
    return "ok\n";
}

} // namespace TouchLight
