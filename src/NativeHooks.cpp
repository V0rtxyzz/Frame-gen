#include "hyper90/NativeHooks.hpp"
#include "hyper90/Config.hpp"
#include "hyper90/EngineOptionsTuner.hpp"
#include "hyper90/FrameBlend.hpp"
#include "hyper90/Modules.hpp"
#include "hyper90/SystemOptimizers.hpp"
#include "hyper90/Telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <sys/socket.h>
#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#endif

namespace hyper90 {

// ============================================================================
// GlPipelineOptimizer Implementation (Modules 4, 5, 6, 7, 8)
// ============================================================================
GlPipelineOptimizer& getGlOptimizer() noexcept {
    static GlPipelineOptimizer sOptimizer;
    return sOptimizer;
}

void GlPipelineOptimizer::resetFrameState() noexcept {
    mBlendState       = -1;
    mDepthTestState   = -1;
    mCullFaceState    = -1;
    mScissorTestState = -1;
    mDepthMaskState   = -1;
    mCurrentProgram   = 0xFFFFFFFFu;
    mActiveTexUnit    = 0;
    mBoundTex2D.fill(0xFFFFFFFFu);
    mMicroBatchCounter = 0;
}

bool GlPipelineOptimizer::shouldSkipCapabilityChange(uint32_t cap, bool enable) noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modGlPipelineEnabled.load(std::memory_order_relaxed)) {
        return false;
    }

    // Block costly mobile fragment capabilities when enabled
    if (enable && cfg.disableDitherMsaa.load(std::memory_order_relaxed)) {
        if (cap == GlConsts::kGlDither ||
            cap == GlConsts::kGlMultisampleExt ||
            cap == GlConsts::kGlSampleAlphaToCoverage) {
            getTelemetry().onGlStateDeduped();
            return true;
        }
    }

    if (!cfg.dedupGlState.load(std::memory_order_relaxed)) {
        return false;
    }

    const int8_t desired = enable ? 1 : 0;
    int8_t* tracked = nullptr;
    switch (cap) {
        case GlConsts::kGlBlend:       tracked = &mBlendState; break;
        case GlConsts::kGlDepthTest:   tracked = &mDepthTestState; break;
        case GlConsts::kGlCullFace:    tracked = &mCullFaceState; break;
        case GlConsts::kGlScissorTest: tracked = &mScissorTestState; break;
        default: break;
    }

    if (tracked != nullptr) {
        if (*tracked == desired) {
            getTelemetry().onGlStateDeduped();
            return true;
        }
        *tracked = desired;
    }
    return false;
}

uint32_t GlPipelineOptimizer::rewriteHintMode(uint32_t target, uint32_t mode) const noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modGlPipelineEnabled.load(std::memory_order_relaxed) ||
        !cfg.fastestShaderHints.load(std::memory_order_relaxed)) {
        return mode;
    }

    if (target == GlConsts::kGlGenerateMipmapHint ||
        target == GlConsts::kGlFragmentShaderDerivativeHint) {
        return GlConsts::kGlFastest;
    }
    return mode;
}

bool GlPipelineOptimizer::shouldSkipUseProgram(uint32_t program) noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modGlPipelineEnabled.load(std::memory_order_relaxed) ||
        !cfg.dedupGlState.load(std::memory_order_relaxed)) {
        mCurrentProgram = program;
        return false;
    }

    if (program != 0 && mCurrentProgram == program) {
        getTelemetry().onGlStateDeduped();
        return true;
    }
    mCurrentProgram = program;
    return false;
}

void GlPipelineOptimizer::onActiveTexture(uint32_t textureEnum) noexcept {
    if (textureEnum >= GlConsts::kGlTexture0 &&
        textureEnum < GlConsts::kGlTexture0 + kTrackedTexUnits) {
        mActiveTexUnit = textureEnum - GlConsts::kGlTexture0;
    } else {
        mActiveTexUnit = 0;
    }
}

bool GlPipelineOptimizer::shouldSkipBindTexture(uint32_t target, uint32_t texture) noexcept {
    if (target != GlConsts::kGlTexture2D || mActiveTexUnit >= kTrackedTexUnits) {
        return false;
    }

    const auto& cfg = getConfig();
    if (!cfg.modGlPipelineEnabled.load(std::memory_order_relaxed) ||
        !cfg.dedupGlState.load(std::memory_order_relaxed)) {
        mBoundTex2D[mActiveTexUnit] = texture;
        return false;
    }

    if (texture != 0 && mBoundTex2D[mActiveTexUnit] == texture) {
        getTelemetry().onGlStateDeduped();
        return true;
    }
    mBoundTex2D[mActiveTexUnit] = texture;
    return false;
}

