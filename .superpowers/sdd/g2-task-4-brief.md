### Task 4: The backends use the thinking format

**Files:**
- Modify: `src/inference/Llm.h` (remove the `THINKING_*` constants and their comment), `src/llama/LlamaCppBackend.h`, `src/llama/LlamaCppBackend.cpp`, `src/llamaserver/LlamaServerBackend.h`, `src/llamaserver/LlamaServerBackend.cpp`, `src/app/main.cpp`, `tools/server/main.cpp`, `tools/probe/main.cpp`, `tools/play/main.cpp`, `src/app/model/EngineHost.cpp`, `README.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`
- Test: `tests/llamaserver/LlamaServerBackendTests.cpp`, `tests/llamaserver/LlamaServerCompleteTests.cpp`, `tests/app/EngineHostTests.cpp`, `tests/llama/*` that name the constants (grep)

**Interfaces:**
- Consumes: `core::ThinkingSetting`, `core::ThinkingFormat`, `core::QWEN_THINKING`, `core::ResolveThinking` (Task 3); `core::Settings::thinking`.
- Produces: `llama::LlamaConfig::thinking` and `llamaserver::LlamaServerConfig::thinking` (`core::ThinkingSetting`, default Auto).

- [ ] **Step 1: Write the failing tests**

Every test that names `inference::THINKING_OPEN_TEXT`, `THINKING_SKIP_TEXT`, `THINKING_END_TEXT`, `THINKING_AFTER_END_TEXT` or `CLOSE_THINKING_TEXT` uses `core::QWEN_THINKING.open`, `.skip`, `.end`, `.afterEnd`, `.close` instead (grep `tests/`; include `core/Thinking.h`).

`tests/llamaserver/LlamaServerBackendTests.cpp`, append:

```cpp
TEST_CASE("LlamaServerBackend writes the thinking block its setting names", "[llamaserver][thinking]")
{
    test::FakeLlamaServer embed;
    // A custom format on a template without the switch: the prompt ends with its skip text, and enable_thinking is sent.
    {
        test::FakeLlamaServer chat;
        llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
        config.thinking = {core::ThinkingMode::Format, {"[T]", "[T][/T]\n", "[/T]", "\n", "Answer now.[/T]\n"}};
        llamaserver::LlamaServerBackend backend(config);
        const std::size_t skipTokens = test::FakeLlamaServer::DefaultTokens("[T][/T]\n", false, true).size();
        CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) ==
              static_cast<int>(test::ServerPromptTokens().size() + skipTokens));
        CHECK(chat.RequestsTo("/apply-template").back().body.at("chat_template_kwargs") ==
              nlohmann::json{{"enable_thinking", true}});
    }
    // "none" on a template with the switch: no block, and no enable_thinking.
    {
        test::FakeLlamaServer chat;
        chat.SetProps(test::ThinkingProps());
        llamaserver::LlamaServerConfig config = test::ConfigFor(chat, embed);
        config.thinking = {core::ThinkingMode::None, {}};
        llamaserver::LlamaServerBackend backend(config);
        CHECK(backend.CountPromptTokens(test::SERVER_PROMPT) == static_cast<int>(test::ServerPromptTokens().size()));
        CHECK_FALSE(chat.RequestsTo("/apply-template").back().body.contains("chat_template_kwargs"));
    }
}
```

`tests/llamaserver/LlamaServerCompleteTests.cpp`, append a case modelled on "LlamaServerBackend::Complete thinks in a first call, then answers in a second that extends it", with the custom format `{"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"}` set in the config on a template without the switch: the first request's prompt ends with the tokens of `"[T]"`, its `stop` is `["[/T]"]`, and its `n_predict` is the budget less the tokens of `"Answer now.[/T]\n"`; copy that case's body and swap the format's texts in.

The live thinking tests (grep `thinks before it answers` in `tests/llama/` and `tests/llamaserver/LlamaServerLiveTests.cpp`): after the existing check with the default (Auto) setting, run the same call once more on a backend whose config has `thinking = {core::ThinkingMode::Format, core::QWEN_THINKING}`, with the same expectations (they skip without the models or servers; they must compile in `tests` and `dev`).

`tests/app/EngineHostTests.cpp`, in "EngineHost rebuilds the backend when the backend kind, a server URL or the slot changes" (or a new case beside it): a settings copy that differs only in `thinking` (`{core::ThinkingMode::None, {}}` with the budgets at 0) makes the host load a new backend (count the factory calls as that test does).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`thinking` is not a config member; the `inference::THINKING_*` names still exist but the tests no longer use them, so the failure is the config).

- [ ] **Step 3: Implement**

`src/inference/Llm.h`: delete the five `THINKING_*`/`CLOSE_THINKING_TEXT` constants and their comment (the comment's explanation moves to `core/Thinking.h`, Task 3).

`src/llama/LlamaCppBackend.h`: `LlamaConfig` gains `core::ThinkingSetting thinking = {}; // settings.json's thinking_format; Auto reads the chat template.`; the class comment says the block's texts come from the resolved format (Auto: Qwen's when the template has the enable_thinking switch); `ThinkingMarkers`' member comments name the format's fields instead of the constants.

