### Task 4: The server module: build option, `SlotPool` and `SlotBackend`

**Files:**
- Modify: `CMakeLists.txt` (the option, its check, the subdirectory)
- Create: `src/server/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (filter and link)
- Create: `src/server/ServerLog.h`, `src/server/SlotPool.h`, `src/server/SlotPool.cpp`, `src/server/SlotBackend.h`, `src/server/SlotBackend.cpp`
- Test: `tests/server/SlotPoolTests.cpp`, `tests/server/SlotBackendTests.cpp`

**Interfaces:**
- Consumes: `inference::LlmBackend` (`src/inference/Llm.h`), `core::ContractError` / `MORPHEUS_REQUIRE` (`src/core/Errors.h`).
- Produces:
  - `using server::Log = std::function<void(const std::string& line)>;` (`server/ServerLog.h`)
  - `class server::SlotPool` with `static constexpr int UPKEEP_SLOT = 0;`, `explicit SlotPool(int slotCount)`, `int SlotCount() const`, `int Acquire(const std::string& key)`, `void Release(const std::string& key)`, `std::string HolderOf(int slot) const`
  - `class server::SlotBackend final : public inference::LlmBackend` with `explicit SlotBackend(inference::LlmBackend& backend)`, `void SetSlot(int slot)`, `int Slot() const`, and every `LlmBackend` override
  - CMake target `morpheus_server_lib`; option `MORPHEUS_BUILD_SERVER`
  - Test tag `[hosted-server]` for every server test that needs no real llama-server. (`[server]` stays the tag of tests that need real llama-servers; never put it on these.)

- [ ] **Step 1: Add the build option and the module**

In `CMakeLists.txt`, after the `option(MORPHEUS_BUILD_LLAMA_SERVER ...)` line:

```cmake
option(MORPHEUS_BUILD_SERVER "Build morpheus_server, the HTTP server for hosted runs (needs MORPHEUS_BUILD_LLAMA_SERVER; no llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
```

after the existing `if(MORPHEUS_BUILD_LLAMA AND NOT MORPHEUS_BUILD_LLAMA_SERVER) ... endif()` check:

```cmake
if(MORPHEUS_BUILD_SERVER AND NOT MORPHEUS_BUILD_LLAMA_SERVER)
    message(FATAL_ERROR "MORPHEUS_BUILD_SERVER (morpheus_server) needs MORPHEUS_BUILD_LLAMA_SERVER.")
endif()
```

and in the engine libraries section, after `if(MORPHEUS_BUILD_LLAMA_SERVER) add_subdirectory(src/llamaserver) endif()`:

```cmake
if(MORPHEUS_BUILD_SERVER)
    add_subdirectory(src/server)
endif()
```

Create `src/server/CMakeLists.txt`:

```cmake
# morpheus_server's library (morpheus_server spec, section 4): run workers, the transport-free API and the HTTP
# adapter. It is not one of the engine libraries a vendored build exposes, so it has no Morpheus:: alias.
add_library(morpheus_server_lib STATIC
    SlotBackend.cpp
    SlotPool.cpp
)
target_include_directories(morpheus_server_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_server_lib
    PUBLIC morpheus_turn morpheus_llamaserver nlohmann_json::nlohmann_json
    PRIVATE morpheus_httplib
)
morpheus_target_defaults(morpheus_server_lib)
```

In `tests/CMakeLists.txt`, after the `/tests/llamaserver/` filter:

```cmake
# The server's tests need morpheus_server_lib; presets without it skip their sources too.
if(NOT MORPHEUS_BUILD_SERVER)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/server/")
endif()
```

and after the `if(MORPHEUS_BUILD_LLAMA_SERVER) target_link_libraries(... morpheus_httplib) endif()` block:

```cmake
if(MORPHEUS_BUILD_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_server_lib)
endif()
```

Create `src/server/ServerLog.h`:

```cpp
#pragma once

#include <functional>
#include <string>

namespace morpheus::server {

// Where the server's layers write a line for the operator: the executable writes them to stderr, tests keep them.
// Called from any thread, so the function must be thread-safe.
using Log = std::function<void(const std::string& line)>;

} // namespace morpheus::server
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/SlotPoolTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

#include "core/Errors.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("SlotPool keeps slot 0 for upkeep and gives conversations the others, least recently used first",
          "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.SlotCount() == 3);
    CHECK(pool.Acquire("run/c1") == 1);
    CHECK(pool.Acquire("run/c2") == 2);
    CHECK(pool.Acquire("run/c1") == 1); // Its own slot again; c1 is now the most recently used.
    // A third conversation takes the slot of the one used least recently (c2), which loses it.
    CHECK(pool.Acquire("run/c3") == 2);
    CHECK(pool.HolderOf(2) == "run/c3");
    CHECK(pool.HolderOf(0) == "");
    // c2's next turn takes a slot again: c1's, now the least recently used.
    CHECK(pool.Acquire("run/c2") == 1);
    CHECK(pool.HolderOf(1) == "run/c2");
}

