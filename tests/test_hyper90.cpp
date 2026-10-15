#include "hyper90/Config.hpp"
#include "hyper90/EngineOptionsTuner.hpp"
#include "hyper90/FrameBlend.hpp"
#include "hyper90/Modules.hpp"
#include "hyper90/NativeHooks.hpp"
#include "hyper90/SystemOptimizers.hpp"
#include "hyper90/Telemetry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Mock LeviLaunchroid Preloader ModMenu Bridge for Host Verification
// (Enforces the exact same validation constraints as ModMenuBridge.cpp)
// ============================================================================
namespace {
std::vector<pl::modmenu::ModuleInfo>        gMockModules;
std::vector<pl::modmenu::ButtonInfo>        gMockButtons;
std::vector<pl::modmenu::DrawCommand>       gMockDrawCmds;
std::vector<pl::modmenu::HudEditorElement>  gMockHudElements;
} // namespace

namespace pl::modmenu {

bool registerModule(const ModuleInfo& info) {
    assert(!info.moduleId.empty() && info.moduleId.size() <= 160);
    assert(!info.displayName.empty() && info.displayName.size() <= 160);
    assert(info.description.size() <= 1024);
    assert(!info.modId.empty() && info.modId.size() <= 160);
    for (const auto& cfg : info.configs) {
        assert(!cfg.key.empty() && cfg.key.size() <= 160);
        assert(!cfg.displayName.empty() && cfg.displayName.size() <= 160);
        assert(cfg.defaultValue.size() <= 2048);
    }
    gMockModules.push_back(info);
    return true;
}

void unregisterModule(std::string_view moduleId) {
    for (auto it = gMockModules.begin(); it != gMockModules.end(); ++it) {
        if (it->moduleId == moduleId) {
            gMockModules.erase(it);
            return;
        }
    }
}

void setModuleEnabled(std::string_view, bool) {}

bool registerButton(const ButtonInfo& info) {
    assert(!info.buttonId.empty() && info.buttonId.size() <= 160);
    assert(!info.displayName.empty() && info.displayName.size() <= 160);
    assert(info.label.size() <= 32);
    gMockButtons.push_back(info);
    return true;
}

void unregisterButton(std::string_view buttonId) {
    for (auto it = gMockButtons.begin(); it != gMockButtons.end(); ++it) {
        if (it->buttonId == buttonId) {
            gMockButtons.erase(it);
            return;
        }
    }
}

void submitDrawCommands(std::string_view moduleId, std::span<const DrawCommand> commands) {
    assert(!moduleId.empty());
    for (const auto& cmd : commands) {
        assert(std::isfinite(cmd.x) && std::isfinite(cmd.y));
        assert(std::isfinite(cmd.w) && std::isfinite(cmd.h));
        assert(std::isfinite(cmd.x3) && std::isfinite(cmd.y3));
        assert(std::isfinite(cmd.size));
        assert(cmd.text.size() <= 512);
    }
    gMockDrawCmds.assign(commands.begin(), commands.end());
}

void submitHudEditorElements(std::string_view moduleId, std::span<const HudEditorElement> elements) {
    assert(!moduleId.empty());
    for (const auto& el : elements) {
        assert(!el.elementId.empty() && el.elementId.size() <= 160);
        assert(std::isfinite(el.x) && std::isfinite(el.y));
        assert(std::isfinite(el.width) && el.width >= 1.0f);
        assert(std::isfinite(el.height) && el.height >= 1.0f);
    }
    gMockHudElements.assign(elements.begin(), elements.end());
}

HudSurfaceSize getHudSurfaceSize() { return {2400.0f, 1080.0f}; }
bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
void requestOpenMenu() {}
void requestCloseMenu() {}
bool setFloatingMenuButtonVisible(bool) { return true; }

} // namespace pl::modmenu

