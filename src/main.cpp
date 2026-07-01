#include <iostream>
#include <stdexcept>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

class App {
protected:
	SDL_Window* _pWindow = nullptr;
	SDL_Renderer* _pRenderer = nullptr;
	ImGuiContext* _pContext = nullptr;
	bool _running = false;
	std::vector<std::string> _messages = std::vector<std::string>(); // C++ refresher: std::vector is already on the heap, no management needed.
	char _input_buffer[512] = {0}; // Imgui API works with char arrays so we'll go with it as well even if it technically doesn't enforce 8 bits on all platforms (does enforce 1 byte though, byte size can vary). C++ refresher: char[N] implicitly converts to char* but char* =/= char[]
public:
	App() {}
	~App() {
		// Fallback for things we can shutdown.
		if (_pRenderer) SDL_DestroyRenderer(_pRenderer);
		_pRenderer = nullptr;
		if (_pWindow) SDL_DestroyWindow(_pWindow);
		_pWindow = nullptr;
		if (_pContext) ImGui::DestroyContext(_pContext);
		_pContext = nullptr;
	}
	int Run() {
		try
		{
			if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error("SDL_Init() failed.");

			_pWindow = SDL_CreateWindow("Morpheus Engine", 1280, 720, 0);
			if (!_pWindow) throw std::runtime_error("Failed to create SDL_Window.");

			_pRenderer = SDL_CreateRenderer(_pWindow, nullptr);
			if (!_pRenderer) throw std::runtime_error("Failed to create SDL_Renderer.");

			IMGUI_CHECKVERSION(); // If this passes, context creation should pass.
			_pContext = ImGui::CreateContext();
			if (!_pContext) throw std::runtime_error("Failed to create imgui context."); // Drop the returned ptr, one context anyways.

			// This order is important.
			if (!ImGui_ImplSDL3_InitForSDLRenderer(_pWindow, _pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDL3_InitForSDLRenderer()");
			if (!ImGui_ImplSDLRenderer3_Init(_pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDLRenderer3_Init()");

			_running = true;
			while (_running)
			{
				// Process SDL events while there's any left for this frame.
				ProcessImguiEvents();

				// Start new frame (order matters).
				ImGui_ImplSDLRenderer3_NewFrame(); // Inform back end there's a new frame.
				ImGui_ImplSDL3_NewFrame(); // Inform platform there's a new frame.
				ImGui::NewFrame(); // Inform ImGui there's a new frame.

				// Immediate mode UI.
				DefineImmediateModeUi();

				// Clear screen.
				ImGui::Render(); // Consolidates all the immediate mode UI. ImGui::GetDrawData() is up to date after this.
				SDL_SetRenderDrawColorFloat(_pRenderer, 0.0f, 0.0f, 0.0f, 1.0f);
				SDL_RenderClear(_pRenderer);

				// Non UI rendering would go here.

				// Draw the UI.
				ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), _pRenderer); // Draw the immediate mode UI.

				// Present.
				SDL_RenderPresent(_pRenderer);
			}

			// Shutdown.
			ImGui_ImplSDLRenderer3_Shutdown();
			ImGui_ImplSDL3_Shutdown();
			ImGui::DestroyContext(_pContext); // Default context assumed.
			_pContext = nullptr;
			SDL_DestroyRenderer(_pRenderer);
			_pRenderer = nullptr;
			SDL_DestroyWindow(_pWindow);
			_pWindow = nullptr;
			SDL_Quit();
			return 0;
		}
		catch (const std::exception& e)
		{
			std::cerr << e.what() << std::endl;
			// TODO: possible resource leakage, track what has or hasn't successfully initialized and tear down accordingly.
			return 1;
		}
	}

protected:
	void DefineImmediateModeUi() {
		// Convention: public facing names are PascalCase, internal id's are _camelCase .
		ImGui::Begin("Chat");

			// _scrollArea
			// Q: BeginChild / EndChild defines a panel in the currently outer panel, right?
			ImGui::BeginChild("_scrollArea", ImVec2(256, 256), 0, 0); // Note: size is in logical imgui pixels (=/= actual pixels depending on scaling!).
			for (std::string& msg : _messages)
			{
				ImGui::TextWrapped(msg.c_str()); // Q: wrapped? Q: why %s, line.c_str() instead of just line.c_str()?
			}
			ImGui::EndChild();

			// _inputField
			// ImGui::BeginChild("_inputField"); // Q: why no BeginChild for the input part of the UI?

			ImGui::Separator(); // Q: diff with SeparatorText?
			ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll;
			if (ImGui::InputText("_inputField", _input_buffer, )) {

			}

		ImGui::End();
	}
	void ProcessImguiEvents() {
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			ImGui_ImplSDL3_ProcessEvent(&e);
			switch (e.type)
			{
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
				_running = false;
			}break;
			default:
				break;
			}
		}
	}
};

int main()
{
	App a = App();
	return a.Run();
}