### Task 3: LlamaServerBackend reads the chat server's slot count and model id

**Files:**
- Modify: `src/llamaserver/LlamaServerBackend.h` (members, two public methods)
- Modify: `src/llamaserver/LlamaServerBackend.cpp` (the constructor's chat `/props` read, a helper for model ids)
- Test: `tests/llamaserver/LlamaServerBackendTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int llamaserver::LlamaServerBackend::SlotCount() const` (the chat server's `/props` `total_slots`) and `std::string llamaserver::LlamaServerBackend::ChatModelId() const` (its `/props` `model_path` file name without the extension). The constructor throws `InferenceError` naming the chat URL when `total_slots` is below 1 or the model path names no file.

- [ ] **Step 1: Write the failing tests**

Append to `tests/llamaserver/LlamaServerBackendTests.cpp`:

```cpp
TEST_CASE("LlamaServerBackend reads the chat server's slot count and model id from /props", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    {
        // The fake's default /props: 3 slots, and a model file the fake names for both servers.
        const llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
        CHECK(backend.SlotCount() == 3);
        CHECK(backend.ChatModelId() == "nomic-embed-text-v1.5.Q8_0");
    }
    {
        chat.SetProps(test::ThinkingProps());
        const llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
        CHECK(backend.ChatModelId() == "Qwen3.5-0.8B-Q4_K_M");
    }
}

TEST_CASE("LlamaServerBackend refuses a chat server with no slot or no model file", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    SECTION("no slot")
    {
        nlohmann::json props = test::ThinkingProps();
        props["total_slots"] = 0;
        chat.SetProps(props);
        CHECK_THROWS_AS(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)), inference::InferenceError);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)),
                          ContainsSubstring(chat.Url()) && ContainsSubstring("total_slots"));
    }
    SECTION("no model file")
    {
        nlohmann::json props = test::ThinkingProps();
        props["model_path"] = "";
        chat.SetProps(props);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)),
                          ContainsSubstring(chat.Url()) && ContainsSubstring("model_path"));
    }
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`
Expected: a compile error, `'SlotCount': is not a member of 'morpheus::llamaserver::LlamaServerBackend'`.

- [ ] **Step 3: Add the members and methods**

In `src/llamaserver/LlamaServerBackend.h`, after `std::string _embeddingModelId = {};`:

```cpp
    int _slotCount = 0;                            // The chat server's slots (/props total_slots).
    std::string _chatModelId = {};                 // The chat server's model file name, without its extension.
```

After `std::string EmbeddingModelId() const override;`:

```cpp
    // The chat server's slots, from its /props total_slots: what morpheus_server shares between conversations.
    int SlotCount() const;
    // The chat server's model: its /props model_path's file name without the extension, as EmbeddingModelId.
    std::string ChatModelId() const;
```

In the constructor's comment, change "the chat server's context per slot, which must hold config.contextSize, and its chat template (whether the model thinks); the embedding server's model file." to "the chat server's context per slot, which must hold config.contextSize, its chat template (whether the model thinks), its slot count and its model file; the embedding server's model file." and add "a chat server with no slot," to the list of `InferenceError` causes.

- [ ] **Step 4: Read them in the constructor**

In `src/llamaserver/LlamaServerBackend.cpp`, in the anonymous namespace, add:

```cpp
// A model's id as Morpheus names it: the file name in a server's /props model_path, without the extension, as the
// in-process backend names its models; "" when the path names no file.
std::string ModelIdFromPath(const std::string& modelPath)
{
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    return dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
}
```

Replace the chat server's `/props` read:

```cpp
    const auto [slotContext, chatTemplate] =
        Ask(*_pChatClient, _config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
            return std::pair(props.at("default_generation_settings").at("n_ctx").get<int>(),
                             props.at("chat_template").get<std::string>());
        });
```

with:

```cpp
    struct ChatProps {
        int slotContext = 0;
        std::string chatTemplate = {};
        int slots = 0;
        std::string modelPath = {};
    };
    const ChatProps chat = Ask(*_pChatClient, _config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return ChatProps{props.at("default_generation_settings").at("n_ctx").get<int>(),
                         props.at("chat_template").get<std::string>(), props.at("total_slots").get<int>(),
                         props.at("model_path").get<std::string>()};
    });
    const int slotContext = chat.slotContext;
    const std::string& chatTemplate = chat.chatTemplate;
    // Its slots, which morpheus_server shares between conversations, and its model's id, for /health.
    if (chat.slots < 1) {
        throw InferenceError(
            std::format("the chat server at {} reports {} slots in /props (total_slots)", _config.chatUrl, chat.slots));
    }
    _slotCount = chat.slots;
    _chatModelId = ModelIdFromPath(chat.modelPath);
    if (_chatModelId.empty()) {
        throw InferenceError(std::format("the chat server at {} names no model file in /props (model_path '{}')",
                                         _config.chatUrl, chat.modelPath));
    }
```

and replace the embedding server's name stripping:

```cpp
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    _embeddingModelId = dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
```

with:

```cpp
    _embeddingModelId = ModelIdFromPath(modelPath);
```

Add the two methods after `EmbeddingModelId()`:

```cpp
int LlamaServerBackend::SlotCount() const
{
    return _slotCount;
}

std::string LlamaServerBackend::ChatModelId() const
{
    return _chatModelId;
}
```

- [ ] **Step 5: Give every chat fake's custom /props its slots and model file**

The chat `/props` now needs `total_slots` and `model_path`. Run `grep -rn "SetProps(" tests/` and, for each call on a fake used as the chat server whose JSON lacks them, add `{"total_slots", 3}` and, if missing, `{"model_path", "/m/chat.gguf"}`. For example, in `"LlamaServerBackend refuses a chat server whose slots hold less than the context size"`:

```cpp
    chat.SetProps({{"default_generation_settings", {{"n_ctx", 4096}}},
                   {"total_slots", 3},
                   {"chat_template", ""},
                   {"model_path", "/m/chat.gguf"}});
```

Calls on a fake used only as the embedding server need nothing.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/
git commit -m "LlamaServerBackend reads the chat server's slot count and model id from /props"
```

---

