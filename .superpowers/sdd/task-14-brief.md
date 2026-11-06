### Task 14: morpheus_play, a recorded playtest, and the docs

**Files:**
- Create: `tools/play/CMakeLists.txt`, `tools/play/main.cpp`, `tests/data/replays/sandcrawler-playtest.json` (recorded)
- Modify: `CMakeLists.txt` (root), `README.md`, `CLAUDE.md`, `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

**Interfaces:**
- Consumes: `turn::Worker`, its commands and events, `turn::RecordingBackend`, `turn::MakeReplayFixture`, `llama::LlamaCppBackend`.
- Produces: the `morpheus_play` executable (`dev` preset).

- [ ] **Step 1: Write the tool**

Create `tools/play/CMakeLists.txt`:

```cmake
add_executable(morpheus_play main.cpp)
target_link_libraries(morpheus_play PRIVATE morpheus_turn morpheus_llama)
morpheus_target_defaults(morpheus_play)
```

In the root `CMakeLists.txt`, inside `if(MORPHEUS_BUILD_LLAMA)`, add `add_subdirectory(tools/play)` after `tools/probe`.

Create `tools/play/main.cpp`:

```cpp
// morpheus_play: plays a world in the terminal through the engine worker, the same path the app will take.
//
// Usage: morpheus_play <settings.json> <world folder> <run.db> [--seed N] [--record <fixture.json>]
// An existing run file is continued; otherwise a new run starts (seeded by --seed, else randomly). --record needs a
// new run and writes a replay fixture (see tests/data/replays) when you quit.
// At the prompt: /undo, /regen, /state, /quit, or your action.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/StateView.h"
#include "context/Transcript.h"
#include "core/Settings.h"
#include "core/Text.h"
#include "core/World.h"
#include "llama/LlamaCppBackend.h"
#include "turn/Replay.h"
#include "turn/Worker.h"

using namespace morpheus;