bool GlPipelineOptimizer::shouldSkipDepthMask(uint8_t flag) noexcept {
    const auto& cfg = getConfig();
    const int8_t desired = (flag != 0) ? 1 : 0;
    if (!cfg.modGlPipelineEnabled.load(std::memory_order_relaxed) ||
        !cfg.dedupGlState.load(std::memory_order_relaxed)) {
        mDepthMaskState = desired;
        return false;
    }

    if (mDepthMaskState == desired) {
        getTelemetry().onGlStateDeduped();
        return true;
    }
    mDepthMaskState = desired;
    return false;
}

uint32_t GlPipelineOptimizer::rewriteClearMask(uint32_t mask) const noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modTbdrTileSaverEnabled.load(std::memory_order_relaxed) ||
        !cfg.optimizeClearCalls.load(std::memory_order_relaxed)) {
        return mask;
    }

    // On packed D24S8 TBDR tile buffers, clearing depth without stencil forces
    // the GPU to load the previous frame's stencil tile from system RAM.
    if ((mask & GlConsts::kGlDepthBufferBit) != 0u) {
        mask |= GlConsts::kGlStencilBufferBit;
    }
    return mask;
}

void GlPipelineOptimizer::rewriteTexParameteri(uint32_t target, uint32_t pname, int32_t& param) noexcept {
    if (target != GlConsts::kGlTexture2D) return;
    const auto& cfg = getConfig();
    if (!cfg.modTextureLodEnabled.load(std::memory_order_relaxed)) return;

    const auto tier = getTelemetry().getCurrentTier();

    if (pname == GlConsts::kGlTextureMinFilter && cfg.fastMipmapFilter.load(std::memory_order_relaxed)) {
        if (static_cast<uint32_t>(param) == GlConsts::kGlLinearMipmapLinear) {
            param = static_cast<int32_t>(
                (tier == GovernorTier::Ultra90Lock)
                    ? GlConsts::kGlNearestMipmapNearest
                    : GlConsts::kGlLinearMipmapNearest);
            getTelemetry().onTextureFilterOptimized();
        } else if (static_cast<uint32_t>(param) == GlConsts::kGlNearestMipmapLinear) {
            param = static_cast<int32_t>(GlConsts::kGlNearestMipmapNearest);
            getTelemetry().onTextureFilterOptimized();
        }
    } else if (pname == GlConsts::kGlTextureMaxAnisotropyExt &&
               cfg.clampAnisotropy.load(std::memory_order_relaxed)) {
        const int maxAniso = (tier >= GovernorTier::Balanced90)
            ? 1
            : std::clamp(cfg.maxAnisotropy.load(std::memory_order_relaxed), 1, 8);
        if (param > maxAniso) {
            param = maxAniso;
            getTelemetry().onTextureFilterOptimized();
        }
    } else if (pname == GlConsts::kGlTextureMaxLevel) {
        const int maxMip = std::clamp(cfg.maxMipLevel.load(std::memory_order_relaxed), 2, 10);
        if (param > maxMip) {
            param = maxMip;
            getTelemetry().onTextureFilterOptimized();
        }
    }
}

void GlPipelineOptimizer::rewriteTexParameterf(uint32_t target, uint32_t pname, float& param) noexcept {
    if (target != GlConsts::kGlTexture2D) return;
    const auto& cfg = getConfig();
    if (!cfg.modTextureLodEnabled.load(std::memory_order_relaxed)) return;

    if (pname == GlConsts::kGlTextureMaxAnisotropyExt &&
        cfg.clampAnisotropy.load(std::memory_order_relaxed)) {
        const auto tier = getTelemetry().getCurrentTier();
        const float maxAniso = (tier >= GovernorTier::Balanced90)
            ? 1.0f
            : static_cast<float>(std::clamp(cfg.maxAnisotropy.load(std::memory_order_relaxed), 1, 8));
        if (param > maxAniso) {
            param = maxAniso;
            getTelemetry().onTextureFilterOptimized();
        }
    } else if (pname == GlConsts::kGlTextureMinFilter &&
               cfg.fastMipmapFilter.load(std::memory_order_relaxed)) {
        int32_t intParam = static_cast<int32_t>(param);
        rewriteTexParameteri(target, pname, intParam);
        param = static_cast<float>(intParam);
    }
}

