#include "hyper90/Modules.hpp"
#include "hyper90/Config.hpp"
#include "hyper90/EngineOptionsTuner.hpp"
#include "hyper90/FrameBlend.hpp"
#include "hyper90/NativeHooks.hpp"
#include "hyper90/SystemOptimizers.hpp"
#include "hyper90/Telemetry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hyper90 {

namespace {

std::filesystem::path gModRootDir;
std::filesystem::path gConfigDir;
bool                  gRegistered = false;
uint64_t              gLastHudUpdateFrame = 0;
bool                  gHudWasVisible = false;
bool                  gCrosshairWasVisible = false;

constexpr const char* kSvgBoltIcon =
    "<svg viewBox=\"0 0 24 24\"><path fill=\"#00FF88\" d=\"M13 2L4 14h7l-2 8 11-12h-7z\"/></svg>";

constexpr const char* kSvgPurgeIcon =
    "<svg viewBox=\"0 0 24 24\"><path fill=\"#4AE0A0\" d=\"M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 "
    "0 1.01-.25 1.97-.7 2.8l1.46 1.46A7.93 7.93 0 0020 12c0-4.42-3.58-8-8-8zm-6 8c0-1.01.25-1.97.7-2.8"
    "L5.24 7.74A7.93 7.93 0 004 12c0 4.42 3.58 8 8 8v3l4-4-4-4v3c-3.31 0-6-2.69-6-6z\"/></svg>";

bool parseBoolValue(std::string_view v) noexcept {
    return v == "true" || v == "1" || v == "TRUE" || v == "on";
}

int parseIntValue(std::string_view v, int fallback, int minVal, int maxVal) noexcept {
    int out = fallback;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{}) {
        return fallback;
    }
    return std::clamp(out, minVal, maxVal);
}

float parseFloatValue(std::string_view v, float fallback, float minVal, float maxVal) noexcept {
    if (v.empty()) return fallback;
    std::string tmp(v);
    char* endPtr = nullptr;
    const float val = std::strtof(tmp.c_str(), &endPtr);
    if (endPtr == tmp.c_str() || !std::isfinite(val)) {
        return fallback;
    }
    return std::clamp(val, minVal, maxVal);
}

void persistConfigQuietly() noexcept {
    if (!gConfigDir.empty()) {
        saveConfigToDisk(gConfigDir);
    }
}

const std::array<ModuleDescriptor, Modules::kModuleCount> kModuleDescriptors = {{
    {
        ModuleIds::kFramePacer,
        "1. 90 FPS Unlocker & Frame Pacer",
        "Unlocks EGL VSync (eglSwapInterval 0) and locks frame delivery to a rock-solid 11.111ms (90 FPS) cadence with Anti-Stutter Spike Guard.",
        true
    },
    {
        ModuleIds::kEngineOptions,
        "2. Bedrock Engine & RenderDragon Tuner",
        "Automatically tunes Minecraft Bedrock options.txt (VSync off, 90 FPS cap, MSAA/Texel-AA off, opaque leaves, fast skies/clouds & lighting).",
        true
    },
    {
        ModuleIds::kCpuScheduler,
        "3. Big.LITTLE Prime Core & Thread Booster",
        "Detects Snapdragon/Dimensity/Exynos Prime & Big CPU cores via sysfs, pins the GL RenderThread, lowers timer slack to 10us, and boosts thread nice priority.",
        true
    },
    {
        ModuleIds::kGlPipeline,
        "4. OpenGL ES Pipeline & State Deduplicator",
        "Forces GL_FASTEST shader/mipmap hints, suppresses mid-frame glFinish() pipeline stalls, disables dithering/MSAA, and deduplicates redundant GL state binds.",
        true
    },
    {
        ModuleIds::kTbdrTileSaver,
        "5. Adreno/Mali TBDR Tile Bandwidth Saver",
        "Discards Depth & Stencil tile writebacks (glInvalidateFramebuffer) before eglSwapBuffers and merges D24S8 clear masks to save mobile GPU memory bandwidth.",
        true
    },
    {
        ModuleIds::kTextureLod,
        "6. VRAM & Texture Mipmap Optimizer",
        "Clamps anisotropic texture filtering to 1x and rewrites trilinear mipmap lookups (GL_LINEAR_MIPMAP_LINEAR) to fast bilinear/nearest mipmaps.",
        true
    },
    {
        ModuleIds::kDrawBatchCuller,
        "7. Zero-Area & Micro-Batch Draw Culler",
        "Rejects degenerate zero-index draw calls and adaptively decimates transient micro-batches whenever FPS dips below the 90 FPS lock threshold.",
        true
    },
    {
        ModuleIds::kEntityParticleGuard,
        "8. Entity Distance & Particle Budget Limiter",
        "Enforces a per-frame particle draw budget and throttles distant entity interpolation to prevent mob/explosion frame drops.",
        true
    },
    {
        ModuleIds::kHudUiAccelerator,
        "9. Zero-Alloc HUD & UI Batch Accelerator",
        "Caches hotbar slots, player vitals, and FNV-1a 64-bit UI text hashes with zero heap allocations, plus optional simplified center crosshair.",
        false
    },
    {
        ModuleIds::kMemoryArenaTuner,
        "10. Bionic Heap & Page-Fault Stutter Shield",
        "Tunes Android Bionic Scudo/jemalloc heap decay time (mallopt) and mlocks hot profiler/hook pages in RAM to eliminate chunk-load page faults.",
        true
    },
    {
        ModuleIds::kCameraInput90Hz,
        "11. 90Hz Sub-Frame Camera & Touch Stabilizer",
        "Applies critically-damped 90Hz exponential camera turn filtering with raw flick bypass, Fullbright cave lighting boost, and TCP_NODELAY socket tuning.",
        true
    },
    {
        ModuleIds::kAdaptiveGovernorHud,
        "12. Adaptive 90-Lock Governor & Telemetry HUD",
        "4-Tier closed-loop 90-FPS governor that dynamically scales culling under load, paired with a movable real-time 90-FPS Telemetry & Frame Budget HUD.",
        false
    },
    {
        ModuleIds::kFrameBlend,
        "13. Frame Blend (Experimental 1.5x / 2.0x Frame Gen)",
        "Presents blended frames between rendered frames (like Mine Frame Gen). Forces VSync on while active, so use it on 90/120 Hz displays only. Adds ~1 display frame of latency and can ghost on fast motion.",
        false
    }
}};

} // namespace

