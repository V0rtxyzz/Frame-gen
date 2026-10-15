#include "hyper90/FrameBlend.hpp"
#include "hyper90/Config.hpp"
#include "hyper90/NativeHooks.hpp"
#include "hyper90/Telemetry.hpp"

#include <algorithm>
#include <atomic>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#endif

namespace hyper90 {

// ============================================================================
// Planner (platform independent)
// ============================================================================
float FrameBlendPlanner::multiplierForMode(int mode) noexcept {
    return (mode == FrameBlendConsts::kModeX15) ? 1.5f : 2.0f;
}

float FrameBlendPlanner::weightForStrengthPct(int strengthPct) noexcept {
    return static_cast<float>(std::clamp(
               strengthPct, FrameBlendConsts::kMinStrengthPct, FrameBlendConsts::kMaxStrengthPct)) /
           100.0f;
}

FrameBlendPlan FrameBlendPlanner::planFrame(int mode, int64_t realFrameNs, bool havePrevious) noexcept {
    FrameBlendPlan plan{};
    const uint64_t index = mFrameCounter++;

    if (!havePrevious || realFrameNs <= 0 ||
        realFrameNs > FrameBlendConsts::kMaxRealFrameNsForGeneration) {
        return plan;
    }

    const bool everyFrame = (mode != FrameBlendConsts::kModeX15);
    if (everyFrame || (index & 1u) == 0u) {
        plan.generate = true;
        plan.presentsThisFrame = 2;
    }
    return plan;
}

namespace {
std::atomic<uint64_t> gGeneratedFrames{0};
std::atomic<bool>     gFailed{false};
std::atomic<bool>     gHistoryResetRequested{false};
} // namespace

bool FrameBlendGl::wantsVsync() noexcept {
    return getConfig().modFrameBlendEnabled.load(std::memory_order_relaxed) &&
           !gFailed.load(std::memory_order_relaxed);
}

uint64_t FrameBlendGl::generatedFrames() noexcept {
    return gGeneratedFrames.load(std::memory_order_relaxed);
}

void FrameBlendGl::resetHistory() noexcept {
    gHistoryResetRequested.store(true, std::memory_order_relaxed);
}

#if defined(__ANDROID__)

// ============================================================================
// GL engine (render thread only)
// ============================================================================
namespace {

constexpr const char* kVertexShader =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

constexpr const char* kFragmentShader =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uCur;\n"
    "uniform sampler2D uPrev;\n"
    "uniform float uWeight;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 c = texture(uCur, vUv).rgb;\n"
    "    if (uWeight > 0.0) {\n"
    "        c = mix(c, texture(uPrev, vUv).rgb, uWeight);\n"
    "    }\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

constexpr int kVerifyFrames = 4;

struct Engine {
    EGLContext        ctx{EGL_NO_CONTEXT};
    GLuint            program{0};
    GLuint            vao{0};
    GLuint            tex[2]{0, 0};
    GLint             uCur{-1};
    GLint             uPrev{-1};
    GLint             uWeight{-1};
    int               width{0};
    int               height{0};
    int               curIdx{0};
    bool              havePrev{false};
    int               verifyFramesLeft{kVerifyFrames};
    int64_t           lastRealSwapNs{0};
    FrameBlendPlanner planner{};
};

Engine gEngine;

struct SavedState {
    GLint     program{0};
    GLint     vao{0};
    GLint     activeTex{GL_TEXTURE0};
    GLint     tex[2]{0, 0};
    GLint     sampler[2]{0, 0};
    GLint     drawFbo{0};
    GLint     readFbo{0};
    GLint     readBuffer{GL_BACK};
    GLint     viewport[4]{0, 0, 0, 0};
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean scissor{GL_FALSE};
    GLboolean blend{GL_FALSE};
    GLboolean depth{GL_FALSE};
    GLboolean cull{GL_FALSE};
    GLboolean stencil{GL_FALSE};
};

void setCap(GLenum cap, GLboolean on) noexcept {
    if (on) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

void saveState(SavedState& s) noexcept {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.blend   = glIsEnabled(GL_BLEND);
    s.depth   = glIsEnabled(GL_DEPTH_TEST);
    s.cull    = glIsEnabled(GL_CULL_FACE);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);

    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex[unit]);
        glGetIntegerv(GL_SAMPLER_BINDING, &s.sampler[unit]);
    }
    glActiveTexture(static_cast<GLenum>(s.activeTex));

