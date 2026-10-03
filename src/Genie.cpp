#include "Genie.hpp"
#include "Globals.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <GLES3/gl32.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/transformer/Transformer.hpp>
#include <sstream>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Genie {

using Clock = std::chrono::steady_clock;

namespace {

    constexpr float MINIMIZE_MS = 520.0f;
    constexpr float RESTORE_MS  = 460.0f;
    // How long the emptied window stays attached after a minimize, covering the
    // fade Hyprland runs when the window leaves for the special workspace.
    constexpr float HOLD_MS = 450.0f;

    std::optional<float> s_progressOverride; // debug: freeze every genie at this progress

    struct {
        uint64_t transforms = 0, amends = 0, noOutput = 0;
        float    lastProgress = -1.0f;
    } s_stats;

    GLuint fbId(const SP<Render::IFramebuffer>& framebuffer) {
        return dynamic_cast<Render::GL::CGLFramebuffer*>(framebuffer.get())->getFBID();
    }

    std::string addressOf(const PHLWINDOW& window) {
        return std::format("0x{:x}", reinterpret_cast<uintptr_t>(window.get()));
    }

    // Runs a dispatcher under either config language: Lua first, then the legacy form.
    void dispatch(const std::string& lua, const std::string& legacy) {
        const auto result = HyprlandAPI::invokeHyprctlCommand("dispatch", lua);
        if (result.starts_with("ok"))
            return;
        HyprlandAPI::invokeHyprctlCommand("dispatch", legacy);
    }

    class CGenieTransformer : public Render::IWindowTransformer {
      public:
        CGenieTransformer(PHLWINDOWREF window, bool minimizing, const CBox& targetGlobal, float durationMs)
            : m_window(window), m_minimizing(minimizing), m_targetGlobal(targetGlobal), m_durationMs(std::max(durationMs, 1.0f)), m_start(Clock::now()) {}

        // 0 = window at rest, 1 = entirely inside the target
        [[nodiscard]] float progress() const {
            if (s_progressOverride)
                return *s_progressOverride;
            const float elapsed = std::chrono::duration<float, std::milli>(Clock::now() - m_start).count();
            const float t       = std::clamp(elapsed / m_durationMs, 0.0f, 1.0f);
            return m_minimizing ? t : 1.0f - t;
        }

        [[nodiscard]] bool finished() const {
            return std::chrono::duration<float, std::milli>(Clock::now() - m_start).count() >= m_durationMs;
        }

        SP<Render::IFramebuffer> transform(SP<Render::IFramebuffer> in) override {
            auto& shaderManager = g_pGlobalState->shaderManager;
            shaderManager.initializeIfNeeded();

            const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
            s_stats.transforms++;
            if (!in || !monitor || !shaderManager.isInitialized())
                return in;

            // resources() is a weak ref to a unique pointer: dereference it, never lock() it
            const auto out = monitor->resources() ? monitor->resources()->getUnusedWorkBuffer() : nullptr;
            if (!out || out == in) {
                s_stats.noOutput++;
                return in;
            }

            const float    scale = monitor->m_scale;
            const Vector2D size  = in->m_size;
            const CBox     box   = m_lastBox.copy().scale(scale);
            CBox           target = m_targetGlobal.copy().translate(-monitor->m_position).scale(scale);

            const float p = progress();
            s_stats.lastProgress = p;

            static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
                2.0f, 0.0f, 0.0f,
                0.0f, 2.0f, 0.0f,
               -1.0f,-1.0f, 1.0f,
            };

            g_pHyprRenderer->blend(false);
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            if (glIsEnabled(GL_SCISSOR_TEST))
                glDisable(GL_SCISSOR_TEST);
            if (glIsEnabled(GL_STENCIL_TEST))
                glDisable(GL_STENCIL_TEST);

            glBindFramebuffer(GL_FRAMEBUFFER, fbId(out));
            g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(out->m_size.x), static_cast<int>(out->m_size.y));

