#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/MemoryMode.h"
#include "core/Settings.h"

namespace morpheus::app {

// The settings.json values the Settings window edits. Model paths stay as written in the file (often relative to
// its folder), so saving does not rewrite them as absolute paths.
struct SettingsEdit {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int contextSize = 0;
    int gpuLayers = 0; // -1 puts every layer on the GPU.
    core::SamplingSettings resolveSampling = {};
    core::SamplingSettings narrateSampling = {};
    std::optional<core::MemoryMode> memoryMode = {}; // nullopt: each world's own default.
};

// Reads the edited values from a settings file. Throws DataError naming the file when it cannot be read, is not
// JSON, or lacks one of them (core::LoadSettings explains a bad file in full).
SettingsEdit ReadSettingsEdit(const std::filesystem::path& file);

// Writes `edit` into the settings file, keeping its other keys (the embedding prefixes, the budget) and their order.
// The new text is first written beside the file as "<name>.new" and loaded with core::LoadSettings. When anything
// fails, the check file is removed, the settings file is left as it was, and a DataError naming the settings file is
// thrown: the file cannot be read or has the wrong shape, the loader rejects the new values, or the
// check file cannot replace the settings file. Otherwise the check file replaces the settings file. Returns the loaded
// settings.
core::Settings SaveSettingsEdit(const std::filesystem::path& file, const SettingsEdit& edit);

} // namespace morpheus::app
