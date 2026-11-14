### Task 6: `turn::HostedSession`: open, conversations, readers

One open hosted run. `Open` creates the run or reopens it (same host data as is; changed host data replaces the stored copy and re-embeds changed lore; an owned file is refused). Conversations start, take events between turns, change participants and end. Folded hosted turns are rendered as transcript entries and indexed for retrieval, which needs `MemoryIndex` to take a lore list and rendered entries. Turns come in Task 7.

**Files:**
- Modify: `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp`
- Create: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Modify: `src/turn/CMakeLists.txt`
- Modify: `tests/hosted/HostedTestSupport.h`, `tests/turn/RetrievalTests.cpp`
- Create: `tests/hosted/HostedSessionTests.cpp`

**Interfaces:**
- Consumes: `store::RunStore` hosted methods, `store::RunKind`, `core::Conversation`, `core::HostedTurn`, `core::ConversationEvent` (Task 2); `context::TranscriptEntry` (Task 3); `turn::HostedWorld`, `HostData`, `HostDataToJson`, `HostDataHash`, `Actor`, `Observation` (Task 4); `HOSTED_TURN_CUE` (Task 5).
- Produces (`turn/Retrieval.h`): `void MemoryIndex::SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store, inference::LlmBackend& backend)` (the world overload stays and calls it with `"world '<id>': lore.csv"`); `void MemoryIndex::SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store, inference::LlmBackend& backend)`.
- Produces (`turn/HostedSession.h`): `constexpr std::string_view turn::HOSTED_WORLD_ID = "hosted"`; `class turn::HostedSession` with `static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings, std::uint64_t seed, inference::LlmBackend& backend)`, `std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation)`, `void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)`, `void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined, const std::vector<std::string>& left, const nlohmann::json& observation)`, `void EndConversation(std::string_view conversationId)`, readers `const store::RunMeta& Meta() const`, `std::optional<core::Conversation> Conversation(std::string_view id)`, `std::vector<core::HostedTurn> Turns(std::string_view conversationId)`, `std::vector<core::ConversationEvent> Events(std::string_view conversationId)`, `std::vector<core::Summary> Summaries()`, `const context::Assembly& LastAssembly() const`; private `core::Conversation OpenConversation(std::string_view conversationId)`, `std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, bool folded)`, `void SyncHistory()`.
- Produces (`tests/hosted/HostedTestSupport.h`): `core::Settings HostedSettings()`, `std::filesystem::path HostedRunFile(std::string_view name)`.

- [ ] **Step 1: Write the failing tests**

Replace the whole of `tests/hosted/HostedTestSupport.h` with:

```cpp
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Settings.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";

// tests/hosted/fixtures in the source tree: host data and requests written from section 5 of the integration
// requirements. The host's recorded fixtures can replace them later.
inline std::filesystem::path HostedFixturesDir()
{
    return RepoDir() / "tests" / "hosted" / "fixtures";
}

// The fixture `name`, parsed. A fixture that cannot be read is a broken test: it throws.
inline nlohmann::json ReadFixture(std::string_view name)
{
    std::ifstream stream(HostedFixturesDir() / name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFixture: cannot open '" + std::string(name) + "'");
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

// open.json: the host data a host sends when it opens a run.
inline turn::HostData FixtureHostData()
{
    return turn::HostDataFromJson(ReadFixture("open.json"));
}

// observation.json: Hulda and Saadia at the inn, and Aela, the player.
inline nlohmann::json FixtureObservation()
{
    return ReadFixture("observation.json");
}

// Settings for hosted session tests: a context of 16384 with roomy budgets (resolve_max_tokens 64, narrate_max_tokens
// 256, narrate_allowance 128, tail_allowance 1024, engine_rules_cap 1024, world_cap 1024, summary_cap 512,
// retrieval_cap 256) and no thinking.
inline core::Settings HostedSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {64, 256, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// A run file in a fresh folder for test `name`.
inline std::filesystem::path HostedRunFile(std::string_view name)
{
    return FreshTempDir(name) / "run.db";
}

} // namespace morpheus::test
```