`src/llama/LlamaCppBackend.cpp`, the constructor's thinking block becomes:

```cpp
    // The thinking format: settings.json's, Auto reading the chat template's enable_thinking switch. Its end marker
    // must be a single token, or the block's end could not be recognised while generating; the other texts may be
    // any tokens.
    const char* pTemplate = llama_model_chat_template(_pChatModel.get(), nullptr);
    if (const std::optional<core::ThinkingFormat> format =
            core::ResolveThinking(_config.thinking, pTemplate != nullptr ? std::string_view(pTemplate) : std::string_view())) {
        const llama_vocab* pVocab = llama_model_get_vocab(_pChatModel.get());
        const std::vector<std::int32_t> end = Tokenize(pVocab, format->end, false, true);
        if (end.size() != 1) {
            throw InferenceError(std::format("the thinking format's end marker '{}' is not a single token of '{}'",
                                             format->end, core::PathText(_config.chatModel)));
        }
        ThinkingMarkers markers;
        markers.open = Tokenize(pVocab, format->open, false, true);
        markers.skip = Tokenize(pVocab, format->skip, false, true);
        markers.close = Tokenize(pVocab, format->close, false, true);
        markers.afterEnd = Tokenize(pVocab, format->afterEnd, false, false);
        markers.endToken = end.front();
        _thinking = std::move(markers);
    }
```

(`Tokenize` of an empty skip must give an empty vector; check it does, or skip the call when `format->skip` is empty.)

`src/llamaserver/LlamaServerBackend.h`: `LlamaServerConfig` gains `core::ThinkingSetting thinking = {};` (same comment); the class and `ThinkingMarkers` comments name the format's fields.

`src/llamaserver/LlamaServerBackend.cpp`: the `if (chatTemplate.find("enable_thinking") ...)` block becomes:

```cpp
    // The thinking format: settings.json's, Auto reading the chat template's enable_thinking switch. Its texts are
    // tokenized once as the in-process backend tokenizes them: special tokens parsed, the text after the end marker
    // as plain text.
    if (const std::optional<core::ThinkingFormat> format = core::ResolveThinking(_config.thinking, chatTemplate)) {
        ThinkingMarkers markers;
        markers.open = ServerTokens(format->open, false, true);
        markers.skip = format->skip.empty() ? std::vector<std::int32_t>() : ServerTokens(format->skip, false, true);
        markers.close = ServerTokens(format->close, false, true);
        markers.end = ServerTokens(format->end, false, true);
        markers.afterEnd = ServerTokens(format->afterEnd, false, false);
        markers.endText = format->end;
        _thinking = std::move(markers);
    }
```

`ThinkingMarkers` gains `std::string endText = {}; // The end marker's text: the thinking request's stop string.`, and the thinking request's `thinking["stop"]` uses `_thinking->endText` instead of `inference::THINKING_END_TEXT`. The `/apply-template` request keeps sending `chat_template_kwargs {"enable_thinking": true}` when `_thinking` is set. Remove every other use of the `inference::THINKING_*` names in both backends.

`src/app/main.cpp` (`MakeLlamaBackend`, `MakeServerBackend`), `tools/server/main.cpp`, `tools/probe/main.cpp`, `tools/play/main.cpp`: each config built from settings gets `config.thinking = settings.thinking;`.

`src/app/model/EngineHost.cpp`, `sameBackend`: add `&& settings.thinking == _settings.thinking` (the backend resolves the format when it loads); its comment names the thinking format.

`README.md`: the settings section describes `thinking_format` (its four forms, what each text is, the checks, the in-process single-token rule for `end`); the `morpheus_server` section recommends `resolve_thinking_tokens` and `narrate_thinking_tokens` 0 for hosted dialogue (a thinking block delays the first spoken sentence). The architecture spec's backend section: one sentence that the thinking block's texts are settings data (`thinking_format`, `core/Thinking.h`).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`, then `ctest --preset tests`; then `cmake --build build/dev --config RelWithDebInfo` (every dev target: the app, the tools and the llama tests compile).
Expected: all pass; the dev build is clean.

- [ ] **Step 5: Commit**

```bash
git add src/inference/Llm.h src/llama/LlamaCppBackend.h src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp src/app/main.cpp tools/server/main.cpp tools/probe/main.cpp tools/play/main.cpp src/app/model/EngineHost.cpp README.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md tests/llamaserver/LlamaServerBackendTests.cpp tests/llamaserver/LlamaServerCompleteTests.cpp tests/app/EngineHostTests.cpp
git commit -m "Backends: the thinking block's texts from the settings' thinking format"
```

(Add any other test file you changed for the constants.)

---

## Finally (controller)

- `ctest --preset tests`; `cmake --build --preset asan` and `ctest --preset asan`.
- `cmake --build build/dev --config RelWithDebInfo` builds clean; the consumer check is ok.
- Live: `[llama]` in-process (incl. `[llama][hosted]` and the thinking test) and `[server]` over llama-server, with `min_p` 0.05 in the live test settings for one run; the thinking tests with `thinking_format` "qwen" as well as auto.
