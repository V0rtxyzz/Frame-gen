#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace hyper90 {

// ============================================================================
// Module IDs (13 Distinct LeviLaunchroid Mod Menu Modules)
// ============================================================================
namespace ModuleIds {
inline constexpr const char* kFramePacer          = "hyper90.frame_pacer";
inline constexpr const char* kEngineOptions       = "hyper90.engine_options";
inline constexpr const char* kCpuScheduler        = "hyper90.cpu_scheduler";
inline constexpr const char* kGlPipeline          = "hyper90.gl_pipeline";
inline constexpr const char* kTbdrTileSaver       = "hyper90.tbdr_tile_saver";
inline constexpr const char* kTextureLod          = "hyper90.texture_lod";
inline constexpr const char* kDrawBatchCuller     = "hyper90.draw_batch_culler";
inline constexpr const char* kEntityParticleGuard = "hyper90.entity_particle_guard";
inline constexpr const char* kHudUiAccelerator    = "hyper90.hud_ui_accelerator";
inline constexpr const char* kMemoryArenaTuner    = "hyper90.memory_arena_tuner";
inline constexpr const char* kCameraInput90Hz     = "hyper90.camera_input_90hz";
inline constexpr const char* kAdaptiveGovernorHud = "hyper90.adaptive_governor_hud";
inline constexpr const char* kFrameBlend          = "hyper90.frame_blend";

inline constexpr const char* kBtnUltraBoost       = "hyper90.btn.ultra_boost";
inline constexpr const char* kBtnPurgeCache       = "hyper90.btn.purge_cache";
} // namespace ModuleIds

// ============================================================================
// Lock-Free Runtime Configuration State for All 13 Optimization Modules
// ============================================================================
struct RuntimeConfig {
    // --- Module 1: 90 FPS Unlocker & Frame Pacer ---
    std::atomic<bool> modFramePacerEnabled{true};
    std::atomic<bool> vsyncBypass{true};
    std::atomic<int>  targetFps{90};
    std::atomic<int>  pacingMode{2}; // 0=Off, 1=SleepOnly, 2=HybridMicroPace
    std::atomic<bool> antiStutterGuard{true};

    // --- Module 2: Bedrock Engine & RenderDragon Tuner (options.txt) ---
    std::atomic<bool> modEngineOptionsEnabled{true};
    std::atomic<bool> disableFancyGfx{true};
    std::atomic<bool> opaqueLeaves{true};
    std::atomic<bool> fastSkiesClouds{true};
    std::atomic<bool> disableMsaaTexelAa{true};
    std::atomic<bool> fastLighting{true};
    std::atomic<int>  viewDistanceChunks{7}; // 5..16 chunks

    // --- Module 3: Big.LITTLE Prime Core & Thread Booster ---
    std::atomic<bool> modCpuSchedulerEnabled{true};
    std::atomic<bool> pinRenderThread{true};
    std::atomic<bool> boostThreadPriority{true};
    std::atomic<int>  coreSelectionMode{0}; // 0=AutoMaxFreq, 1=UpperHalf, 2=AllCores
    std::atomic<int>  reapplyIntervalSec{5};

    // --- Module 4: OpenGL ES Pipeline & State Deduplicator ---
    std::atomic<bool> modGlPipelineEnabled{true};
    std::atomic<bool> fastestShaderHints{true};
    std::atomic<bool> disableDitherMsaa{true};
    std::atomic<bool> suppressGlFinish{true};
    std::atomic<bool> dedupGlState{true};

    // --- Module 5: Adreno/Mali TBDR Tile Bandwidth Saver ---
    std::atomic<bool> modTbdrTileSaverEnabled{true};
    std::atomic<bool> discardDepthStencil{true};
    std::atomic<bool> optimizeClearCalls{true};

