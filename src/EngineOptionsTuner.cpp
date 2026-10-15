#include "hyper90/EngineOptionsTuner.hpp"
#include "hyper90/Config.hpp"
#include "hyper90/Telemetry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace hyper90 {

namespace {

std::vector<std::pair<std::string, std::string>> buildDesiredOptionPairs() {
    const auto& cfg = getConfig();
    std::vector<std::pair<std::string, std::string>> desired;

    // Always unlock VSync & set max framerate when Frame Pacer or Engine Options is active
    if (cfg.modFramePacerEnabled.load(std::memory_order_relaxed) ||
        cfg.modEngineOptionsEnabled.load(std::memory_order_relaxed)) {
        if (cfg.vsyncBypass.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_vsync", "0");
        }
        const int targetFps = std::clamp(cfg.targetFps.load(std::memory_order_relaxed), 60, 240);
        desired.emplace_back("gfx_max_framerate", std::to_string(targetFps));
    }

    if (cfg.modEngineOptionsEnabled.load(std::memory_order_relaxed)) {
        if (cfg.disableMsaaTexelAa.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_msaa", "1");
            desired.emplace_back("gfx_texel_aa", "0");
        }
        if (cfg.disableFancyGfx.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_fancygraphics", "0");
            desired.emplace_back("gfx_bubbles_particles", "0");
            desired.emplace_back("gfx_particleviewdistance", "0.45");
            desired.emplace_back("graphics_mode", "0"); // 0 = Simple/Fancy (non-Deferred/RayTracing)
        }
        if (cfg.opaqueLeaves.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_transparentleaves", "0");
        }
        if (cfg.fastSkiesClouds.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_fancyskies", "0");
            desired.emplace_back("gfx_renderclouds", "0");
        }
        if (cfg.fastLighting.load(std::memory_order_relaxed)) {
            desired.emplace_back("gfx_smoothlighting", "0");
        }
        const int chunks = std::clamp(cfg.viewDistanceChunks.load(std::memory_order_relaxed), 5, 16);
        desired.emplace_back("gfx_viewdistance", std::to_string(chunks * 16));
    }

    if (cfg.modHudAcceleratorEnabled.load(std::memory_order_relaxed) &&
        cfg.disableUiAnimations.load(std::memory_order_relaxed)) {
        desired.emplace_back("gfx_screen_animations", "0");
    }

    if (cfg.modCameraInputEnabled.load(std::memory_order_relaxed) &&
        cfg.fullbrightGamma.load(std::memory_order_relaxed)) {
        desired.emplace_back("gfx_gamma", "10.0");
    }

    return desired;
}

std::vector<std::filesystem::path> collectCandidatePaths(const std::filesystem::path& modDir) {
    std::vector<std::filesystem::path> candidates;
    try {
        // LeviLaunchroid modDir is <minecraftRoot>/<profileId>/mods/<modId>
        const auto modsDir = modDir.parent_path();
        const auto profileDir = modsDir.parent_path();
        const auto minecraftRoot = profileDir.parent_path();

        if (!profileDir.empty()) {
            candidates.push_back(profileDir / "external" / "games" / "com.mojang" / "minecraftpe" / "options.txt");
            candidates.push_back(profileDir / "internal" / "games" / "com.mojang" / "minecraftpe" / "options.txt");
            candidates.push_back(profileDir / "games" / "com.mojang" / "minecraftpe" / "options.txt");
        }
        if (!minecraftRoot.empty()) {
            const auto sharedDir = minecraftRoot / "_shared";
            candidates.push_back(sharedDir / "external" / "games" / "com.mojang" / "minecraftpe" / "options.txt");
            candidates.push_back(sharedDir / "internal" / "games" / "com.mojang" / "minecraftpe" / "options.txt");
        }
#if defined(__ANDROID__)
        candidates.emplace_back("/sdcard/games/com.mojang/minecraftpe/options.txt");
        candidates.emplace_back("/storage/emulated/0/games/com.mojang/minecraftpe/options.txt");
        candidates.emplace_back(
            "/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/games/com.mojang/minecraftpe/options.txt");
#endif
    } catch (...) {
    }
    return candidates;
}

} // namespace