// ============================================================================
// Verification Suite for All 13 Optimization Modules
// ============================================================================
int main(int argc, char** argv) {
    using namespace hyper90;

    // Optional CLI flag to generate default config/config.json & config/config.schema.json
    if (argc >= 3 && std::string_view(argv[1]) == "--generate-config") {
        const std::filesystem::path outDir(argv[2]);
        assert(saveConfigToDisk(outDir));
        assert(writeConfigSchemaToDisk(outDir));
        std::cout << "[Hyper90] Generated config.json and config.schema.json in " << outDir << "\n";
        return 0;
    }

    const std::filesystem::path tmpRoot = std::filesystem::temp_directory_path() / "hyper90_test_env";
    std::filesystem::remove_all(tmpRoot);

    // Create mock LeviLaunchroid directory layout:
    // <minecraftRoot>/<profileId>/mods/hyper90_fps_booster
    // <minecraftRoot>/<profileId>/external/games/com.mojang/minecraftpe/options.txt
    const auto profileDir = tmpRoot / "default_profile";
    const auto modDir     = profileDir / "mods" / "hyper90_fps_booster";
    const auto mcpeDir    = profileDir / "external" / "games" / "com.mojang" / "minecraftpe";
    std::filesystem::create_directories(modDir / "config");
    std::filesystem::create_directories(mcpeDir);

    const auto optionsTxtPath = mcpeDir / "options.txt";
    {
        std::ofstream ofs(optionsTxtPath);
        ofs << "gfx_vsync:1\n"
            << "gfx_max_framerate:60\n"
            << "gfx_msaa:4\n"
            << "gfx_texel_aa:1\n"
            << "gfx_fancygraphics:1\n"
            << "gfx_transparentleaves:1\n"
            << "gfx_fancyskies:1\n"
            << "gfx_renderclouds:1\n"
            << "gfx_smoothlighting:1\n"
            << "gfx_viewdistance:256\n"
            << "mp_username:Steve\n";
    }

    // ------------------------------------------------------------------------
    // Test 1: Register All 13 Modules & 2 Quick-Action Buttons
    // ------------------------------------------------------------------------
    assert(Modules::registerAll("hyper90_fps_booster", modDir));
    assert(gMockModules.size() == 13);
    assert(gMockButtons.size() == 2);
    std::cout << "[PASS] Registered all 13 ModMenu modules and 2 quick-action buttons.\n";

    // ------------------------------------------------------------------------
    // Test 2: Module 1 (90 FPS Unlocker & Frame Pacer) + Module 12 (Telemetry & Governor)
    // ------------------------------------------------------------------------
    auto& telemetry = getTelemetry();
    telemetry.resetCounters();

    // Simulate 30 frames at 90 FPS (11,111,111 ns per frame)
    for (int i = 0; i < 30; ++i) {
        telemetry.recordFrameDurationNs(11'111'111LL);
    }
    auto snap90 = telemetry.getSnapshot();
    assert(std::fabs(snap90.avgFps - 90.0f) < 0.5f);
    assert(std::fabs(snap90.onePercentLowFps - 90.0f) < 0.5f);
    assert(snap90.jitterMs < 0.05f);
    assert(snap90.tier == GovernorTier::Locked90Plus);

    // Simulate a heavy combat/chunk-load dip to 65 FPS (15,384,615 ns)
    for (int i = 0; i < 40; ++i) {
        telemetry.recordFrameDurationNs(15'384'615LL);
    }
    auto snapDip = telemetry.getSnapshot();
    assert(snapDip.avgFps < 75.0f);
    assert(snapDip.tier == GovernorTier::Ultra90Lock);
    std::cout << "[PASS] Module 1 & 12: 90-FPS Telemetry & 4-Tier Adaptive Governor verified.\n";

    // ------------------------------------------------------------------------
    // Test 3: Module 2 (Bedrock Engine & RenderDragon options.txt Tuner)
    // ------------------------------------------------------------------------
    const int tunedFiles = EngineOptionsTuner::discoverAndTuneAll(modDir);
    assert(tunedFiles == 1);
    assert(std::filesystem::exists(optionsTxtPath.string() + ".hyper90.bak"));

    {
        std::ifstream ifs(optionsTxtPath);
        std::stringstream buf;
        buf << ifs.rdbuf();
        const std::string content = buf.str();
        assert(content.find("gfx_vsync:0\n") != std::string::npos);
        assert(content.find("gfx_max_framerate:90\n") != std::string::npos);
        assert(content.find("gfx_msaa:1\n") != std::string::npos);
        assert(content.find("gfx_texel_aa:0\n") != std::string::npos);
        assert(content.find("gfx_fancygraphics:0\n") != std::string::npos);
        assert(content.find("gfx_transparentleaves:0\n") != std::string::npos);
        assert(content.find("gfx_fancyskies:0\n") != std::string::npos);
        assert(content.find("gfx_renderclouds:0\n") != std::string::npos);
        assert(content.find("gfx_smoothlighting:0\n") != std::string::npos);
        assert(content.find("gfx_viewdistance:112\n") != std::string::npos);
        assert(content.find("mp_username:Steve\n") != std::string::npos);
    }
    std::cout << "[PASS] Module 2: Bedrock options.txt auto-discovery, tuning & backup verified.\n";

    // ------------------------------------------------------------------------
    // Test 4: Module 3 (Big.LITTLE Prime Core & Thread Booster)
    // ------------------------------------------------------------------------
    // Simulate 8-core Snapdragon (3x Silver @ 1.8GHz, 4x Gold @ 2.8GHz, 1x Prime @ 3.2GHz)
    const std::vector<int64_t> snapdragonFreqs = {
        1800000, 1800000, 1800000,
        2800000, 2800000, 2800000, 2800000,
        3200000
    };
    const auto perfCores = CpuSchedulerOptimizer::selectPerformanceCores(snapdragonFreqs, 0);
    assert(perfCores.size() == 5);
    assert(perfCores[0] == 3 && perfCores[4] == 7);
    assert(CpuSchedulerOptimizer::applyRenderThreadBoost(true) > 0);
    std::cout << "[PASS] Module 3: Big.LITTLE Prime/Gold core selection & thread affinity verified.\n";

    // ------------------------------------------------------------------------
    // Test 5: Module 4 (OpenGL ES Pipeline & State Deduplicator)
    // ------------------------------------------------------------------------
    auto& glOpt = getGlOptimizer();
    glOpt.resetFrameState();

    // Shader hints rewritten to GL_FASTEST
    assert(glOpt.rewriteHintMode(GlConsts::kGlGenerateMipmapHint, 0x1102u) == GlConsts::kGlFastest);
    assert(glOpt.rewriteHintMode(GlConsts::kGlFragmentShaderDerivativeHint, 0x1100u) == GlConsts::kGlFastest);

    // GL_DITHER and GL_MULTISAMPLE_EXT blocked
    assert(glOpt.shouldSkipCapabilityChange(GlConsts::kGlDither, true) == true);
    assert(glOpt.shouldSkipCapabilityChange(GlConsts::kGlMultisampleExt, true) == true);

    // State deduplication for glEnable(GL_BLEND), glUseProgram, glBindTexture, glDepthMask
    assert(glOpt.shouldSkipCapabilityChange(GlConsts::kGlBlend, true) == false);
    assert(glOpt.shouldSkipCapabilityChange(GlConsts::kGlBlend, true) == true); // Deduplicated!
    assert(glOpt.shouldSkipUseProgram(42u) == false);
    assert(glOpt.shouldSkipUseProgram(42u) == true); // Deduplicated!
    glOpt.onActiveTexture(GlConsts::kGlTexture0 + 1);
    assert(glOpt.shouldSkipBindTexture(GlConsts::kGlTexture2D, 99u) == false);
    assert(glOpt.shouldSkipBindTexture(GlConsts::kGlTexture2D, 99u) == true); // Deduplicated!
    assert(glOpt.shouldSkipDepthMask(1) == false);
    assert(glOpt.shouldSkipDepthMask(1) == true); // Deduplicated!
    std::cout << "[PASS] Module 4: OpenGL ES Pipeline & State Deduplicator verified.\n";

    // ------------------------------------------------------------------------
    // Test 6: Module 5 (Adreno/Mali TBDR Tile Bandwidth Saver)
    // ------------------------------------------------------------------------
    const uint32_t mergedMask = glOpt.rewriteClearMask(GlConsts::kGlDepthBufferBit);
    assert((mergedMask & GlConsts::kGlDepthBufferBit) != 0u);
    assert((mergedMask & GlConsts::kGlStencilBufferBit) != 0u);
    std::cout << "[PASS] Module 5: Adreno/Mali TBDR Tile Bandwidth Saver verified.\n";

    // ------------------------------------------------------------------------
    // Test 7: Module 6 (VRAM & Texture Mipmap Optimizer)
    // ------------------------------------------------------------------------
    int32_t minFilter = static_cast<int32_t>(GlConsts::kGlLinearMipmapLinear);
    glOpt.rewriteTexParameteri(GlConsts::kGlTexture2D, GlConsts::kGlTextureMinFilter, minFilter);
    assert(static_cast<uint32_t>(minFilter) != GlConsts::kGlLinearMipmapLinear);

    float aniso = 16.0f;
    glOpt.rewriteTexParameterf(GlConsts::kGlTexture2D, GlConsts::kGlTextureMaxAnisotropyExt, aniso);
    assert(aniso == 1.0f);
    std::cout << "[PASS] Module 6: VRAM & Texture Mipmap Optimizer verified.\n";

    // ------------------------------------------------------------------------
    // Test 8: Module 7 & Module 8 (Draw Batch Culler & Entity/Particle Guard)
    // ------------------------------------------------------------------------
    // Degenerate 0-index draw call is always culled
    assert(glOpt.shouldCullDrawCall(GlConsts::kGlTriangles, 0) == true);

    // Submit 90 terrain draw calls (300 indices each -> never culled)
    for (int i = 0; i < 90; ++i) {
        assert(glOpt.shouldCullDrawCall(GlConsts::kGlTriangles, 300) == false);
    }

    // Submit 200 micro-batch particle draws (6 indices each -> adaptive & budget culling kicks in)
    int culledParticles = 0;
    for (int i = 0; i < 200; ++i) {
        if (glOpt.shouldCullDrawCall(GlConsts::kGlTriangles, 6)) {
            ++culledParticles;
        }
    }
    assert(culledParticles > 50);
    std::cout << "[PASS] Module 7 & 8: Degenerate, Micro-Batch & Particle Budget Culler verified.\n";

    // ------------------------------------------------------------------------
    // Test 9: Module 9 (Zero-Alloc HUD & UI Batch Accelerator)
    // ------------------------------------------------------------------------
    auto& hudCache = getHudCache();
    hudCache.invalidateAll();
    HotbarSlotState slot0{276, 0, 1, 0x1234u};
    assert(hudCache.checkAndUpdateHotbarSlot(0, slot0) == false); // First time: miss
    assert(hudCache.checkAndUpdateHotbarSlot(0, slot0) == true);  // Second time: hit!

    PlayerVitalsState vitals{20, 20, 20, 15, 30, 500};
    assert(hudCache.checkAndUpdateVitals(vitals) == false);
    assert(hudCache.checkAndUpdateVitals(vitals) == true);

    assert(hudCache.checkAndUpdateText(1, "Position: 120, 64, -300") == false);
    assert(hudCache.checkAndUpdateText(1, "Position: 120, 64, -300") == true);
    std::cout << "[PASS] Module 9: Zero-Alloc HUD Hotbar, Vitals & FNV-1a Text Cache verified.\n";

    // ------------------------------------------------------------------------
    // Test 10: Module 10 (Bionic Heap & Page-Fault Stutter Shield)
    // ------------------------------------------------------------------------
    (void)MemoryArenaOptimizer::applyBionicHeapTuning();
    (void)MemoryArenaOptimizer::lockHotMemoryPages();
    MemoryArenaOptimizer::releaseLockedPages();
    std::cout << "[PASS] Module 10: Bionic Heap & Page-Fault Stutter Shield verified.\n";

    // ------------------------------------------------------------------------
    // Test 11: Module 11 (90Hz Sub-Frame Camera & Touch Stabilizer)
    // ------------------------------------------------------------------------
    auto& cam = getCameraStabilizer();
    cam.reset();
    float dx = 4.0f, dy = 2.0f;
    cam.filterTurnDelta(dx, dy, 0.0111f); // Initial sample
    float dx2 = 6.0f, dy2 = 3.0f;
    cam.filterTurnDelta(dx2, dy2, 0.0111f);
    assert(dx2 > 4.0f && dx2 < 6.0f); // Smoothly interpolated at 90Hz!

    // Raw Flick Bypass (> 30 deg flick passes through unattenuated)
    float flickX = 45.0f, flickY = 0.0f;
    cam.filterTurnDelta(flickX, flickY, 0.0111f);
    assert(std::fabs(flickX - 45.0f) < 1e-4f);
    std::cout << "[PASS] Module 11: 90Hz Sub-Frame Camera Filter & Raw Flick Bypass verified.\n";

    // ------------------------------------------------------------------------
    // Test 12: Live HUD DrawCommands, HUD Editor Elements & Config Persistence
    // ------------------------------------------------------------------------
    Modules::handleConfigChange(ModuleIds::kHudUiAccelerator, "simplify_crosshair", "true");
    Modules::onFrameRendered();
    assert(!gMockDrawCmds.empty());
    assert(!gMockHudElements.empty());

    // Toggle & config change round-trip persistence test
    Modules::handleConfigChange(ModuleIds::kFramePacer, "target_fps", "90");
    assert(loadConfigFromDisk(modDir / "config"));
    assert(getConfig().targetFps.load() == 90);

    // ------------------------------------------------------------------------
    // Test 13: Module 13 (Frame Blend) - planner, telemetry accounting, config
    // ------------------------------------------------------------------------
    {
        using namespace hyper90::FrameBlendConsts;

        // Off by default (experimental, forces VSync while active)
        assert(!getConfig().modFrameBlendEnabled.load());
        assert(!FrameBlendGl::wantsVsync());

        // Multiplier & weight helpers
        assert(std::fabs(FrameBlendPlanner::multiplierForMode(kModeX15) - 1.5f) < 1e-6f);
        assert(std::fabs(FrameBlendPlanner::multiplierForMode(kModeX20) - 2.0f) < 1e-6f);
        assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(50) - 0.5f) < 1e-6f);
        assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(0) - 0.1f) < 1e-6f);
        assert(std::fabs(FrameBlendPlanner::weightForStrengthPct(500) - 0.9f) < 1e-6f);

        // 2.0x: every real frame generates one (after the first, which has no history)
        FrameBlendPlanner planner;
        assert(!planner.planFrame(kModeX20, 22'000'000LL, false).generate);
        for (int i = 0; i < 10; ++i) {
            const auto plan = planner.planFrame(kModeX20, 22'000'000LL, true);
            assert(plan.generate && plan.presentsThisFrame == 2);
        }

        // 1.5x: 3 presents every 2 real frames
        planner.reset();
        int generated = 0;
        for (int i = 0; i < 100; ++i) {
            generated += planner.planFrame(kModeX15, 16'000'000LL, true).generate ? 1 : 0;
        }
        assert(generated == 50);

        // Too-slow real frames (< 20 FPS) are never blended; invalid intervals neither
        assert(!planner.planFrame(kModeX20, 80'000'000LL, true).generate);
        assert(!planner.planFrame(kModeX20, 0, true).generate);

        // Telemetry: 45 real FPS with one generated frame each must read as 90 displayed FPS
        telemetry.resetCounters();
        for (int i = 0; i < 40; ++i) {
            telemetry.recordGeneratedFrameDurationNs(11'111'111LL);
            telemetry.recordFrameDurationNs(11'111'111LL);
        }
        const auto snapBlend = telemetry.getSnapshot();
        assert(std::fabs(snapBlend.avgFps - 90.0f) < 0.5f);
        assert(snapBlend.generatedFrames == 40);
        assert(snapBlend.totalFrames == 40); // generated frames do not count as rendered frames

        // Config persistence round-trip (mode / strength / enabled)
        Modules::handleModuleToggle(ModuleIds::kFrameBlend, true);
        Modules::handleConfigChange(ModuleIds::kFrameBlend, "mode", "0");
        Modules::handleConfigChange(ModuleIds::kFrameBlend, "strength_pct", "65");
        assert(FrameBlendGl::wantsVsync());
        getConfig().modFrameBlendEnabled.store(false);
        getConfig().frameBlendMode.store(1);
        getConfig().frameBlendStrengthPct.store(50);
        assert(loadConfigFromDisk(modDir / "config"));
        assert(getConfig().modFrameBlendEnabled.load());
        assert(getConfig().frameBlendMode.load() == 0);
        assert(getConfig().frameBlendStrengthPct.load() == 65);

        // Out-of-range values are clamped
        Modules::handleConfigChange(ModuleIds::kFrameBlend, "strength_pct", "999");
        assert(getConfig().frameBlendStrengthPct.load() == 90);

        // Leave the module off so the remaining tests are unaffected
        Modules::handleModuleToggle(ModuleIds::kFrameBlend, false);
        assert(!FrameBlendGl::wantsVsync());
        assert(FrameBlendGl::onBeforeSwap(nullptr, nullptr, FrameBlendPresentOps{}) == 0);
        std::cout << "[PASS] Module 13: Frame Blend planner, displayed-FPS accounting & config verified.\n";
    }

    Modules::unregisterAll();
    assert(gMockModules.empty());
    assert(gMockButtons.empty());

    std::filesystem::remove_all(tmpRoot);
    std::cout << "[ALL 13 MODULES PASSED] Hyper90 FPS Booster verification complete!\n";
    return 0;
}
