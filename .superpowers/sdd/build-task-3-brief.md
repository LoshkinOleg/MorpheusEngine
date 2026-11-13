### Task 3: Linux presets and the Windows-only test

`CMakePresets.json` gains `linux-tests` and `linux-release` with matching build and test presets, shown only on Linux; the Windows presets are shown only on Windows. The one test that depends on Windows file sharing is compiled only on Windows.

**Files:**
- Modify: `CMakePresets.json` (whole file)
- Modify: `tests/app/SettingsEditTests.cpp:104-119`
- Test: `cmake --list-presets`, `ctest --preset tests`

**Interfaces:**
- Consumes (Task 1): `MORPHEUS_GPU_BACKEND`; `dev` sets `Vulkan`, `dev-asan` sets `CPU`.
- Produces: configure, build and test presets `linux-tests` (binary dir `build/linux-tests`) and `linux-release` (binary dir `build/linux-release`); Task 4's README and the owner's Linux acceptance use them.

- [ ] **Step 1: See the Linux presets missing**

Run: `cmake --list-presets`
Expected (before): `Available configure presets:` followed by `"dev"`, `"tests"`, `"asan"` and `"dev-asan"` with their display names.

Run: `cmake --preset linux-tests; echo "exit $?"`
Expected (FAIL): `CMake Error: No such preset in D:/MorpheusEngine: "linux-tests"` and `exit 1`.

- [ ] **Step 2: Write the presets**

Replace the whole of `CMakePresets.json` with:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "displayName": "App + tests",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/dev",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "ON",
        "MORPHEUS_BUILD_TESTS": "ON",
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_GPU_BACKEND": "Vulkan"
      }
    },
    {
      "name": "tests",
      "displayName": "Engine libraries + tests only (no llama.cpp or SDL3)",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_BUILD_LLAMA": "OFF"
      }
    },
    {
      "name": "asan",
      "displayName": "Tests under AddressSanitizer",
      "inherits": "tests",
      "binaryDir": "${sourceDir}/build/asan",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_ASAN": "ON"
      }
    },
    {
      "name": "dev-asan",
      "displayName": "llama.cpp on the CPU + tests under AddressSanitizer",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/dev-asan",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "cacheVariables": {
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_GPU_BACKEND": "CPU",
        "MORPHEUS_ASAN": "ON",
        "GGML_OPENMP": "OFF"
      }
    },
    {
      "name": "linux-tests",
      "displayName": "Linux: engine libraries + tests only (no llama.cpp or SDL3)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++",
        "MORPHEUS_BUILD_APP": "OFF",
        "MORPHEUS_BUILD_LLAMA": "OFF",
        "MORPHEUS_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "linux-release",
      "displayName": "Linux: app + tools + tests, llama.cpp with CUDA (needs the CUDA toolkit)",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++",
        "MORPHEUS_BUILD_APP": "ON",
        "MORPHEUS_BUILD_LLAMA": "ON",
        "MORPHEUS_BUILD_TESTS": "ON",
        "MORPHEUS_GPU_BACKEND": "CUDA",
        "CMAKE_CUDA_ARCHITECTURES": "native"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "tests",
      "configurePreset": "tests",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "dev-asan",
      "configurePreset": "dev-asan",
      "configuration": "RelWithDebInfo",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" }
    },
    {
      "name": "linux-tests",
      "configurePreset": "linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" }
    },
    {
      "name": "linux-release",
      "configurePreset": "linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" }
    }
  ],
  "testPresets": [
    {
      "name": "dev",
      "configurePreset": "dev",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "tests",
      "configurePreset": "tests",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "configuration": "Debug",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "dev-asan",
      "configurePreset": "dev-asan",
      "configuration": "RelWithDebInfo",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "linux-tests",
      "configurePreset": "linux-tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    },
    {
      "name": "linux-release",
      "configurePreset": "linux-release",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error" }
    }
  ]
}
```

- [ ] **Step 3: Check the presets on Windows**

Run: `cmake --list-presets=all`
Expected: the file parses (no `CMake Error`); the configure, build and test lists each show exactly `"dev"`, `"tests"`, `"asan"` and `"dev-asan"`, and no `linux-` preset.

Run: `cmake --preset linux-tests; echo "exit $?"`
Expected (PASS: the preset exists but is disabled here): `CMake Error: Could not use disabled preset "linux-tests"` and `exit 1`.

Run: `cmake --preset tests`
Expected: configures as before (`-- Generating done`).

- [ ] **Step 4: Compile the Windows-only test only on Windows**

In `tests/app/SettingsEditTests.cpp`, replace

```cpp
TEST_CASE("SaveSettingsEdit reports a file it cannot replace and leaves no check file", "[settings_edit]")
{
```

with

```cpp
// Windows only: the test needs a replace that fails, and only Windows refuses to replace a file that another handle
// holds open without delete sharing. On Linux the rename replaces the file regardless, so the save succeeds and there
// is no failure to report.
#ifdef _WIN32
TEST_CASE("SaveSettingsEdit reports a file it cannot replace and leaves no check file", "[settings_edit]")
{
```

and replace

```cpp
    holder.close();
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}
```

with

```cpp
    holder.close();
    CHECK(test::ReadFile(file) == before);
    CHECK_FALSE(std::filesystem::exists(dir / "settings.json.new"));
}
#endif
```

(That second block occurs once: it is the end of this test case, just before `TEST_CASE("ReadSettingsEdit names the file when it cannot be read", ...)`.)

- [ ] **Step 5: The test still runs on Windows**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[settings_edit]"`
Expected: `All tests passed (<n> assertions in 7 test cases)`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 315`. (On Linux the same suite has 314; that is the owner's check in Task 4.)

- [ ] **Step 6: Commit**

```bash
git add CMakePresets.json tests/app/SettingsEditTests.cpp
git commit -m "Add the linux-tests and linux-release presets" -m "Each OS lists only its own presets. The test that relies on Windows refusing to replace an open file is compiled only on Windows." -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