bool GlPipelineOptimizer::shouldCullDrawCall(uint32_t mode, int32_t count) noexcept {
    const auto& cfg = getConfig();

    // 1. Degenerate draw call rejection
    if (count <= 0) {
        if (cfg.modDrawBatchCullerEnabled.load(std::memory_order_relaxed) &&
            cfg.cullDegenerateDraws.load(std::memory_order_relaxed)) {
            getTelemetry().onDrawCallCulled();
            return true;
        }
        return false;
    }

    const int currentDraws = getTelemetry().getLastFrameDrawCalls();
    const auto tier = getTelemetry().getCurrentTier();

    // 2. Module 7 & 8: Micro-batch & particle budget culling on small triangle batches
    if (mode == GlConsts::kGlTriangles && count <= 36) {
        // Never cull early-frame terrain or late-frame UI if within safe draw budget
        const int maxParticleBudget = std::clamp(
            cfg.maxParticlesPerFrame.load(std::memory_order_relaxed), 20, 1000);
        const int effectiveParticleCap = (tier >= GovernorTier::Aggressive90)
            ? (maxParticleBudget * 60) / 100
            : maxParticleBudget;

        const int maxMicroIndices = std::clamp(
            cfg.microBatchMaxIndices.load(std::memory_order_relaxed), 6, 60);

        if (count <= maxMicroIndices) {
            ++mMicroBatchCounter;

            // Module 8: Hard particle micro-batch cap per frame
            if (cfg.modEntityParticleEnabled.load(std::memory_order_relaxed) &&
                cfg.limitParticles.load(std::memory_order_relaxed) &&
                static_cast<int>(mMicroBatchCounter) > effectiveParticleCap &&
                currentDraws > 64) {
                getTelemetry().onDrawCallCulled();
                return true;
            }

            // Module 7: Adaptive micro-batch decimation when FPS dips below trigger threshold
            if (cfg.modDrawBatchCullerEnabled.load(std::memory_order_relaxed) &&
                cfg.adaptiveMicroBatchCull.load(std::memory_order_relaxed) &&
                currentDraws > 80) {
                const float avgFps = getTelemetry().getSnapshot().avgFps;
                const float triggerFps = static_cast<float>(
                    cfg.cullTriggerFps.load(std::memory_order_relaxed));

                if (tier == GovernorTier::Ultra90Lock || avgFps < triggerFps - 6.0f) {
                    // Cull every 2nd micro-batch in Ultra 90-Lock rescue mode
                    if ((mMicroBatchCounter & 1u) == 1u) {
                        getTelemetry().onDrawCallCulled();
                        return true;
                    }
                } else if (tier == GovernorTier::Aggressive90 || avgFps < triggerFps) {
                    // Cull every 4th micro-batch in Aggressive 90 mode
                    if ((mMicroBatchCounter & 3u) == 3u) {
                        getTelemetry().onDrawCallCulled();
                        return true;
                    }
                }
            }
        }
    }

    getTelemetry().onDrawCallSubmitted();
    return false;
}

// ============================================================================
// Android Runtime Hook Implementations
// ============================================================================
#if defined(__ANDROID__)
namespace {

std::atomic<bool> gHooksInstalled{false};
std::atomic<bool> gMcSymbolsHooked{false};
thread_local bool tIsRenderThread{false};
EGLDisplay        gLastDisplay{EGL_NO_DISPLAY};
int               gAppliedSwapInterval{-1};
std::atomic<int>  gEngineSwapInterval{-1}; // last interval the game itself asked for

using PFN_glInvalidateFramebuffer = void (*)(GLenum target, GLsizei numAttachments, const GLenum* attachments);
using PFN_glDiscardFramebufferEXT = void (*)(GLenum target, GLsizei numAttachments, const GLenum* attachments);
PFN_glInvalidateFramebuffer gGlInvalidateFramebuffer = nullptr;
PFN_glDiscardFramebufferEXT gGlDiscardFramebufferEXT = nullptr;

// --- Hook 1: eglSwapInterval ---
using EglSwapIntervalFn = EGLBoolean (*)(EGLDisplay, EGLint);
EglSwapIntervalFn orig_eglSwapInterval = nullptr;
pl::memory::HookHandle h_eglSwapInterval{};

EGLBoolean hook_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    const auto& cfg = getConfig();
    gEngineSwapInterval.store(static_cast<int>(interval), std::memory_order_relaxed);
    if (FrameBlendGl::wantsVsync()) {
        // Module 13: every queued frame (generated + real) must own a vsync.
        interval = 1;
    } else if (cfg.modFramePacerEnabled.load(std::memory_order_relaxed) &&
               cfg.vsyncBypass.load(std::memory_order_relaxed)) {
        interval = 0;
    }
    return orig_eglSwapInterval ? orig_eglSwapInterval(dpy, interval) : EGL_TRUE;
}

