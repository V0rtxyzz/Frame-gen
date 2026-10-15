#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hyper90 {

// ============================================================================
// Portable GL Constants (GLES2 / GLES3)
// ============================================================================
namespace GlConsts {
inline constexpr uint32_t kGlDepthBufferBit               = 0x00000100u;
inline constexpr uint32_t kGlStencilBufferBit             = 0x00000400u;
inline constexpr uint32_t kGlColorBufferBit               = 0x00004000u;
inline constexpr uint32_t kGlTriangles                    = 0x0004u;
inline constexpr uint32_t kGlCullFace                     = 0x0B44u;
inline constexpr uint32_t kGlDepthTest                    = 0x0B71u;
inline constexpr uint32_t kGlDither                       = 0x0BD0u;
inline constexpr uint32_t kGlBlend                        = 0x0BE2u;
inline constexpr uint32_t kGlScissorTest                  = 0x0C11u;
inline constexpr uint32_t kGlTexture2D                    = 0x0DE1u;
inline constexpr uint32_t kGlFastest                      = 0x1101u;
inline constexpr uint32_t kGlDepthExt                     = 0x1801u;
inline constexpr uint32_t kGlStencilExt                   = 0x1802u;
inline constexpr uint32_t kGlNearestMipmapNearest         = 0x2700u;
inline constexpr uint32_t kGlLinearMipmapNearest          = 0x2701u;
inline constexpr uint32_t kGlNearestMipmapLinear          = 0x2702u;
inline constexpr uint32_t kGlLinearMipmapLinear           = 0x2703u;
inline constexpr uint32_t kGlTextureMinFilter             = 0x2801u;
inline constexpr uint32_t kGlMultisampleExt               = 0x809Du;
inline constexpr uint32_t kGlSampleAlphaToCoverage        = 0x809Eu;
inline constexpr uint32_t kGlTextureMaxLevel              = 0x813Du;
inline constexpr uint32_t kGlGenerateMipmapHint           = 0x8192u;
inline constexpr uint32_t kGlTexture0                     = 0x84C0u;
inline constexpr uint32_t kGlTextureMaxAnisotropyExt      = 0x84FEu;
inline constexpr uint32_t kGlFragmentShaderDerivativeHint = 0x8B8Bu;
inline constexpr uint32_t kGlFramebuffer                  = 0x8D40u;
inline constexpr uint32_t kGlFramebufferBinding           = 0x8CA6u;
} // namespace GlConsts

// ============================================================================
// RenderThread GL State Deduplicator & Optimizer Engine
// ============================================================================
class alignas(64) GlPipelineOptimizer {
public:
    static constexpr size_t kTrackedTexUnits = 16;

    GlPipelineOptimizer() noexcept { resetFrameState(); }

    void resetFrameState() noexcept;

    // Module 4: Returns true if the glEnable/glDisable call should be skipped/suppressed
    bool shouldSkipCapabilityChange(uint32_t cap, bool enable) noexcept;

    // Module 4: Rewrites glHint mode to GL_FASTEST when appropriate
    uint32_t rewriteHintMode(uint32_t target, uint32_t mode) const noexcept;

    // Module 4: Returns true if glUseProgram(program) is redundant
    bool shouldSkipUseProgram(uint32_t program) noexcept;

    // Module 4: Tracks active texture unit and deduplicates glBindTexture(GL_TEXTURE_2D, tex)
    void onActiveTexture(uint32_t textureEnum) noexcept;
    bool shouldSkipBindTexture(uint32_t target, uint32_t texture) noexcept;

    // Module 4: Deduplicates glDepthMask(flag)
    bool shouldSkipDepthMask(uint8_t flag) noexcept;

    // Module 5: Rewrites glClear mask to combine depth+stencil on TBDR GPUs
    uint32_t rewriteClearMask(uint32_t mask) const noexcept;

    // Module 6: Rewrites glTexParameteri / glTexParameterf for fast mipmaps & clamped anisotropy
    void rewriteTexParameteri(uint32_t target, uint32_t pname, int32_t& param) noexcept;
    void rewriteTexParameterf(uint32_t target, uint32_t pname, float& param) noexcept;

    // Module 7 & 8: Determines whether a draw call should be culled (degenerate or micro-batch/particle budget)
    bool shouldCullDrawCall(uint32_t mode, int32_t count) noexcept;

private:
    int8_t   mBlendState{-1};      // -1=unknown, 0=disabled, 1=enabled
    int8_t   mDepthTestState{-1};
    int8_t   mCullFaceState{-1};
    int8_t   mScissorTestState{-1};
    int8_t   mDepthMaskState{-1};
    uint32_t mCurrentProgram{0xFFFFFFFFu};
    uint32_t mActiveTexUnit{0};
    std::array<uint32_t, kTrackedTexUnits> mBoundTex2D{};
    uint32_t mMicroBatchCounter{0};
};

GlPipelineOptimizer& getGlOptimizer() noexcept;

// ============================================================================
// RAII guard: while alive, GL calls made by the mod itself on the render thread
// skip the state deduplicator / draw culler hooks (they go straight to the driver).
// Used by Module 13 (Frame Blend) for its own full-screen passes. No-op on host.
// ============================================================================
class GlHookBypassScope {
public:
    GlHookBypassScope() noexcept;
    ~GlHookBypassScope() noexcept;
    GlHookBypassScope(const GlHookBypassScope&) = delete;
    GlHookBypassScope& operator=(const GlHookBypassScope&) = delete;

private:
    bool mPrevious{false};
};

// ============================================================================
// Native Hook Lifecycle Manager
// ============================================================================
class NativeHooks {
public:
    static bool installAll() noexcept;
    static void uninstallAll() noexcept;
    static bool isInstalled() noexcept;

    // Attempt late resolution of libminecraftpe.so symbols if loaded after enable()
    static void tryResolveMinecraftSymbols() noexcept;
};

} // namespace hyper90