const std::array<ModuleDescriptor, Modules::kModuleCount>& Modules::getDescriptors() noexcept {
    return kModuleDescriptors;
}

void Modules::handleModuleToggle(std::string_view moduleId, bool enabled) noexcept {
    auto& cfg = getConfig();

    if (moduleId == ModuleIds::kFramePacer) {
        cfg.modFramePacerEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kEngineOptions) {
        cfg.modEngineOptionsEnabled.store(enabled, std::memory_order_relaxed);
        if (enabled && !gModRootDir.empty()) {
            EngineOptionsTuner::discoverAndTuneAll(gModRootDir);
        } else if (!enabled && !gModRootDir.empty()) {
            EngineOptionsTuner::restoreAllBackups(gModRootDir);
        }
    } else if (moduleId == ModuleIds::kCpuScheduler) {
        cfg.modCpuSchedulerEnabled.store(enabled, std::memory_order_relaxed);
        if (enabled) {
            CpuSchedulerOptimizer::applyRenderThreadBoost(true);
        }
    } else if (moduleId == ModuleIds::kGlPipeline) {
        cfg.modGlPipelineEnabled.store(enabled, std::memory_order_relaxed);
        getGlOptimizer().resetFrameState();
    } else if (moduleId == ModuleIds::kTbdrTileSaver) {
        cfg.modTbdrTileSaverEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kTextureLod) {
        cfg.modTextureLodEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kDrawBatchCuller) {
        cfg.modDrawBatchCullerEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kEntityParticleGuard) {
        cfg.modEntityParticleEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kHudUiAccelerator) {
        cfg.modHudAcceleratorEnabled.store(enabled, std::memory_order_relaxed);
        getHudCache().invalidateAll();
    } else if (moduleId == ModuleIds::kMemoryArenaTuner) {
        cfg.modMemoryArenaEnabled.store(enabled, std::memory_order_relaxed);
        if (enabled) {
            MemoryArenaOptimizer::applyBionicHeapTuning();
            MemoryArenaOptimizer::lockHotMemoryPages();
        } else {
            MemoryArenaOptimizer::releaseLockedPages();
        }
    } else if (moduleId == ModuleIds::kCameraInput90Hz) {
        cfg.modCameraInputEnabled.store(enabled, std::memory_order_relaxed);
        getCameraStabilizer().reset();
    } else if (moduleId == ModuleIds::kAdaptiveGovernorHud) {
        cfg.modAdaptiveGovernorEnabled.store(enabled, std::memory_order_relaxed);
    } else if (moduleId == ModuleIds::kFrameBlend) {
        cfg.modFrameBlendEnabled.store(enabled, std::memory_order_relaxed);
        FrameBlendGl::resetHistory();
    }

    persistConfigQuietly();
}

