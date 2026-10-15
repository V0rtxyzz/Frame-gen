#include "hyper90/Config.hpp"
#include "hyper90/EngineOptionsTuner.hpp"
#include "hyper90/Modules.hpp"
#include "hyper90/NativeHooks.hpp"
#include "hyper90/SystemOptimizers.hpp"
#include "hyper90/Telemetry.hpp"

#include "pl/Mod.hpp"

namespace hyper90 {

class Hyper90Mod {
public:
    Hyper90Mod() : mSelf(*pl::mod::NativeMod::current()) {}

    static Hyper90Mod& getInstance() {
        static Hyper90Mod sInstance;
        return sInstance;
    }

    [[nodiscard]] const pl::mod::NativeMod& getSelf() const noexcept {
        return mSelf;
    }

    bool load() {
        const auto& self = getSelf();
        const auto configDir = self.getConfigDir();
        const auto modDir = self.getModDir();

        // 1. Load or initialize persistent JSON configuration & schema
        loadConfigFromDisk(configDir);
        writeConfigSchemaToDisk(configDir);

        // 2. Module 2: Pre-tune Bedrock options.txt before MinecraftActivity starts Mojang C++ engine
        EngineOptionsTuner::discoverAndTuneAll(modDir);

        // 3. Module 10: Tune Bionic Scudo/jemalloc heap decay time & lock hot pages
        MemoryArenaOptimizer::applyBionicHeapTuning();
        MemoryArenaOptimizer::lockHotMemoryPages();

        // 4. Register all 13 ModMenu modules & 2 quick-action buttons
        Modules::registerAll(self.getId(), modDir);

        self.getLogger().info(
            "[Hyper90] Loaded 13-Module 90-FPS Engine & Optimizer Suite (target={} FPS)",
            getConfig().targetFps.load());
        return true;
    }

    bool enable() {
        const auto& self = getSelf();
        const auto modDir = self.getModDir();

        // 1. Ensure all 13 ModMenu modules are registered
        Modules::registerAll(self.getId(), modDir);

        // 2. Install EGL, GLES2/3, libc & Bedrock symbol hooks
        NativeHooks::installAll();

        // 3. Apply CPU Prime Core scheduling & options.txt tuning
        CpuSchedulerOptimizer::applyRenderThreadBoost(true);
        EngineOptionsTuner::discoverAndTuneAll(modDir);

        // 4. Publish initial HUD Telemetry overlay
        Modules::onFrameRendered();

        self.getLogger().info("[Hyper90] Enabled all 13 optimization modules & native hooks.");
        return true;
    }

    bool disable() {
        const auto& self = getSelf();
        saveConfigToDisk(self.getConfigDir());

        NativeHooks::uninstallAll();
        Modules::unregisterAll();
        MemoryArenaOptimizer::releaseLockedPages();

        self.getLogger().info("[Hyper90] Disabled hooks and saved configuration.");
        return true;
    }

    bool unload() {
        disable();
        return true;
    }

private:
    pl::mod::NativeMod& mSelf;
};

} // namespace hyper90

PL_REGISTER_MOD(hyper90::Hyper90Mod, hyper90::Hyper90Mod::getInstance())
