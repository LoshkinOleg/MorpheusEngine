#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>

#include "llama/LlamaCppBackend.h"

namespace morpheus::test {

// Reads an environment variable; "" when unset. _dupenv_s avoids MSVC's getenv deprecation warning under /WX.
inline std::string EnvironmentVariable(const char* name)
{
#ifdef _MSC_VER
    char* pValue = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&pValue, &length, name) != 0 || pValue == nullptr) {
        return {};
    }
    std::string value(pValue);
    std::free(pValue);
    return value;
#else
    const char* pValue = std::getenv(name);
    return pValue != nullptr ? pValue : "";
#endif
}

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
// with MORPHEUS_TEST_EMBED_MODEL. The calling test is skipped when either is unset. Shared like SharedBackend.
inline llama::LlamaCppBackend& SharedThinkingBackend()
{
    const std::string chatModel = EnvironmentVariable("MORPHEUS_TEST_THINKING_MODEL");
    const std::string embedModel = EnvironmentVariable("MORPHEUS_TEST_EMBED_MODEL");
    if (chatModel.empty() || embedModel.empty()) {
        SKIP("set MORPHEUS_TEST_THINKING_MODEL and MORPHEUS_TEST_EMBED_MODEL to run thinking tests");
    }
    static std::unique_ptr<llama::LlamaCppBackend> pBackend = [&] {
        llama::LlamaConfig config;
        config.chatModel = chatModel;
        config.embeddingModel = embedModel;
        config.embeddingQueryPrefix = "search_query: ";
        config.embeddingDocumentPrefix = "search_document: ";
        config.contextSize = 8192; // A resolve prompt, a thinking block and a long answer.
        config.gpuLayers = -1;
        return std::make_unique<llama::LlamaCppBackend>(config);
    }();
    return *pBackend;
}

} // namespace morpheus::test