void Modules::handleConfigChange(
    std::string_view moduleId, std::string_view key, std::string_view value) noexcept {
    auto& cfg = getConfig();

    if (moduleId == ModuleIds::kFramePacer) {
        if (key == "vsync_bypass") {
            cfg.vsyncBypass.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "target_fps") {
            cfg.targetFps.store(parseIntValue(value, 90, 60, 240), std::memory_order_relaxed);
        } else if (key == "pacing_mode") {
            cfg.pacingMode.store(parseIntValue(value, 2, 0, 2), std::memory_order_relaxed);
        } else if (key == "anti_stutter_guard") {
            cfg.antiStutterGuard.store(parseBoolValue(value), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kEngineOptions) {
        if (key == "disable_fancy_gfx") {
            cfg.disableFancyGfx.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "opaque_leaves") {
            cfg.opaqueLeaves.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "fast_skies_clouds") {
            cfg.fastSkiesClouds.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "disable_msaa_texel_aa") {
            cfg.disableMsaaTexelAa.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "fast_lighting") {
            cfg.fastLighting.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "view_distance_chunks") {
            cfg.viewDistanceChunks.store(parseIntValue(value, 7, 5, 16), std::memory_order_relaxed);
        } else if (key == "apply_now" && !gModRootDir.empty()) {
            EngineOptionsTuner::discoverAndTuneAll(gModRootDir);
        }
        if (!gModRootDir.empty()) {
            EngineOptionsTuner::discoverAndTuneAll(gModRootDir);
        }
    } else if (moduleId == ModuleIds::kCpuScheduler) {
        if (key == "pin_render_thread") {
            cfg.pinRenderThread.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "boost_thread_priority") {
            cfg.boostThreadPriority.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "core_selection_mode") {
            cfg.coreSelectionMode.store(parseIntValue(value, 0, 0, 2), std::memory_order_relaxed);
        } else if (key == "reapply_interval_sec") {
            cfg.reapplyIntervalSec.store(parseIntValue(value, 5, 1, 30), std::memory_order_relaxed);
        }
        CpuSchedulerOptimizer::applyRenderThreadBoost(true);
    } else if (moduleId == ModuleIds::kGlPipeline) {
        if (key == "fastest_shader_hints") {
            cfg.fastestShaderHints.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "disable_dither_msaa") {
            cfg.disableDitherMsaa.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "suppress_gl_finish") {
            cfg.suppressGlFinish.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "dedup_gl_state") {
            cfg.dedupGlState.store(parseBoolValue(value), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kTbdrTileSaver) {
        if (key == "discard_depth_stencil") {
            cfg.discardDepthStencil.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "optimize_clear_calls") {
            cfg.optimizeClearCalls.store(parseBoolValue(value), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kTextureLod) {
        if (key == "clamp_anisotropy") {
            cfg.clampAnisotropy.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "max_anisotropy") {
            cfg.maxAnisotropy.store(parseIntValue(value, 1, 1, 8), std::memory_order_relaxed);
        } else if (key == "fast_mipmap_filter") {
            cfg.fastMipmapFilter.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "max_mip_level") {
            cfg.maxMipLevel.store(parseIntValue(value, 4, 2, 10), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kDrawBatchCuller) {
        if (key == "cull_degenerate_draws") {
            cfg.cullDegenerateDraws.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "adaptive_micro_batch_cull") {
            cfg.adaptiveMicroBatchCull.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "micro_batch_max_indices") {
            cfg.microBatchMaxIndices.store(parseIntValue(value, 18, 6, 60), std::memory_order_relaxed);
        } else if (key == "cull_trigger_fps") {
            cfg.cullTriggerFps.store(parseIntValue(value, 88, 60, 120), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kEntityParticleGuard) {
        if (key == "entity_distance_culling") {
            cfg.entityDistanceCulling.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "max_entity_distance") {
            cfg.maxEntityDistance.store(parseIntValue(value, 48, 16, 128), std::memory_order_relaxed);
        } else if (key == "limit_particles") {
            cfg.limitParticles.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "max_particles_per_frame") {
            cfg.maxParticlesPerFrame.store(parseIntValue(value, 180, 20, 1000), std::memory_order_relaxed);
        } else if (key == "fast_entity_interp") {
            cfg.fastEntityInterp.store(parseBoolValue(value), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kHudUiAccelerator) {
        if (key == "cache_hotbar_vitals") {
            cfg.cacheHotbarVitals.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "cache_ui_text") {
            cfg.cacheUiText.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "disable_ui_animations") {
            cfg.disableUiAnimations.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "simplify_crosshair") {
            cfg.simplifyCrosshair.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "crosshair_size") {
            cfg.crosshairSize.store(parseIntValue(value, 10, 4, 24), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kMemoryArenaTuner) {
        if (key == "bionic_heap_retain") {
            cfg.bionicHeapRetain.store(parseBoolValue(value), std::memory_order_relaxed);
            MemoryArenaOptimizer::applyBionicHeapTuning();
        } else if (key == "heap_decay_time_ms") {
            cfg.heapDecayTimeMs.store(parseIntValue(value, 15000, 1000, 60000), std::memory_order_relaxed);
            MemoryArenaOptimizer::applyBionicHeapTuning();
        } else if (key == "lock_hot_pages") {
            const bool lockOn = parseBoolValue(value);
            cfg.lockHotPages.store(lockOn, std::memory_order_relaxed);
            if (lockOn) MemoryArenaOptimizer::lockHotMemoryPages();
            else MemoryArenaOptimizer::releaseLockedPages();
        }
    } else if (moduleId == ModuleIds::kCameraInput90Hz) {
        if (key == "smooth_camera_90hz") {
            cfg.smoothCamera90Hz.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "camera_responsiveness") {
            cfg.cameraResponsiveness.store(parseFloatValue(value, 28.0f, 10.0f, 60.0f), std::memory_order_relaxed);
        } else if (key == "raw_flick_bypass") {
            cfg.rawFlickBypass.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "fullbright_gamma") {
            cfg.fullbrightGamma.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "low_delay_sockets") {
            cfg.lowDelaySockets.store(parseBoolValue(value), std::memory_order_relaxed);
        }
    } else if (moduleId == ModuleIds::kAdaptiveGovernorHud) {
        if (key == "adaptive_90_governor") {
            cfg.adaptive90Governor.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "ultra_boost_override") {
            cfg.ultraBoostOverride.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "show_fps_hud") {
            cfg.showFpsHud.store(parseBoolValue(value), std::memory_order_relaxed);
        } else if (key == "hud_style") {
            cfg.hudStyle.store(parseIntValue(value, 1, 0, 2), std::memory_order_relaxed);
        } else if (key == "hudPosX") {
            cfg.hudPosX.store(parseFloatValue(value, 16.0f, 0.0f, 3840.0f), std::memory_order_relaxed);
        } else if (key == "hudPosY") {
            cfg.hudPosY.store(parseFloatValue(value, 16.0f, 0.0f, 2160.0f), std::memory_order_relaxed);
        } else if (key == "accent_color") {
            cfg.accentColorArgb.store(parseHexColorArgb(value, 0xFF00FF88u), std::memory_order_relaxed);
        } else if (key == "reset_telemetry") {
            getTelemetry().resetCounters();
        }
    } else if (moduleId == ModuleIds::kFrameBlend) {
        if (key == "mode") {
            cfg.frameBlendMode.store(parseIntValue(value, 1, 0, 1), std::memory_order_relaxed);
            FrameBlendGl::resetHistory();
        } else if (key == "strength_pct") {
            cfg.frameBlendStrengthPct.store(
                parseIntValue(value, 50, FrameBlendConsts::kMinStrengthPct, FrameBlendConsts::kMaxStrengthPct),
                std::memory_order_relaxed);
        }
    }

    persistConfigQuietly();
}

bool Modules::registerAll(std::string_view ownerModId, const std::filesystem::path& modRootDir) noexcept {
    gModRootDir = modRootDir;
    gConfigDir  = modRootDir / "config";
    if (gRegistered) {
        return true;
    }

    const auto& c = getConfig();
    using pl::modmenu::ConfigType;
    using pl::modmenu::ModuleBuilder;
    const std::string modIdStr(ownerModId);
    bool allOk = true;

    // 1. Module 1: 90 FPS Unlocker & Frame Pacer
    allOk &= ModuleBuilder(kModuleDescriptors[0].id, kModuleDescriptors[0].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[0].description)
        .defaultEnabled(c.modFramePacerEnabled.load())
        .hideInHudEditor(kModuleDescriptors[0].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("vsync_bypass", "Unlock EGL VSync (Interval 0)", ConfigType::Toggle,
                c.vsyncBypass.load() ? "true" : "false")
        .config("target_fps", "Target FPS Cap (90 = Locked 90Hz)", ConfigType::SliderInt,
                std::to_string(c.targetFps.load()), "60", "144")
        .config("pacing_mode", "Frame Pacer Engine", ConfigType::Radio,
                std::to_string(c.pacingMode.load()), "Off,Kernel Sleep,Hybrid 90Hz Micro-Pacer")
        .config("anti_stutter_guard", "Anti-Stutter Spike Catch-Up Guard", ConfigType::Toggle,
                c.antiStutterGuard.load() ? "true" : "false")
        .registerModule();

    // 2. Module 2: Bedrock Engine & RenderDragon Tuner
    allOk &= ModuleBuilder(kModuleDescriptors[1].id, kModuleDescriptors[1].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[1].description)
        .defaultEnabled(c.modEngineOptionsEnabled.load())
        .hideInHudEditor(kModuleDescriptors[1].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("disable_msaa_texel_aa", "Disable MSAA & Texel Anti-Aliasing", ConfigType::Toggle,
                c.disableMsaaTexelAa.load() ? "true" : "false")
        .config("disable_fancy_gfx", "Fast RenderDragon Graphics Mode", ConfigType::Toggle,
                c.disableFancyGfx.load() ? "true" : "false")
        .config("opaque_leaves", "Opaque Leaves (Eliminate Forest Overdraw)", ConfigType::Toggle,
                c.opaqueLeaves.load() ? "true" : "false")
        .config("fast_skies_clouds", "Fast Skies & Disable 3D Clouds", ConfigType::Toggle,
                c.fastSkiesClouds.load() ? "true" : "false")
        .config("fast_lighting", "Fast Flat Block Lighting", ConfigType::Toggle,
                c.fastLighting.load() ? "true" : "false")
        .config("view_distance_chunks", "Optimal Render Distance (Chunks)", ConfigType::SliderInt,
                std::to_string(c.viewDistanceChunks.load()), "5", "16")
        .config("apply_now", "Re-Apply Engine Options Now", ConfigType::Button, "Tune Now")
        .registerModule();

    // 3. Module 3: Big.LITTLE Prime Core & Thread Booster
    allOk &= ModuleBuilder(kModuleDescriptors[2].id, kModuleDescriptors[2].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[2].description)
        .defaultEnabled(c.modCpuSchedulerEnabled.load())
        .hideInHudEditor(kModuleDescriptors[2].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("pin_render_thread", "Pin GL RenderThread to Prime/Big Cores", ConfigType::Toggle,
                c.pinRenderThread.load() ? "true" : "false")
        .config("boost_thread_priority", "Elevate RenderThread Priority & 10us Timer", ConfigType::Toggle,
                c.boostThreadPriority.load() ? "true" : "false")
        .config("core_selection_mode", "CPU Cluster Topology Strategy", ConfigType::Radio,
                std::to_string(c.coreSelectionMode.load()),
                "Auto Sysfs Max-Freq (>=75%),Upper Performance Half,All Online Cores")
        .config("reapply_interval_sec", "Affinity Re-Pin Interval (sec)", ConfigType::SliderInt,
                std::to_string(c.reapplyIntervalSec.load()), "1", "30")
        .registerModule();

    // 4. Module 4: OpenGL ES Pipeline & State Deduplicator
    allOk &= ModuleBuilder(kModuleDescriptors[3].id, kModuleDescriptors[3].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[3].description)
        .defaultEnabled(c.modGlPipelineEnabled.load())
        .hideInHudEditor(kModuleDescriptors[3].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("fastest_shader_hints", "Force GL_FASTEST Shader & Mipmap Hints", ConfigType::Toggle,
                c.fastestShaderHints.load() ? "true" : "false")
        .config("disable_dither_msaa", "Block GL_DITHER & Multisample Alpha", ConfigType::Toggle,
                c.disableDitherMsaa.load() ? "true" : "false")
        .config("suppress_gl_finish", "Suppress Blocking glFinish() Stalls", ConfigType::Toggle,
                c.suppressGlFinish.load() ? "true" : "false")
        .config("dedup_gl_state", "Deduplicate Redundant Shader/Texture Binds", ConfigType::Toggle,
                c.dedupGlState.load() ? "true" : "false")
        .registerModule();

    // 5. Module 5: Adreno/Mali TBDR Tile Bandwidth Saver
    allOk &= ModuleBuilder(kModuleDescriptors[4].id, kModuleDescriptors[4].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[4].description)
        .defaultEnabled(c.modTbdrTileSaverEnabled.load())
        .hideInHudEditor(kModuleDescriptors[4].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("discard_depth_stencil", "Invalidate Depth/Stencil Tile Writeback", ConfigType::Toggle,
                c.discardDepthStencil.load() ? "true" : "false")
        .config("optimize_clear_calls", "Coalesce Packed D24S8 Tile Clears", ConfigType::Toggle,
                c.optimizeClearCalls.load() ? "true" : "false")
        .registerModule();

    // 6. Module 6: VRAM & Texture Mipmap Optimizer
    allOk &= ModuleBuilder(kModuleDescriptors[5].id, kModuleDescriptors[5].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[5].description)
        .defaultEnabled(c.modTextureLodEnabled.load())
        .hideInHudEditor(kModuleDescriptors[5].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("clamp_anisotropy", "Clamp Anisotropic Filtering", ConfigType::Toggle,
                c.clampAnisotropy.load() ? "true" : "false")
        .config("max_anisotropy", "Max Anisotropy Samples", ConfigType::SliderInt,
                std::to_string(c.maxAnisotropy.load()), "1", "8", "clamp_anisotropy")
        .config("fast_mipmap_filter", "Rewrite Trilinear to Fast Bilinear Mipmaps", ConfigType::Toggle,
                c.fastMipmapFilter.load() ? "true" : "false")
        .config("max_mip_level", "Max Texture Mipmap Level", ConfigType::SliderInt,
                std::to_string(c.maxMipLevel.load()), "2", "10")
        .registerModule();

    // 7. Module 7: Zero-Area & Micro-Batch Draw Culler
    allOk &= ModuleBuilder(kModuleDescriptors[6].id, kModuleDescriptors[6].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[6].description)
        .defaultEnabled(c.modDrawBatchCullerEnabled.load())
        .hideInHudEditor(kModuleDescriptors[6].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("cull_degenerate_draws", "Cull Zero-Index Degenerate Draw Calls", ConfigType::Toggle,
                c.cullDegenerateDraws.load() ? "true" : "false")
        .config("adaptive_micro_batch_cull", "Adaptive Micro-Batch Spike Culling", ConfigType::Toggle,
                c.adaptiveMicroBatchCull.load() ? "true" : "false")
        .config("micro_batch_max_indices", "Micro-Batch Max Index Threshold", ConfigType::SliderInt,
                std::to_string(c.microBatchMaxIndices.load()), "6", "60", "adaptive_micro_batch_cull")
        .config("cull_trigger_fps", "Activation FPS Threshold", ConfigType::SliderInt,
                std::to_string(c.cullTriggerFps.load()), "60", "120", "adaptive_micro_batch_cull")
        .registerModule();

    // 8. Module 8: Entity Distance & Particle Budget Limiter
    allOk &= ModuleBuilder(kModuleDescriptors[7].id, kModuleDescriptors[7].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[7].description)
        .defaultEnabled(c.modEntityParticleEnabled.load())
        .hideInHudEditor(kModuleDescriptors[7].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("entity_distance_culling", "Enable Entity Distance Culling", ConfigType::Toggle,
                c.entityDistanceCulling.load() ? "true" : "false")
        .config("max_entity_distance", "Max Entity Render Distance (Blocks)", ConfigType::SliderInt,
                std::to_string(c.maxEntityDistance.load()), "16", "128", "entity_distance_culling")
        .config("limit_particles", "Enforce Per-Frame Particle Budget", ConfigType::Toggle,
                c.limitParticles.load() ? "true" : "false")
        .config("max_particles_per_frame", "Max Particle Micro-Batches / Frame", ConfigType::SliderInt,
                std::to_string(c.maxParticlesPerFrame.load()), "20", "1000", "limit_particles")
        .config("fast_entity_interp", "Throttle Sub-Tick Entity Interpolation", ConfigType::Toggle,
                c.fastEntityInterp.load() ? "true" : "false")
        .registerModule();

    // 9. Module 9: Zero-Alloc HUD & UI Batch Accelerator
    allOk &= ModuleBuilder(kModuleDescriptors[8].id, kModuleDescriptors[8].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[8].description)
        .defaultEnabled(c.modHudAcceleratorEnabled.load())
        .hideInHudEditor(kModuleDescriptors[8].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("cache_hotbar_vitals", "Cache Hotbar Slots & Player Vitals", ConfigType::Toggle,
                c.cacheHotbarVitals.load() ? "true" : "false")
        .config("cache_ui_text", "FNV-1a Zero-Alloc UI Text Hash Cache", ConfigType::Toggle,
                c.cacheUiText.load() ? "true" : "false")
        .config("disable_ui_animations", "Disable Screen Transition Animations", ConfigType::Toggle,
                c.disableUiAnimations.load() ? "true" : "false")
        .config("simplify_crosshair", "Render Fast Center Overlay Crosshair", ConfigType::Toggle,
                c.simplifyCrosshair.load() ? "true" : "false")
        .config("crosshair_size", "Crosshair Radius (px)", ConfigType::SliderInt,
                std::to_string(c.crosshairSize.load()), "4", "24", "simplify_crosshair")
        .registerModule();

    // 10. Module 10: Bionic Heap & Page-Fault Stutter Shield
    allOk &= ModuleBuilder(kModuleDescriptors[9].id, kModuleDescriptors[9].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[9].description)
        .defaultEnabled(c.modMemoryArenaEnabled.load())
        .hideInHudEditor(kModuleDescriptors[9].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("bionic_heap_retain", "Tune Bionic Scudo/jemalloc Decay Time", ConfigType::Toggle,
                c.bionicHeapRetain.load() ? "true" : "false")
        .config("heap_decay_time_ms", "Heap Page Retain Time (ms)", ConfigType::SliderInt,
                std::to_string(c.heapDecayTimeMs.load()), "1000", "60000", "bionic_heap_retain")
        .config("lock_hot_pages", "mlock() Hot Hook & Telemetry Arena in RAM", ConfigType::Toggle,
                c.lockHotPages.load() ? "true" : "false")
        .registerModule();

    // 11. Module 11: 90Hz Sub-Frame Camera & Touch Stabilizer
    allOk &= ModuleBuilder(kModuleDescriptors[10].id, kModuleDescriptors[10].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[10].description)
        .defaultEnabled(c.modCameraInputEnabled.load())
        .hideInHudEditor(kModuleDescriptors[10].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("smooth_camera_90hz", "90Hz Critically-Damped Camera Filter", ConfigType::Toggle,
                c.smoothCamera90Hz.load() ? "true" : "false")
        .config("camera_responsiveness", "Camera Filter Responsiveness", ConfigType::SliderFloat,
                "28.0", "10.0", "60.0", "smooth_camera_90hz")
        .config("raw_flick_bypass", "Instant Raw Flick Bypass (Zero-Lag 180s)", ConfigType::Toggle,
                c.rawFlickBypass.load() ? "true" : "false", "", "", "smooth_camera_90hz")
        .config("fullbright_gamma", "Fullbright Cave Gamma (Skip Torch Remesh)", ConfigType::Toggle,
                c.fullbrightGamma.load() ? "true" : "false")
        .config("low_delay_sockets", "TCP_NODELAY & Low-Latency Game Sockets", ConfigType::Toggle,
                c.lowDelaySockets.load() ? "true" : "false")
        .registerModule();

    // 12. Module 12: Adaptive 90-Lock Governor & Telemetry HUD
    allOk &= ModuleBuilder(kModuleDescriptors[11].id, kModuleDescriptors[11].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[11].description)
        .defaultEnabled(c.modAdaptiveGovernorEnabled.load())
        .hideInHudEditor(kModuleDescriptors[11].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("adaptive_90_governor", "Closed-Loop 4-Tier 90-FPS Governor", ConfigType::Toggle,
                c.adaptive90Governor.load() ? "true" : "false")
        .config("ultra_boost_override", "Force Ultra 90-Lock Override", ConfigType::Toggle,
                c.ultraBoostOverride.load() ? "true" : "false")
        .config("show_fps_hud", "Show Live 90-FPS Telemetry Overlay", ConfigType::Toggle,
                c.showFpsHud.load() ? "true" : "false")
        .config("hud_style", "Telemetry HUD Layout", ConfigType::Radio,
                std::to_string(c.hudStyle.load()), "Compact Pill,Full 90-FPS Telemetry Card,Minimal FPS Only",
                "", "show_fps_hud")
        .config("hudPosX", "HUD Position X", ConfigType::SliderFloat,
                std::to_string(static_cast<int>(c.hudPosX.load())), "0.0", "1600.0", "show_fps_hud")
        .config("hudPosY", "HUD Position Y", ConfigType::SliderFloat,
                std::to_string(static_cast<int>(c.hudPosY.load())), "0.0", "900.0", "show_fps_hud")
        .config("accent_color", "HUD Accent Color", ConfigType::Color,
                formatHexColorRgb(c.accentColorArgb.load()), "", "", "show_fps_hud")
        .config("reset_telemetry", "Reset Telemetry Counters", ConfigType::Button, "Reset")
        .registerModule();

    // 13. Module 13: Frame Blend (Experimental 1.5x / 2.0x Frame Gen)
    allOk &= ModuleBuilder(kModuleDescriptors[12].id, kModuleDescriptors[12].displayName)
        .modId(modIdStr)
        .description(kModuleDescriptors[12].description)
        .defaultEnabled(c.modFrameBlendEnabled.load())
        .hideInHudEditor(kModuleDescriptors[12].hideInHudEditor)
        .onToggle(&Modules::handleModuleToggle)
        .onConfigChanged(&Modules::handleConfigChange)
        .config("mode", "Generation Multiplier", ConfigType::Radio,
                std::to_string(c.frameBlendMode.load()),
                "1.5x (1 generated frame every 2 rendered),2.0x (1 generated frame per rendered)")
        .config("strength_pct", "Blend Strength (% of previous frame)", ConfigType::SliderInt,
                std::to_string(c.frameBlendStrengthPct.load()),
                std::to_string(FrameBlendConsts::kMinStrengthPct),
                std::to_string(FrameBlendConsts::kMaxStrengthPct))
        .registerModule();

    // Register Quick-Action Floating Buttons
    (void)pl::modmenu::ButtonBuilder(ModuleIds::kBtnUltraBoost, "Ultra 90-Lock Toggle")
        .moduleId(ModuleIds::kAdaptiveGovernorHud)
        .modId(modIdStr)
        .label("90-LOCK")
        .behavior(pl::modmenu::ButtonBehavior::Toggle)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
        .svgIcon(kSvgBoltIcon, false)
        .onEvent([](std::string_view /*btnId*/, pl::modmenu::ButtonEvent event, float value) {
            if (event == pl::modmenu::ButtonEvent::StateChanged ||
                event == pl::modmenu::ButtonEvent::Click) {
                const bool active = (value > 0.5f) ||
                    !getConfig().ultraBoostOverride.load(std::memory_order_relaxed);
                getConfig().ultraBoostOverride.store(active, std::memory_order_relaxed);
                persistConfigQuietly();
            }
        })
        .registerButton();

    (void)pl::modmenu::ButtonBuilder(ModuleIds::kBtnPurgeCache, "Purge & Re-Boost")
        .moduleId(ModuleIds::kAdaptiveGovernorHud)
        .modId(modIdStr)
        .label("PURGE")
        .behavior(pl::modmenu::ButtonBehavior::Click)
        .stylePreset(pl::modmenu::ButtonStylePreset::Keycap)
        .svgIcon(kSvgPurgeIcon, false)
        .onEvent([](std::string_view /*btnId*/, pl::modmenu::ButtonEvent event, float /*value*/) {
            if (event != pl::modmenu::ButtonEvent::Click) return;
            getGlOptimizer().resetFrameState();
            getHudCache().invalidateAll();
            CpuSchedulerOptimizer::applyRenderThreadBoost(true);
            MemoryArenaOptimizer::applyBionicHeapTuning();
            if (!gModRootDir.empty()) {
                EngineOptionsTuner::discoverAndTuneAll(gModRootDir);
            }
        })
        .registerButton();

    gRegistered = allOk;
    return allOk;
}

void Modules::unregisterAll() noexcept {
    if (!gRegistered) return;

    pl::modmenu::submitDrawCommands(ModuleIds::kAdaptiveGovernorHud, {});
    pl::modmenu::submitHudEditorElements(ModuleIds::kAdaptiveGovernorHud, {});
    pl::modmenu::submitDrawCommands(ModuleIds::kHudUiAccelerator, {});

    pl::modmenu::unregisterButton(ModuleIds::kBtnPurgeCache);
    pl::modmenu::unregisterButton(ModuleIds::kBtnUltraBoost);

    for (auto it = kModuleDescriptors.rbegin(); it != kModuleDescriptors.rend(); ++it) {
        pl::modmenu::unregisterModule(it->id);
    }

    gRegistered = false;
}

std::vector<pl::modmenu::DrawCommand> Modules::buildHudDrawCommands() noexcept {
    std::vector<pl::modmenu::DrawCommand> cmds;
    const auto& cfg = getConfig();
    if (!cfg.modAdaptiveGovernorEnabled.load(std::memory_order_relaxed) ||
        !cfg.showFpsHud.load(std::memory_order_relaxed)) {
        return cmds;
    }

    const FrameSnapshot snap = getTelemetry().getSnapshot();
    const float x = std::clamp(cfg.hudPosX.load(std::memory_order_relaxed), 0.0f, 3840.0f);
    const float y = std::clamp(cfg.hudPosY.load(std::memory_order_relaxed), 0.0f, 2160.0f);
    const int style = std::clamp(cfg.hudStyle.load(std::memory_order_relaxed), 0, 2);
    const int accent = static_cast<int>(cfg.accentColorArgb.load(std::memory_order_relaxed));

    // Dynamic FPS status color: Green (>=88), Gold (75..88), Red (<75)
    int fpsColor = accent;
    if (snap.avgFps < 75.0f) {
        fpsColor = static_cast<int>(0xFFFF4D4Du);
    } else if (snap.avgFps < 88.0f) {
        fpsColor = static_cast<int>(0xFFFFCC00u);
    }

    using pl::modmenu::DrawCommand;
    using pl::modmenu::DrawCommandType;

    if (style == 2) {
        // Minimal FPS Readout
        char line[64]{};
        std::snprintf(line, sizeof(line), "90-LOCK  %.0f FPS  (1%% %.0f)", snap.avgFps, snap.onePercentLowFps);

        DrawCommand bg{};
        bg.type  = DrawCommandType::RectFilled;
        bg.x     = x;
        bg.y     = y;
        bg.w     = 265.0f;
        bg.h     = 36.0f;
        bg.x3    = 8.0f; // Rounded corners in HudOverlay.java
        bg.color = static_cast<int>(0xCC101418u);
        cmds.push_back(bg);

        DrawCommand txt{};
        txt.type  = DrawCommandType::Text;
        txt.x     = x + 12.0f;
        txt.y     = y + 25.0f;
        txt.w     = 0.0f;
        txt.h     = 0.0f;
        txt.size  = 21.0f;
        txt.color = fpsColor;
        txt.text  = line;
        cmds.push_back(txt);
        return cmds;
    }

    if (style == 0) {
        // Compact Pill with 90-FPS Frame Budget Bar
        constexpr float kCardW = 310.0f;
        constexpr float kCardH = 68.0f;

        DrawCommand bg{};
        bg.type  = DrawCommandType::RectFilled;
        bg.x     = x;
        bg.y     = y;
        bg.w     = kCardW;
        bg.h     = kCardH;
        bg.x3    = 10.0f;
        bg.color = static_cast<int>(0xD8101418u);
        cmds.push_back(bg);

        char line1[96]{};
        std::snprintf(
            line1, sizeof(line1),
            "HYPER90  %.1f FPS  |  %.2f ms",
            snap.avgFps, snap.lastFrameMs);

        DrawCommand t1{};
        t1.type  = DrawCommandType::Text;
        t1.x     = x + 12.0f;
        t1.y     = y + 26.0f;
        t1.size  = 21.0f;
        t1.color = fpsColor;
        t1.text  = line1;
        cmds.push_back(t1);

        char line2[96]{};
        std::snprintf(
            line2, sizeof(line2),
            "1%% Low: %.0f  |  %s",
            snap.onePercentLowFps, governorTierName(snap.tier));

        DrawCommand t2{};
        t2.type  = DrawCommandType::Text;
        t2.x     = x + 12.0f;
        t2.y     = y + 52.0f;
        t2.size  = 18.0f;
        t2.color = static_cast<int>(0xFFD0D8E0u);
        t2.text  = line2;
        cmds.push_back(t2);
        return cmds;
    }

    // Style 1 (Default): Full 90-FPS Telemetry Card + Frame Budget Bar
    constexpr float kCardW = 340.0f;
    constexpr float kCardH = 142.0f;

    DrawCommand panelBg{};
    panelBg.type  = DrawCommandType::RectFilled;
    panelBg.x     = x;
    panelBg.y     = y;
    panelBg.w     = kCardW;
    panelBg.h     = kCardH;
    panelBg.x3    = 10.0f;
    panelBg.color = static_cast<int>(0xDD101418u);
    cmds.push_back(panelBg);

    DrawCommand topStripe{};
    topStripe.type  = DrawCommandType::RectFilled;
    topStripe.x     = x;
    topStripe.y     = y;
    topStripe.w     = kCardW;
    topStripe.h     = 4.0f;
    topStripe.x3    = 4.0f;
    topStripe.color = accent;
    cmds.push_back(topStripe);

    DrawCommand border{};
    border.type  = DrawCommandType::Rect;
    border.x     = x;
    border.y     = y;
    border.w     = kCardW;
    border.h     = kCardH;
    border.x3    = 10.0f;
    border.size  = 1.5f;
    border.color = static_cast<int>(0x664AE0A0u);
    cmds.push_back(border);

    char headerBuf[96]{};
    std::snprintf(
        headerBuf, sizeof(headerBuf),
        "HYPER90   %.1f FPS   (1%% %.0f)",
        snap.avgFps, snap.onePercentLowFps);

    DrawCommand headerTxt{};
    headerTxt.type  = DrawCommandType::Text;
    headerTxt.x     = x + 12.0f;
    headerTxt.y     = y + 28.0f;
    headerTxt.size  = 22.0f;
    headerTxt.color = fpsColor;
    headerTxt.text  = headerBuf;
    cmds.push_back(headerTxt);

    char timingBuf[96]{};
    std::snprintf(
        timingBuf, sizeof(timingBuf),
        "Frame: %.2f ms (Jit %.2f ms) | %s",
        snap.lastFrameMs, snap.jitterMs, governorTierName(snap.tier));

    DrawCommand timingTxt{};
    timingTxt.type  = DrawCommandType::Text;
    timingTxt.x     = x + 12.0f;
    timingTxt.y     = y + 54.0f;
    timingTxt.size  = 17.5f;
    timingTxt.color = static_cast<int>(0xFFE2E8F0u);
    timingTxt.text  = timingBuf;
    cmds.push_back(timingTxt);

    char savingsBuf[112]{};
    std::snprintf(
        savingsBuf, sizeof(savingsBuf),
        "GL Dedup: %llu | TBDR: %llu | Culled: %llu",
        static_cast<unsigned long long>(snap.glStateDedupSaved),
        static_cast<unsigned long long>(snap.tbdrTilesDiscarded),
        static_cast<unsigned long long>(snap.drawCallsCulled));

    DrawCommand savingsTxt{};
    savingsTxt.type  = DrawCommandType::Text;
    savingsTxt.x     = x + 12.0f;
    savingsTxt.y     = y + 78.0f;
    savingsTxt.size  = 16.5f;
    savingsTxt.color = static_cast<int>(0xFFA8B8C8u);
    savingsTxt.text  = savingsBuf;
    cmds.push_back(savingsTxt);

    char sysBuf[112]{};
    std::snprintf(
        sysBuf, sizeof(sysBuf),
        "Big Cores: %d | Draws/F: %d | HUD Hits: %llu",
        snap.pinnedBigCores,
        snap.lastFrameDrawCalls,
        static_cast<unsigned long long>(snap.hudCacheHits));

    DrawCommand sysTxt{};
    sysTxt.type  = DrawCommandType::Text;
    sysTxt.x     = x + 12.0f;
    sysTxt.y     = y + 102.0f;
    sysTxt.size  = 16.5f;
    sysTxt.color = static_cast<int>(0xFFA8B8C8u);
    sysTxt.text  = sysBuf;
    cmds.push_back(sysTxt);

    // 90-FPS Stability Bar (fills 100% at >=90 FPS)
    const float barTrackW = kCardW - 24.0f;
    const float fillRatio = std::clamp(snap.avgFps / 90.0f, 0.08f, 1.0f);

    DrawCommand barTrack{};
    barTrack.type  = DrawCommandType::RectFilled;
    barTrack.x     = x + 12.0f;
    barTrack.y     = y + 118.0f;
    barTrack.w     = barTrackW;
    barTrack.h     = 10.0f;
    barTrack.x3    = 5.0f;
    barTrack.color = static_cast<int>(0xFF222A32u);
    cmds.push_back(barTrack);

    DrawCommand barFill{};
    barFill.type  = DrawCommandType::RectFilled;
    barFill.x     = x + 12.0f;
    barFill.y     = y + 118.0f;
    barFill.w     = barTrackW * fillRatio;
    barFill.h     = 10.0f;
    barFill.x3    = 5.0f;
    barFill.color = fpsColor;
    cmds.push_back(barFill);

    return cmds;
}

void Modules::onFrameRendered() noexcept {
    const auto& cfg = getConfig();
    const uint64_t totalFrames = getTelemetry().getSnapshot().totalFrames;

    // 1. Update simplified center crosshair (Module 9)
    const bool wantCrosshair =
        cfg.modHudAcceleratorEnabled.load(std::memory_order_relaxed) &&
        cfg.simplifyCrosshair.load(std::memory_order_relaxed);

    if (wantCrosshair) {
        if (!gCrosshairWasVisible || (totalFrames & 0x1Fu) == 0u) {
            const float r = static_cast<float>(
                std::clamp(cfg.crosshairSize.load(std::memory_order_relaxed), 4, 24));
            const int color = static_cast<int>(cfg.accentColorArgb.load(std::memory_order_relaxed));

            // LeviLaunchroid HudOverlay.java screen-center coordinate convention: -20000.0f + offset
            std::vector<pl::modmenu::DrawCommand> crossCmds(2);
            crossCmds[0].type  = pl::modmenu::DrawCommandType::Line;
            crossCmds[0].x     = -20000.0f - r;
            crossCmds[0].y     = -20000.0f;
            crossCmds[0].w     = r * 2.0f;
            crossCmds[0].h     = 0.0f;
            crossCmds[0].size  = 2.0f;
            crossCmds[0].color = color;

            crossCmds[1].type  = pl::modmenu::DrawCommandType::Line;
            crossCmds[1].x     = -20000.0f;
            crossCmds[1].y     = -20000.0f - r;
            crossCmds[1].w     = 0.0f;
            crossCmds[1].h     = r * 2.0f;
            crossCmds[1].size  = 2.0f;
            crossCmds[1].color = color;

            pl::modmenu::submitDrawCommands(ModuleIds::kHudUiAccelerator, crossCmds);
            gCrosshairWasVisible = true;
        }
    } else if (gCrosshairWasVisible) {
        pl::modmenu::submitDrawCommands(ModuleIds::kHudUiAccelerator, {});
        gCrosshairWasVisible = false;
    }

    // 2. Update 90-FPS Telemetry HUD (Module 12) every 12 frames (~133ms at 90 FPS)
    const bool wantHud =
        cfg.modAdaptiveGovernorEnabled.load(std::memory_order_relaxed) &&
        cfg.showFpsHud.load(std::memory_order_relaxed);

    if (!wantHud) {
        if (gHudWasVisible) {
            pl::modmenu::submitDrawCommands(ModuleIds::kAdaptiveGovernorHud, {});
            pl::modmenu::submitHudEditorElements(ModuleIds::kAdaptiveGovernorHud, {});
            gHudWasVisible = false;
        }
        return;
    }

    if (gHudWasVisible && (totalFrames - gLastHudUpdateFrame) < 12) {
        return;
    }
    gLastHudUpdateFrame = totalFrames;

    const auto cmds = buildHudDrawCommands();
    pl::modmenu::submitDrawCommands(ModuleIds::kAdaptiveGovernorHud, cmds);

    // Register movable HUD Editor bounding box so users can drag & snap the HUD
    pl::modmenu::HudEditorElement elem{};
    elem.elementId     = "hyper90.hud.telemetry_card";
    elem.displayName   = "Hyper90 Telemetry HUD";
    elem.positionKeyX  = "hudPosX";
    elem.positionKeyY  = "hudPosY";
    elem.snapGroup     = "hyper90";
    elem.x             = std::clamp(cfg.hudPosX.load(std::memory_order_relaxed), 0.0f, 3840.0f);
    elem.y             = std::clamp(cfg.hudPosY.load(std::memory_order_relaxed), 0.0f, 2160.0f);
    elem.width         = 340.0f;
    elem.height        = 142.0f;
    elem.gridSize      = 8.0f;
    elem.snapThreshold = 10.0f;
    elem.gridGap       = 6.0f;
    elem.snapFlags     = pl::modmenu::HudSnapGrid |
                         pl::modmenu::HudSnapElements |
                         pl::modmenu::HudSnapScreenCenter;

    std::vector<pl::modmenu::HudEditorElement> elements{elem};
    pl::modmenu::submitHudEditorElements(ModuleIds::kAdaptiveGovernorHud, elements);
    gHudWasVisible = true;
}

} // namespace hyper90
