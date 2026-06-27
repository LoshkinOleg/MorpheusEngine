#include <iostream>
#include <stdexcept>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

int main()
{
	SDL_Window* pWindow = nullptr;
	SDL_Renderer* pRenderer = nullptr;
	try
	{
		if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error("SDL_Init() failed.");

		pWindow = SDL_CreateWindow("Morpheus Engine", 1280, 720, 0);
		if (!pWindow) throw std::runtime_error("Failed to create SDL_Window.");

		pRenderer = SDL_CreateRenderer(pWindow, nullptr);
		if (!pRenderer) throw std::runtime_error("Failed to create SDL_Renderer.");

		IMGUI_CHECKVERSION(); // If this passes, context creation should pass.
		if (!ImGui::CreateContext()) throw std::runtime_error("Failed to create imgui context."); // Drop the returned ptr, one context anyways.

		// This order is important.
		if (!ImGui_ImplSDL3_InitForSDLRenderer(pWindow, pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDL3_InitForSDLRenderer()");
		if (!ImGui_ImplSDLRenderer3_Init(pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDLRenderer3_Init()");

		bool running = true;
		while (running)
		{
			// Process SDL events while there's any left for this frame.
			SDL_Event e;
			while (SDL_PollEvent(&e))
			{
				ImGui_ImplSDL3_ProcessEvent(&e);
				switch (e.type)
				{
				case SDL_EVENT_QUIT:
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
					running = false;
				}break;
				default:
					break;
				}
			}

			// Start new frame (order matters).
			ImGui_ImplSDLRenderer3_NewFrame(); // Inform back end there's a new frame.
			ImGui_ImplSDL3_NewFrame(); // Inform platform there's a new frame.
			ImGui::NewFrame(); // Inform ImGui there's a new frame.

			// Immediate mode UI.
			ImGui::Begin("Hello");
			ImGui::Text("Running yo");
			ImGui::End();
			// !Immediate mode UI.

			// Clear screen.
			ImGui::Render(); // Consolidates all the immediate mode UI. ImGui::GetDrawData() is up to date after this.
			SDL_SetRenderDrawColorFloat(pRenderer, 0.0f, 0.0f, 0.0f, 1.0f);
			SDL_RenderClear(pRenderer);

			// Draw the UI.
			ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), pRenderer); // Draw the immediate mode UI.

			// Present.
			SDL_RenderPresent(pRenderer);
		}

		// Shutdown.
		ImGui_ImplSDLRenderer3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(); // Default context assumed.
		SDL_DestroyRenderer(pRenderer);
		SDL_DestroyWindow(pWindow);
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