            const auto& uniforms = shaderManager.genieUniforms;
            auto        shader   = g_pHyprOpenGL->useShader(shaderManager.genieShader);
            shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
            shader->setUniformInt(SHADER_TEX, 0);
            glUniform2f(uniforms.fbSize, static_cast<float>(size.x), static_cast<float>(size.y));
            glUniform4f(uniforms.srcBox, static_cast<float>(box.x), static_cast<float>(box.y), static_cast<float>(box.w), static_cast<float>(box.h));
            glUniform3f(uniforms.target, static_cast<float>(target.x), static_cast<float>(target.x + target.w), static_cast<float>(target.y + target.h * 0.5));
            glUniform1f(uniforms.progress, p);

            glActiveTexture(GL_TEXTURE0);
            in->getTexture()->bind();
            glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);

            // back to what Hyprland had bound when it called us
            g_pHyprRenderer->blend(true);
            glBindFramebuffer(GL_FRAMEBUFFER, fbId(in));
            g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(in->m_size.x), static_cast<int>(in->m_size.y));

            return out;
        }

        // Hyprland draws our result only inside the box this reports. Borrow the
        // motion-blur extents (one sample, no motion) to widen it to everything
        // between the window and its target.
        void amendTransformedRenderData(const CBox& currentBox, SMotionBlurData* data) override {
            m_lastBox = currentBox;
            s_stats.amends++;
            if (!data)
                return;

            const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (!monitor)
                return;

            const CBox covered = coveredGlobal(monitor).translate(-monitor->m_position);
            data->enabled  = true;
            data->previous = covered;
            data->current  = covered;
            data->samples  = 1;
        }

        [[nodiscard]] CBox coveredGlobal(const PHLMONITOR& monitor) const {
            const CBox   window = m_lastBox.copy().translate(monitor->m_position);
            const double x1     = std::min(window.x, m_targetGlobal.x);
            const double y1     = std::min(window.y, m_targetGlobal.y);
            const double x2     = std::max(window.x + window.w, m_targetGlobal.x + m_targetGlobal.w);
            const double y2     = std::max(window.y + window.h, m_targetGlobal.y + m_targetGlobal.h);
            return CBox{x1, y1, x2 - x1, y2 - y1}.expand(4);
        }

        PHLWINDOWREF m_window;
        bool         m_minimizing;
        CBox         m_targetGlobal; // global logical coordinates
        float        m_durationMs;
        Clock::time_point m_start;
        CBox         m_lastBox;      // monitor-local logical box Hyprland last gave us
    };

    struct SAnimation {
        PHLWINDOWREF        window;
        CGenieTransformer*  transformer = nullptr; // owned by window->m_transformers
        bool                minimizing  = true;
        SP<CEventLoopTimer> timer;
        bool                holding = false;       // animation done, window parked, waiting out the fade
    };

    struct SMinimized {
        PHLWINDOWREF window;
        CBox         target; // where it went, so it can come back out of the same spot
    };

    std::vector<SAnimation> s_animations;

    // Damage added while a frame renders is cleared with that frame, so the next
    // frame of an animation is requested from outside rendering, on this tick.
    SP<CEventLoopTimer> s_tick;
    constexpr int       TICK_MS = 7;

    void onTick() {
        if (s_animations.empty())
            return; // goes idle until the next attach() re-arms it
        for (const auto& animation : s_animations) {
            const auto window  = animation.window.lock();
            const auto monitor = window ? window->m_monitor.lock() : nullptr;
            if (monitor && animation.transformer)
                g_pHyprRenderer->damageBox(animation.transformer->coveredGlobal(monitor));
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
    std::vector<SMinimized> s_minimized;
    HANDLE                  s_handle = nullptr;

    CBox defaultTarget(const PHLMONITOR& monitor) {
        const double size = 56.0;
        return CBox{monitor->m_position.x + (monitor->m_size.x - size) * 0.5, monitor->m_position.y + monitor->m_size.y - size - 10.0, size, size};
    }

    void detach(const PHLWINDOW& window, CGenieTransformer* transformer) {
        if (!window)
            return;
        std::erase_if(window->m_transformers, [&](const auto& t) { return t.get() == transformer; });
        g_pHyprRenderer->damageWindow(window);
    }

    void eraseAnimation(CGenieTransformer* transformer) {
        std::erase_if(s_animations, [&](const SAnimation& a) {
            if (a.transformer != transformer)
                return false;
            if (a.timer)
                g_pEventLoopManager->removeTimer(a.timer);
            return true;
        });
    }

    SAnimation* animationFor(const PHLWINDOW& window) {
        for (auto& a : s_animations) {
            if (a.window.lock() == window)
                return &a;
        }
        return nullptr;
    }

    // Timer callback: the animation's clock ran out (or its hold did).
    void onTimer(CGenieTransformer* transformer) {
        auto it = std::ranges::find_if(s_animations, [&](const SAnimation& a) { return a.transformer == transformer; });
        if (it == s_animations.end())
            return;

        const auto window = it->window.lock();
        if (!window) {
            eraseAnimation(transformer);
            return;
        }

        if (it->minimizing && !it->holding) {
            // Fully poured in: park the window, keep it invisible while Hyprland fades it out.
            it->holding = true;
            dispatch(std::format("hl.dsp.window.move({{ workspace = \"{}\", follow = false, window = \"address:{}\" }})", MINIMIZED_WORKSPACE, addressOf(window)),
                     std::format("movetoworkspacesilent {},address:{}", MINIMIZED_WORKSPACE, addressOf(window)));
            it->timer->updateTimeout(std::chrono::milliseconds(static_cast<int>(HOLD_MS)));
            return;
        }

        detach(window, transformer);
        eraseAnimation(transformer);
    }

    CGenieTransformer* attach(const PHLWINDOW& window, bool minimizing, const CBox& target, float durationMs) {
        auto  transformer = makeUnique<CGenieTransformer>(PHLWINDOWREF{window}, minimizing, target, durationMs);
        auto* raw         = transformer.get();
        // seed the box so the very first frame's damage covers the whole path
        if (const auto monitor = window->m_monitor.lock())
            raw->m_lastBox = window->getFullWindowBoundingBox().translate(-monitor->m_position);
        window->m_transformers.emplace_back(std::move(transformer));

        SAnimation animation;
        animation.window      = window;
        animation.transformer = raw;
        animation.minimizing  = minimizing;
        animation.timer       = makeShared<CEventLoopTimer>(std::chrono::milliseconds(static_cast<int>(durationMs) + 20),
                                                            [raw](SP<CEventLoopTimer>, void*) { onTimer(raw); }, nullptr);
        g_pEventLoopManager->addTimer(animation.timer);
        s_animations.emplace_back(std::move(animation));

        if (const auto monitor = window->m_monitor.lock())
            g_pHyprRenderer->damageBox(raw->coveredGlobal(monitor));
        armTick();
        return raw;
    }

    bool isMinimized(const PHLWINDOW& window) {
        return std::ranges::any_of(s_minimized, [&](const SMinimized& m) { return m.window.lock() == window; });
    }

    std::string minimize(PHLWINDOW window, std::optional<CBox> target, float durationMs) {
        if (!window)
            window = Desktop::focusState()->window();
        if (!window || !window->m_isMapped)
            return "no window";
        if (animationFor(window) || isMinimized(window))
            return "busy";

        const auto monitor = window->m_monitor.lock();
        if (!monitor)
            return "no monitor";

        const CBox destination = target.value_or(defaultTarget(monitor));
        attach(window, true, destination, durationMs > 0 ? durationMs : MINIMIZE_MS);
        s_minimized.push_back({PHLWINDOWREF{window}, destination});
        return "ok";
    }

    std::string restore(PHLWINDOW window, std::optional<CBox> target, float durationMs) {
        std::erase_if(s_minimized, [](const SMinimized& m) { return !m.window.lock(); });
        if (s_minimized.empty())
            return "nothing minimized";

        auto it = window ? std::ranges::find_if(s_minimized, [&](const SMinimized& m) { return m.window.lock() == window; }) : std::prev(s_minimized.end());
        if (it == s_minimized.end())
            return "not minimized";

        window = it->window.lock();
        const CBox origin = target.value_or(it->target);
        s_minimized.erase(it);

        // A restore requested mid-minimize: drop the half-run animation first.
        if (auto* running = animationFor(window)) {
            auto* transformer = running->transformer;
            detach(window, transformer);
            eraseAnimation(transformer);
        }

        const auto monitor   = Desktop::focusState()->monitor();
        const auto workspace = monitor ? monitor->m_activeWorkspace : nullptr;
        if (!workspace)
            return "no workspace";

        // Attach first, so the window is already inside the lamp when it lands on the workspace.
        attach(window, false, origin, durationMs > 0 ? durationMs : RESTORE_MS);
        dispatch(std::format("hl.dsp.window.move({{ workspace = {}, follow = false, window = \"address:{}\" }})", workspace->m_id, addressOf(window)),
                 std::format("movetoworkspacesilent {},address:{}", workspace->m_id, addressOf(window)));
        dispatch(std::format("hl.dsp.focus({{ window = \"address:{}\" }})", addressOf(window)), std::format("focuswindow address:{}", addressOf(window)));
        return "ok";
    }

    PHLWINDOW windowByAddress(std::string_view text) {
        if (text.starts_with("address:"))
            text.remove_prefix(8);
        if (text.starts_with("0x"))
            text.remove_prefix(2);
        uintptr_t value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value, 16).ec != std::errc{})
            return nullptr;
        for (const auto& window : Desktop::windowState()->windows()) {
            if (reinterpret_cast<uintptr_t>(window.get()) == value)
                return window;
        }
        return nullptr;
    }

    struct SRequest {
        PHLWINDOW           window;
        std::optional<CBox> target;
        float               durationMs = 0.0f;
        bool                badWindow  = false;
    };

    // [address:0x..] [x y w h] [duration_ms]
    SRequest parseRequest(std::string_view args) {
        SRequest           request;
        std::vector<float> numbers;
        std::istringstream stream{std::string(args)};
        std::string        token;
        while (stream >> token) {
            if (token.starts_with("address:") || token.starts_with("0x")) {
                request.window    = windowByAddress(token);
                request.badWindow = !request.window;
                continue;
            }
            try {
                numbers.push_back(std::stof(token));
            } catch (...) {}
        }
        if (numbers.size() >= 4)
            request.target = CBox{numbers[0], numbers[1], numbers[2], numbers[3]};
        if (numbers.size() == 5 || numbers.size() == 1)
            request.durationMs = numbers.back();
        return request;
    }

    int luaArgsToRequest(lua_State* L, SRequest& request) {
        std::string args;
        const int   count = lua_gettop(L);
        for (int i = 1; i <= count; i++) {
            if (lua_type(L, i) == LUA_TNUMBER)
                args += std::format("{} ", lua_tonumber(L, i));
            else if (lua_type(L, i) == LUA_TSTRING)
                args += std::string(lua_tostring(L, i)) + " ";
        }
        request = parseRequest(args);
        return 0;
    }

    int luaMinimize(lua_State* L) {
        SRequest request;
        luaArgsToRequest(L, request);
        const auto result = request.badWindow ? std::string("no such window") : minimize(request.window, request.target, request.durationMs);
        lua_pushstring(L, result.c_str());
        return 1;
    }

    int luaRestore(lua_State* L) {
        SRequest request;
        luaArgsToRequest(L, request);
        const auto result = request.badWindow ? std::string("no such window") : restore(request.window, request.target, request.durationMs);
        lua_pushstring(L, result.c_str());
        return 1;
    }

    // Minimize the focused window, or bring the last one back when nothing is focused.
    int luaToggle(lua_State* L) {
        SRequest request;
        luaArgsToRequest(L, request);
        const auto focused = Desktop::focusState()->window();
        const auto result  = focused && !isMinimized(focused) ? minimize(focused, request.target, request.durationMs) : restore(nullptr, request.target, request.durationMs);
        lua_pushstring(L, result.c_str());
        return 1;
    }

} // namespace

