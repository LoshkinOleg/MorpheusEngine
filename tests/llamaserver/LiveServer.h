#pragma once

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "TestEnvironment.h"
#include "llamaserver/LlamaServerBackend.h"

namespace morpheus::test {

// The [server] tests run against a llama-server the owner starts (README, "The [server] tests"):
//   MORPHEUS_TEST_SERVER_URL        the chat server, e.g. http://127.0.0.1:8080, started with --parallel 3 or more and
//                                   at least LIVE_CONTEXT_SIZE tokens per slot
//   MORPHEUS_TEST_EMBED_SERVER_URL  the embedding server (--embedding), e.g. http://127.0.0.1:8081
// The calling test is skipped when either is unset.
constexpr int LIVE_CONTEXT_SIZE = 16384;

// A config for `slot` of the owner's servers, with nomic-embed's prefixes. Skips the calling test when a variable is
// unset; call it on the test's own thread.
inline llamaserver::LlamaServerConfig LiveServerConfig(int slot)
{
    const std::string chatUrl = EnvironmentVariable("MORPHEUS_TEST_SERVER_URL");
    const std::string embeddingUrl = EnvironmentVariable("MORPHEUS_TEST_EMBED_SERVER_URL");
    if (chatUrl.empty() || embeddingUrl.empty()) {
        SKIP("set MORPHEUS_TEST_SERVER_URL and MORPHEUS_TEST_EMBED_SERVER_URL to run the [server] tests");
    }
    llamaserver::LlamaServerConfig config;
    config.chatUrl = chatUrl;
    config.embeddingUrl = embeddingUrl;
    config.slot = slot;
    config.contextSize = LIVE_CONTEXT_SIZE;
    config.embeddingQueryPrefix = "search_query: ";
    config.embeddingDocumentPrefix = "search_document: ";
    return config;
}

} // namespace morpheus::test