Create `tests/hosted/HostedSessionTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "store/RunStore.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using namespace nlohmann::literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;

namespace {

// The participant ids of conversation `id`, in join order.
std::vector<std::string> ParticipantIds(turn::HostedSession& session, const std::string& id)
{
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    std::vector<std::string> ids;
    for (const core::Participant& participant : conversation->participants) {
        ids.push_back(participant.id);
    }
    return ids;
}

} // namespace

TEST_CASE("Open creates a hosted run with its host data and indexes the retrievable lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384); // No model call is made: no responder.
    const std::filesystem::path file = test::HostedRunFile("hosted_open");
    const turn::HostData data = test::FixtureHostData();
    turn::HostedSession session = turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend);
    CHECK(session.Meta().kind == store::RunKind::Hosted);
    CHECK(session.Meta().worldId == "hosted");
    CHECK(session.Meta().worldHash == turn::HostDataHash(data));
    CHECK(session.Meta().rngSeed == 42u);
    CHECK(session.Meta().modelId == "chat");
    CHECK(backend.EmbeddedTexts() == 2); // Hulda and Saadia; Whiterun and the inn are always on.
    CHECK(session.Summaries().empty());
    CHECK(std::filesystem::exists(file));
}

TEST_CASE("Open with the same host data reopens the run as it is", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_reopen");
    std::string id;
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    }
    const int embedded = backend.EmbeddedTexts();
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 7, backend);
    CHECK(backend.EmbeddedTexts() == embedded); // The stored vectors are reused.
    CHECK(reopened.Meta().rngSeed == 42u);      // The run's seed, not the call's.
    CHECK(ParticipantIds(reopened, id) == std::vector<std::string>{test::HULDA, test::AELA});
}

TEST_CASE("Open with changed host data replaces the stored copy and re-embeds the changed lore", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    const std::filesystem::path file = test::HostedRunFile("hosted_changed");
    {
        turn::HostedSession session =
            turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend);
    }
    // A mod update: one lore entry and the catalog change.
    turn::HostData changed = test::FixtureHostData();
    changed.lore[2].data = "Hulda has kept the inn for twenty years.";
    changed.catalog["actions"].push_back(nlohmann::json{{"id", "trade"}});
    const int embedded = backend.EmbeddedTexts();
    {
        turn::HostedSession reopened = turn::HostedSession::Open(file, changed, test::HostedSettings(), 42, backend);
        CHECK(reopened.Meta().worldHash == turn::HostDataHash(changed));
        CHECK(backend.EmbeddedTexts() == embedded + 1); // Only Hulda's entry is embedded again.
    }
    store::RunStore stored = store::RunStore::Open(file);
    CHECK(stored.HostData() == turn::HostDataToJson(changed));
    CHECK(stored.Meta().worldHash == turn::HostDataHash(changed));
}

TEST_CASE("Open refuses a file that holds an owned run", "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_owned");
    store::RunMeta meta;
    meta.worldId = "mini";
    {
        store::RunStore owned = store::RunStore::Create(file, meta, nlohmann::json::object());
    }
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, backend),
                         core::DataError,
                         MessageMatches(ContainsSubstring("is an owned run of world 'mini', not a hosted run")));
}

TEST_CASE("Open checks the host data before it touches the file, and leaves no file when the lore fails",
          "[hosted][session]")
{
    const std::filesystem::path file = test::HostedRunFile("hosted_bad_data");
    turn::HostData data = test::FixtureHostData();
    data.catalog = R"({"actions": []})"_json;
    test::ScriptedBackend backend(16384);
    CHECK_THROWS_WITH(turn::HostedSession::Open(file, data, test::HostedSettings(), 42, backend),
                      "catalog at /actions: 'actions' must be a non-empty array");
    CHECK_FALSE(std::filesystem::exists(file));

    // Lore the embedding model cannot take is the host's DataError, and the new file goes again.
    test::ScriptedBackend small(16384);
    small.SetEmbedLimit(20);
    CHECK_THROWS_MATCHES(turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, small),
                         core::DataError,
                         MessageMatches(ContainsSubstring("host data: lore entry 'Hulda' cannot be embedded")));
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("StartConversation stores the participants with their names and bios, in order", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_start"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::AELA, test::HULDA}, observation);
    CHECK(id == "c1");
    const std::optional<core::Conversation> conversation = session.Conversation(id);
    REQUIRE(conversation);
    REQUIRE(conversation->participants.size() == 2);
    CHECK(conversation->participants[0].id == test::AELA);
    CHECK(conversation->participants[0].name == "Aela");
    CHECK(conversation->participants[0].bio.empty());
    CHECK(conversation->participants[1].name == "Hulda");
    CHECK(conversation->participants[1].bio == "Hulda keeps the Bannered Mare.");
    CHECK_FALSE(conversation->ended);
    CHECK(session.StartConversation({test::SAADIA}, observation) == "c2");

    // Each fault is the host's DataError naming the field, and starts nothing.
    CHECK_THROWS_WITH(session.StartConversation({}, observation),
                      "participants: a conversation needs at least one participant");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, "00000000"}, observation),
                      "participants/1: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA, test::HULDA}, observation),
                      "participants/1: '0001A6A4' is listed twice");
    nlohmann::json broken = observation;
    broken.at("actors").at(0)["id"] = "";
    CHECK_THROWS_WITH(session.StartConversation({test::HULDA}, broken), ContainsSubstring("observation at /actors/0/id"));
    CHECK_FALSE(session.Conversation("c3"));
}

TEST_CASE("RecordEvents adds events to an open conversation, after its latest turn", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_events"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.RecordEvents(id, {R"({"type": "ambient", "text": "Rain starts."})"_json});
    session.RecordEvents(id, {R"({"text": "A door opens."})"_json, R"({"text": "Saadia looks up."})"_json});
    const std::vector<core::ConversationEvent> events = session.Events(id);
    REQUIRE(events.size() == 3);
    CHECK(events[0].afterTurn == 0); // Before the conversation's first turn.
    CHECK(events[0].event.at("type") == "ambient");
    CHECK(events[2].event.at("text") == "Saadia looks up.");

    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"type": "ambient"})"_json}),
                      "events/0/text: an event needs 'text', a string");
    CHECK_THROWS_WITH(session.RecordEvents("c9", {}), "conversation_id: there is no conversation 'c9'");
    CHECK(session.Events(id).size() == 3);
}

TEST_CASE("SetParticipants removes those who left, then adds those who joined at the end", "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_participants"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const nlohmann::json observation = test::FixtureObservation();
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, observation);

    session.SetParticipants(id, {test::SAADIA}, {}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::HULDA, test::AELA, test::SAADIA});
    CHECK(session.Conversation(id)->participants[2].bio == "Saadia serves at the inn.");
    // A leave and a rejoin in one call moves the actor to the end.
    session.SetParticipants(id, {test::HULDA}, {test::HULDA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::AELA, test::SAADIA, test::HULDA});
    session.SetParticipants(id, {}, {test::AELA}, observation);
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});

    // Each fault is the host's DataError naming the field, and changes nothing.
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, observation),
                      "joined/0: '0001B07C' is already a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {"00000000"}, {}, observation),
                      "joined/0: '00000000' is not an actor of the observation");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::AELA}, observation),
                      "left/0: '00000007' is not a participant of conversation 'c1'");
    CHECK_THROWS_WITH(session.SetParticipants(id, {}, {test::SAADIA, test::HULDA}, observation),
                      "left: conversation 'c1' would have no participant left");
    CHECK(ParticipantIds(session, id) == std::vector<std::string>{test::SAADIA, test::HULDA});
}

TEST_CASE("EndConversation ends a conversation once, and an ended conversation takes no more changes",
          "[hosted][session]")
{
    test::ScriptedBackend backend(16384);
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_end"), test::FixtureHostData(),
                                                            test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.EndConversation(id);
    CHECK(session.Conversation(id)->ended);
    CHECK_THROWS_WITH(session.EndConversation(id), "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.RecordEvents(id, {R"({"text": "Late."})"_json}),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.SetParticipants(id, {test::SAADIA}, {}, test::FixtureObservation()),
                      "conversation_id: conversation 'c1' has ended");
    CHECK_THROWS_WITH(session.EndConversation("c9"), "conversation_id: there is no conversation 'c9'");
}
```