void init(HANDLE handle) {
    s_handle = handle;
    HyprlandAPI::addLuaFunction(handle, "hyprglass", "minimize", luaMinimize);
    HyprlandAPI::addLuaFunction(handle, "hyprglass", "restore", luaRestore);
    HyprlandAPI::addLuaFunction(handle, "hyprglass", "toggle_minimize", luaToggle);
}

void shutdown() {
    for (auto& animation : s_animations) {
        if (animation.timer)
            g_pEventLoopManager->removeTimer(animation.timer);
        if (const auto window = animation.window.lock())
            std::erase_if(window->m_transformers, [&](const auto& t) { return t.get() == animation.transformer; });
    }
    s_animations.clear();
    s_minimized.clear();
    if (s_tick) {
        g_pEventLoopManager->removeTimer(s_tick);
        s_tick.reset();
    }
    if (s_handle) {
        HyprlandAPI::removeLuaFunction(s_handle, "hyprglass", "minimize");
        HyprlandAPI::removeLuaFunction(s_handle, "hyprglass", "restore");
        HyprlandAPI::removeLuaFunction(s_handle, "hyprglass", "toggle_minimize");
    }
}

bool isAnimating(const PHLWINDOW& window) {
    return std::ranges::any_of(s_animations, [&](const SAnimation& a) { return a.window.lock() == window; });
}