TEST_CASE("SlotPool frees a released conversation's slot for the next one", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.Acquire("a/c1") == 1);
    CHECK(pool.Acquire("a/c2") == 2);
    pool.Release("a/c1");
    CHECK(pool.HolderOf(1) == "");
    CHECK(pool.Acquire("b/c1") == 1);
    pool.Release("never/held"); // Nothing to free: nothing changes.
    CHECK(pool.HolderOf(1) == "b/c1");
    CHECK(pool.HolderOf(2) == "a/c2");
}

TEST_CASE("SlotPool with one slot shares slot 0 between upkeep and every conversation", "[hosted-server][slots]")
{
    server::SlotPool pool(1);
    CHECK(pool.Acquire("a/c1") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.Acquire("a/c2") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.HolderOf(0) == "");
}

TEST_CASE("SlotPool hands out conversation slots safely from several threads", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    std::vector<int> slots(400, -1);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, &slots, t] {
                for (int i = 0; i < 100; ++i) {
                    slots[static_cast<std::size_t>(t * 100 + i)] = pool.Acquire("run/c" + std::to_string(t * 100 + i));
                }
            });
        }
    }
    for (const int slot : slots) {
        CHECK((slot == 1 || slot == 2));
    }
}

TEST_CASE("SlotPool refuses no slot and an empty key", "[hosted-server][slots]")
{
    CHECK_THROWS_AS(server::SlotPool(0), core::ContractError);
    server::SlotPool pool(2);
    CHECK_THROWS_AS(pool.Acquire(""), core::ContractError);
    CHECK_THROWS_AS(pool.HolderOf(2), core::ContractError);
}
```

Create `tests/server/SlotBackendTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"
#include "server/SlotBackend.h"

using namespace morpheus;

namespace {

const inference::Prompt PROMPT = {{{inference::Role::System, "Be brief."}, {inference::Role::User, "Hi"}}, 1};

} // namespace

TEST_CASE("SlotBackend puts calls left on the default slot on its current slot, and keeps a slot a call chose",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096,
                                [](const inference::Prompt&, const inference::GenerationSpec&) { return std::string("ok"); });
    server::SlotBackend backend(inner);
    CHECK(backend.Slot() == -1);
    inference::GenerationSpec spec;
    spec.maxTokens = 8;
    CHECK(backend.Complete(PROMPT, spec, {}, {}).text == "ok"); // No slot set yet: -1 stays.
    backend.SetSlot(2);
    backend.Complete(PROMPT, spec, {}, {});
    inference::GenerationSpec chosen = spec;
    chosen.slot = 1;
    backend.Complete(PROMPT, chosen, {}, {});
    REQUIRE(inner.Calls().size() == 3);
    CHECK(inner.Calls()[0].spec.slot == -1);
    CHECK(inner.Calls()[1].spec.slot == 2);
    CHECK(inner.Calls()[2].spec.slot == 1);
    CHECK(backend.Slot() == 2);
}