    // Read buffer of the default framebuffer (restored explicitly below).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &s.readBuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFbo));
}

void restoreState(const SavedState& s) noexcept {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(static_cast<GLenum>(s.readBuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s.drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFbo));

    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    setCap(GL_SCISSOR_TEST, s.scissor);
    setCap(GL_BLEND, s.blend);
    setCap(GL_DEPTH_TEST, s.depth);
    setCap(GL_CULL_FACE, s.cull);
    setCap(GL_STENCIL_TEST, s.stencil);

    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex[unit]));
        glBindSampler(static_cast<GLuint>(unit), static_cast<GLuint>(s.sampler[unit]));
    }
    glActiveTexture(static_cast<GLenum>(s.activeTex));

    glBindVertexArray(static_cast<GLuint>(s.vao));
    glUseProgram(static_cast<GLuint>(s.program));
}

GLuint compileShader(GLenum type, const char* src) noexcept {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool buildProgram(Engine& e) noexcept {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return false;
    }

    const GLuint prog = glCreateProgram();
    if (prog == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        glDeleteProgram(prog);
        return false;
    }

    e.program = prog;
    e.uCur    = glGetUniformLocation(prog, "uCur");
    e.uPrev   = glGetUniformLocation(prog, "uPrev");
    e.uWeight = glGetUniformLocation(prog, "uWeight");
    glGenVertexArrays(1, &e.vao);
    return e.vao != 0 && e.uCur >= 0 && e.uPrev >= 0 && e.uWeight >= 0;
}

void deleteTextures(Engine& e, bool contextAlive) noexcept {
    if (contextAlive && (e.tex[0] != 0 || e.tex[1] != 0)) {
        glDeleteTextures(2, e.tex);
    }
    e.tex[0] = 0;
    e.tex[1] = 0;
    e.width = 0;
    e.height = 0;
    e.havePrev = false;
    e.curIdx = 0;
}

void releaseAll(Engine& e, bool contextAlive) noexcept {
    deleteTextures(e, contextAlive);
    if (contextAlive) {
        if (e.program != 0) glDeleteProgram(e.program);
        if (e.vao != 0) glDeleteVertexArrays(1, &e.vao);
    }
    e.program = 0;
    e.vao = 0;
    e.uCur = e.uPrev = e.uWeight = -1;
}

bool createTextures(Engine& e, int w, int h) noexcept {
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(2, e.tex);
    if (e.tex[0] == 0 || e.tex[1] == 0) return false;

    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, e.tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    e.width = w;
    e.height = h;
    e.havePrev = false;
    e.curIdx = 0;
    return true;
}

bool ensureResources(Engine& e, int w, int h) noexcept {
    if (e.program == 0 && !buildProgram(e)) {
        return false;
    }
    if (e.tex[0] == 0 || e.width != w || e.height != h) {
        deleteTextures(e, true);
        if (!createTextures(e, w, h)) return false;
    }
    return true;
}

void drawFullscreen(const Engine& e, GLuint prevTex, GLuint curTex, float weight) noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, e.width, e.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Every pixel is overwritten: tell tile-based GPUs not to load the old color.
    const GLenum colorAttachment = GL_COLOR;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &colorAttachment);

    glUseProgram(e.program);
    glUniform1i(e.uCur, 0);
    glUniform1i(e.uPrev, 1);
    glUniform1f(e.uWeight, weight);

    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, curTex);
    glActiveTexture(GL_TEXTURE1);
    glBindSampler(1, 0);
    glBindTexture(GL_TEXTURE_2D, prevTex);

    glBindVertexArray(e.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void failModule(Engine& e, const char* reason) noexcept {
    gFailed.store(true, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_WARN, "Hyper90", "Frame Blend disabled: %s", reason);
    releaseAll(e, true);
}

} // namespace

