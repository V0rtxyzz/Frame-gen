#include "hyper90/SystemOptimizers.hpp"
#include "hyper90/Config.hpp"
#include "hyper90/Telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hyper90 {

// ============================================================================
// Module 3: Big.LITTLE Prime Core & Thread Booster
// ============================================================================
std::vector<int> CpuSchedulerOptimizer::selectPerformanceCores(
    const std::vector<int64_t>& coreMaxFreqKhz, int mode) {
    const int n = static_cast<int>(coreMaxFreqKhz.size());
    std::vector<int> selected;
    if (n <= 0) return selected;

    if (mode == 2 || n <= 2) {
        for (int i = 0; i < n; ++i) selected.push_back(i);
        return selected;
    }

    int64_t maxFreq = 0;
    int64_t minFreq = INT64_MAX;
    for (int64_t f : coreMaxFreqKhz) {
        if (f > maxFreq) maxFreq = f;
        if (f > 0 && f < minFreq) minFreq = f;
    }

    // If mode == 0 (AutoMaxFreq) and sysfs reported heterogeneous frequencies:
    // select all cores running at >= 75% of the prime core's max frequency.
    if (mode == 0 && maxFreq > 0 && minFreq < maxFreq) {
        const int64_t threshold = (maxFreq * 75) / 100;
        for (int i = 0; i < n; ++i) {
            if (coreMaxFreqKhz[i] >= threshold) {
                selected.push_back(i);
            }
        }
        if (!selected.empty()) {
            return selected;
        }
    }

    // Fallback or mode == 1 (UpperHalf): on standard Android big.LITTLE SoCs,
    // cores [n/2 .. n-1] are the performance (Big + Prime) cluster.
    const int startCore = std::max(1, n / 2);
    for (int i = startCore; i < n; ++i) {
        selected.push_back(i);
    }
    return selected;
}

namespace {
int64_t readSysfsCpuMaxFreqKhz(int coreIndex) noexcept {
    char pathBuf[128]{};
    std::snprintf(
        pathBuf, sizeof(pathBuf),
        "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", coreIndex);
    std::ifstream ifs(pathBuf);
    if (!ifs) {
        std::snprintf(
            pathBuf, sizeof(pathBuf),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", coreIndex);
        ifs.open(pathBuf);
    }
    int64_t freq = 0;
    if (ifs) {
        ifs >> freq;
    }
    return freq;
}
} // namespace

int CpuSchedulerOptimizer::applyRenderThreadBoost(bool /*forceReapply*/) noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modCpuSchedulerEnabled.load(std::memory_order_relaxed)) {
        return 0;
    }

    long numCores = sysconf(_SC_NPROCESSORS_CONF);
    if (numCores <= 0 || numCores > 32) {
        numCores = 8;
    }

    std::vector<int64_t> freqs(static_cast<size_t>(numCores), 0);
    for (int i = 0; i < static_cast<int>(numCores); ++i) {
        freqs[static_cast<size_t>(i)] = readSysfsCpuMaxFreqKhz(i);
    }

    const int mode = cfg.coreSelectionMode.load(std::memory_order_relaxed);
    const auto selectedCores = selectPerformanceCores(freqs, mode);

    if (cfg.pinRenderThread.load(std::memory_order_relaxed) && !selectedCores.empty()) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (int core : selectedCores) {
            if (core >= 0 && core < CPU_SETSIZE) {
                CPU_SET(core, &mask);
            }
        }
        (void)sched_setaffinity(0, sizeof(cpu_set_t), &mask);
    }

    if (cfg.boostThreadPriority.load(std::memory_order_relaxed)) {
        // Reduce timer slack to 10 microseconds so clock_nanosleep in Frame Pacer wakes accurately
#if defined(PR_SET_TIMERSLACK)
        (void)prctl(PR_SET_TIMERSLACK, 10'000UL, 0UL, 0UL, 0UL);
#endif
        // Try high-priority nice values (-10 -> -8 -> -5 -> -2)
        for (int niceVal : {-10, -8, -5, -2}) {
            if (setpriority(PRIO_PROCESS, 0, niceVal) == 0) {
                break;
            }
        }
    }

    const int pinnedCount = static_cast<int>(selectedCores.size());
    getTelemetry().setPinnedBigCores(pinnedCount);
    return pinnedCount;
}