TEST_CASE("SlotBackend forwards counting, embedding and the model id, and refuses a slot below -1",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096);
    server::SlotBackend backend(inner);
    CHECK(backend.CountTokens("abcdefgh") == inner.CountTokens("abcdefgh"));
    CHECK(backend.CountPromptTokens(PROMPT) == inner.CountPromptTokens(PROMPT));
    CHECK(backend.ContextSize() == 4096);
    CHECK(backend.Embed({"a lighthouse"}, inference::EmbedPurpose::Query) ==
          inner.Embed({"a lighthouse"}, inference::EmbedPurpose::Query));
    CHECK(backend.EmbeddingModelId() == inner.EmbeddingModelId());
    CHECK_THROWS_AS(backend.SetSlot(-2), core::ContractError);
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --preset tests` (the option is new), `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/SlotPool.h'` (and `morpheus_server_lib` failing for want of its sources).

- [ ] **Step 4: Write `SlotPool`**

Create `src/server/SlotPool.h`:

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace morpheus::server {

// The chat server's slots, shared by every run of morpheus_server (spec, section 2). Slot 0 is the upkeep slot: the
// calls of folding and condensing. The others go to conversations, the least recently used first: with more
// conversations than slots, the conversation used least recently loses its slot, and its next turn takes one again
// (its prompt is then read again instead of reused from the slot's cache). With a single slot, upkeep and every
// conversation share slot 0. Thread-safe.
class SlotPool {
public:
    static constexpr int UPKEEP_SLOT = 0;

private:
    // Who holds a conversation slot, and when it last used it.
    struct Seat {
        std::string key = {};      // The holding conversation's key; empty when the slot is free.
        std::uint64_t lastUse = 0; // _clock at its holder's last Acquire.
    };

    mutable std::mutex _mutex = {};
    int _slotCount = 0;
    std::vector<Seat> _seats = {}; // Indexed by slot. Seat 0 (upkeep) is never handed to a conversation when there are more.
    std::uint64_t _clock = 0;      // Counts Acquire calls: the order of use.

public:
    // Throws ContractError for a count below 1.
    explicit SlotPool(int slotCount);

    int SlotCount() const;

    // The slot of conversation `key` (its run's and its own id, e.g. "save_1/c2"), marked as used now: the slot it
    // holds, else a free conversation slot, else the slot of the conversation used least recently, which loses it.
    // With one slot: 0. Throws ContractError for an empty key.
    int Acquire(const std::string& key);

    // Frees conversation `key`'s slot, if it holds one.
    void Release(const std::string& key);

    // The key of the conversation holding `slot`; "" when it is free, and always for slot 0. Throws ContractError for
    // a slot out of range.
    std::string HolderOf(int slot) const;
};

} // namespace morpheus::server
```

Create `src/server/SlotPool.cpp`:

```cpp
#include "server/SlotPool.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotPool::SlotPool(int slotCount)
    : _slotCount(slotCount)
    , _seats(static_cast<std::size_t>(std::max(slotCount, 0)))
{
    MORPHEUS_REQUIRE(slotCount >= 1, std::format("SlotPool: a server has at least one slot, not {}", slotCount));
}

int SlotPool::SlotCount() const
{
    return _slotCount;
}

int SlotPool::Acquire(const std::string& key)
{
    MORPHEUS_REQUIRE(!key.empty(), "SlotPool::Acquire: a conversation needs a key");
    std::scoped_lock lock(_mutex);
    // One slot: upkeep and every conversation share it.
    if (_slotCount == 1) {
        return UPKEEP_SLOT;
    }
    ++_clock;

    // The conversation's own slot, else a free one, else the one used least recently.
    int chosen = -1;
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key == key) {
            chosen = slot;
        }
    }
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key.empty()) {
            chosen = slot;
        }
    }
    if (chosen < 0) {
        chosen = 1;
        for (int slot = 2; slot < _slotCount; ++slot) {
            if (_seats[static_cast<std::size_t>(slot)].lastUse < _seats[static_cast<std::size_t>(chosen)].lastUse) {
                chosen = slot;
            }
        }
    }
    Seat& seat = _seats[static_cast<std::size_t>(chosen)];
    seat.key = key;
    seat.lastUse = _clock;
    return chosen;
}