std::optional<std::string> handleHyprctl(std::string_view request, bool json) {
    const auto takeWord = [&](std::string_view word) {
        if (!request.starts_with(word))
            return false;
        if (request.size() > word.size() && request[word.size()] != ' ')
            return false;
        request.remove_prefix(word.size());
        while (!request.empty() && request.front() == ' ')
            request.remove_prefix(1);
        return true;
    };

    if (takeWord("genie-progress")) {
        if (request == "off" || request.empty())
            s_progressOverride.reset();
        else {
            try {
                s_progressOverride = std::clamp(std::stof(std::string(request)), 0.0f, 1.0f);
            } catch (...) { return "usage: genie-progress <0..1|off>\n"; }
        }
        for (const auto& a : s_animations)
            if (const auto w = a.window.lock())
                g_pHyprRenderer->damageWindow(w, true);
        return "ok\n";
    }

    if (takeWord("genie-stats"))
        return std::format("transforms {} amends {} noOutput {} lastProgress {:.3f} active {}\n", s_stats.transforms, s_stats.amends, s_stats.noOutput,
                           s_stats.lastProgress, s_animations.size());

    if (takeWord("minimized")) {
        std::erase_if(s_minimized, [](const SMinimized& m) { return !m.window.lock(); });
        std::string out = json ? "[" : "";
        bool        first = true;
        for (const auto& entry : s_minimized) {
            const auto window = entry.window.lock();
            if (json) {
                out += std::format("{}{{\"address\": \"{}\", \"class\": \"{}\"}}", first ? "" : ", ", addressOf(window), window->m_class);
                first = false;
            } else
                out += std::format("{} {}\n", addressOf(window), window->m_class);
        }
        return json ? out + "]\n" : (out.empty() ? "none\n" : out);
    }

    const bool wantsMinimize = takeWord("minimize");
    const bool wantsRestore  = !wantsMinimize && takeWord("restore");
    if (!wantsMinimize && !wantsRestore)
        return std::nullopt;

    const auto parsed = parseRequest(request);
    if (parsed.badWindow)
        return "no such window\n";
    return (wantsMinimize ? minimize(parsed.window, parsed.target, parsed.durationMs) : restore(parsed.window, parsed.target, parsed.durationMs)) + "\n";
}

} // namespace Genie
