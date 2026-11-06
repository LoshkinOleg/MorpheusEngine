### Task 11: The app executable

This task replaces the prototype with `src/app/main.cpp`. The new app opens an SDL3 window with an SDL_Renderer, starts ImGui with docking, the Segoe UI font and a layout saved beside `settings.json`, and makes the llama.cpp backend through `AppController`. It draws `Ui` every frame. Visual Studio gets `morpheus` as its startup project, running from the repository root. `--frames N` exits after N frames, as a smoke check.

**Files:**
- Create: `src/app/main.cpp`
- Delete: `src/main.cpp`
- Modify: `CMakeLists.txt`, `src/app/CMakeLists.txt`, `.gitignore`, `README.md`, `CLAUDE.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

**Interfaces:**
- Consumes:
  - `AppController::PathsUnder`, `AppController(AppPaths, BackendFactory)` and `Ui::Draw`;
  - `llama::LlamaConfig` and `llama::LlamaCppBackend`;
  - `core::PathText`;
  - ImGui's SDL3 and SDL_Renderer backends.
- Produces: the `morpheus` executable, run as `morpheus [root] [--frames N]`.

- [ ] **Step 1: Write the app**

Create `src/app/main.cpp`:

```cpp
// morpheus: the app. An SDL3 window with Dear ImGui docking windows over the engine worker (architecture spec, "UI").
//
// Usage: morpheus [root] [--frames N]
// root holds settings.json, game_projects/ and saves/ (default: the current directory); Visual Studio starts the app
// from the repository root. --frames N exits after N frames: a smoke check that the window, the font, the ImGui
// backends and the settings start on this machine.

#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "app/model/AppController.h"
#include "app/ui/Ui.h"
#include "core/Text.h"
#include "llama/LlamaCppBackend.h"

using namespace morpheus;

namespace {

constexpr const char* WINDOW_TITLE = "Morpheus Engine";
constexpr int WINDOW_WIDTH = 1600; // Before display scaling.
constexpr int WINDOW_HEIGHT = 900;
constexpr float FONT_SIZE = 18.0f; // Before display scaling.
constexpr const char* USAGE = "usage: morpheus [root] [--frames N]\n";

struct WindowDeleter {
    void operator()(SDL_Window* pWindow) const
    {
        SDL_DestroyWindow(pWindow);
    }
};
struct RendererDeleter {
    void operator()(SDL_Renderer* pRenderer) const
    {
        SDL_DestroyRenderer(pRenderer);
    }
};
struct ImGuiContextDeleter {
    void operator()(ImGuiContext* pContext) const
    {
        ImGui::DestroyContext(pContext);
    }
};

// SDL's video subsystem: started by the constructor, shut down by the destructor.
class SdlSession {
public:
    SdlSession()
    {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
    }
    ~SdlSession()
    {
        SDL_Quit();
    }
    SdlSession(const SdlSession&) = delete;
    SdlSession& operator=(const SdlSession&) = delete;
};

// ImGui's SDL3 platform backend and SDL_Renderer backend, shut down in reverse order.
class ImGuiBackends {
public:
    ImGuiBackends(SDL_Window* pWindow, SDL_Renderer* pRenderer)
    {
        if (!ImGui_ImplSDL3_InitForSDLRenderer(pWindow, pRenderer)) {
            throw std::runtime_error("the ImGui SDL3 backend failed to start");
        }
        if (!ImGui_ImplSDLRenderer3_Init(pRenderer)) {
            ImGui_ImplSDL3_Shutdown();
            throw std::runtime_error("the ImGui SDL_Renderer backend failed to start");
        }
    }
    ~ImGuiBackends()
    {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }
    ImGuiBackends(const ImGuiBackends&) = delete;
    ImGuiBackends& operator=(const ImGuiBackends&) = delete;
};

// The llama.cpp backend for the engine's settings. Runs on EngineHost's loader thread.
std::unique_ptr<inference::LlmBackend> MakeLlamaBackend(const core::Settings& settings)
{
    llama::LlamaConfig config;
    config.chatModel = settings.chatModel;
    config.embeddingModel = settings.embeddingModel;
    config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
    config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
    config.contextSize = settings.contextSize;
    config.gpuLayers = settings.gpuLayers;
    return std::make_unique<llama::LlamaCppBackend>(config);
}

} // namespace

