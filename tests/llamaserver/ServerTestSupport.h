#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/FakeLlamaServer.h"
#include "inference/Llm.h"
#include "llamaserver/LlamaServerBackend.h"

namespace morpheus::test {

// A short prompt, and the text of the fake's default chatml template around its two contents.
inline const inference::Prompt SERVER_PROMPT = {{{inference::Role::System, "Be brief."}, {inference::Role::User, "Hi"}}, 1};
inline const std::vector<std::string> SERVER_TEMPLATE_PIECES = {
    "<|im_start|>system\n", "<|im_end|>\n<|im_start|>user\n", "<|im_end|>\n<|im_start|>assistant\n"};

// SERVER_PROMPT's tokens as LlamaServerBackend makes them through the fake's default tokenizer: each template piece
// with special tokens parsed, each content as plain text, in order (the default tokenizer adds no BOS).
inline std::vector<int> ServerPromptTokens()
{
    std::vector<int> tokens;
    auto append = [&tokens](const std::vector<int>& more) { tokens.insert(tokens.end(), more.begin(), more.end()); };
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[0], false, true));
    append(FakeLlamaServer::DefaultTokens("Be brief.", false, false));
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[1], false, true));
    append(FakeLlamaServer::DefaultTokens("Hi", false, false));
    append(FakeLlamaServer::DefaultTokens(SERVER_TEMPLATE_PIECES[2], false, true));
    return tokens;
}

// A backend config on `chat` and `embed`: slot 1, a context of 8192 tokens (the fakes hold 16384 per slot), and
// nomic-embed's prefixes.
inline llamaserver::LlamaServerConfig ConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)
{
    llamaserver::LlamaServerConfig config;
    config.chatUrl = chat.Url();
    config.embeddingUrl = embed.Url();
    config.slot = 1;
    config.contextSize = 8192;
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    return config;
}

// /props of a chat server whose template has the enable_thinking switch, as a Qwen3.5 server's has.
inline nlohmann::json ThinkingProps()
{
    return {{"default_generation_settings", {{"n_ctx", 16384}}},
            {"total_slots", 3},
            {"chat_template",
             "{%- if enable_thinking is defined and enable_thinking is false %}{{- '<think>\\n\\n</think>\\n\\n' }}{%- endif %}"},
            {"model_path", "/models/Qwen3.5-0.8B-Q4_K_M.gguf"}};
}

} // namespace morpheus::test
