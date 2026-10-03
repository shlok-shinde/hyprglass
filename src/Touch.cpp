#include "Touch.hpp"
#include "GlassDecoration.hpp"
#include "Globals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <sstream>

namespace TouchLight {

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr float RADIUS     = 150.f; // logical px the light reaches, fully grown
    constexpr float ATTACK_MS  = 90.f;  // press -> full light
    constexpr float GROW_MS    = 300.f; // the lit area swells out from the press point
    constexpr float RELEASE_MS = 650.f; // release -> dark again
    constexpr float REACH      = 1.9f;  // damage this many radii around the point
    constexpr int   TICK_MS    = 8;

    struct SState {
        bool              active = false; // something is lit, or fading
        bool              held   = false;
        bool              follow = false; // track the pointer while held (real presses, not `hyprctl ... touch`)
        Vector2D          pos;
        Clock::time_point pressAt;
        Clock::time_point releaseAt;      // fade starts here (never before the attack completes)
        CBox              lastDamage;
    } s;

    SP<CEventLoopTimer> s_tick;

    float msSince(Clock::time_point t, Clock::time_point now) {
        return std::chrono::duration<float, std::milli>(now - t).count();
    }

    float smooth(float x) {
        x = std::clamp(x, 0.f, 1.f);
        return x * x * (3.f - 2.f * x);
    }

    float strengthAt(Clock::time_point now) {
        if (!s.active)
            return 0.f;
        const float attack = smooth(msSince(s.pressAt, now) / ATTACK_MS);
        if (s.held || now < s.releaseAt)
            return attack;
        const float k = std::clamp(msSince(s.releaseAt, now) / RELEASE_MS, 0.f, 1.f);
        return attack * (1.f - k) * (1.f - k);
    }

    float radiusAt(Clock::time_point now) {
        const float g  = std::clamp(msSince(s.pressAt, now) / GROW_MS, 0.f, 1.f);
        float       r  = RADIUS * (0.45f + 0.55f * (1.f - std::pow(1.f - g, 3.f)));
        if (!s.held && now > s.releaseAt)
            r *= 1.f + 0.3f * std::clamp(msSince(s.releaseAt, now) / RELEASE_MS, 0.f, 1.f); // spreads as it fades
        return r;
    }

    CBox lightBox(Clock::time_point now) {
        const float reach = radiusAt(now) * REACH;
        return CBox{s.pos.x - reach, s.pos.y - reach, reach * 2.0, reach * 2.0}.round();
    }

    void damage(const CBox& box) {
        if (box.w > 0 && box.h > 0)
            g_pHyprRenderer->damageBox(box);
    }

    // Damage added while a frame renders is cleared with that frame, so the
    // next frame is requested from out here (same as the genie's tick).
    void onTick() {
        const auto now = Clock::now();
        if (s.held && s.follow)
            s.pos = Pointer::mgr()->position(); // the light follows the pointer while pressed

        const CBox box = lightBox(now);
        damage(s.lastDamage);
        damage(box);
        s.lastDamage = box;

        if (!s.held && now > s.releaseAt && msSince(s.releaseAt, now) > RELEASE_MS) {
            s.active = false; // fully faded: one last damage (above) wiped it, go idle
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

    PHLMONITOR monitorAt(const Vector2D& pos) {
        for (const auto& monitor : State::monitorState()->monitors())
            if (monitor && CBox{monitor->m_position, monitor->m_size}.containsPoint(pos))
                return monitor;
        return nullptr;
    }

    // Is there glass under this point? Same stacking order input uses: overlay
    // and top layers, then windows, then bottom and background layers.
    bool glassAt(const Vector2D& pos) {
        const auto monitor = monitorAt(pos);
        if (!monitor)
            return false;
        const auto hits = Desktop::viewState()->hitTest();

        auto layerHit = [&](int layer, bool& glass) {
            PHLLS    found;
            Vector2D local;
            if (!hits.layerSurfaceAt(pos, &monitor->m_layerSurfaceLayers[layer], &local, &found) || !found)
                return false;
            glass = layerHasGlass(found);
            return true;
        };

        bool glass = false;
        for (int layer : {3 /* overlay */, 2 /* top */})
            if (layerHit(layer, glass))
                return glass;

        if (const auto window = hits.windowAt(pos, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING)) {
            const auto* decoration = glassDecorationFor(window);
            return decoration && decoration->isGlassEnabled();
        }

        for (int layer : {1 /* bottom */, 0 /* background */})
            if (layerHit(layer, glass))
                return glass;
        return false;
    }

    void press(const Vector2D& pos, bool follow) {
        const auto now = Clock::now();
        if (!s.active)
            s.lastDamage = {};
        s.active    = true;
        s.held      = true;
        s.follow    = follow;
        s.pos       = pos;
        s.pressAt   = now;
        s.releaseAt = now;
        armTick();
    }

    void release() {
        if (!s.held)
            return;
        s.held      = false;
        // a quick click still reaches full light before fading
        s.releaseAt = std::max(Clock::now(), s.pressAt + std::chrono::milliseconds(static_cast<int>(ATTACK_MS)));
        armTick();
    }
} // namespace

void init() {
    g_pGlobalState->listeners.push_back(Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo&) {
        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto pos = Pointer::mgr()->position();
            if (glassAt(pos))
                press(pos, true);
        } else
            release();
    }));
}

void shutdown() {
    if (s_tick) {
        g_pEventLoopManager->removeTimer(s_tick);
        s_tick.reset();
    }
    s = {};
}

SLight current() {
    const auto now = Clock::now();
    const float strength = strengthAt(now);
    if (strength <= 0.f)
        return {};
    return {.posGlobal = s.pos, .strength = strength, .radius = radiusAt(now)};
}

std::optional<std::string> handleHyprctl(std::string_view request, bool) {
    std::string        word;
    std::istringstream in{std::string(request)};
    in >> word;
    if (word != "touch" && word != "touch-hold" && word != "touch-release" && word != "touch-probe")
        return std::nullopt;

    if (word == "touch-probe") { // would a press here light anything?
        double x = 0, y = 0;
        if (!(in >> x >> y))
            return "usage: touch-probe X Y\n";
        return glassAt(Vector2D{x, y}) ? "glass\n" : "not glass\n";
    }

    if (word == "touch-release") {
        release();
        return "ok\n";
    }

    double x = 0, y = 0;
    if (!(in >> x >> y))
        return std::format("usage: {} X Y{}\n", word, word == "touch" ? " [hold_ms]" : "");
    press(Vector2D{x, y}, false);

    if (word == "touch") {
        int holdMs = 120;
        in >> holdMs;
        s.held      = false;
        s.releaseAt = s.pressAt + std::chrono::milliseconds(std::max(holdMs, static_cast<int>(ATTACK_MS)));
    }
    return "ok\n";
}

} // namespace TouchLight