// --- Hook 2: eglSwapBuffers ---
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn orig_eglSwapBuffers = nullptr;
pl::memory::HookHandle h_eglSwapBuffers{};

void discardDepthStencilOnTbdr() noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modTbdrTileSaverEnabled.load(std::memory_order_relaxed) ||
        !cfg.discardDepthStencil.load(std::memory_order_relaxed)) {
        return;
    }

    GLint currentFbo = 0;
    glGetIntegerv(GlConsts::kGlFramebufferBinding, &currentFbo);
    if (currentFbo != 0) {
        return;
    }

    static const GLenum kAttachments[2] = {
        GlConsts::kGlDepthExt,
        GlConsts::kGlStencilExt
    };

    if (gGlInvalidateFramebuffer != nullptr) {
        gGlInvalidateFramebuffer(GlConsts::kGlFramebuffer, 2, kAttachments);
        getTelemetry().onTbdrTileDiscarded();
    } else if (gGlDiscardFramebufferEXT != nullptr) {
        gGlDiscardFramebufferEXT(GlConsts::kGlFramebuffer, 2, kAttachments);
        getTelemetry().onTbdrTileDiscarded();
    }
}

// Module 13 callback: presents the current back buffer through the ORIGINAL eglSwapBuffers.
bool presentViaOriginalSwap(void* dpy, void* surface) {
    if (orig_eglSwapBuffers == nullptr) return false;
    return orig_eglSwapBuffers(static_cast<EGLDisplay>(dpy), static_cast<EGLSurface>(surface)) == EGL_TRUE;
}

EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    tIsRenderThread = true;
    const auto& cfg = getConfig();

    // 1. Enforce the swap interval on the active display:
    //    Module 13 (Frame Blend) -> 1 (VSync), Module 1 (vsync bypass) -> 0,
    //    otherwise whatever the game itself requested.
    if (orig_eglSwapInterval != nullptr && dpy != EGL_NO_DISPLAY) {
        int wantInterval = gEngineSwapInterval.load(std::memory_order_relaxed);
        if (FrameBlendGl::wantsVsync()) {
            wantInterval = 1;
        } else if (cfg.modFramePacerEnabled.load(std::memory_order_relaxed) &&
                   cfg.vsyncBypass.load(std::memory_order_relaxed)) {
            wantInterval = 0;
        }
        if (wantInterval >= 0 && (dpy != gLastDisplay || wantInterval != gAppliedSwapInterval)) {
            gLastDisplay = dpy;
            gAppliedSwapInterval = wantInterval;
            orig_eglSwapInterval(dpy, wantInterval);
        }
    }

    // 2. Late-bind libminecraftpe.so symbols if needed
    if (!gMcSymbolsHooked.load(std::memory_order_relaxed)) {
        NativeHooks::tryResolveMinecraftSymbols();
    }

    // 3. Module 3: Periodic Big.LITTLE Prime Core affinity & priority boost on RenderThread
    CpuSchedulerOptimizer::onRenderFrameTick();

    // 3b. Module 13: Frame Blend. May present a generated frame (extra swap) and redraw the real one.
    FrameBlendPresentOps blendOps{};
    blendOps.present       = &presentViaOriginalSwap;
    blendOps.beforePresent = &discardDepthStencilOnTbdr;
    const int blendPresents = FrameBlendGl::onBeforeSwap(dpy, surface, blendOps);
    getTelemetry().setPresentsThisFrame(blendPresents);

    // 4. Module 5: Discard Depth & Stencil tile writeback before buffer swap
    discardDepthStencilOnTbdr();

    // 5. Reset GL shadow state before external overlays or buffer swap
    getGlOptimizer().resetFrameState();

    // 6. Perform actual buffer swap
    const EGLBoolean res = orig_eglSwapBuffers ? orig_eglSwapBuffers(dpy, surface) : EGL_FALSE;

    // 7. Module 1 & Module 12: Hybrid 90-FPS Micro-Pacer + Telemetry + Adaptive Governor
    getTelemetry().paceAndRecordFrame();

    // 8. Update HUD overlay draw commands
    Modules::onFrameRendered();

    return res;
}