In `tests/turn/RetrievalTests.cpp`, replace `#include "TestPaths.h"` with

```cpp
#include "TestPaths.h"
#include "context/Transcript.h"
```

and append:

```cpp
TEST_CASE("Folded transcript entries are searchable history under their turn number", "[retrieval]")
{
    const core::World world = MiniWorld();
    store::RunStore store = NewRun("retrieval_entries", world);
    context::TranscriptEntry entry;
    entry.turn = 4;
    entry.fact = "Aela: Where is the brass key? Hulda [none]: Under the bar.";
    test::ScriptedBackend backend(16384);
    turn::MemoryIndex index("search_document: ");
    index.SyncEntries({entry}, store, backend);

    const std::vector<turn::RetrievalHit> hits =
        index.Retrieve(turn::RetrievalQuery{"brass key"}, ALL_SOURCES, 1000, 6, backend);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].kind == store::EmbeddingKind::Turn);
    CHECK(hits[0].refId == "4");
    CHECK(hits[0].text == "Turn 4: Aela: Where is the brass key? Hulda [none]: Under the bar.");

    // The stored vector is reused, and an empty list leaves no history.
    const int embedded = backend.EmbeddedTexts();
    index.SyncEntries({entry}, store, backend);
    CHECK(backend.EmbeddedTexts() == embedded);
    index.SyncEntries({}, store, backend);
    CHECK(index.Retrieve(turn::RetrievalQuery{"brass key"}, ALL_SOURCES, 1000, 6, backend).empty());
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile errors, among them `Cannot open include file: 'turn/HostedSession.h'` and `'SyncEntries': is not a member of 'morpheus::turn::MemoryIndex'`.

- [ ] **Step 3: Let MemoryIndex take a lore list and rendered entries**

In `src/turn/Retrieval.h`, replace

```cpp
#include "core/World.h"
#include "inference/Llm.h"
```

with

```cpp
#include "context/Transcript.h"
#include "core/World.h"
#include "inference/Llm.h"
```

replace

```cpp
    // Indexes every lore entry that is not always on as "subject: data", embedding entries one at a time where
    // the stored vector is missing or stale. Throws DataError naming the subject when an entry cannot be embedded
    // (too long for the embedding model) or a subject appears twice.
    void SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend);