void CpuSchedulerOptimizer::onRenderFrameTick() noexcept {
    static int64_t sLastBoostNs = 0;
    const auto& cfg = getConfig();
    if (!cfg.modCpuSchedulerEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    const int64_t nowNs = TelemetryEngine::monotonicNowNs();
    const int64_t intervalNs =
        static_cast<int64_t>(std::clamp(cfg.reapplyIntervalSec.load(std::memory_order_relaxed), 1, 30)) *
        1'000'000'000LL;

    if (sLastBoostNs == 0 || (nowNs - sLastBoostNs) >= intervalNs) {
        sLastBoostNs = nowNs;
        applyRenderThreadBoost(false);
    }
}

// ============================================================================
// Module 9: Zero-Alloc HUD & UI Batch Accelerator
// ============================================================================
HudCacheEngine& getHudCache() noexcept {
    static HudCacheEngine sHudCache;
    return sHudCache;
}

bool HudCacheEngine::checkAndUpdateHotbarSlot(size_t slot, const HotbarSlotState& state) noexcept {
    if (slot >= kHotbarSlots) return false;
    if (!getConfig().modHudAcceleratorEnabled.load(std::memory_order_relaxed) ||
        !getConfig().cacheHotbarVitals.load(std::memory_order_relaxed)) {
        mHotbar[slot] = state;
        mHotbarValid[slot] = true;
        return false;
    }

    if (mHotbarValid[slot] && mHotbar[slot] == state) {
        getTelemetry().onHudCacheHit();
        return true;
    }
    mHotbar[slot] = state;
    mHotbarValid[slot] = true;
    return false;
}

bool HudCacheEngine::checkAndUpdateVitals(const PlayerVitalsState& state) noexcept {
    if (!getConfig().modHudAcceleratorEnabled.load(std::memory_order_relaxed) ||
        !getConfig().cacheHotbarVitals.load(std::memory_order_relaxed)) {
        mVitals = state;
        mVitalsValid = true;
        return false;
    }

    if (mVitalsValid && mVitals == state) {
        getTelemetry().onHudCacheHit();
        return true;
    }
    mVitals = state;
    mVitalsValid = true;
    return false;
}

bool HudCacheEngine::checkAndUpdateText(size_t textSlotId, std::string_view text) noexcept {
    const size_t idx = textSlotId % kTextSlotCap;
    const uint64_t h = fnv1a64(text);

    if (!getConfig().modHudAcceleratorEnabled.load(std::memory_order_relaxed) ||
        !getConfig().cacheUiText.load(std::memory_order_relaxed)) {
        mTextHashes[idx] = h;
        mTextValid[idx] = true;
        return false;
    }

    if (mTextValid[idx] && mTextHashes[idx] == h) {
        getTelemetry().onHudCacheHit();
        return true;
    }
    mTextHashes[idx] = h;
    mTextValid[idx] = true;
    return false;
}

void HudCacheEngine::invalidateAll() noexcept {
    mHotbarValid.fill(false);
    mVitalsValid = false;
    mTextValid.fill(false);
}

// ============================================================================
// Module 10: Bionic Heap & Page-Fault Stutter Shield
// ============================================================================
namespace {
alignas(4096) uint8_t sHotScratchArena[65536]{};
bool sPagesLocked = false;
} // namespace

bool MemoryArenaOptimizer::applyBionicHeapTuning() noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modMemoryArenaEnabled.load(std::memory_order_relaxed) ||
        !cfg.bionicHeapRetain.load(std::memory_order_relaxed)) {
        return false;
    }

    using MalloptFn = int (*)(int, int);
    auto fn = reinterpret_cast<MalloptFn>(dlsym(RTLD_DEFAULT, "mallopt"));
    if (!fn) {
        return false;
    }

    // Android Bionic M_DECAY_TIME = -100.
    // Scudo (the default allocator since Android 11) only accepts -1, 0 or 1 here and ABORTS the
    // whole process on any other value (CHECK failed @ wrappers_c.inc: value >= -1 && value <= 1).
    // 0 = release freed memory to the OS immediately, 1 = delayed release (keeps freed chunks
    // cached, which is what "heap retain" wants). So any configured decay time > 0 maps to 1.
    constexpr int kBionicMDecayTime = -100;
    constexpr int kScudoDecayDelayed = 1;
    const int decayMs = std::clamp(cfg.heapDecayTimeMs.load(std::memory_order_relaxed), 1000, 60000);
    const int scudoValue = (decayMs > 0) ? kScudoDecayDelayed : 0;
    return fn(kBionicMDecayTime, scudoValue) != 0;
}