// --- Hook 3: glHint ---
using GlHintFn = void (*)(GLenum, GLenum);
GlHintFn orig_glHint = nullptr;
pl::memory::HookHandle h_glHint{};

void hook_glHint(GLenum target, GLenum mode) {
    if (orig_glHint) {
        orig_glHint(target, getGlOptimizer().rewriteHintMode(target, mode));
    }
}

// --- Hook 4 & 5: glEnable / glDisable ---
using GlEnableDisableFn = void (*)(GLenum);
GlEnableDisableFn orig_glEnable = nullptr;
GlEnableDisableFn orig_glDisable = nullptr;
pl::memory::HookHandle h_glEnable{};
pl::memory::HookHandle h_glDisable{};

void hook_glEnable(GLenum cap) {
    if (tIsRenderThread && getGlOptimizer().shouldSkipCapabilityChange(cap, true)) {
        return;
    }
    if (orig_glEnable) orig_glEnable(cap);
}

void hook_glDisable(GLenum cap) {
    if (tIsRenderThread && getGlOptimizer().shouldSkipCapabilityChange(cap, false)) {
        return;
    }
    if (orig_glDisable) orig_glDisable(cap);
}

// --- Hook 6: glUseProgram ---
using GlUseProgramFn = void (*)(GLuint);
GlUseProgramFn orig_glUseProgram = nullptr;
pl::memory::HookHandle h_glUseProgram{};

void hook_glUseProgram(GLuint program) {
    if (tIsRenderThread && getGlOptimizer().shouldSkipUseProgram(program)) {
        return;
    }
    if (orig_glUseProgram) orig_glUseProgram(program);
}

// --- Hook 7 & 8: glActiveTexture / glBindTexture ---
using GlActiveTextureFn = void (*)(GLenum);
using GlBindTextureFn = void (*)(GLenum, GLuint);
GlActiveTextureFn orig_glActiveTexture = nullptr;
GlBindTextureFn orig_glBindTexture = nullptr;
pl::memory::HookHandle h_glActiveTexture{};
pl::memory::HookHandle h_glBindTexture{};

void hook_glActiveTexture(GLenum texture) {
    if (tIsRenderThread) {
        getGlOptimizer().onActiveTexture(texture);
    }
    if (orig_glActiveTexture) orig_glActiveTexture(texture);
}

void hook_glBindTexture(GLenum target, GLuint texture) {
    if (tIsRenderThread && getGlOptimizer().shouldSkipBindTexture(target, texture)) {
        return;
    }
    if (orig_glBindTexture) orig_glBindTexture(target, texture);
}

// --- Hook 9: glDepthMask ---
using GlDepthMaskFn = void (*)(GLboolean);
GlDepthMaskFn orig_glDepthMask = nullptr;
pl::memory::HookHandle h_glDepthMask{};

void hook_glDepthMask(GLboolean flag) {
    if (tIsRenderThread && getGlOptimizer().shouldSkipDepthMask(flag)) {
        return;
    }
    if (orig_glDepthMask) orig_glDepthMask(flag);
}

// --- Hook 10: glFinish ---
using GlFinishFn = void (*)();
GlFinishFn orig_glFinish = nullptr;
pl::memory::HookHandle h_glFinish{};

void hook_glFinish() {
    const auto& cfg = getConfig();
    if (cfg.modGlPipelineEnabled.load(std::memory_order_relaxed) &&
        cfg.suppressGlFinish.load(std::memory_order_relaxed)) {
        getTelemetry().onGlFinishSuppressed();
        return;
    }
    if (orig_glFinish) orig_glFinish();
}

// --- Hook 11: glClear ---
using GlClearFn = void (*)(GLbitfield);
GlClearFn orig_glClear = nullptr;
pl::memory::HookHandle h_glClear{};

void hook_glClear(GLbitfield mask) {
    if (orig_glClear) {
        orig_glClear(getGlOptimizer().rewriteClearMask(mask));
    }
}

// --- Hook 12 & 13: glTexParameteri / glTexParameterf ---
using GlTexParameteriFn = void (*)(GLenum, GLenum, GLint);
using GlTexParameterfFn = void (*)(GLenum, GLenum, GLfloat);
GlTexParameteriFn orig_glTexParameteri = nullptr;
GlTexParameterfFn orig_glTexParameterf = nullptr;
pl::memory::HookHandle h_glTexParameteri{};
pl::memory::HookHandle h_glTexParameterf{};

void hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    int32_t p = param;
    getGlOptimizer().rewriteTexParameteri(target, pname, p);
    if (orig_glTexParameteri) orig_glTexParameteri(target, pname, p);
}

void hook_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    float p = param;
    getGlOptimizer().rewriteTexParameterf(target, pname, p);
    if (orig_glTexParameterf) orig_glTexParameterf(target, pname, p);
}

// --- Hook 14 & 15: glDrawElements / glDrawArrays ---
using GlDrawElementsFn = void (*)(GLenum, GLsizei, GLenum, const void*);
using GlDrawArraysFn = void (*)(GLenum, GLint, GLsizei);
GlDrawElementsFn orig_glDrawElements = nullptr;
GlDrawArraysFn orig_glDrawArrays = nullptr;
pl::memory::HookHandle h_glDrawElements{};
pl::memory::HookHandle h_glDrawArrays{};

void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (tIsRenderThread && getGlOptimizer().shouldCullDrawCall(mode, count)) {
        return;
    }
    if (orig_glDrawElements) orig_glDrawElements(mode, count, type, indices);
}

void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (tIsRenderThread && getGlOptimizer().shouldCullDrawCall(mode, count)) {
        return;
    }
    if (orig_glDrawArrays) orig_glDrawArrays(mode, first, count);
}

// --- Hook 16: fopen (intercepts options.txt load to auto-tune before Bedrock reads it) ---
using FopenFn = FILE* (*)(const char*, const char*);
FopenFn orig_fopen = nullptr;
pl::memory::HookHandle h_fopen{};
thread_local bool tInFopenHook = false;

FILE* hook_fopen(const char* path, const char* mode) {
    if (!tInFopenHook && path != nullptr && mode != nullptr && mode[0] == 'r') {
        if (EngineOptionsTuner::isMinecraftOptionsPath(path) &&
            getConfig().modEngineOptionsEnabled.load(std::memory_order_relaxed)) {
            tInFopenHook = true;
            EngineOptionsTuner::tuneOptionsFile(path);
            tInFopenHook = false;
        }
    }
    return orig_fopen ? orig_fopen(path, mode) : nullptr;
}

// --- Hook 17: connect (applies TCP_NODELAY & IPTOS_LOWDELAY on game sockets) ---
using ConnectFn = int (*)(int, const struct sockaddr*, socklen_t);
ConnectFn orig_connect = nullptr;
pl::memory::HookHandle h_connect{};

int hook_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    CameraInput90HzStabilizer::optimizeSocketDescriptor(sockfd);
    return orig_connect ? orig_connect(sockfd, addr, addrlen) : -1;
}

// --- Hooks 18-20: libminecraftpe.so Engine Symbol Hooks ---
using LocalPlayerApplyTurnFn = void (*)(void* self, float dx, float dy);
LocalPlayerApplyTurnFn orig_LocalPlayer_applyTurn = nullptr;
pl::memory::HookHandle h_LocalPlayer_applyTurn{};

void hook_LocalPlayer_applyTurn(void* self, float dx, float dy) {
    const float lastMs = getTelemetry().getSnapshot().lastFrameMs;
    const float dtSec = std::clamp(lastMs * 0.001f, 0.004f, 0.050f);
    getCameraStabilizer().filterTurnDelta(dx, dy, dtSec);
    if (orig_LocalPlayer_applyTurn) {
        orig_LocalPlayer_applyTurn(self, dx, dy);
    }
}

using ActorRenderInterpFn = void (*)(void* self, void* actor, float a);
ActorRenderInterpFn orig_ActorRender_enableInterpolation = nullptr;
pl::memory::HookHandle h_ActorRender_enableInterpolation{};

void hook_ActorRender_enableInterpolation(void* self, void* actor, float a) {
    const auto& cfg = getConfig();
    if (cfg.modEntityParticleEnabled.load(std::memory_order_relaxed) &&
        cfg.fastEntityInterp.load(std::memory_order_relaxed)) {
        const auto tier = getTelemetry().getCurrentTier();
        if (tier >= GovernorTier::Aggressive90) {
            return;
        }
    }
    if (orig_ActorRender_enableInterpolation) {
        orig_ActorRender_enableInterpolation(self, actor, a);
    }
}

