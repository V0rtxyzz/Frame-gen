#include "hyper90/Config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace hyper90 {

RuntimeConfig& getConfig() noexcept {
    static RuntimeConfig sConfig;
    return sConfig;
}

uint32_t parseHexColorArgb(std::string_view hex, uint32_t fallback) noexcept {
    if (hex.empty()) return fallback;
    if (hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 8) return fallback;

    uint32_t value = 0;
    auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (ec != std::errc{} || ptr != hex.data() + hex.size()) {
        return fallback;
    }
    if (hex.size() == 6) {
        return 0xFF000000u | value;
    }
    return value;
}

std::string formatHexColorRgb(uint32_t argb) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned int>(argb & 0x00FFFFFFu));
    return std::string(buf);
}

namespace {

template <typename T>
void readJsonAtom(const nlohmann::json& j, const char* key, std::atomic<T>& target, T minVal, T maxVal) noexcept {
    if (!j.is_object() || !j.contains(key)) return;
    try {
        T val = j.at(key).get<T>();
        if (val < minVal) val = minVal;
        if (val > maxVal) val = maxVal;
        target.store(val, std::memory_order_relaxed);
    } catch (...) {
    }
}

void readJsonBool(const nlohmann::json& j, const char* key, std::atomic<bool>& target) noexcept {
    if (!j.is_object() || !j.contains(key)) return;
    try {
        if (j.at(key).is_boolean()) {
            target.store(j.at(key).get<bool>(), std::memory_order_relaxed);
        }
    } catch (...) {
    }
}

} // namespace

