#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hyper90 {

class EngineOptionsTuner {
public:
    // Pure transformation of Bedrock options.txt content based on current RuntimeConfig
    static std::string tuneOptionsContent(std::string_view originalContent);

    // Tune a specific options.txt file on disk (creates .hyper90.bak backup first)
    static bool tuneOptionsFile(const std::filesystem::path& optionsFilePath) noexcept;

    // Scan LeviLaunchroid profile & shared directories relative to modDir and tune all found options.txt files
    static int discoverAndTuneAll(const std::filesystem::path& modDir) noexcept;

    // Restore .hyper90.bak backups if the user disables Module 2
    static int restoreAllBackups(const std::filesystem::path& modDir) noexcept;

    // Check if a file path opened via fopen is Minecraft's options.txt
    static bool isMinecraftOptionsPath(std::string_view path) noexcept;
};

} // namespace hyper90
