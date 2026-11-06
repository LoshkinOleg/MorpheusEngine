## Global Constraints

- **Toolchain:** C++20 (`cxx_std_20`), MSVC through the `Visual Studio 17 2022` generator, x64, Windows 10. CMake >= 3.25.
- **Dependencies:**
  - ImGui moves to `https://github.com/ocornut/imgui.git` at `b61e56346a92cfcaf1f43a545ca37b0b32239654` (`v1.92.8-docking`).
  - SDL3 stays at `8e37db5e797b6167f3a00d697d816a684bd259c7` (3.4.10).
  - llama.cpp stays at `960d628f46d17ea9c10aa944b061b2341afeb3b5`.
  - Nothing else is added.
- **Flags:** every MorpheusEngine target calls `morpheus_target_defaults()` (`/W4 /WX /permissive- /Zc:preprocessor /utf-8`). Never apply these to fetched dependencies, ImGui's targets included.
- **Namespaces and targets:**
  - The namespaces are `morpheus::core`, `store`, `inference`, `context`, `llama`, `turn` and `app`.
  - Targets are `morpheus_<module>` (`morpheus_app_model`, `morpheus_app_ui`); the app executable stays `morpheus`.
  - Includes are written as `"module/File.h"` (`"app/model/RunModel.h"`, `"app/ui/Ui.h"`).
- **Error policy:**
  - Engine bugs throw `core::ContractError` (via `MORPHEUS_REQUIRE`).
  - Bad world, settings or saved data throws `core::DataError` or `store::StoreError`, naming the file and field.
  - Model and runtime failures throw `inference::InferenceError`.
  - A turn that cannot run as asked throws `turn::TurnError`; the worker reports it as an `Error` event, which the Play window shows.
  - The UI never swallows an exception. Startup failures end in an error box naming the cause.
- **Module boundaries:**
  - Only `morpheus_llama`, `tools/probe`, `tools/play` and `src/app/main.cpp` link llama.cpp.
  - Only `src/app/main.cpp` includes SDL or the ImGui backends.
  - `morpheus_app_model` includes no ImGui. `morpheus_app_model` and `morpheus_app_ui` build and test under the `tests` and `asan` presets.
- **Threading:**
  - The UI thread owns `AppController`, `RunModel`, `EngineHost` and all ImGui calls.
  - The worker thread is the only user of the backend once the worker exists.
  - `EngineHost`'s loader thread constructs the backend and hands it over under a mutex.
- **Coding rules:** follow `CLAUDE.md`. Source comments and docs are ASCII only, so write non-ASCII test text as `\x` escapes.
- **Shell:**
  - Commands run from the repository root in Git Bash.
  - Never stage `dotnet/`, `models/`, `settings.json` or `imgui.ini`.
- **Commit trailer:** end each commit message with `-m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"`, where `<Model>` is the implementing model's name.
- **Model-backed tests:** `[llama]` tests skip unless both variables are exported in the same shell command:

```bash
export MORPHEUS_TEST_CHAT_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
export MORPHEUS_TEST_EMBED_MODEL="C:/Users/user/Desktop/MorpheusEngine/models/nomic-embed-text-v1.5.Q8_0.gguf"
```