namespace {

constexpr std::string_view USAGE =
    "usage: morpheus_play <settings.json> <world folder> <run.db> [--seed N] [--record <fixture.json>]\n";

// What the player has seen of the run, kept from the events for /state and the replay fixture.
struct RunView {
    std::vector<core::TurnRecord> history = {};
    nlohmann::json state = nullptr;
};

// Prints one command's events up to its closing event and updates `view`. Returns false if it closed with an Error.
bool PrintUntilClosing(turn::Worker& worker, RunView& view)
{
    for (;;) {
        const std::optional<turn::Event> event = worker.WaitEvent(std::chrono::milliseconds(100));
        if (!event) {
            continue;
        }
        if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&*event)) {
            std::cout << pPiece->piece << std::flush;
        } else if (const auto* pResolved = std::get_if<turn::TurnResolved>(&*event)) {
            const std::string mechanics = context::RenderMechanicsLine(pResolved->record);
            if (!mechanics.empty()) {
                std::cout << mechanics << '\n';
            }
            for (const core::RejectedChange& rejected : pResolved->record.rejected) {
                std::cout << "  (rejected: " << rejected.reason << ")\n";
            }
            std::cout << '\n';
        } else if (const auto* pFailed = std::get_if<turn::MaintenanceFailed>(&*event)) {
            std::cerr << "maintenance: " << pFailed->message << '\n';
        } else if (const auto* pCompleted = std::get_if<turn::TurnCompleted>(&*event)) {
            // "[resolve 41 tok in 830 ms (49.4 tok/s), narrate 120 tok in 2100 ms; prompt 2311 tok, 2290 reused]"
            const nlohmann::json& resolve = pCompleted->record.timings.at("resolve");
            const nlohmann::json& narrate = pCompleted->record.timings.at("narrate");
            const int resolveTokens = resolve.at("generated").get<int>();
            const double resolveMs = resolve.at("generate_ms").get<double>();
            std::cout << std::format("\n[resolve {} tok in {:.0f} ms ({:.1f} tok/s), narrate {} tok in {:.0f} ms; "
                                     "prompt {} tok, {} reused]\n",
                                     resolveTokens, resolveMs, resolveTokens * 1000.0 / std::max(1.0, resolveMs),
                                     narrate.at("generated").get<int>(), narrate.at("generate_ms").get<double>(),
                                     resolve.at("prompt_tokens").get<int>(), resolve.at("reused").get<int>());
            if (!view.history.empty() && view.history.back().number == pCompleted->record.number) {
                view.history.back() = pCompleted->record; // A regenerated turn replaces the last one.
            } else {
                view.history.push_back(pCompleted->record);
            }
            view.state = pCompleted->state;
            return true;
        } else if (const auto* pOpened = std::get_if<turn::RunOpened>(&*event)) {
            std::cout << std::format("{} turn(s) played in world '{}'.{}\n", pOpened->history.size(),
                                     pOpened->meta.worldId,
                                     pOpened->worldChanged ? " The world's files changed since this run started." : "");
            if (!pOpened->history.empty()) {
                std::cout << '\n' << pOpened->history.back().narration << '\n';
            }
            view.history = pOpened->history;
            view.state = pOpened->state;
            return true;
        } else if (const auto* pChanged = std::get_if<turn::StateChanged>(&*event)) {
            std::cout << std::format("Back to turn {}.\n", pChanged->history.size());
            view.history = pChanged->history;
            view.state = pChanged->state;
            return true;
        } else if (std::holds_alternative<turn::TurnCancelled>(*event)) {
            std::cout << "\n[cancelled]\n";
            return true;
        } else {
            std::cerr << "error: " << std::get<turn::Error>(*event).message << '\n';
            return false;
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << USAGE;
        return 2;
    }
    std::optional<std::uint64_t> seed;
    std::optional<std::filesystem::path> recordFile;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if (arg == "--record" && i + 1 < argc) {
            recordFile = argv[++i];
        } else {
            std::cerr << USAGE;
            return 2;
        }
    }

    try {
        const core::Settings settings = core::LoadSettings(argv[1]);
        const std::filesystem::path worldFolder = argv[2];
        const std::filesystem::path runFile = argv[3];
        const bool resume = std::filesystem::exists(runFile);
        if (resume && recordFile) {
            std::cerr << "--record needs a new run file\n";
            return 2;
        }

        llama::LlamaConfig config;
        config.chatModel = settings.chatModel;
        config.embeddingModel = settings.embeddingModel;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;
        config.contextSize = settings.contextSize;
        config.gpuLayers = settings.gpuLayers;
        llama::LlamaCppBackend llamaBackend(config);
        std::optional<turn::RecordingBackend> recording;
        if (recordFile) {
            recording.emplace(llamaBackend);
        }
        inference::LlmBackend& backend =
            recording ? static_cast<inference::LlmBackend&>(*recording) : static_cast<inference::LlmBackend&>(llamaBackend);

        std::random_device entropy;
        const std::uint64_t runSeed =
            seed.value_or((static_cast<std::uint64_t>(entropy()) << 32) | static_cast<std::uint64_t>(entropy()));
        RunView view;
        std::vector<turn::ReplayStep> steps;
        {
            turn::Worker worker(settings, backend);
            if (resume) {
                worker.Post(turn::LoadRun{worldFolder, runFile});
            } else {
                worker.Post(turn::StartRun{worldFolder, runFile, runSeed});
            }
            if (!PrintUntilClosing(worker, view)) {
                return 1;
            }

            std::string line;
            while ((std::cout << "\n> " << std::flush) && std::getline(std::cin, line)) {
                line = core::Trim(line);
                if (line.empty()) {
                    continue;
                }
                if (line == "/quit") {
                    break;
                }
                if (line == "/state") {
                    std::cout << context::RenderStateView(view.state);
                    continue;
                }
                turn::ReplayStep step;
                if (line == "/undo") {
                    step.action = turn::ReplayStep::Action::Undo;
                    worker.Post(turn::UndoTurn{});
                } else if (line == "/regen") {
                    step.action = turn::ReplayStep::Action::Regenerate;
                    worker.Post(turn::RegenerateTurn{});
                } else {
                    step.input = line;
                    worker.Post(turn::SubmitInput{line});
                }
                std::cout << '\n';
                if (PrintUntilClosing(worker, view)) {
                    steps.push_back(std::move(step)); // Failed commands change nothing and are not replayed.
                }
            }
        }

        if (recordFile) {
            const core::MemoryMode mode = settings.memoryMode.value_or(core::LoadWorld(worldFolder).defaultMemoryMode);
            std::ofstream out(*recordFile, std::ios::binary);
            out << turn::MakeReplayFixture(core::PathText(worldFolder), runSeed, mode, steps, *recording, view.history,
                                           view.state)
                       .dump(2)
                << '\n';
            if (!out) {
                throw std::runtime_error("cannot write " + core::PathText(*recordFile));
            }
            std::cout << "Wrote " << core::PathText(*recordFile) << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
```

Add `#include <stdexcept>` and `<utility>`.

- [ ] **Step 2: Build it and play a recorded session**

Run: `cmake --preset dev && cmake --build --preset dev` (long).
Expected: `morpheus_play.exe` builds.

Play five inputs on sandcrawler with recording. Run it from the repo root; `saves/` is gitignored.

```bash
mkdir -p saves/playtest && rm -f saves/playtest/m3.db
printf 'I check the fuel gauge.\nI climb onto the roof and scan the horizon with the compass.\nI order the crew to ration water.\n/undo\nI order the crew to ration water, half portions for everyone.\n/regen\n/state\n/quit\n' \
  | ./build/dev/bin/Debug/morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/m3.db --seed 1234 --record tests/data/replays/sandcrawler-playtest.json
```

Expected:
- Every turn prints a bracketed mechanics line (or nothing, for a turn without checks or changes), streamed narration, and a timing line.
- `/state` prints the state view.
- The fixture is written.

If the run fails, fix the cause in the code, not the inputs. Put the full console output in the report. Note:
- the resolve tok/s, the grammar decode speed;
- prompt reuse on turns 2 onward;
- how the narration and the proposed changes read: sensible checks? valid paths? rejections?

Two follow-up rules:
- **If a resolve output was cut off** (the record says "could not be resolved"), say so in the report.
- **If `RETRIEVAL_MIN_SCORE` looks wrong,** say so. That is the case if every lore entry comes back for unrelated inputs, or nothing ever does. Check the `retrieval` scores in the turn telemetry: query `saves/playtest/m3.db` with the sqlite shell or a debug print. Do not tune the constants in this task; report the observed scores.

- [ ] **Step 3: Run the replay test against the recording**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`
Expected: PASS for both fixtures. The recorded one replays its rolls, changes, narration and final state with no model.

- [ ] **Step 4: Update the docs**

`README.md`:
- Layout: add `src/turn` (turn orchestration: grammar, resolve and apply, retrieval, folding, session, worker, replay) and `tools/play` (`morpheus_play`, `dev` preset only).
- **Models and settings:** add that `budget.retrieval_cap` caps retrieved memory in the prompt, and that the optional top-level `"memory_mode": "engine" | "assisted"` overrides the world's default.
- New section **Play in the terminal**, with the `morpheus_play` usage line, the prompt commands (`/undo`, `/regen`, `/state`, `/quit`), and the note that `--record <file>` writes a replay fixture for `tests/data/replays`.

`CLAUDE.md` layout: add `src/turn` and `tools/play` lines matching the README.

Architecture spec `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`:
- **Process and modules:**
  - Commands: `StartRun`, `LoadRun`, `SubmitInput`, `UndoTurn`, `RegenerateTurn`; Cancel is `Worker::Cancel`, not a command.
  - Events: `RunOpened`, `TokenStreamed`, `TurnResolved`, `TurnCompleted`, `TurnCancelled`, `StateChanged`, `MaintenanceFailed`, `Error`.
  - Add: "Every command ends with exactly one closing event."
- **Module table, `turn` row:** "Turn orchestration (grammar, resolve and apply, retrieval, folding, session), the worker thread, replay recording".
- **Turn flow:**
  - **Step 1:** retrieval is capped by `retrieval_cap` and at most 6 hits. Turns still in L3 are not searchable, and assisted-mode notes are retrieved here.
  - **Step 2:** the engine generates the GBNF itself (`turn::BuildResolveGrammar`) from the live state and the schema, with size limits. Remove the json-schema-to-grammar wording.
  - **Step 4:** the narrate message is fitted to `narrate_allowance`, dropping rejected changes first.
  - **Undo and regenerate:** "Regenerate keeps the dice (`CheckSeed`) and samples new text (`SamplingSeed` with the turn's attempt number). A cancelled regenerate keeps the previous turn."
  - **`memory_mode`:** an optional `settings.json` key overrides the world's default. Notes are embedded and retrieved (T2), not pinned in L2, so the cached prefix stays stable.
- **Error policy:** add `TurnError` (a turn that cannot run as asked).
- **Inference interface:** replace the two milestone-3 grammar bullets with "The resolve and lookup grammars are generated by `turn::BuildResolveGrammar` / `BuildLookupGrammar`."
- **Settings sentence:** `memory_mode` is optional, and the budget includes `retrieval_cap`.
- **World data:** the change rules gain "removing more of a counted item than is held is rejected".
- **Testing:** replay fixtures live in `tests/data/replays` and are recorded with `morpheus_play --record`.

- [ ] **Step 5: Run everything and commit**

Run: `ctest --preset tests`, then `ctest --preset dev` with the model variables exported.
Expected: all pass.

```bash
git add CMakeLists.txt tools/play tests/data/replays/sandcrawler-playtest.json README.md CLAUDE.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md
git commit -m "Add morpheus_play, a recorded sandcrawler playtest, and M3 docs" -m "Co-Authored-By: Claude <Model> 5.5 <noreply@anthropic.com>"
```

---