```

with

```cpp
    // Indexes every entry of `lore` that is not always on as "subject: data", embedding entries one at a time where
    // the stored vector is missing or stale. `source` names the lore in errors ("world 'mini': lore.csv", "host data:
    // lore"). Throws DataError naming the source and the subject when an entry cannot be embedded (too long for the
    // embedding model) or a subject appears twice.
    void SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store,
                  inference::LlmBackend& backend);

    // SyncLore of the world's lore.csv, named "world '<id>': lore.csv".
    void SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend);
```

after the `SyncHistory` declaration (its line `    void SyncHistory(store::RunStore& store, int foldedThrough, inference::LlmBackend& backend);`), insert:

```cpp

    // Indexes `folded` (a hosted run's folded turns, as transcript entries) as searchable history: each embedded as
    // its fact cut to EMBED_TEXT_CHARS and shown as "Turn N: <fact>" with the fact cut to 2 * HISTORY_SNIPPET_CHARS,
    // embedding where missing or stale. Replaces every turn entry; notes are left as they are. Throws InferenceError
    // when the backend cannot embed a text (the engine cuts them, so that is a runtime failure).
    void SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store,
                     inference::LlmBackend& backend);
```

and replace

```cpp
    // Vectors for `documents` of one kind, aligned with them: stored ones whose hash still matches, the rest
    // embedded one text per call and stored.
    std::vector<std::vector<float>> Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                            store::RunStore& store, inference::LlmBackend& backend) const;
```

with

```cpp
    // Vectors for `documents` of one kind, aligned with them: stored ones whose hash still matches, the rest
    // embedded one text per call and stored. `loreSource` names lore in the DataError for an entry that cannot be
    // embedded; it is unused for other kinds.
    std::vector<std::vector<float>> Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                            std::string_view loreSource, store::RunStore& store,
                                            inference::LlmBackend& backend) const;