    // --- Module 6: VRAM & Texture Mipmap Optimizer ---
    std::atomic<bool> modTextureLodEnabled{true};
    std::atomic<bool> clampAnisotropy{true};
    std::atomic<int>  maxAnisotropy{1}; // 1..8
    std::atomic<bool> fastMipmapFilter{true};
    std::atomic<int>  maxMipLevel{4};   // 2..10

    // --- Module 7: Zero-Area & Micro-Batch Draw Culler ---
    std::atomic<bool> modDrawBatchCullerEnabled{true};
    std::atomic<bool> cullDegenerateDraws{true};
    std::atomic<bool> adaptiveMicroBatchCull{true};
    std::atomic<int>  microBatchMaxIndices{18}; // 6..60
    std::atomic<int>  cullTriggerFps{88};       // 60..120

    // --- Module 8: Entity Distance & Particle Budget Limiter ---
    std::atomic<bool> modEntityParticleEnabled{true};
    std::atomic<bool> entityDistanceCulling{true};
    std::atomic<int>  maxEntityDistance{48};     // 16..128 blocks
    std::atomic<bool> limitParticles{true};
    std::atomic<int>  maxParticlesPerFrame{180}; // 20..1000
    std::atomic<bool> fastEntityInterp{true};

    // --- Module 9: Zero-Alloc HUD & UI Batch Accelerator ---
    std::atomic<bool> modHudAcceleratorEnabled{true};
    std::atomic<bool> cacheHotbarVitals{true};
    std::atomic<bool> cacheUiText{true};
    std::atomic<bool> disableUiAnimations{true};
    std::atomic<bool> simplifyCrosshair{false};
    std::atomic<int>  crosshairSize{10}; // 4..24 px

    // --- Module 10: Bionic Heap & Page-Fault Stutter Shield ---
    std::atomic<bool> modMemoryArenaEnabled{true};
    std::atomic<bool> bionicHeapRetain{true};
    std::atomic<int>  heapDecayTimeMs{15000}; // 1000..60000 ms
    std::atomic<bool> lockHotPages{true};

    // --- Module 11: 90Hz Sub-Frame Camera & Touch Stabilizer ---
    std::atomic<bool> modCameraInputEnabled{true};
    std::atomic<bool> smoothCamera90Hz{true};
    std::atomic<float> cameraResponsiveness{28.0f}; // 10.0..60.0
    std::atomic<bool> rawFlickBypass{true};
    std::atomic<bool> fullbrightGamma{true};
    std::atomic<bool> lowDelaySockets{true};

    // --- Module 12: Adaptive 90-Lock Governor & Telemetry HUD ---
    std::atomic<bool> modAdaptiveGovernorEnabled{true};
    std::atomic<bool> adaptive90Governor{true};
    std::atomic<bool> ultraBoostOverride{false};
    std::atomic<bool> showFpsHud{true};
    std::atomic<int>  hudStyle{1}; // 0=Minimal, 1=Detailed, 2=CompactBar
    std::atomic<float> hudPosX{16.0f};
    std::atomic<float> hudPosY{16.0f};
    std::atomic<uint32_t> accentColorArgb{0xFF00FF88u};

    // --- Module 13: Frame Blend (Experimental 1.5x / 2.0x frame generation) ---
    // Off by default: it forces VSync on while active and is meant for 90/120 Hz displays.
    std::atomic<bool> modFrameBlendEnabled{false};
    std::atomic<int>  frameBlendMode{1};          // 0=1.5x, 1=2.0x
    std::atomic<int>  frameBlendStrengthPct{50};  // 10..90 (weight of the previous frame)
};

RuntimeConfig& getConfig() noexcept;

uint32_t parseHexColorArgb(std::string_view hex, uint32_t fallback = 0xFF00FF88u) noexcept;
std::string formatHexColorRgb(uint32_t argb);

bool loadConfigFromDisk(const std::filesystem::path& configDir) noexcept;
bool saveConfigToDisk(const std::filesystem::path& configDir) noexcept;
bool writeConfigSchemaToDisk(const std::filesystem::path& configDir) noexcept;

} // namespace hyper90
