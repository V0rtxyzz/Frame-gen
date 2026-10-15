#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pl/ModMenu.hpp"

namespace hyper90 {

struct ModuleDescriptor {
    const char* id;
    const char* displayName;
    const char* description;
    bool        hideInHudEditor;
};

class Modules {
public:
    static constexpr size_t kModuleCount = 13;

    // Return the canonical descriptors for all 13 optimization modules
    static const std::array<ModuleDescriptor, kModuleCount>& getDescriptors() noexcept;

    // Register all 13 modules and 2 quick-action buttons with LeviLaunchroid ModMenu
    static bool registerAll(std::string_view ownerModId, const std::filesystem::path& modRootDir) noexcept;

    // Unregister all 13 modules and 2 buttons cleanly on disable/unload
    static void unregisterAll() noexcept;

    // Build the HUD DrawCommand list for the current telemetry & crosshair state (testable on host & Android)
    static std::vector<pl::modmenu::DrawCommand> buildHudDrawCommands() noexcept;

    // Called from eglSwapBuffers to update live HUD overlay & HUD editor elements
    static void onFrameRendered() noexcept;

    // Handle live toggle or config change from ModMenu UI (also callable directly from unit tests)
    static void handleModuleToggle(std::string_view moduleId, bool enabled) noexcept;
    static void handleConfigChange(std::string_view moduleId, std::string_view key, std::string_view value) noexcept;
};

} // namespace hyper90