```

In `src/turn/Retrieval.cpp`, replace the whole function `void MemoryIndex::SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend)` (from its signature to its closing brace) with:

```cpp
void MemoryIndex::SyncLore(std::string_view source, const std::vector<core::LoreEntry>& lore, store::RunStore& store,
                           inference::LlmBackend& backend)
{
    std::vector<Document> documents;
    std::vector<Entry> entries;
    std::set<std::string> subjects;
    for (const core::LoreEntry& entry : lore) {
        if (entry.alwaysOn) {
            continue; // Always in L1.
        }
        if (!subjects.insert(entry.subject).second) {
            throw core::DataError(std::format("{} has two entries for subject '{}'", source, entry.subject));
        }
        const std::string text = entry.subject + ": " + entry.data;
        documents.push_back({entry.subject, text});
        entries.push_back({store::EmbeddingKind::Lore, entry.subject, Lowered(entry.subject), text, {}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Lore, documents, source, store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Lore, std::move(entries));
}

void MemoryIndex::SyncLore(const core::World& world, store::RunStore& store, inference::LlmBackend& backend)
{
    SyncLore(std::format("world '{}': lore.csv", world.id), world.lore, store, backend);
}
```

In `MemoryIndex::SyncHistory`, replace

```cpp
    std::vector<std::vector<float>> turnVectors = Vectors(store::EmbeddingKind::Turn, turnDocuments, store, backend);
```

with

```cpp
    std::vector<std::vector<float>> turnVectors = Vectors(store::EmbeddingKind::Turn, turnDocuments, "", store, backend);
```

and

```cpp
    std::vector<std::vector<float>> noteVectors = Vectors(store::EmbeddingKind::Note, noteDocuments, store, backend);
```

with

```cpp
    std::vector<std::vector<float>> noteVectors = Vectors(store::EmbeddingKind::Note, noteDocuments, "", store, backend);
```

After the closing brace of `MemoryIndex::SyncHistory`, insert:

```cpp

void MemoryIndex::SyncEntries(const std::vector<context::TranscriptEntry>& folded, store::RunStore& store,
                              inference::LlmBackend& backend)
{
    // Each folded turn under its run-wide number: its fact is what is embedded and, cut shorter, what is shown.
    std::vector<Document> documents;
    std::vector<Entry> entries;
    for (const context::TranscriptEntry& turn : folded) {
        const std::string refId = std::to_string(turn.turn);
        documents.push_back({refId, core::CutUtf8(turn.fact, EMBED_TEXT_CHARS)});
        entries.push_back({store::EmbeddingKind::Turn, refId, {},
                           std::format("Turn {}: {}", turn.turn, core::CutUtf8(turn.fact, 2 * HISTORY_SNIPPET_CHARS)),
                           {}});
    }
    std::vector<std::vector<float>> vectors = Vectors(store::EmbeddingKind::Turn, documents, "", store, backend);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].vector = std::move(vectors[i]);
    }
    Replace(store::EmbeddingKind::Turn, std::move(entries));
}
```

Replace the signature line of `MemoryIndex::Vectors`

```cpp
std::vector<std::vector<float>> MemoryIndex::Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                                     store::RunStore& store, inference::LlmBackend& backend) const
```

with

```cpp
std::vector<std::vector<float>> MemoryIndex::Vectors(store::EmbeddingKind kind, const std::vector<Document>& documents,
                                                     std::string_view loreSource, store::RunStore& store,
                                                     inference::LlmBackend& backend) const
```

and inside it replace

```cpp
                throw core::DataError(
                    std::format("lore.csv entry '{}' cannot be embedded: {}", document.refId, error.what()));
```

with

```cpp
                throw core::DataError(
                    std::format("{} entry '{}' cannot be embedded: {}", loreSource, document.refId, error.what()));
```

and the comment line above it, `        // A lore entry that cannot be embedded is a world problem (too long for the embedding model): a DataError`, with `        // A lore entry that cannot be embedded is a problem of its source (too long for the embedding model): a DataError`.

- [ ] **Step 4: Write HostedSession (without turns)**

