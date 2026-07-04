#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

#include "llama.h"

// TODO: direct cout, cwarn and cerr to a dedicated logger rather than just console, currently it's a pain to copy paste logs.
// TODO: actually run inference with user input
// TODO: move model loading and inference to a separate thread to avoid freezing the UI
/*
	TODO: use qwen's chat template:

	Worth pairing with: on Qwen3-Instruct you should apply the chat template (llama_chat_apply_template) rather than prepending "You: " / "Dungeon Master: " yourself — the model was trained on <|im_start|>user … <|im_end|> style delimiters and behaves noticeably better with them. Your TODO about stripping "You: " before tokenizing is pointing at the same problem.
*/

class App {
protected:
	SDL_Window* _pWindow = nullptr;
	SDL_Renderer* _pRenderer = nullptr;
	ImGuiContext* _pImguiContext = nullptr;
	llama_model* _pModel = nullptr;
	llama_context* _pLlamaContext = nullptr;
	llama_sampler* _pSampler = nullptr;
	bool _running = false;
	std::vector<std::string> _messages = std::vector<std::string>(); // C++ refresher: std::vector is already on the heap, no management needed.
	char _input_buffer[512] = "\0"; // Imgui API works with char arrays so we'll go with it as well even if it technically doesn't enforce 8 bits on all platforms (does enforce 1 byte though, byte size can vary). C++ refresher: char[N] implicitly converts to char* but char* =/= char[]
public:
	App() {}
	~App() {
		// Fallback for things we can shutdown.
		
		// llama
		if (_pSampler) llama_sampler_free(_pSampler);
		_pSampler = nullptr;
		if (_pLlamaContext) llama_free(_pLlamaContext);
		_pLlamaContext = nullptr;
		if (_pModel) llama_model_free(_pModel);
		_pModel = nullptr;

		// imgui
		if (_pImguiContext) ImGui::DestroyContext(_pImguiContext);
		_pImguiContext = nullptr;

		// sdl
		if (_pRenderer) SDL_DestroyRenderer(_pRenderer);
		_pRenderer = nullptr;
		if (_pWindow) SDL_DestroyWindow(_pWindow);
		_pWindow = nullptr;
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
			_pImguiContext = ImGui::CreateContext();
			if (!_pImguiContext) throw std::runtime_error("Failed to create imgui context."); // Drop the returned ptr, one context anyways.

			// This order is important.
			if (!ImGui_ImplSDL3_InitForSDLRenderer(_pWindow, _pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDL3_InitForSDLRenderer()");
			if (!ImGui_ImplSDLRenderer3_Init(_pRenderer)) throw std::runtime_error("Failed to ImGui_ImplSDLRenderer3_Init()");

			// llama
			llama_backend_init();

			ggml_backend_load_all(); // Enables a bunch of backends for inference. Technically possible to enable only those we want but it's cumbersome.
			
			// Load model
			auto modelParams = llama_model_default_params();
			modelParams.n_gpu_layers = 0; // Full CPU for now.
			modelParams.use_mlock = true; // Force keep model in ram, don't evict back to disk.
			modelParams.progress_callback = &App::ProcessLlamaModelLoading;
			modelParams.progress_callback_user_data = this;
			_pModel = llama_model_load_from_file(MORPHEUS_MODELS_DIR "/4b-instruct-2507-q4_K_M.gguf", modelParams);
			if (!_pModel) throw std::runtime_error("Failed to load model.");
			
			// Init llama context
			auto ctxParams = llama_context_default_params();
			ctxParams.n_ctx = 8192; // Context length.
			ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED; // Optimization.
			ctxParams.type_k = GGML_TYPE_Q8_0; // Quantize to reduce memory footprint of KV cache.
			ctxParams.type_v = GGML_TYPE_Q8_0;
			ctxParams.n_seq_max = 1; // One sequence at a time: sequence ~= one inference call at a time.
			ctxParams.abort_callback = &App::ProcessLlamaModelGeneration; // Apparently only works for CPU only inference.
			ctxParams.abort_callback_data = this;
			_pLlamaContext = llama_init_from_model(_pModel, ctxParams);
			if (!_pLlamaContext) throw std::runtime_error("Failed to create llama context.");
			
			// Init llama sampler. Note: different samplers can be used on the same model for different tasks.
			auto samplerParams = llama_sampler_chain_default_params();
			_pSampler = llama_sampler_chain_init(samplerParams);
			if (!_pSampler) throw std::runtime_error("Failed to create llama sampler.");
			// TODO: look into what sampler setup works best.
			// Note: don't manage the ptr resulting from llama_sampler_init_... since it's managed by llama when passed to llama_sampler_chain_add().
			llama_sampler_chain_add(_pSampler, llama_sampler_init_top_k(40)); // Keep only the 40 most fitting tokens.
			llama_sampler_chain_add(_pSampler, llama_sampler_init_top_p(0.9f, 1)); // Group the remaining candidates into groups and drop the least likely ones.
			llama_sampler_chain_add(_pSampler, llama_sampler_init_temp(0.8f)); // <1: Favor the most likely tokens. >1: more randomness. 0: always pick the most likely.
			llama_sampler_chain_add(_pSampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED)); // LLAMA_DEFAULT_SEED is random every run. Set to something if you want reproducibility.

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
			llama_sampler_free(_pSampler);
			_pSampler = nullptr;
			llama_free(_pLlamaContext);
			_pLlamaContext = nullptr;
			llama_model_free(_pModel);
			_pModel = nullptr;
			llama_backend_free();
			ImGui_ImplSDLRenderer3_Shutdown();
			ImGui_ImplSDL3_Shutdown();
			ImGui::DestroyContext(_pImguiContext); // Default context assumed.
			_pImguiContext = nullptr;
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
		// Convention: public facing names are PascalCase, internal id's are ##PascalCase
		ImGui::Begin("Chat");

			// _scrollArea
			ImGui::BeginChild("_scrollArea", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()), 0, 0); // Note: size is in logical imgui pixels (=/= actual pixels depending on scaling!).
			for (std::string& msg : _messages)
			{
				// Note: TextWrapped ensures that any overly long text gets sent to a new line.
				ImGui::TextWrapped(msg.c_str());
			}
			ImGui::EndChild();

			// ##InputField
			ImGui::SetNextItemWidth(-1); // -1 means use all remaining width.
			if (ImGui::InputText("##InputField", _input_buffer, 512, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll))
			{
				// True -> input submitted (via enter)
				if (IsValidInput(_input_buffer))
				{
					// Add the user's input to the _messages.
					_messages.push_back(std::string("You: ") + _input_buffer);
					_input_buffer[0] = '\0'; // A C string that starts with a this char is considered empty.

					// Prompt the LLM.
					auto reply = GenerateReply();
					_messages.push_back("Dungeon Master: " + reply);
				}
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
	std::string GenerateReply()
	{
		std::string reply = "";
		const llama_vocab* pVocab = llama_model_get_vocab(_pModel); // Const because llama owns the resource.

		// TODO: Right now, the last message is guaranteed to be from the user. This will change so add a mechanism to actually find the last user message.
		// TODO: prompt is what will need to be compiled for a memGPT style system. Q: are system instructions supposed to be part of this or is there some other infra for this?
		std::string prompt = _messages.back(); // Last message ref.
		prompt = prompt.substr(std::string("You: ").size()); // Trim the "You: "
		llama_chat_message promptMsg = {"user", prompt.c_str()};
		std::vector<char> templatedPrompt(8192); // TODO: make configurable or use the recommended size
		auto templatedNrOfBytes = llama_chat_apply_template(
			nullptr /*means use model's built in template*/,
			&promptMsg,
			1 /*one message*/,
			true /*make prompt end with <|im_start|>assistant */,
			templatedPrompt.data(),
			templatedPrompt.size()
		);
		if (templatedNrOfBytes < 0 || templatedNrOfBytes > templatedPrompt.size()) throw std::runtime_error("GenerateReply(): templatization failed.");
		prompt = std::string(templatedPrompt.data(), templatedPrompt.size()); // TODO: clean up all of these string allocations.

		// TODO: make a logger for this kind of thing.
		std::cout << "Templated prompt:\n" << prompt << std::endl;

		// TODO: implement prefix caching instead of nuking potentially useful cache on every call. Note: "If a turn ever diverges partway (e.g. edit/retry), you use llama_memory_seq_rm to drop tokens from position N onward and resubmit from there."
		// We're clearing the KV cache between GenerateReply() calls because we can't assume the new prompt matches the old prompt. Note that we don't clear the KV cache between each generation step, only between GenerateReply() calls.
		// Note: llama calls the KV cache "memory" since it's the generic way of calling it, other models have other mechanisms.
		llama_memory_clear(llama_get_memory(_pLlamaContext), true);

		// Tokenize
		// Figure out how much tokens we need for the prompt. llama_tokenize() will return a negative count since there's not enough space in the output (which is set to nullptr and max size to 0).
		// We make two tokenize calls since that seems to be the only way to guarantee required vector size.
		int nrTokensRequiredForPrompt = -llama_tokenize(pVocab, prompt.c_str(), prompt.size()/*Narrowing conversion, who cares*/, nullptr, 0, false, true); // add_special is false since the template already took care of those.
		std::vector<llama_token> tokens(nrTokensRequiredForPrompt); // Easier to just allocate on each call rather than having it be part of App.
		if (llama_tokenize(pVocab, prompt.c_str(), prompt.size(), tokens.data(), nrTokensRequiredForPrompt, false, true) < 0) {
			throw std::runtime_error("GenerateReply(): tokenization failed.");
		}

		// Create a single batch. A prompt has many tokens, so the batch size is >1 here.
		llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size()); // TODO: apparently this function will be deprecated, says to avoid using it, only there to facilitate transition to the new batch api.

		// Generate.
		// TODO: ensure no infinite loops
		int generated = 0;
		llama_token next = 0;
		while (generated < 512) // 512 tokens of output at most. // TODO: make configurable
		{
			// Decode = run the model forwards on a batch and update KV cache.
			int result = llama_decode(_pLlamaContext, batch);
			// 0 is success.
			if (result != 0) throw std::runtime_error("GenerateReply(): llama_decode() failed with the following output: " + std::to_string(result));

			// Sample an output token from the model.
			next = llama_sampler_sample(_pSampler, _pLlamaContext, -1); // -1 means pick the last logit - the one we've retained from the sampling process?
			if (llama_vocab_is_eog(pVocab, next))
			{
				generated++;
				break; // If llm returned an end of generation token has been emitted, stop generation.
			}

			// Convert token to readable text. Piece = string chunk that a token corresponds to.
			char piece[256]; // Array for holding the actual string that the token corresponds to.
			int pieceLen = llama_token_to_piece(pVocab, next, piece, sizeof(piece), 0, true); // special == true doesn't mean "interpret token as special", it means "render any special tokens as text".

			if (pieceLen > 0) {
				reply.append(piece, pieceLen);
			}

			llama_sampler_accept(_pSampler, next); // Lock in the generated token, needed depending on the sampler's setup.
			batch = llama_batch_get_one(&next, 1); // Generation is done one token at a time, hence the batch size of 1. Since generation is sequential, we can't have a batch size >1 because output[N+1] depends on output[N]. output[<N] is already in KV cache as well so len of 1 it is.
			generated++;
		}

		// TODO: detect if EOG token has not been emitted

		return reply;
	}

// Static methods and helpers.
	static bool IsValidInput(char input[512]) {
		if (input[0] == '\0') return false; // A C string that starts with a this char is considered empty.
		return true;
	}
	static bool ProcessLlamaModelLoading(float progress, void* usrData) {
		App* self = static_cast<App*>(usrData); // App instance.
		std::cout << "Progress: " << progress << std::endl;
		return true; // Returning false cancels the load apparently.
	}
	static bool ProcessLlamaModelGeneration(void* usrData) {
		App* self = static_cast<App*>(usrData); // App instance.
		return false; // Returning true aborts inference. Works only if inference is done on CPU apparently.
	}
};

int main()
{
	App a = App();
	return a.Run();
}