int main(int argc, char** argv)
{
    // Arguments: an optional root folder, and --frames N.
    std::optional<std::filesystem::path> rootArgument;
    std::optional<long long> frameLimit;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--frames" && i + 1 < argc) {
            const std::string_view text = argv[++i];
            long long frames = 0;
            const auto [pNext, error] = std::from_chars(text.data(), text.data() + text.size(), frames);
            if (error != std::errc() || pNext != text.data() + text.size() || frames < 1) {
                std::cerr << USAGE;
                return 2;
            }
            frameLimit = frames;
        } else if (!rootArgument && !argument.starts_with("--")) {
            rootArgument = std::filesystem::path(std::string(argument));
        } else {
            std::cerr << USAGE;
            return 2;
        }
    }

    try {
        const std::filesystem::path root = rootArgument.value_or(std::filesystem::current_path());

        // The window and its renderer, sized for the display's scale.
        const SdlSession sdl;
        const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (scale <= 0.0f) {
            throw std::runtime_error(std::string("SDL_GetDisplayContentScale failed: ") + SDL_GetError());
        }
        const std::unique_ptr<SDL_Window, WindowDeleter> pWindow(
            SDL_CreateWindow(WINDOW_TITLE, static_cast<int>(WINDOW_WIDTH * scale), static_cast<int>(WINDOW_HEIGHT * scale),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
        if (!pWindow) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }
        const std::unique_ptr<SDL_Renderer, RendererDeleter> pRenderer(SDL_CreateRenderer(pWindow.get(), nullptr));
        if (!pRenderer) {
            throw std::runtime_error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
        }
        SDL_SetRenderVSync(pRenderer.get(), 1); // Caps the frame rate at the display's.

        // ImGui. The layout is saved beside settings.json. ImGui keeps the file name's pointer until the context is
        // destroyed, so the string is declared first and outlives it.
        const std::string iniFile = core::PathText(root / "imgui.ini");
        IMGUI_CHECKVERSION();
        const std::unique_ptr<ImGuiContext, ImGuiContextDeleter> pImGui(ImGui::CreateContext());
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = iniFile.c_str();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;

        // Segoe UI covers the punctuation narration uses (dashes, curly quotes), which ImGui's built-in font lacks.
        const char* pWindowsDir = SDL_getenv("WINDIR");
        if (!pWindowsDir) {
            throw std::runtime_error("WINDIR is not set, so the Windows fonts folder cannot be found");
        }
        const std::filesystem::path fontFile = std::filesystem::path(pWindowsDir) / "Fonts" / "segoeui.ttf";
        if (!std::filesystem::exists(fontFile)) {
            throw std::runtime_error("font file " + core::PathText(fontFile) + " does not exist");
        }
        io.Fonts->AddFontFromFileTTF(core::PathText(fontFile).c_str(), FONT_SIZE);

        const ImGuiBackends backends(pWindow.get(), pRenderer.get());

        // The app: settings, worlds and the engine (its models load in the background while the UI draws).
        app::AppController controller(app::AppController::PathsUnder(root), MakeLlamaBackend);
        app::Ui ui;

        bool running = true;
        for (long long frame = 0; running && (!frameLimit || frame < *frameLimit); ++frame) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                     event.window.windowID == SDL_GetWindowID(pWindow.get()))) {
                    running = false;
                }
            }
            controller.Frame(); // Take the worker's events even while minimized.
            if ((SDL_GetWindowFlags(pWindow.get()) & SDL_WINDOW_MINIMIZED) != 0) {
                SDL_Delay(10); // Nothing to draw.
                continue;
            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ui.Draw(controller);
            ImGui::Render();
            SDL_SetRenderScale(pRenderer.get(), io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColorFloat(pRenderer.get(), 0.08f, 0.08f, 0.08f, 1.0f);
            SDL_RenderClear(pRenderer.get());
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), pRenderer.get());
            SDL_RenderPresent(pRenderer.get());
        }
        return 0; // Members unwind in reverse: the worker stops, then ImGui (saving the layout), then SDL.
    } catch (const std::exception& error) {
        // Startup failures (invalid settings.json, a missing font) and UI bugs end here, shown rather than swallowed.
        std::cerr << "error: " << error.what() << '\n';
        if (!frameLimit) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, WINDOW_TITLE, error.what(), nullptr);
        }
        return 1;
    }
}
```

- [ ] **Step 2: Wire it into the build**

In `src/app/CMakeLists.txt`, append:

```cmake
if(MORPHEUS_BUILD_APP)
    # The app: the SDL3 window, ImGui's SDL backends and the llama.cpp backend.
    add_executable(morpheus main.cpp)
    target_link_libraries(morpheus PRIVATE morpheus_app_ui morpheus_llama imgui_sdl3)
    morpheus_target_defaults(morpheus)
    # F5 in Visual Studio runs it from the repository root, where settings.json, game_projects/ and saves/ are.
    set_target_properties(morpheus PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
```

In the root `CMakeLists.txt`:
- Delete the whole `# ---- App (prototype, replaced in milestone 4)` block (`add_executable(morpheus src/main.cpp)` through its `endif()`). That removes the `MORPHEUS_MODELS_DIR` define with it.
- Change the option text to `option(MORPHEUS_BUILD_APP "Build the morpheus app (needs MORPHEUS_BUILD_LLAMA; fetches SDL3)" ON)`.
- Add `SYSTEM` to `FetchContent_Declare(sdl3 ...)`, after its `GIT_SHALLOW` line, with the comment `# Its headers are external: no /W4 /WX`. `main.cpp` includes SDL's headers under `/WX`.
- After the `add_subdirectory(src/app)` block from Task 3, add:

```cmake
if(MORPHEUS_BUILD_APP)
    # Visual Studio opens the solution with the app as the startup project.
    set_property(DIRECTORY ${PROJECT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT morpheus)
endif()
```

Delete the prototype: `git rm src/main.cpp`.

In `.gitignore`, add a line `/imgui.ini` after `/settings.json`.

- [ ] **Step 3: Build and run the smoke check**

Run: `cmake --preset dev && cmake --build --preset dev --config RelWithDebInfo --target morpheus`
Expected: `build/dev/bin/RelWithDebInfo/morpheus.exe` builds with no warnings (`/WX`). A warning from SDL's or ImGui's headers means their include directories are not `SYSTEM`; fix that in CMake, not with pragmas.

Run from the repository root (`settings.json` and the models are there): `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120; echo "exit $?"`
Expected:
- a window opens for about two seconds and closes;
- the command prints `exit 0` once the models finish loading (the app waits for the loader when it closes);
- `imgui.ini` now exists at the root and is ignored (`git status --short` does not list it).

Then run `./build/dev/bin/RelWithDebInfo/morpheus.exe nowhere --frames 1; echo "exit $?"`. It prints an error naming the missing `settings.json` under `nowhere`, then `exit 1`, with no message box (`--frames` mode).

Check the solution: `grep -c "morpheus" build/dev/MorpheusEngine.sln` finds the project. In `build/dev/src/app/morpheus.vcxproj`, `LocalDebuggerWorkingDirectory` is the repository root.

- [ ] **Step 4: Update the docs**

`README.md`:
- In the preset table, the `tests` row reads `Engine libraries and tests only (no llama.cpp or SDL3)`.
- Add this section before `## Play in the terminal`:

~~~markdown
## Play in the app

The `dev` preset builds `morpheus`, the app. In Visual Studio, open `build/dev/MorpheusEngine.sln`: `morpheus` is the startup project and runs from the repository root, so F5 (or Ctrl+F5) starts it. Pick the RelWithDebInfo configuration to play: Debug is about 2-4x slower. From a shell, run it from the repository root, or pass the root folder:

```
./build/dev/bin/RelWithDebInfo/morpheus.exe [root]
```

The root holds `settings.json`, `game_projects/` and `saves/`. The models load in the background while the window opens. The windows dock and can be rearranged; the layout is saved in `imgui.ini` at the root (gitignored), and deleting it restores the default.

- **Runs:** the worlds in `game_projects/` and each world's runs, saved as `saves/<world id>/<name>.db`. Start a new run (a name, and a seed or blank for a random one) or continue a saved one.
- **Play:** the story, with each turn's rolls and changes in grey under your input. Enter plays the turn (Ctrl+Enter for a new line); Undo, Regenerate (same dice, new text) and Cancel work on the latest turn.
- **State:** the world state, with what the latest turn changed highlighted.
- **Turn log:** each turn's intent, rolls, applied and rejected changes, notes, lookups and per-call token counts and timings, and the raw resolve output.
- **Context:** the latest resolve prompt by layer: tokens per layer, KV-cache reuse per call, and the text as sections or raw.
- **Memory:** the story summaries, the notes, and what retrieval put in the latest turn's prompt, with scores.
- **Settings:** model paths, context size, GPU layers, sampling and memory mode. Apply saves `settings.json` and restarts the engine, reopening the run; changing the models, context size or GPU layers reloads the models. The budget is shown and edited in `settings.json`.

`--frames N` exits after N frames, a quick check that the app starts on this machine.
~~~

- In `## Layout`, replace the `src/main.cpp` line with:

```markdown
- `src/app`: the app. `model/` is its state and actions (no SDL or ImGui), `ui/` its ImGui windows, `main.cpp` the SDL3 window (`dev` preset only)
```

`CLAUDE.md`:
- In "Build and test", the `dev` line ends `... the app (`morpheus`, the startup project in Visual Studio, run from the repo root), `morpheus_probe` and `morpheus_play`.`
- In "Layout", replace `- src/main.cpp: prototype app, replaced in milestone 4.` with:

```markdown
- src/app: the app. model/ (morpheus_app_model): its state and actions, no SDL, ImGui or llama.cpp. ui/ (morpheus_app_ui): the ImGui windows, needing only ImGui's core, so tests draw them headless (tests/app/HeadlessImGui.h). main.cpp (`morpheus`, `dev` preset only): the SDL3 window and the llama.cpp backend. ImGui is upstream's docking branch; IM_ASSERT throws (src/app/ui/ImGuiConfig.h).
```

- Change the module rule `Only this module, tools/probe and tools/play include <llama.h>.` (in the `src/llama` line) to `Only this module, tools/probe, tools/play and src/app/main.cpp link llama.cpp; only src/llama and the tools include <llama.h>.`

Architecture spec, "UI (ImGui docking windows)": add one row to the table and one paragraph below it:

```markdown
| Runs | Worlds in `game_projects/` and their runs in `saves/<world id>/`: start a new run or continue one |
```

```markdown
The windows dock over one SDL3 window (Dear ImGui's docking branch, SDL_Renderer backend). The UI thread draws from `app::AppController`, which applies the worker's events once per frame. The backend loads on a background thread, so the window opens at once. Settings apply by restarting the worker: the loaded models are kept unless the model paths, embedding prefixes, context size or GPU layers changed, and the open run reopens.
```

In the spec's "Process and modules" table, change the `app` row's responsibility to `SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings` and its dependencies to `turn`.

- [ ] **Step 5: Run every fast suite**

Run: `ctest --preset tests`, then `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: everything passes under both presets.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt src/app/main.cpp .gitignore README.md CLAUDE.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Replace the prototype with the SDL3 and ImGui app" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

(The `git rm src/main.cpp` from Step 2 is already staged.)

---

