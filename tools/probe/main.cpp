// morpheus_probe: loads settings.json and a world, assembles a resolve-style prompt, and streams two
// completions, printing token usage and KV-cache reuse. A manual check that the model, the Vulkan backend and
// prefix caching work on this machine.
//
// Usage: morpheus_probe <settings.json> <world folder> "<player input>"

#include <exception>
#include <format>
#include <iostream>
#include <string_view>

#include "context/PromptBuilder.h"
#include "core/Settings.h"
#include "core/World.h"
#include "llama/LlamaCppBackend.h"

using namespace morpheus;

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: morpheus_probe <settings.json> <world folder> \"<player input>\"\n";
        return 2;
    }
    try {
        const core::Settings settings = core::LoadSettings(argv[1]);
        const core::World world = core::LoadWorld(argv[2]);

        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend backend(config);

        // A plain-prose stand-in for the resolve call: the real engine rules and grammar arrive in milestone 3.
        context::ContextInputs inputs;
        inputs.engineRules = "You are the game master of a solo text RPG. Reply in plain prose.";
        inputs.state = world.initialState;
        inputs.playerInput = argv[3];
        inputs.instruction = "Narrate what happens next in two or three sentences.";
        const context::Assembly assembly = context::AssembleResolvePrompt(world, inputs, settings.budget, backend);
        std::cout << std::format("prompt: {} tokens (engine rules {}, world {}, tail {}; transcript {} of {} budget); fits: {}\n",
                                 assembly.tokens.total, assembly.tokens.engineRules, assembly.tokens.world,
                                 assembly.tokens.tail, assembly.transcriptCost, assembly.transcriptBudget,
                                 assembly.Fits() ? "yes" : "no");

        inference::GenerationSpec spec;
        spec.maxTokens = settings.budget.narrateMaxTokens;
        spec.sampling.temperature = static_cast<float>(settings.narrateSampling.temperature);
        spec.sampling.topK = settings.narrateSampling.topK;
        spec.sampling.topP = static_cast<float>(settings.narrateSampling.topP);
        spec.sampling.seed = 1234;

        // The second run should reuse almost the whole prompt from the KV cache.
        for (int run = 1; run <= 2; ++run) {
            std::cout << "\n--- run " << run << " ---\n";
            const inference::Completion completion = backend.Complete(
                assembly.prompt, spec, [](std::string_view piece) { std::cout << piece << std::flush; }, {});
            std::cout << std::format("\n[prompt {} tokens: {} reused, {} evaluated in {:.0f} ms; {} generated in {:.0f} ms]\n",
                                     completion.promptTokens, completion.tokensReused, completion.tokensEvaluated,
                                     completion.promptMs, completion.tokensGenerated, completion.generateMs);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
