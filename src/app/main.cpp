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