using OptionsGetGammaFn = float (*)(void* self);
OptionsGetGammaFn orig_Options_getGamma = nullptr;
pl::memory::HookHandle h_Options_getGamma{};

float hook_Options_getGamma(void* self) {
    const auto& cfg = getConfig();
    if (cfg.modCameraInputEnabled.load(std::memory_order_relaxed) &&
        cfg.fullbrightGamma.load(std::memory_order_relaxed)) {
        return 12.0f;
    }
    return orig_Options_getGamma ? orig_Options_getGamma(self) : 1.0f;
}

template <typename FnPtr>
void installSymHook(
    void* libHandle,
    const char* symName,
    FnPtr detour,
    FnPtr& origOut,
    pl::memory::HookHandle& handleOut) noexcept {
    if (handleOut.installed() || libHandle == nullptr) return;
    void* target = dlsym(libHandle, symName);
    if (!target) return;
    pl::memory::FuncPtr orig = nullptr;
    handleOut = pl::memory::HookHandle(
        target,
        reinterpret_cast<pl::memory::FuncPtr>(detour),
        &orig,
        pl::memory::HookPriority::Normal);
    if (handleOut.installed() && orig != nullptr) {
        origOut = reinterpret_cast<FnPtr>(orig);
    }
}

void unhookSafe(pl::memory::HookHandle& h) noexcept {
    h.reset();
}

} // namespace

GlHookBypassScope::GlHookBypassScope() noexcept : mPrevious(tIsRenderThread) {
    tIsRenderThread = false;
}

GlHookBypassScope::~GlHookBypassScope() noexcept {
    tIsRenderThread = mPrevious;
}