Create `src/turn/HostedSession.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "context/Transcript.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/HostedWorld.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// Every hosted run's world id (store::RunMeta::worldId).
constexpr std::string_view HOSTED_WORLD_ID = "hosted";

// One open hosted run (hosted-world spec, section 1): the counterpart of turn::Session for a world the host owns and
// sends as data. Every backend call happens on the caller's thread. Not thread-safe.
class HostedSession {
private:
    HostedWorld _world; // Set by the constructor.
    core::Settings _settings = {};
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index; // Set by the constructor.
    context::Assembly _lastAssembly = {};

public:
    // Creates the run in `file` when the file does not exist, with `seed` for sampling, and indexes the retrievable
    // lore; or reopens it: the same host data (equal content hash) reopens as it is, changed host data (a mod update)
    // replaces the stored copy, and changed lore is re-embedded. The host data is checked first (HostedWorld's
    // DataError). Throws DataError for a file that holds an owned run, StoreError for a damaged one, and DataError if
    // lore cannot be embedded (a new file is then removed).
    static HostedSession Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                              std::uint64_t seed, inference::LlmBackend& backend);

    // Starts a conversation of `participantIds` (in this join order) and returns its id. Every participant must be an
    // actor of the observation, once; their names and bios are taken from it and stored. Throws DataError naming the
    // field ("participants", "participants/<i>", or the observation's pointer).
    std::string StartConversation(const std::vector<std::string>& participantIds, const nlohmann::json& observation);

    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn. Throws DataError naming the field.
    void RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events);

    // Removes those who `left`, then adds those who `joined` at the end of the join order, with their names and bios
    // from the observation. A conversation keeps at least one participant. Throws DataError naming the field
    // ("left/<i>", "joined/<i>", "left", or the observation's pointer).
    void SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                         const std::vector<std::string>& left, const nlohmann::json& observation);

    // Marks the conversation ended; the next Maintain folds its remaining turns. Throws DataError for an unknown or
    // ended conversation.
    void EndConversation(std::string_view conversationId);

    const store::RunMeta& Meta() const;
    std::optional<core::Conversation> Conversation(std::string_view id);
    std::vector<core::HostedTurn> Turns(std::string_view conversationId);         // Oldest first.
    std::vector<core::ConversationEvent> Events(std::string_view conversationId); // Recorded between turns, in order.
    std::vector<core::Summary> Summaries();
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the inspector and the tests.

private:
    HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                  inference::LlmBackend& backend);

    // Conversation `conversationId`, which must exist and be open. Throws DataError "conversation_id: ..." otherwise.
    core::Conversation OpenConversation(std::string_view conversationId);

    // A conversation's turns as transcript entries, oldest first: the folded ones (folded == true) or those still in
    // L3. A turn's user message is the events before it (recorded since the conversation's previous turn, then its
    // request's own) in parentheses and the player's line, one per line; HOSTED_TURN_CUE when it has neither. Its
    // assistant message is "<speaker> [<action id> <params>]: <line>", without the params when there are none and
    // without the bracket for a refused action. Names come from the turn's own observation.
    std::vector<context::TranscriptEntry> Entries(const core::Conversation& conversation, bool folded);

    // Indexes every conversation's folded turns as searchable history.
    void SyncHistory();
};

} // namespace morpheus::turn
```

Create `src/turn/HostedSession.cpp`:

