#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hyper90 {

// ============================================================================
// Module 3: Big.LITTLE Prime Core & Thread Booster
// ============================================================================
class CpuSchedulerOptimizer {
public:
    // Select big/prime core indices given per-core max frequencies (kHz) and mode (0=AutoMaxFreq, 1=UpperHalf, 2=All)
    static std::vector<int> selectPerformanceCores(const std::vector<int64_t>& coreMaxFreqKhz, int mode);

    // Probe sysfs on the current device and pin the calling thread (RenderThread) to performance cores
    static int applyRenderThreadBoost(bool forceReapply = false) noexcept;

    // Called once per frame on the RenderThread; re-applies periodically per config
    static void onRenderFrameTick() noexcept;
};

// ============================================================================
// Module 9: Zero-Alloc HUD & UI Batch Accelerator
// ============================================================================
struct HotbarSlotState {
    int32_t  itemId{-1};
    int32_t  auxValue{0};
    int32_t  stackCount{0};
    uint32_t durabilityHash{0};

    bool operator==(const HotbarSlotState& o) const noexcept {
        return itemId == o.itemId && auxValue == o.auxValue &&
               stackCount == o.stackCount && durabilityHash == o.durabilityHash;
    }
};

struct PlayerVitalsState {
    int32_t health{20};
    int32_t maxHealth{20};
    int32_t hunger{20};
    int32_t armor{0};
    int32_t xpLevel{0};
    int32_t xpPermille{0}; // 0..1000

    bool operator==(const PlayerVitalsState& o) const noexcept {
        return health == o.health && maxHealth == o.maxHealth &&
               hunger == o.hunger && armor == o.armor &&
               xpLevel == o.xpLevel && xpPermille == o.xpPermille;
    }
};

class alignas(64) HudCacheEngine {
public:
    static constexpr size_t kHotbarSlots  = 9;
    static constexpr size_t kTextSlotCap  = 32;

    // Zero-allocation 64-bit FNV-1a hash
    static constexpr uint64_t fnv1a64(std::string_view text) noexcept {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : text) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    // Returns true if the slot state is UNCHANGED (cache hit -> skip redraw)
    bool checkAndUpdateHotbarSlot(size_t slot, const HotbarSlotState& state) noexcept;

    // Returns true if player vitals are UNCHANGED (cache hit -> skip redraw)
    bool checkAndUpdateVitals(const PlayerVitalsState& state) noexcept;

    // Returns true if the UI text at textSlotId is UNCHANGED (zero heap allocation!)
    bool checkAndUpdateText(size_t textSlotId, std::string_view text) noexcept;

    void invalidateAll() noexcept;

private:
    std::array<HotbarSlotState, kHotbarSlots> mHotbar{};
    std::array<bool, kHotbarSlots>            mHotbarValid{};
    PlayerVitalsState                         mVitals{};
    bool                                      mVitalsValid{false};
    std::array<uint64_t, kTextSlotCap>        mTextHashes{};
    std::array<bool, kTextSlotCap>            mTextValid{};
};

HudCacheEngine& getHudCache() noexcept;

// ============================================================================
// Module 10: Bionic Heap & Page-Fault Stutter Shield
// ============================================================================
class MemoryArenaOptimizer {
public:
    static bool applyBionicHeapTuning() noexcept;
    static bool lockHotMemoryPages() noexcept;
    static void releaseLockedPages() noexcept;
};

// ============================================================================
// Module 11: 90Hz Sub-Frame Camera & Touch Stabilizer + Low-Delay Sockets
// ============================================================================
class CameraInput90HzStabilizer {
public:
    void filterTurnDelta(float& dx, float& dy, float dtSeconds) noexcept;
    void reset() noexcept;

    static void optimizeSocketDescriptor(int fd) noexcept;

private:
    float mSmoothedDx{0.0f};
    float mSmoothedDy{0.0f};
    bool  mInitialized{false};
};

CameraInput90HzStabilizer& getCameraStabilizer() noexcept;

} // namespace hyper90
