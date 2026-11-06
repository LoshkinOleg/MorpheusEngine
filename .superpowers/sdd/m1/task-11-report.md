# Task 11 report: milestone 1 verification

Status: DONE. No source or CMake changes were needed, so nothing was committed.

## Step 1: AddressSanitizer

- `cmake --preset asan`: configured OK (nlohmann, SQLite and Catch2 fetched).
- `cmake --build --preset asan`: built OK. The only message was the expected `LNK4300: ignoring '/INCREMENTAL' because input module contains ASAN metadata`.
- `ctest --preset asan`: `100% tests passed out of 57`.
- `grep -ci addresssanitizer` on the ctest output returned 0.
- Running `build/asan/bin/Debug/morpheus_tests.exe` directly: `All tests passed (898 assertions in 57 test cases)`.
- ASan was active:
  - `/fsanitize=address` is present in the generated vcxproj files.
  - `clang_rt.asan_dynamic-x86_64.dll` and `clang_rt.asan_dbg_dynamic-x86_64.dll` were copied next to `morpheus_tests.exe`.

## Step 2: dev preset

- `cmake --preset dev`: exit 0 (about 158 s). It fetched and configured llama.cpp, SDL3 and ImGui.
- `cmake --build --preset dev`: exit 0. It produced `build/dev/bin/Debug/morpheus.exe` and `morpheus_tests.exe`, along with `llama.dll`, `ggml*.dll`, `SDL3.dll` and the Catch2 DLLs.
- The only `src/main.cpp` messages were warnings (C4267 size_t to int32_t at lines 299, 301 and 306; C4189 unused `self` at lines 351 and 356). The brief says to leave these alone, and I did.
- The only "error" substring in the build log is the file name `SDL_error.c`. There were no compile or link errors.

## Step 3: ctest --preset dev

`100% tests passed out of 57`.

## Fixes

None.

## Concerns

None. `git status` is clean. The untracked `dotnet/` folder was ignored.