int FrameBlendGl::onBeforeSwap(
    void* eglDisplay, void* eglSurface, const FrameBlendPresentOps& ops) noexcept {
    auto& e = gEngine;
    const auto& cfg = getConfig();
    const EGLContext currentCtx = eglGetCurrentContext();

    // --- Module disabled: free GL objects once, on the thread that owns them ---
    if (!cfg.modFrameBlendEnabled.load(std::memory_order_relaxed)) {
        if (e.program != 0 || e.tex[0] != 0 || e.vao != 0) {
            releaseAll(e, currentCtx != EGL_NO_CONTEXT && currentCtx == e.ctx);
        }
        gFailed.store(false, std::memory_order_relaxed);
        e.havePrev = false;
        e.lastRealSwapNs = 0;
        e.verifyFramesLeft = kVerifyFrames;
        e.planner.reset();
        return 0;
    }

    if (gFailed.load(std::memory_order_relaxed) || ops.present == nullptr ||
        currentCtx == EGL_NO_CONTEXT) {
        return 0;
    }

    // --- New GL context (app resume / surface recreation): old names are dead ---
    if (currentCtx != e.ctx) {
        releaseAll(e, false);
        e.ctx = currentCtx;
        e.lastRealSwapNs = 0;
        e.verifyFramesLeft = kVerifyFrames;
    }
    if (gHistoryResetRequested.exchange(false, std::memory_order_relaxed)) {
        e.havePrev = false;
        e.planner.reset();
    }

    EGLint w = 0;
    EGLint h = 0;
    if (!eglQuerySurface(static_cast<EGLDisplay>(eglDisplay), static_cast<EGLSurface>(eglSurface),
                         EGL_WIDTH, &w) ||
        !eglQuerySurface(static_cast<EGLDisplay>(eglDisplay), static_cast<EGLSurface>(eglSurface),
                         EGL_HEIGHT, &h) ||
        w <= 0 || h <= 0) {
        return 0;
    }

    const int64_t nowNs = TelemetryEngine::monotonicNowNs();
    const int64_t realFrameNs = (e.lastRealSwapNs != 0) ? (nowNs - e.lastRealSwapNs) : 0;
    e.lastRealSwapNs = nowNs;

    // Our own GL calls must not feed the state deduplicator / draw culler.
    GlHookBypassScope bypass;

    SavedState saved;
    saveState(saved);

    if (e.verifyFramesLeft > 0) {
        while (glGetError() != GL_NO_ERROR) {
            // drop errors left behind by the game so verification stays accurate
        }
    }

    if (!ensureResources(e, w, h)) {
        failModule(e, "could not create GL resources (needs OpenGL ES 3.0)");
        restoreState(saved);
        return 0;
    }

    // 1. Capture the finished frame from the default framebuffer.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e.tex[e.curIdx]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    const GLuint curTex  = e.tex[e.curIdx];
    const GLuint prevTex = e.tex[e.curIdx ^ 1];

    // 2. Generated frame: blend(previous, current) -> present, then redraw the real frame.
    const FrameBlendPlan plan = e.planner.planFrame(
        cfg.frameBlendMode.load(std::memory_order_relaxed), realFrameNs, e.havePrev);

    int presents = 1;
    if (plan.generate) {
        const float weight = FrameBlendPlanner::weightForStrengthPct(
            cfg.frameBlendStrengthPct.load(std::memory_order_relaxed));

        drawFullscreen(e, prevTex, curTex, weight);
        if (ops.beforePresent != nullptr) ops.beforePresent();
        const bool presented = ops.present(eglDisplay, eglSurface);

        // The new back buffer is undefined: redraw the real frame for the caller's swap.
        drawFullscreen(e, curTex, curTex, 0.0f);

        if (presented) {
            presents = 2;
            gGeneratedFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    e.curIdx ^= 1;
    e.havePrev = true;

    if (e.verifyFramesLeft > 0) {
        --e.verifyFramesLeft;
        if (glGetError() != GL_NO_ERROR) {
            failModule(e, "GL error while capturing/blending frames");
        }
    }

    restoreState(saved);
    return presents;
}

#else // Host build (unit tests): no GL, module is always inert.

int FrameBlendGl::onBeforeSwap(void*, void*, const FrameBlendPresentOps&) noexcept {
    return 0;
}

#endif

} // namespace hyper90
