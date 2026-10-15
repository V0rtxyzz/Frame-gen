#include "hyper90/Telemetry.hpp"
#include "hyper90/Config.hpp"

#include <algorithm>
#include <cmath>
#include <time.h>

namespace hyper90 {

namespace {
inline void cpuRelaxYield() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#endif
}
} // namespace

TelemetryEngine& getTelemetry() noexcept {
    static TelemetryEngine sEngine;
    return sEngine;
}

const char* governorTierName(GovernorTier tier) noexcept {
    switch (tier) {
        case GovernorTier::Locked90Plus: return "LOCKED 90+";
        case GovernorTier::Balanced90:   return "BALANCED 90";
        case GovernorTier::Aggressive90: return "AGGRESSIVE 90";
        case GovernorTier::Ultra90Lock:  return "ULTRA 90-LOCK";
    }
    return "LOCKED 90+";
}

TelemetryEngine::TelemetryEngine() noexcept {
    mFrameDurationsNs.fill(11'111'111LL);
}

int64_t TelemetryEngine::monotonicNowNs() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + static_cast<int64_t>(ts.tv_nsec);
}

void TelemetryEngine::paceAndRecordFrame() noexcept {
    const auto& cfg = getConfig();
    int64_t nowNs = monotonicNowNs();

    if (mLastSwapTimestampNs == 0) {
        mLastSwapTimestampNs = nowNs;
        return;
    }

    int64_t rawElapsedNs = nowNs - mLastSwapTimestampNs;
    if (rawElapsedNs < 100'000LL) {
        rawElapsedNs = 100'000LL;
    }

    // --- Module 13: Frame Blend. 0 = inactive (legacy behaviour), 1 = real only, 2 = generated + real.
    // While active the swap is VSync-locked, so the sleep pacer stays idle and every presented
    // frame (generated ones included) counts towards the displayed FPS.
    const int presents = std::clamp(mPresentsThisFrame.exchange(0, std::memory_order_relaxed), 0, 2);
    const bool blendLocked = presents >= 1;

    // --- Module 1: 90 FPS Unlocker & Hybrid Micro-Pacer ---
    const bool pacerActive =
        cfg.modFramePacerEnabled.load(std::memory_order_relaxed) && !blendLocked;
    const int pacingMode = cfg.pacingMode.load(std::memory_order_relaxed);
    const int targetFps = std::clamp(cfg.targetFps.load(std::memory_order_relaxed), 60, 240);

    // Target frame budget in nanoseconds (e.g. 90 FPS -> 11,111,111 ns)
    const int64_t targetFrameNs = 1'000'000'000LL / static_cast<int64_t>(targetFps);

    // Anti-Stutter Spike Guard: if the previous frame exceeded budget by >5%,
    // skip pacing sleep on this frame so the render queue catches up immediately.
    const bool skipWaitDueToSpike =
        cfg.antiStutterGuard.load(std::memory_order_relaxed) && mLastFrameWasSpike;

    if (pacerActive && pacingMode > 0 && !skipWaitDueToSpike && rawElapsedNs < targetFrameNs) {
        const int64_t deadlineNs = mLastSwapTimestampNs + targetFrameNs;
        int64_t remainingNs = deadlineNs - nowNs;

        // Coarse kernel sleep up to 350 us before deadline
        constexpr int64_t kSpinMarginNs = 350'000LL;
        if (remainingNs > kSpinMarginNs) {
            const int64_t sleepNs = remainingNs - kSpinMarginNs;
            struct timespec req {};
            req.tv_sec = static_cast<time_t>(sleepNs / 1'000'000'000LL);
            req.tv_nsec = static_cast<long>(sleepNs % 1'000'000'000LL);
            clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
            nowNs = monotonicNowNs();
        }

        // Hybrid Micro-Pace sub-350us spin-yield to hit exact 11.111ms frame cadence
        if (pacingMode == 2) {
            int spinGuard = 0;
            while (nowNs < deadlineNs && spinGuard < 4096) {
                cpuRelaxYield();
                nowNs = monotonicNowNs();
                ++spinGuard;
            }
        }
    }

    const int64_t finalDurationNs = std::max<int64_t>(100'000LL, nowNs - mLastSwapTimestampNs);
    mLastSwapTimestampNs = nowNs;

    if (blendLocked) {
        // Split the real frame time evenly across every frame that reached the display.
        const int64_t shareNs = std::max<int64_t>(100'000LL, finalDurationNs / presents);
        mLastFrameWasSpike = (shareNs > (targetFrameNs + targetFrameNs / 18));
        for (int i = 1; i < presents; ++i) {
            recordGeneratedFrameDurationNs(shareNs);
        }
        recordFrameDurationNs(shareNs);
        return;
    }

    mLastFrameWasSpike = (finalDurationNs > (targetFrameNs + targetFrameNs / 18));
    recordFrameDurationNs(finalDurationNs);
}

void TelemetryEngine::recordFrameDurationNs(int64_t durationNs) noexcept {
    durationNs = std::clamp<int64_t>(durationNs, 100'000LL, 500'000'000LL);

    const int draws = mCurrentFrameDraws.exchange(0, std::memory_order_relaxed);
    const int culled = mCurrentFrameCulled.exchange(0, std::memory_order_relaxed);
    mLastFrameDraws.store(draws, std::memory_order_relaxed);
    mLastFrameCulled.store(culled, std::memory_order_relaxed);

    const uint64_t frameNo = mTotalFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    pushSampleAndRefresh(durationNs, frameNo);
}

void TelemetryEngine::recordGeneratedFrameDurationNs(int64_t durationNs) noexcept {
    durationNs = std::clamp<int64_t>(durationNs, 100'000LL, 500'000'000LL);
    mGeneratedFrames.fetch_add(1, std::memory_order_relaxed);
    pushSampleAndRefresh(durationNs, mTotalFrames.load(std::memory_order_relaxed));
}

void TelemetryEngine::pushSampleAndRefresh(int64_t durationNs, uint64_t frameNo) noexcept {
    mFrameDurationsNs[mWriteIndex] = durationNs;
    mWriteIndex = (mWriteIndex + 1) % kWindowSize;
    if (mSampleCount < kWindowSize) {
        ++mSampleCount;
    }

    const float lastMs = static_cast<float>(durationNs) / 1'000'000.0f;
    mLastFrameMs.store(lastMs, std::memory_order_relaxed);

    // Recompute windowed average FPS, 1% Low FPS, and Jitter every frame (window <= 120)
    int64_t totalNs = 0;
    int64_t worstNs = 0;
    int64_t secondWorstNs = 0;
    int64_t jitterAccumNs = 0;

    int64_t prevNs = mFrameDurationsNs[0];
    for (size_t i = 0; i < mSampleCount; ++i) {
        const int64_t d = mFrameDurationsNs[i];
        totalNs += d;
        if (d >= worstNs) {
            secondWorstNs = worstNs;
            worstNs = d;
        } else if (d > secondWorstNs) {
            secondWorstNs = d;
        }
        if (i > 0) {
            jitterAccumNs += std::abs(d - prevNs);
            prevNs = d;
        }
    }

    const float avgNs = static_cast<float>(totalNs) / static_cast<float>(mSampleCount);
    const float avgFps = (avgNs > 1.0f) ? (1'000'000'000.0f / avgNs) : 90.0f;
    const int64_t lowRefNs = (mSampleCount >= 20 && secondWorstNs > 0) ? secondWorstNs : worstNs;
    const float onePctLowFps = (lowRefNs > 0)
        ? (1'000'000'000.0f / static_cast<float>(lowRefNs))
        : avgFps;
    const float jitterMs = (mSampleCount > 1)
        ? (static_cast<float>(jitterAccumNs) / static_cast<float>(mSampleCount - 1)) / 1'000'000.0f
        : 0.0f;

    mAvgFps.store(avgFps, std::memory_order_relaxed);
    mOnePercentLowFps.store(onePctLowFps, std::memory_order_relaxed);
    mJitterMs.store(jitterMs, std::memory_order_relaxed);

    // Evaluate the 4-tier Adaptive 90-Lock Governor
    if ((frameNo & 0x7u) == 0u || mSampleCount <= 16) {
        evaluateGovernor(avgFps, onePctLowFps, lastMs);
    }
}

void TelemetryEngine::evaluateGovernor(float avgFps, float onePctLow, float lastFrameMs) noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modAdaptiveGovernorEnabled.load(std::memory_order_relaxed)) {
        mGovernorTier.store(static_cast<int>(GovernorTier::Locked90Plus), std::memory_order_relaxed);
        return;
    }

    if (cfg.ultraBoostOverride.load(std::memory_order_relaxed)) {
        mGovernorTier.store(static_cast<int>(GovernorTier::Ultra90Lock), std::memory_order_relaxed);
        return;
    }

    if (!cfg.adaptive90Governor.load(std::memory_order_relaxed)) {
        mGovernorTier.store(static_cast<int>(GovernorTier::Balanced90), std::memory_order_relaxed);
        return;
    }

    const float target = static_cast<float>(cfg.targetFps.load(std::memory_order_relaxed));
    GovernorTier nextTier = GovernorTier::Locked90Plus;

    if (avgFps < target * 0.86f || onePctLow < target * 0.78f || lastFrameMs > 14.5f) {
        nextTier = GovernorTier::Ultra90Lock;
    } else if (avgFps < target * 0.94f || onePctLow < target * 0.86f) {
        nextTier = GovernorTier::Aggressive90;
    } else if (avgFps < target * 0.99f || onePctLow < target * 0.92f) {
        nextTier = GovernorTier::Balanced90;
    } else {
        nextTier = GovernorTier::Locked90Plus;
    }

    mGovernorTier.store(static_cast<int>(nextTier), std::memory_order_relaxed);
}

void TelemetryEngine::resetCounters() noexcept {
    mGlStateDedupSaved.store(0, std::memory_order_relaxed);
    mTbdrTilesDiscarded.store(0, std::memory_order_relaxed);
    mDrawCallsCulled.store(0, std::memory_order_relaxed);
    mGlFinishSuppressed.store(0, std::memory_order_relaxed);
    mTextureFiltersOptimized.store(0, std::memory_order_relaxed);
    mHudCacheHits.store(0, std::memory_order_relaxed);
    mGeneratedFrames.store(0, std::memory_order_relaxed);
    mPresentsThisFrame.store(0, std::memory_order_relaxed);
    mTotalFrames.store(0, std::memory_order_relaxed);
    mSampleCount = 0;
    mWriteIndex = 0;
    mLastSwapTimestampNs = 0;
}

FrameSnapshot TelemetryEngine::getSnapshot() const noexcept {
    FrameSnapshot s{};
    s.avgFps                  = mAvgFps.load(std::memory_order_relaxed);
    s.onePercentLowFps        = mOnePercentLowFps.load(std::memory_order_relaxed);
    s.lastFrameMs             = mLastFrameMs.load(std::memory_order_relaxed);
    s.jitterMs                = mJitterMs.load(std::memory_order_relaxed);
    s.tier                    = static_cast<GovernorTier>(mGovernorTier.load(std::memory_order_relaxed));
    s.totalFrames             = mTotalFrames.load(std::memory_order_relaxed);
    s.glStateDedupSaved       = mGlStateDedupSaved.load(std::memory_order_relaxed);
    s.tbdrTilesDiscarded      = mTbdrTilesDiscarded.load(std::memory_order_relaxed);
    s.drawCallsCulled         = mDrawCallsCulled.load(std::memory_order_relaxed);
    s.glFinishSuppressed      = mGlFinishSuppressed.load(std::memory_order_relaxed);
    s.textureFiltersOptimized = mTextureFiltersOptimized.load(std::memory_order_relaxed);
    s.hudCacheHits            = mHudCacheHits.load(std::memory_order_relaxed);
    s.generatedFrames         = mGeneratedFrames.load(std::memory_order_relaxed);
    s.lastFrameDrawCalls      = mLastFrameDraws.load(std::memory_order_relaxed);
    s.lastFrameCulledDraws    = mLastFrameCulled.load(std::memory_order_relaxed);
    s.pinnedBigCores          = mPinnedBigCores.load(std::memory_order_relaxed);
    s.optionsFilesTuned       = mOptionsFilesTuned.load(std::memory_order_relaxed);
    return s;
}

} // namespace hyper90