void NativeHooks::tryResolveMinecraftSymbols() noexcept {
    if (gMcSymbolsHooked.load(std::memory_order_relaxed)) return;

    void* mcLib = dlopen("libminecraftpe.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!mcLib) return;

    installSymHook(
        mcLib,
        "_ZN11LocalPlayer9applyTurnEff",
        hook_LocalPlayer_applyTurn,
        orig_LocalPlayer_applyTurn,
        h_LocalPlayer_applyTurn);
    installSymHook(
        mcLib,
        "_ZN11ActorRender19enableInterpolationEP5Actorf",
        hook_ActorRender_enableInterpolation,
        orig_ActorRender_enableInterpolation,
        h_ActorRender_enableInterpolation);
    installSymHook(
        mcLib,
        "_ZNK7Options8getGammaEv",
        hook_Options_getGamma,
        orig_Options_getGamma,
        h_Options_getGamma);

    dlclose(mcLib);
    gMcSymbolsHooked.store(true, std::memory_order_relaxed);
}

bool NativeHooks::installAll() noexcept {
    if (gHooksInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    // 1. Resolve TBDR framebuffer discard extensions
    void* gles3Lib = dlopen("libGLESv3.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!gles3Lib) gles3Lib = dlopen("libGLESv3.so", RTLD_LAZY);
    if (gles3Lib) {
        gGlInvalidateFramebuffer = reinterpret_cast<PFN_glInvalidateFramebuffer>(
            dlsym(gles3Lib, "glInvalidateFramebuffer"));
    }
    if (!gGlInvalidateFramebuffer) {
        gGlInvalidateFramebuffer = reinterpret_cast<PFN_glInvalidateFramebuffer>(
            eglGetProcAddress("glInvalidateFramebuffer"));
    }
    gGlDiscardFramebufferEXT = reinterpret_cast<PFN_glDiscardFramebufferEXT>(
        eglGetProcAddress("glDiscardFramebufferEXT"));

    // 2. Hook libEGL.so
    void* eglLib = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!eglLib) eglLib = dlopen("libEGL.so", RTLD_LAZY);
    if (eglLib) {
        installSymHook(eglLib, "eglSwapInterval", hook_eglSwapInterval, orig_eglSwapInterval, h_eglSwapInterval);
        installSymHook(eglLib, "eglSwapBuffers",  hook_eglSwapBuffers,  orig_eglSwapBuffers,  h_eglSwapBuffers);
    }

    // 3. Hook libGLESv2.so
    void* gles2Lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!gles2Lib) gles2Lib = dlopen("libGLESv2.so", RTLD_LAZY);
    if (gles2Lib) {
        installSymHook(gles2Lib, "glHint",           hook_glHint,           orig_glHint,           h_glHint);
        installSymHook(gles2Lib, "glEnable",         hook_glEnable,         orig_glEnable,         h_glEnable);
        installSymHook(gles2Lib, "glDisable",        hook_glDisable,        orig_glDisable,        h_glDisable);
        installSymHook(gles2Lib, "glUseProgram",     hook_glUseProgram,     orig_glUseProgram,     h_glUseProgram);
        installSymHook(gles2Lib, "glActiveTexture",  hook_glActiveTexture,  orig_glActiveTexture,  h_glActiveTexture);
        installSymHook(gles2Lib, "glBindTexture",    hook_glBindTexture,    orig_glBindTexture,    h_glBindTexture);
        installSymHook(gles2Lib, "glDepthMask",      hook_glDepthMask,      orig_glDepthMask,      h_glDepthMask);
        installSymHook(gles2Lib, "glFinish",         hook_glFinish,         orig_glFinish,         h_glFinish);
        installSymHook(gles2Lib, "glClear",          hook_glClear,          orig_glClear,          h_glClear);
        installSymHook(gles2Lib, "glTexParameteri",  hook_glTexParameteri,  orig_glTexParameteri,  h_glTexParameteri);
        installSymHook(gles2Lib, "glTexParameterf",  hook_glTexParameterf,  orig_glTexParameterf,  h_glTexParameterf);
        installSymHook(gles2Lib, "glDrawElements",   hook_glDrawElements,   orig_glDrawElements,   h_glDrawElements);
        installSymHook(gles2Lib, "glDrawArrays",     hook_glDrawArrays,     orig_glDrawArrays,     h_glDrawArrays);
    }

    // 4. Hook libc.so (fopen for options.txt auto-tuner, connect for low-delay sockets)
    void* libcHandle = dlopen("libc.so", RTLD_LAZY | RTLD_NOLOAD);
    if (libcHandle) {
        installSymHook(libcHandle, "fopen",   hook_fopen,   orig_fopen,   h_fopen);
        installSymHook(libcHandle, "connect", hook_connect, orig_connect, h_connect);
        dlclose(libcHandle);
    }

    // 5. Try resolving libminecraftpe.so symbols immediately (or defer until first swap)
    tryResolveMinecraftSymbols();

    gHooksInstalled.store(true, std::memory_order_relaxed);
    return true;
}

void NativeHooks::uninstallAll() noexcept {
    if (!gHooksInstalled.load(std::memory_order_relaxed)) {
        return;
    }

    unhookSafe(h_LocalPlayer_applyTurn);
    unhookSafe(h_ActorRender_enableInterpolation);
    unhookSafe(h_Options_getGamma);
    unhookSafe(h_connect);
    unhookSafe(h_fopen);
    unhookSafe(h_glDrawArrays);
    unhookSafe(h_glDrawElements);
    unhookSafe(h_glTexParameterf);
    unhookSafe(h_glTexParameteri);
    unhookSafe(h_glClear);
    unhookSafe(h_glFinish);
    unhookSafe(h_glDepthMask);
    unhookSafe(h_glBindTexture);
    unhookSafe(h_glActiveTexture);
    unhookSafe(h_glUseProgram);
    unhookSafe(h_glDisable);
    unhookSafe(h_glEnable);
    unhookSafe(h_glHint);
    unhookSafe(h_eglSwapBuffers);
    unhookSafe(h_eglSwapInterval);

    FrameBlendGl::resetHistory();
    gLastDisplay = EGL_NO_DISPLAY;
    gAppliedSwapInterval = -1;
    gMcSymbolsHooked.store(false, std::memory_order_relaxed);
    gHooksInstalled.store(false, std::memory_order_relaxed);
}

bool NativeHooks::isInstalled() noexcept {
    return gHooksInstalled.load(std::memory_order_relaxed);
}

#else // Host build (for unit tests)

namespace {
std::atomic<bool> gHostHooksInstalled{false};
}

GlHookBypassScope::GlHookBypassScope() noexcept = default;
GlHookBypassScope::~GlHookBypassScope() noexcept = default;

void NativeHooks::tryResolveMinecraftSymbols() noexcept {}

bool NativeHooks::installAll() noexcept {
    gHostHooksInstalled.store(true, std::memory_order_relaxed);
    return true;
}

void NativeHooks::uninstallAll() noexcept {
    gHostHooksInstalled.store(false, std::memory_order_relaxed);
}

bool NativeHooks::isInstalled() noexcept {
    return gHostHooksInstalled.load(std::memory_order_relaxed);
}

#endif

} // namespace hyper90
