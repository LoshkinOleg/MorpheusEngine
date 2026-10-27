#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace morpheus::test {

// A clean, empty directory under the system temp folder, one per name. Wiped first if it already exists.
inline std::filesystem::path FreshTempDir(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "morpheus_tests" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// tests/data in the source tree.
inline std::filesystem::path TestDataDir()
{
    return MORPHEUS_TEST_DATA_DIR;
}

// Repository root in the source tree.
inline std::filesystem::path RepoDir()
{
    return MORPHEUS_REPO_DIR;
}

// Overwrites a file with exactly `text` (binary mode: no newline translation).
inline void WriteFile(const std::filesystem::path& file, std::string_view text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    // A silently failed write would make a test pass or fail for the wrong reason, so fail loudly.
    if (stream.fail()) {
        throw std::runtime_error("WriteFile: cannot write '" + file.generic_string() + "'");
    }
}

} // namespace morpheus::test
