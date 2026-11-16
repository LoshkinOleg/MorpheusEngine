#pragma once

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "TestEnvironment.h"
#include "core/Thinking.h"
#include "llama/LlamaCppBackend.h"

namespace morpheus::test {

// Model-backed tests need real GGUF files, named by environment variables:
//   MORPHEUS_TEST_CHAT_MODEL   chat model (Qwen3-4B-Instruct-2507-Q4_K_M.gguf)
//   MORPHEUS_TEST_EMBED_MODEL  embedding model (nomic-embed-text-v1.5.Q8_0.gguf)
// The calling test is skipped when either is unset. Loading is slow, so every test shares one backend.
inline llama::LlamaCppBackend& SharedBackend()
{
    const std::string chatModel = EnvironmentVariable("MORPHEUS_TEST_CHAT_MODEL");
    const std::string embedModel = EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL");
    if (chatModel.empty() || embedModel.empty()) {
        SKIP("set MORPHEUS_TEST_CHAT_MODEL and MORPHEUS_TEST_EMBED_MODEL to run model-backed tests");
    }
    static std::unique_ptr<llama::LlamaCppBackend> pBackend = [&] {
        llama::LlamaConfig config;
        config.chatModel = chatModel;
        config.embeddingModel = embedModel;
        config.embeddingQueryPrefix = "search_query: ";
        config.embeddingDocumentPrefix = "search_document: ";
        config.contextSize = 4096;
        config.gpuLayers = -1;
        return std::make_unique<llama::LlamaCppBackend>(config);
    }();
    return *pBackend;
}

// Thinking tests need a thinking chat model too (its chat template has enable_thinking, as Qwen3.5 and later do):
//   MORPHEUS_TEST_THINKING_MODEL  e.g. Qwen3.8-27B-UD-Q4_K_M.gguf, or a small Qwen3.5 for a quick run
// with MORPHEUS_TEST_EMBED_MODEL. The calling test is skipped when either is unset.
// Skips the calling test unless the thinking model and the embedding model are set. Call it outside any CHECK (a CHECK
// would catch the skip).
inline void RequireThinkingModel()
{
    if (EnvironmentVariable("MORPHEUS_TEST_THINKING_MODEL").empty() ||
        EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL").empty()) {
        SKIP("set MORPHEUS_TEST_THINKING_MODEL and MORPHEUS_TEST_EMBED_MODEL to run thinking tests");
    }
}

// A new backend on the thinking model with the given thinking setting (a test that loads its own setting). A test that
// only checks the setting at load, while the shared backend still holds the GPU, passes 0 gpuLayers (the model on the
// CPU) and a small contextSize so the load does not compete for GPU memory.
inline std::unique_ptr<llama::LlamaCppBackend> MakeThinkingBackend(
    const core::ThinkingSetting& thinking, int gpuLayers = -1, int contextSize = 8192)
{
    RequireThinkingModel();
    llama::LlamaConfig config;
    config.chatModel = EnvironmentVariable("MORPHEUS_TEST_THINKING_MODEL");
    config.embeddingModel = EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL");
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    config.contextSize = contextSize; // The default holds a resolve prompt, a thinking block and a long answer.
    config.gpuLayers = gpuLayers;
    config.thinking = thinking;
    return std::make_unique<llama::LlamaCppBackend>(config);
}

// The Auto-setting thinking backend, shared like SharedBackend.
inline llama::LlamaCppBackend& SharedThinkingBackend()
{
    static std::unique_ptr<llama::LlamaCppBackend> pBackend = MakeThinkingBackend({});
    return *pBackend;
}

} // namespace morpheus::test