bool loadConfigFromDisk(const std::filesystem::path& configDir) noexcept {
    try {
        const auto cfgPath = configDir / "config.json";
        if (!std::filesystem::exists(cfgPath)) {
            saveConfigToDisk(configDir);
            writeConfigSchemaToDisk(configDir);
            return true;
        }
        std::ifstream ifs(cfgPath);
        if (!ifs) return false;

        nlohmann::json root = nlohmann::json::parse(ifs, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return false;

        auto& c = getConfig();

        if (root.contains("frame_pacer")) {
            const auto& s = root["frame_pacer"];
            readJsonBool(s, "enabled", c.modFramePacerEnabled);
            readJsonBool(s, "vsync_bypass", c.vsyncBypass);
            readJsonAtom<int>(s, "target_fps", c.targetFps, 60, 240);
            readJsonAtom<int>(s, "pacing_mode", c.pacingMode, 0, 2);
            readJsonBool(s, "anti_stutter_guard", c.antiStutterGuard);
        }
        if (root.contains("engine_options")) {
            const auto& s = root["engine_options"];
            readJsonBool(s, "enabled", c.modEngineOptionsEnabled);
            readJsonBool(s, "disable_fancy_gfx", c.disableFancyGfx);
            readJsonBool(s, "opaque_leaves", c.opaqueLeaves);
            readJsonBool(s, "fast_skies_clouds", c.fastSkiesClouds);
            readJsonBool(s, "disable_msaa_texel_aa", c.disableMsaaTexelAa);
            readJsonBool(s, "fast_lighting", c.fastLighting);
            readJsonAtom<int>(s, "view_distance_chunks", c.viewDistanceChunks, 5, 16);
        }
        if (root.contains("cpu_scheduler")) {
            const auto& s = root["cpu_scheduler"];
            readJsonBool(s, "enabled", c.modCpuSchedulerEnabled);
            readJsonBool(s, "pin_render_thread", c.pinRenderThread);
            readJsonBool(s, "boost_thread_priority", c.boostThreadPriority);
            readJsonAtom<int>(s, "core_selection_mode", c.coreSelectionMode, 0, 2);
            readJsonAtom<int>(s, "reapply_interval_sec", c.reapplyIntervalSec, 1, 30);
        }
        if (root.contains("gl_pipeline")) {
            const auto& s = root["gl_pipeline"];
            readJsonBool(s, "enabled", c.modGlPipelineEnabled);
            readJsonBool(s, "fastest_shader_hints", c.fastestShaderHints);
            readJsonBool(s, "disable_dither_msaa", c.disableDitherMsaa);
            readJsonBool(s, "suppress_gl_finish", c.suppressGlFinish);
            readJsonBool(s, "dedup_gl_state", c.dedupGlState);
        }
        if (root.contains("tbdr_tile_saver")) {
            const auto& s = root["tbdr_tile_saver"];
            readJsonBool(s, "enabled", c.modTbdrTileSaverEnabled);
            readJsonBool(s, "discard_depth_stencil", c.discardDepthStencil);
            readJsonBool(s, "optimize_clear_calls", c.optimizeClearCalls);
        }
        if (root.contains("texture_lod")) {
            const auto& s = root["texture_lod"];
            readJsonBool(s, "enabled", c.modTextureLodEnabled);
            readJsonBool(s, "clamp_anisotropy", c.clampAnisotropy);
            readJsonAtom<int>(s, "max_anisotropy", c.maxAnisotropy, 1, 8);
            readJsonBool(s, "fast_mipmap_filter", c.fastMipmapFilter);
            readJsonAtom<int>(s, "max_mip_level", c.maxMipLevel, 2, 10);
        }
        if (root.contains("draw_batch_culler")) {
            const auto& s = root["draw_batch_culler"];
            readJsonBool(s, "enabled", c.modDrawBatchCullerEnabled);
            readJsonBool(s, "cull_degenerate_draws", c.cullDegenerateDraws);
            readJsonBool(s, "adaptive_micro_batch_cull", c.adaptiveMicroBatchCull);
            readJsonAtom<int>(s, "micro_batch_max_indices", c.microBatchMaxIndices, 6, 60);
            readJsonAtom<int>(s, "cull_trigger_fps", c.cullTriggerFps, 60, 120);
        }
        if (root.contains("entity_particle_guard")) {
            const auto& s = root["entity_particle_guard"];
            readJsonBool(s, "enabled", c.modEntityParticleEnabled);
            readJsonBool(s, "entity_distance_culling", c.entityDistanceCulling);
            readJsonAtom<int>(s, "max_entity_distance", c.maxEntityDistance, 16, 128);
            readJsonBool(s, "limit_particles", c.limitParticles);
            readJsonAtom<int>(s, "max_particles_per_frame", c.maxParticlesPerFrame, 20, 1000);
            readJsonBool(s, "fast_entity_interp", c.fastEntityInterp);
        }
        if (root.contains("hud_ui_accelerator")) {
            const auto& s = root["hud_ui_accelerator"];
            readJsonBool(s, "enabled", c.modHudAcceleratorEnabled);
            readJsonBool(s, "cache_hotbar_vitals", c.cacheHotbarVitals);
            readJsonBool(s, "cache_ui_text", c.cacheUiText);
            readJsonBool(s, "disable_ui_animations", c.disableUiAnimations);
            readJsonBool(s, "simplify_crosshair", c.simplifyCrosshair);
            readJsonAtom<int>(s, "crosshair_size", c.crosshairSize, 4, 24);
        }
        if (root.contains("memory_arena_tuner")) {
            const auto& s = root["memory_arena_tuner"];
            readJsonBool(s, "enabled", c.modMemoryArenaEnabled);
            readJsonBool(s, "bionic_heap_retain", c.bionicHeapRetain);
            readJsonAtom<int>(s, "heap_decay_time_ms", c.heapDecayTimeMs, 1000, 60000);
            readJsonBool(s, "lock_hot_pages", c.lockHotPages);
        }
        if (root.contains("camera_input_90hz")) {
            const auto& s = root["camera_input_90hz"];
            readJsonBool(s, "enabled", c.modCameraInputEnabled);
            readJsonBool(s, "smooth_camera_90hz", c.smoothCamera90Hz);
            readJsonAtom<float>(s, "camera_responsiveness", c.cameraResponsiveness, 10.0f, 60.0f);
            readJsonBool(s, "raw_flick_bypass", c.rawFlickBypass);
            readJsonBool(s, "fullbright_gamma", c.fullbrightGamma);
            readJsonBool(s, "low_delay_sockets", c.lowDelaySockets);
        }
        if (root.contains("adaptive_governor_hud")) {
            const auto& s = root["adaptive_governor_hud"];
            readJsonBool(s, "enabled", c.modAdaptiveGovernorEnabled);
            readJsonBool(s, "adaptive_90_governor", c.adaptive90Governor);
            readJsonBool(s, "ultra_boost_override", c.ultraBoostOverride);
            readJsonBool(s, "show_fps_hud", c.showFpsHud);
            readJsonAtom<int>(s, "hud_style", c.hudStyle, 0, 2);
            readJsonAtom<float>(s, "hud_pos_x", c.hudPosX, 0.0f, 3840.0f);
            readJsonAtom<float>(s, "hud_pos_y", c.hudPosY, 0.0f, 2160.0f);
            if (s.contains("accent_color") && s["accent_color"].is_string()) {
                c.accentColorArgb.store(
                    parseHexColorArgb(s["accent_color"].get<std::string>(), 0xFF00FF88u),
                    std::memory_order_relaxed);
            }
        }
        if (root.contains("frame_blend")) {
            const auto& s = root["frame_blend"];
            readJsonBool(s, "enabled", c.modFrameBlendEnabled);
            readJsonAtom<int>(s, "mode", c.frameBlendMode, 0, 1);
            readJsonAtom<int>(s, "strength_pct", c.frameBlendStrengthPct, 10, 90);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool saveConfigToDisk(const std::filesystem::path& configDir) noexcept {
    try {
        std::filesystem::create_directories(configDir);
        const auto& c = getConfig();

        nlohmann::json root = {
            {"version", 1},
            {"frame_pacer", {
                {"enabled", c.modFramePacerEnabled.load(std::memory_order_relaxed)},
                {"vsync_bypass", c.vsyncBypass.load(std::memory_order_relaxed)},
                {"target_fps", c.targetFps.load(std::memory_order_relaxed)},
                {"pacing_mode", c.pacingMode.load(std::memory_order_relaxed)},
                {"anti_stutter_guard", c.antiStutterGuard.load(std::memory_order_relaxed)}
            }},
            {"engine_options", {
                {"enabled", c.modEngineOptionsEnabled.load(std::memory_order_relaxed)},
                {"disable_fancy_gfx", c.disableFancyGfx.load(std::memory_order_relaxed)},
                {"opaque_leaves", c.opaqueLeaves.load(std::memory_order_relaxed)},
                {"fast_skies_clouds", c.fastSkiesClouds.load(std::memory_order_relaxed)},
                {"disable_msaa_texel_aa", c.disableMsaaTexelAa.load(std::memory_order_relaxed)},
                {"fast_lighting", c.fastLighting.load(std::memory_order_relaxed)},
                {"view_distance_chunks", c.viewDistanceChunks.load(std::memory_order_relaxed)}
            }},
            {"cpu_scheduler", {
                {"enabled", c.modCpuSchedulerEnabled.load(std::memory_order_relaxed)},
                {"pin_render_thread", c.pinRenderThread.load(std::memory_order_relaxed)},
                {"boost_thread_priority", c.boostThreadPriority.load(std::memory_order_relaxed)},
                {"core_selection_mode", c.coreSelectionMode.load(std::memory_order_relaxed)},
                {"reapply_interval_sec", c.reapplyIntervalSec.load(std::memory_order_relaxed)}
            }},
            {"gl_pipeline", {
                {"enabled", c.modGlPipelineEnabled.load(std::memory_order_relaxed)},
                {"fastest_shader_hints", c.fastestShaderHints.load(std::memory_order_relaxed)},
                {"disable_dither_msaa", c.disableDitherMsaa.load(std::memory_order_relaxed)},
                {"suppress_gl_finish", c.suppressGlFinish.load(std::memory_order_relaxed)},
                {"dedup_gl_state", c.dedupGlState.load(std::memory_order_relaxed)}
            }},
            {"tbdr_tile_saver", {
                {"enabled", c.modTbdrTileSaverEnabled.load(std::memory_order_relaxed)},
                {"discard_depth_stencil", c.discardDepthStencil.load(std::memory_order_relaxed)},
                {"optimize_clear_calls", c.optimizeClearCalls.load(std::memory_order_relaxed)}
            }},
            {"texture_lod", {
                {"enabled", c.modTextureLodEnabled.load(std::memory_order_relaxed)},
                {"clamp_anisotropy", c.clampAnisotropy.load(std::memory_order_relaxed)},
                {"max_anisotropy", c.maxAnisotropy.load(std::memory_order_relaxed)},
                {"fast_mipmap_filter", c.fastMipmapFilter.load(std::memory_order_relaxed)},
                {"max_mip_level", c.maxMipLevel.load(std::memory_order_relaxed)}
            }},
            {"draw_batch_culler", {
                {"enabled", c.modDrawBatchCullerEnabled.load(std::memory_order_relaxed)},
                {"cull_degenerate_draws", c.cullDegenerateDraws.load(std::memory_order_relaxed)},
                {"adaptive_micro_batch_cull", c.adaptiveMicroBatchCull.load(std::memory_order_relaxed)},
                {"micro_batch_max_indices", c.microBatchMaxIndices.load(std::memory_order_relaxed)},
                {"cull_trigger_fps", c.cullTriggerFps.load(std::memory_order_relaxed)}
            }},
            {"entity_particle_guard", {
                {"enabled", c.modEntityParticleEnabled.load(std::memory_order_relaxed)},
                {"entity_distance_culling", c.entityDistanceCulling.load(std::memory_order_relaxed)},
                {"max_entity_distance", c.maxEntityDistance.load(std::memory_order_relaxed)},
                {"limit_particles", c.limitParticles.load(std::memory_order_relaxed)},
                {"max_particles_per_frame", c.maxParticlesPerFrame.load(std::memory_order_relaxed)},
                {"fast_entity_interp", c.fastEntityInterp.load(std::memory_order_relaxed)}
            }},
            {"hud_ui_accelerator", {
                {"enabled", c.modHudAcceleratorEnabled.load(std::memory_order_relaxed)},
                {"cache_hotbar_vitals", c.cacheHotbarVitals.load(std::memory_order_relaxed)},
                {"cache_ui_text", c.cacheUiText.load(std::memory_order_relaxed)},
                {"disable_ui_animations", c.disableUiAnimations.load(std::memory_order_relaxed)},
                {"simplify_crosshair", c.simplifyCrosshair.load(std::memory_order_relaxed)},
                {"crosshair_size", c.crosshairSize.load(std::memory_order_relaxed)}
            }},
            {"memory_arena_tuner", {
                {"enabled", c.modMemoryArenaEnabled.load(std::memory_order_relaxed)},
                {"bionic_heap_retain", c.bionicHeapRetain.load(std::memory_order_relaxed)},
                {"heap_decay_time_ms", c.heapDecayTimeMs.load(std::memory_order_relaxed)},
                {"lock_hot_pages", c.lockHotPages.load(std::memory_order_relaxed)}
            }},
            {"camera_input_90hz", {
                {"enabled", c.modCameraInputEnabled.load(std::memory_order_relaxed)},
                {"smooth_camera_90hz", c.smoothCamera90Hz.load(std::memory_order_relaxed)},
                {"camera_responsiveness", c.cameraResponsiveness.load(std::memory_order_relaxed)},
                {"raw_flick_bypass", c.rawFlickBypass.load(std::memory_order_relaxed)},
                {"fullbright_gamma", c.fullbrightGamma.load(std::memory_order_relaxed)},
                {"low_delay_sockets", c.lowDelaySockets.load(std::memory_order_relaxed)}
            }},
            {"adaptive_governor_hud", {
                {"enabled", c.modAdaptiveGovernorEnabled.load(std::memory_order_relaxed)},
                {"adaptive_90_governor", c.adaptive90Governor.load(std::memory_order_relaxed)},
                {"ultra_boost_override", c.ultraBoostOverride.load(std::memory_order_relaxed)},
                {"show_fps_hud", c.showFpsHud.load(std::memory_order_relaxed)},
                {"hud_style", c.hudStyle.load(std::memory_order_relaxed)},
                {"hud_pos_x", c.hudPosX.load(std::memory_order_relaxed)},
                {"hud_pos_y", c.hudPosY.load(std::memory_order_relaxed)},
                {"accent_color", formatHexColorRgb(c.accentColorArgb.load(std::memory_order_relaxed))}
            }},
            {"frame_blend", {
                {"enabled", c.modFrameBlendEnabled.load(std::memory_order_relaxed)},
                {"mode", c.frameBlendMode.load(std::memory_order_relaxed)},
                {"strength_pct", c.frameBlendStrengthPct.load(std::memory_order_relaxed)}
            }}
        };

        std::ofstream ofs(configDir / "config.json");
        if (!ofs) return false;
        ofs << root.dump(2) << '\n';
        return true;
    } catch (...) {
        return false;
    }
}

bool writeConfigSchemaToDisk(const std::filesystem::path& configDir) noexcept {
    try {
        std::filesystem::create_directories(configDir);
        nlohmann::json schema = {
            {"$schema", "https://json-schema.org/draft/2020-12/schema"},
            {"title", "Hyper90 FPS Booster Configuration"},
            {"description", "Configuration schema for all 13 native 90-FPS optimization modules."},
            {"type", "object"},
            {"properties", {
                {"version", {{"type", "integer"}, {"default", 1}}},
                {"frame_pacer", {{"type", "object"}}},
                {"engine_options", {{"type", "object"}}},
                {"cpu_scheduler", {{"type", "object"}}},
                {"gl_pipeline", {{"type", "object"}}},
                {"tbdr_tile_saver", {{"type", "object"}}},
                {"texture_lod", {{"type", "object"}}},
                {"draw_batch_culler", {{"type", "object"}}},
                {"entity_particle_guard", {{"type", "object"}}},
                {"hud_ui_accelerator", {{"type", "object"}}},
                {"memory_arena_tuner", {{"type", "object"}}},
                {"camera_input_90hz", {{"type", "object"}}},
                {"adaptive_governor_hud", {{"type", "object"}}},
                {"frame_blend", {{"type", "object"}}}
            }}
        };
        std::ofstream ofs(configDir / "config.schema.json");
        if (!ofs) return false;
        ofs << schema.dump(2) << '\n';
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace hyper90