bool MemoryArenaOptimizer::lockHotMemoryPages() noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modMemoryArenaEnabled.load(std::memory_order_relaxed) ||
        !cfg.lockHotPages.load(std::memory_order_relaxed)) {
        return false;
    }

    // Touch every page in the hot scratch arena to fault it in upfront
    for (size_t offset = 0; offset < sizeof(sHotScratchArena); offset += 4096) {
        sHotScratchArena[offset] = static_cast<uint8_t>(offset & 0xFFu);
    }

    bool ok = (mlock(sHotScratchArena, sizeof(sHotScratchArena)) == 0);
    (void)mlock(&getTelemetry(), sizeof(TelemetryEngine));
    (void)mlock(&getConfig(), sizeof(RuntimeConfig));
    (void)mlock(&getHudCache(), sizeof(HudCacheEngine));
    sPagesLocked = ok;
    return ok;
}

void MemoryArenaOptimizer::releaseLockedPages() noexcept {
    if (sPagesLocked) {
        (void)munlock(sHotScratchArena, sizeof(sHotScratchArena));
        (void)munlock(&getTelemetry(), sizeof(TelemetryEngine));
        (void)munlock(&getConfig(), sizeof(RuntimeConfig));
        (void)munlock(&getHudCache(), sizeof(HudCacheEngine));
        sPagesLocked = false;
    }
}

// ============================================================================
// Module 11: 90Hz Sub-Frame Camera & Touch Stabilizer + Low-Delay Sockets
// ============================================================================
CameraInput90HzStabilizer& getCameraStabilizer() noexcept {
    static CameraInput90HzStabilizer sStabilizer;
    return sStabilizer;
}

void CameraInput90HzStabilizer::filterTurnDelta(float& dx, float& dy, float dtSeconds) noexcept {
    const auto& cfg = getConfig();
    if (!cfg.modCameraInputEnabled.load(std::memory_order_relaxed) ||
        !cfg.smoothCamera90Hz.load(std::memory_order_relaxed)) {
        return;
    }

    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        dx = 0.0f;
        dy = 0.0f;
        return;
    }

    // Raw Flick Bypass: instant 180-degree turns pass through with zero filter lag
    if (cfg.rawFlickBypass.load(std::memory_order_relaxed)) {
        const float magSq = dx * dx + dy * dy;
        if (magSq > 900.0f) { // > 30 degrees/frame flick
            mSmoothedDx = dx;
            mSmoothedDy = dy;
            mInitialized = true;
            return;
        }
    }

    dtSeconds = std::clamp(dtSeconds, 0.002f, 0.050f);
    const float resp = std::clamp(cfg.cameraResponsiveness.load(std::memory_order_relaxed), 10.0f, 60.0f);
    const float alpha = std::clamp(1.0f - std::exp(-resp * dtSeconds), 0.15f, 1.0f);

    if (!mInitialized) {
        mSmoothedDx = dx;
        mSmoothedDy = dy;
        mInitialized = true;
        return;
    }

    mSmoothedDx += (dx - mSmoothedDx) * alpha;
    mSmoothedDy += (dy - mSmoothedDy) * alpha;
    dx = mSmoothedDx;
    dy = mSmoothedDy;
}

void CameraInput90HzStabilizer::reset() noexcept {
    mSmoothedDx = 0.0f;
    mSmoothedDy = 0.0f;
    mInitialized = false;
}

void CameraInput90HzStabilizer::optimizeSocketDescriptor(int fd) noexcept {
    if (fd < 0) return;
    const auto& cfg = getConfig();
    if (!cfg.modCameraInputEnabled.load(std::memory_order_relaxed) ||
        !cfg.lowDelaySockets.load(std::memory_order_relaxed)) {
        return;
    }

    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int tos = 0x10; // IPTOS_LOWDELAY
    (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    int prio = 6;
    (void)setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
}

} // namespace hyper90