std::string EngineOptionsTuner::tuneOptionsContent(std::string_view originalContent) {
    const auto desiredList = buildDesiredOptionPairs();
    std::unordered_map<std::string, std::string> desiredMap;
    desiredMap.reserve(desiredList.size());
    for (const auto& [k, v] : desiredList) {
        desiredMap[k] = v;
    }

    std::unordered_set<std::string> seenKeys;
    std::ostringstream out;

    std::string contentStr(originalContent);
    std::istringstream in(contentStr);
    std::string line;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colonPos = line.find(':');
        if (colonPos != std::string::npos && colonPos > 0) {
            std::string key = line.substr(0, colonPos);
            auto it = desiredMap.find(key);
            if (it != desiredMap.end()) {
                out << key << ':' << it->second << '\n';
                seenKeys.insert(key);
                continue;
            }
        }
        out << line << '\n';
    }

    // Append any desired keys not yet present in options.txt
    for (const auto& [k, v] : desiredList) {
        if (seenKeys.find(k) == seenKeys.end()) {
            out << k << ':' << v << '\n';
            seenKeys.insert(k);
        }
    }

    return out.str();
}

bool EngineOptionsTuner::tuneOptionsFile(const std::filesystem::path& optionsFilePath) noexcept {
    try {
        if (!std::filesystem::exists(optionsFilePath)) {
            return false;
        }

        // Create backup once if not already present
        std::filesystem::path backupPath = optionsFilePath;
        backupPath += ".hyper90.bak";
        if (!std::filesystem::exists(backupPath)) {
            std::error_code ec;
            std::filesystem::copy_file(
                optionsFilePath, backupPath, std::filesystem::copy_options::skip_existing, ec);
        }

        std::ifstream ifs(optionsFilePath, std::ios::in | std::ios::binary);
        if (!ifs) return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        ifs.close();

        const std::string original = ss.str();
        const std::string tuned = tuneOptionsContent(original);

        if (tuned == original) {
            return true;
        }

        std::ofstream ofs(optionsFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!ofs) return false;
        ofs.write(tuned.data(), static_cast<std::streamsize>(tuned.size()));
        ofs.flush();
        return ofs.good();
    } catch (...) {
        return false;
    }
}

int EngineOptionsTuner::discoverAndTuneAll(const std::filesystem::path& modDir) noexcept {
    if (!getConfig().modEngineOptionsEnabled.load(std::memory_order_relaxed)) {
        return 0;
    }

    int tunedCount = 0;
    std::unordered_set<std::string> visited;

    for (const auto& path : collectCandidatePaths(modDir)) {
        try {
            const std::string key = path.lexically_normal().string();
            if (!visited.insert(key).second) continue;

            if (std::filesystem::exists(path)) {
                if (tuneOptionsFile(path)) {
                    ++tunedCount;
                }
            }
        } catch (...) {
        }
    }

    if (tunedCount > 0) {
        getTelemetry().setOptionsFilesTuned(tunedCount);
    }
    return tunedCount;
}

int EngineOptionsTuner::restoreAllBackups(const std::filesystem::path& modDir) noexcept {
    int restored = 0;
    for (const auto& path : collectCandidatePaths(modDir)) {
        try {
            std::filesystem::path backupPath = path;
            backupPath += ".hyper90.bak";
            if (std::filesystem::exists(backupPath)) {
                std::error_code ec;
                std::filesystem::copy_file(
                    backupPath, path, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    ++restored;
                }
            }
        } catch (...) {
        }
    }
    return restored;
}

bool EngineOptionsTuner::isMinecraftOptionsPath(std::string_view path) noexcept {
    if (path.size() < 11) return false;
    constexpr std::string_view kSuffix = "options.txt";
    if (path.substr(path.size() - kSuffix.size()) != kSuffix) {
        return false;
    }
    return path.find("minecraftpe") != std::string_view::npos ||
           path.find("com.mojang") != std::string_view::npos;
}

} // namespace hyper90