```cpp
#include "turn/HostedSession.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

namespace {

// The names of the host's lore in errors.
constexpr std::string_view HOST_LORE_SOURCE = "host data: lore";
// The variables the engine fills in the hosted texts.
constexpr std::string_view SPEAKER_VARIABLE = "{speaker}";
constexpr std::string_view PLAYER_VARIABLE = "{player}";

// `text` with every {speaker} and {player} replaced by the speaker's and the player's names.
std::string FillVariables(std::string_view text, std::string_view speaker, std::string_view player)
{
    std::string filled;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::string_view rest = text.substr(at);
        if (rest.starts_with(SPEAKER_VARIABLE)) {
            filled += speaker;
            at += SPEAKER_VARIABLE.size();
        } else if (rest.starts_with(PLAYER_VARIABLE)) {
            filled += player;
            at += PLAYER_VARIABLE.size();
        } else {
            filled += text[at];
            ++at;
        }
    }
    return filled;
}

} // namespace

HostedSession::HostedSession(HostedWorld world, core::Settings settings, store::RunStore store, MemoryIndex index,
                             inference::LlmBackend& backend)
    : _world(std::move(world))
    , _settings(std::move(settings))
    , _store(std::move(store))
    , _pBackend(&backend)
    , _index(std::move(index))
{
}

HostedSession HostedSession::Open(const std::filesystem::path& file, HostData data, core::Settings settings,
                                  std::uint64_t seed, inference::LlmBackend& backend)
{
    // The host data is checked before the file is touched: a fault is the host's DataError.
    HostedWorld world(data);
    const nlohmann::json stored = HostDataToJson(data);
    const std::string hash = HostDataHash(data);
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }

    if (!exists) {
        // A new run: its meta and host data, then the retrievable lore indexed.
        store::RunMeta meta;
        meta.kind = store::RunKind::Hosted;
        meta.worldId = std::string(HOSTED_WORLD_ID);
        meta.worldHash = hash;
        meta.modelId = core::PathText(settings.chatModel.stem());
        meta.rngSeed = seed;
        meta.settings = core::SettingsToJson(settings);
        store::RunStore store = store::RunStore::CreateHosted(file, meta, stored);
        MemoryIndex index(settings.embeddingDocumentPrefix);
        try {
            index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
        } catch (...) {
            // The run never started: close the database (a moved-from store holds no handle), then remove the file.
            { store::RunStore closing = std::move(store); }
            // A file that cannot be removed is deliberately secondary: the caller needs the error that stopped the open.
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            throw;
        }
        return HostedSession(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    }

    // An existing run, which must be hosted. Changed host data replaces the stored copy; SyncLore re-embeds only the
    // lore whose text changed (by its content hash), and the folded turns are indexed again.
    store::RunStore store = store::RunStore::Open(file);
    if (store.Meta().kind != store::RunKind::Hosted) {
        throw core::DataError(std::format("run '{}' is an owned run of world '{}', not a hosted run",
                                          core::PathText(file), store.Meta().worldId));
    }
    if (store.Meta().worldHash != hash) {
        store.ReplaceHostData(stored, hash);
    }
    MemoryIndex index(settings.embeddingDocumentPrefix);
    index.SyncLore(HOST_LORE_SOURCE, data.lore, store, backend);
    HostedSession session(std::move(world), std::move(settings), std::move(store), std::move(index), backend);
    session.SyncHistory();
    return session;
}

std::string HostedSession::StartConversation(const std::vector<std::string>& participantIds,
                                             const nlohmann::json& observation)
{
    const Observation observed = _world.Observe(observation);
    if (participantIds.empty()) {
        throw core::DataError("participants: a conversation needs at least one participant");
    }
    // Each participant is an actor of the observation, once; its name and bio come from there.
    std::vector<core::Participant> participants;
    for (std::size_t i = 0; i < participantIds.size(); ++i) {
        const Actor* pActor = observed.Find(participantIds[i]);
        if (pActor == nullptr) {
            throw core::DataError(
                std::format("participants/{}: '{}' is not an actor of the observation", i, participantIds[i]));
        }
        const bool repeated = std::any_of(participants.begin(), participants.end(),
                                          [pActor](const core::Participant& participant) { return participant.id == pActor->id; });
        if (repeated) {
            throw core::DataError(std::format("participants/{}: '{}' is listed twice", i, participantIds[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    return _store.CreateConversation(participants);
}

void HostedSession::RecordEvents(std::string_view conversationId, const std::vector<nlohmann::json>& events)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    HostedWorld::CheckEvents(events);
    // They follow the conversation's latest turn (0 before its first), so they come before its next.
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    _store.AddConversationEvents(conversation.id, played.empty() ? 0 : played.back().number, events);
}

void HostedSession::SetParticipants(std::string_view conversationId, const std::vector<std::string>& joined,
                                    const std::vector<std::string>& left, const nlohmann::json& observation)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    const Observation observed = _world.Observe(observation);
    std::vector<core::Participant> participants = conversation.participants;
    // Where participant `id` is in the join order.
    auto position = [&participants](const std::string& id) {
        return std::find_if(participants.begin(), participants.end(),
                            [&id](const core::Participant& participant) { return participant.id == id; });
    };

    // Those who left go first, so a leave and a rejoin in one call moves the actor to the end.
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto it = position(left[i]);
        if (it == participants.end()) {
            throw core::DataError(std::format("left/{}: '{}' is not a participant of conversation '{}'", i, left[i],
                                              conversation.id));
        }
        participants.erase(it);
    }
    // Those who joined go to the end of the join order, with their names and bios from the observation.
    for (std::size_t i = 0; i < joined.size(); ++i) {
        if (position(joined[i]) != participants.end()) {
            throw core::DataError(std::format("joined/{}: '{}' is already a participant of conversation '{}'", i,
                                              joined[i], conversation.id));
        }
        const Actor* pActor = observed.Find(joined[i]);
        if (pActor == nullptr) {
            throw core::DataError(std::format("joined/{}: '{}' is not an actor of the observation", i, joined[i]));
        }
        participants.push_back({pActor->id, pActor->name, pActor->bio});
    }
    if (participants.empty()) {
        throw core::DataError(std::format("left: conversation '{}' would have no participant left", conversation.id));
    }
    _store.SetParticipants(conversation.id, participants);
}

void HostedSession::EndConversation(std::string_view conversationId)
{
    const core::Conversation conversation = OpenConversation(conversationId);
    _store.EndConversation(conversation.id);
}

const store::RunMeta& HostedSession::Meta() const
{
    return _store.Meta();
}

std::optional<core::Conversation> HostedSession::Conversation(std::string_view id)
{
    return _store.Conversation(id);
}

std::vector<core::HostedTurn> HostedSession::Turns(std::string_view conversationId)
{
    return _store.HostedTurns(conversationId);
}

std::vector<core::ConversationEvent> HostedSession::Events(std::string_view conversationId)
{
    return _store.ConversationEvents(conversationId);
}

std::vector<core::Summary> HostedSession::Summaries()
{
    return _store.Summaries();
}

const context::Assembly& HostedSession::LastAssembly() const
{
    return _lastAssembly;
}

core::Conversation HostedSession::OpenConversation(std::string_view conversationId)
{
    std::optional<core::Conversation> conversation = _store.Conversation(conversationId);
    if (!conversation) {
        throw core::DataError(std::format("conversation_id: there is no conversation '{}'", conversationId));
    }
    if (conversation->ended) {
        throw core::DataError(std::format("conversation_id: conversation '{}' has ended", conversationId));
    }
    return std::move(*conversation);
}

std::vector<context::TranscriptEntry> HostedSession::Entries(const core::Conversation& conversation, bool folded)
{
    const std::vector<core::ConversationEvent> recorded = _store.ConversationEvents(conversation.id);
    std::vector<context::TranscriptEntry> entries;
    int previous = 0; // The conversation's turn before the one rendered; 0 before its first.
    for (const core::HostedTurn& turn : _store.HostedTurns(conversation.id)) {
        if ((turn.number <= conversation.foldedThrough) == folded) {
            // Names from the turn's own observation, as it was when the turn was played.
            const std::vector<Actor> actors = HostedWorld::ActorsOf(turn.observation);
            const auto speaker = std::find_if(actors.begin(), actors.end(),
                                              [&turn](const Actor& actor) { return actor.id == turn.speakerId; });
            const auto player = std::find_if(actors.begin(), actors.end(), [](const Actor& actor) { return actor.isPlayer; });
            MORPHEUS_REQUIRE(speaker != actors.end() && player != actors.end(),
                             "HostedSession::Entries: a stored turn's speaker or player is not in its observation");

            // What came before the line: the events since the previous turn, the request's events, the player's line.
            std::vector<std::string> before;
            for (const core::ConversationEvent& event : recorded) {
                if (event.afterTurn == previous) {
                    before.push_back("(" + event.event.at("text").get<std::string>() + ")");
                }
            }
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            if (turn.playerText) {
                before.push_back(player->name + ": " + *turn.playerText);
            }

            // The line under the speaker's name, with the requested action in brackets; a refused one shows none.
            std::string line = speaker->name;
            if (turn.adjudication.requested) {
                const nlohmann::json& params = turn.adjudication.params;
                line += params.empty() ? std::format(" [{}]", turn.adjudication.actionId)
                                       : std::format(" [{} {}]", turn.adjudication.actionId,
                                                     params.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            }
            line += ": " + turn.line;

            // The user message holds what came before, one per line, or the cue; the fact is all of it on one line.
            std::string user;
            std::string fact;
            for (const std::string& part : before) {
                user += (user.empty() ? "" : "\n") + part;
                fact += part + " ";
            }
            if (user.empty()) {
                user = FillVariables(HOSTED_TURN_CUE, speaker->name, player->name);
            }
            fact += line;

            context::TranscriptEntry entry;
            entry.turn = turn.number;
            entry.messages = {{inference::Role::User, std::move(user)}, {inference::Role::Assistant, line}};
            entry.foldMessages = entry.messages;
            entry.fact = std::move(fact);
            entries.push_back(std::move(entry));
        }
        previous = turn.number;
    }
    return entries;
}

void HostedSession::SyncHistory()
{
    // Every conversation's folded turns, rendered as entries, are the run's searchable history.
    std::vector<context::TranscriptEntry> folded;
    for (const core::Conversation& conversation : _store.Conversations()) {
        for (context::TranscriptEntry& entry : Entries(conversation, true)) {
            folded.push_back(std::move(entry));
        }
    }
    _index.SyncEntries(folded, _store, *_pBackend);
}

} // namespace morpheus::turn
```

(`FillVariables` has one caller until Task 7 gives it its second.)

In `src/turn/CMakeLists.txt`, replace

```cmake
    Folding.cpp
    HostedWorld.cpp
```

with

```cmake
    Folding.cpp
    HostedSession.cpp
    HostedWorld.cpp
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[session],[retrieval]"`
Expected: builds with no warnings; `All tests passed` (`Session`'s tests and the hosted session tests carry `[session]`).

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 473` (463 + 10), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 6: Byte check and commit**

```bash
FILES="src/turn/Retrieval.h src/turn/Retrieval.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp src/turn/CMakeLists.txt tests/hosted/HostedTestSupport.h tests/hosted/HostedSessionTests.cpp tests/turn/RetrievalTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "Add turn::HostedSession: open, reopen and host data updates, conversations, events, participants" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

