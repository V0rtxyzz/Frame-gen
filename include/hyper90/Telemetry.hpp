#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace hyper90 {

enum class GovernorTier : int {
    Locked90Plus = 0, // >= 89.5 FPS: Rock-solid 90 FPS locked
    Balanced90   = 1, // 84.0 - 89.5 FPS: Mild dynamic LOD & mipmap boost
    Aggressive90 = 2, // 75.0 - 84.0 FPS: Particle budget & micro-batch culling active
    Ultra90Lock  = 3  // < 75.0 FPS or manual Ultra override: Maximum 90-FPS rescue
};

struct FrameSnapshot {
    float        avgFps{90.0f};
    float        onePercentLowFps{90.0f};
    float        lastFrameMs{11.11f};
    float        jitterMs{0.0f};
    GovernorTier tier{GovernorTier::Locked90Plus};
    uint64_t     totalFrames{0};
    uint64_t     glStateDedupSaved{0};
    uint64_t     tbdrTilesDiscarded{0};
    uint64_t     drawCallsCulled{0};
    uint64_t     glFinishSuppressed{0};
    uint64_t     textureFiltersOptimized{0};
    uint64_t     generatedFrames{0};
    uint64_t     hudCacheHits{0};
    int          lastFrameDrawCalls{0};
    int          lastFrameCulledDraws{0};
    int          pinnedBigCores{0};
    int          optionsFilesTuned{0};
};

class alignas(64) TelemetryEngine {
public:
    static constexpr size_t kWindowSize = 120;

    TelemetryEngine() noexcept;

    // Called inside eglSwapBuffers: paces the frame to targetFps and records metrics.
    void paceAndRecordFrame() noexcept;

    // Direct frame recording for deterministic testing with synthetic timestamps.
    void recordFrameDurationNs(int64_t durationNs) noexcept;

    // Module 13: records one extra presented (generated) frame in the FPS window without
    // touching per-frame draw counters. Used by paceAndRecordFrame() and unit tests.
    void recordGeneratedFrameDurationNs(int64_t durationNs) noexcept;

    // Module 13: how many frames reach the display for the real frame about to be recorded.
    // 0 = Frame Blend inactive (legacy pacing), 1 = real only, 2 = generated + real.
    void setPresentsThisFrame(int presents) noexcept {
        mPresentsThisFrame.store(presents, std::memory_order_relaxed);
    }

    void resetCounters() noexcept;

    FrameSnapshot getSnapshot() const noexcept;
    GovernorTier getCurrentTier() const noexcept {
        return static_cast<GovernorTier>(mGovernorTier.load(std::memory_order_relaxed));
    }

    int getLastFrameDrawCalls() const noexcept {
        return mCurrentFrameDraws.load(std::memory_order_relaxed);
    }

    // Per-module atomic telemetry increments
    void onGlStateDeduped() noexcept {
        mGlStateDedupSaved.fetch_add(1, std::memory_order_relaxed);
    }
    void onTbdrTileDiscarded() noexcept {
        mTbdrTilesDiscarded.fetch_add(1, std::memory_order_relaxed);
    }
    void onDrawCallSubmitted() noexcept {
        mCurrentFrameDraws.fetch_add(1, std::memory_order_relaxed);
    }
    void onDrawCallCulled() noexcept {
        mCurrentFrameCulled.fetch_add(1, std::memory_order_relaxed);
        mDrawCallsCulled.fetch_add(1, std::memory_order_relaxed);
    }
    void onGlFinishSuppressed() noexcept {
        mGlFinishSuppressed.fetch_add(1, std::memory_order_relaxed);
    }
    void onTextureFilterOptimized() noexcept {
        mTextureFiltersOptimized.fetch_add(1, std::memory_order_relaxed);
    }
    void onHudCacheHit() noexcept {
        mHudCacheHits.fetch_add(1, std::memory_order_relaxed);
    }
    void setPinnedBigCores(int count) noexcept {
        mPinnedBigCores.store(count, std::memory_order_relaxed);
    }
    void setOptionsFilesTuned(int count) noexcept {
        mOptionsFilesTuned.store(count, std::memory_order_relaxed);
    }

    static int64_t monotonicNowNs() noexcept;

private:
    void pushSampleAndRefresh(int64_t durationNs, uint64_t frameNo) noexcept;
    void evaluateGovernor(float avgFps, float onePctLow, float lastFrameMs) noexcept;

    std::array<int64_t, kWindowSize> mFrameDurationsNs{};
    size_t   mWriteIndex{0};
    size_t   mSampleCount{0};
    int64_t  mLastSwapTimestampNs{0};
    bool     mLastFrameWasSpike{false};

    std::atomic<float>    mAvgFps{90.0f};
    std::atomic<float>    mOnePercentLowFps{90.0f};
    std::atomic<float>    mLastFrameMs{11.11f};
    std::atomic<float>    mJitterMs{0.0f};
    std::atomic<int>      mGovernorTier{static_cast<int>(GovernorTier::Locked90Plus)};

    std::atomic<uint64_t> mTotalFrames{0};
    std::atomic<uint64_t> mGlStateDedupSaved{0};
    std::atomic<uint64_t> mTbdrTilesDiscarded{0};
    std::atomic<uint64_t> mDrawCallsCulled{0};
    std::atomic<uint64_t> mGlFinishSuppressed{0};
    std::atomic<uint64_t> mTextureFiltersOptimized{0};
    std::atomic<uint64_t> mHudCacheHits{0};
    std::atomic<uint64_t> mGeneratedFrames{0};
    std::atomic<int>      mPresentsThisFrame{0};

    std::atomic<int>      mCurrentFrameDraws{0};
    std::atomic<int>      mCurrentFrameCulled{0};
    std::atomic<int>      mLastFrameDraws{0};
    std::atomic<int>      mLastFrameCulled{0};
    std::atomic<int>      mPinnedBigCores{0};
    std::atomic<int>      mOptionsFilesTuned{0};
};

TelemetryEngine& getTelemetry() noexcept;
const char* governorTierName(GovernorTier tier) noexcept;

} // namespace hyper90