void SlotPool::Release(const std::string& key)
{
    std::scoped_lock lock(_mutex);
    for (Seat& seat : _seats) {
        if (seat.key == key) {
            seat = Seat{};
        }
    }
}

std::string SlotPool::HolderOf(int slot) const
{
    MORPHEUS_REQUIRE(slot >= 0 && slot < _slotCount,
                     std::format("SlotPool::HolderOf: slot {} is not one of the {} slots", slot, _slotCount));
    std::scoped_lock lock(_mutex);
    return _seats[static_cast<std::size_t>(slot)].key;
}

} // namespace morpheus::server
```

- [ ] **Step 5: Write `SlotBackend`**

Create `src/server/SlotBackend.h`:

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

namespace morpheus::server {

// An LlmBackend that forwards every call to another and puts its calls on a chosen server slot: Complete sets
// GenerationSpec::slot to the current slot when the call left it at -1 (the backend's default slot), and keeps a slot
// the call chose. A run's worker sets the slot before each job (the conversation's) and before folding (the upkeep
// slot), so HostedSession's calls land on the right slot without knowing about slots. Not thread-safe, like the
// backend it wraps.
class SlotBackend final : public inference::LlmBackend {
private:
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives this.
    int _slot = -1;                             // -1: the wrapped backend's default slot.

public:
    explicit SlotBackend(inference::LlmBackend& backend);

    // The slot of the calls that follow; -1 for the wrapped backend's default. Throws ContractError below -1.
    void SetSlot(int slot);
    int Slot() const;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
};

} // namespace morpheus::server
```

Create `src/server/SlotBackend.cpp`:

```cpp
#include "server/SlotBackend.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotBackend::SlotBackend(inference::LlmBackend& backend)
    : _pBackend(&backend)
{
}

void SlotBackend::SetSlot(int slot)
{
    MORPHEUS_REQUIRE(slot >= -1, std::format("SlotBackend::SetSlot: slot {} is below -1", slot));
    _slot = slot;
}

int SlotBackend::Slot() const
{
    return _slot;
}

int SlotBackend::CountTokens(std::string_view text)
{
    return _pBackend->CountTokens(text);
}

int SlotBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pBackend->CountPromptTokens(prompt);
}

int SlotBackend::ContextSize() const
{
    return _pBackend->ContextSize();
}

inference::Completion SlotBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                            const inference::TokenCallback& onToken, std::stop_token stop)
{
    // A call that chose no slot goes on the current one; a call that chose one keeps it.
    if (spec.slot != -1 || _slot == -1) {
        return _pBackend->Complete(prompt, spec, onToken, stop);
    }
    inference::GenerationSpec onSlot = spec;
    onSlot.slot = _slot;
    return _pBackend->Complete(prompt, onSlot, onToken, stop);
}

std::vector<std::uint8_t> SlotBackend::SaveState()
{
    return _pBackend->SaveState();
}

void SlotBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pBackend->RestoreState(state);
}

std::vector<std::vector<float>> SlotBackend::Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose)
{
    return _pBackend->Embed(texts, purpose);
}

std::string SlotBackend::EmbeddingModelId() const
{
    return _pBackend->EmbeddingModelId();
}

} // namespace morpheus::server
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: `All tests passed`.

Then check that a vendored build still defines no server target: `cmake -S cmake/consumer-check -B build/consumer-check` and `cmake --build build/consumer-check` (README, "Using Morpheus from another project"). Expected: configure succeeds (it fails when the vendored build defines more than the engine libraries and `morpheus_httplib`).

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt src/server/ tests/server/
git commit -m "morpheus_server_lib: the MORPHEUS_BUILD_SERVER option, SlotPool and SlotBackend"
```

---

