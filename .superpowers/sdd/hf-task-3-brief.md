### Task 3: Lore scopes and conditions: retrieval and prompts, API 1.6.0

**Files:** `src/turn/Retrieval.h`, `src/turn/Retrieval.cpp` (`Retrieve`), `src/turn/HostedSession.cpp` (the turn's retrieval, `Pass`, the decide prompt's lore), `src/server/Api.h` (`API_VERSION`); tests `tests/turn/RetrievalTests.cpp`, `tests/hosted/HostedLoreTests.cpp` (new), every `"1.5.0"` under `tests/server/`; docs README, CHANGELOG, `tests/hosted/fixtures/README.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`.

- [ ] **Step 1: Tests first.**
  - `Retrieve` with a lore filter skips lore entries the filter refuses (by subject) and keeps others; without one, nothing changes.
  - Hosted turns with host lore `{"subject": "Wards", "data": "...", "scopes": ["magic"]}`, `{"subject": "Bread", "data": "..."}` and an always-on `{"subject": "The College", "data": "...", "always_on": true, "requires": {"pointer": "/quests/MG03", "min": 40}}`. An observation where Hulda has `knows: ["magic"]` and Saadia none, with `quests.MG03` at 30 or 40. Check:
    - Hulda's turn about wards retrieves "Wards"; Saadia's does not; both can retrieve "Bread".
    - At stage 30 the system message has no "The College"; at 40 it does.
    - The decide prompt and a fold prompt follow the same always-on rule.

    Use the existing retrieval-asserting hosted tests (grep `timings["retrieval"]` or `retrieved` in tests/hosted) for how to observe hits.
- [ ] **Step 2:** `MemoryIndex::Retrieve` gains a last parameter `const std::function<bool(std::string_view subject)>& loreVisible = {}`. With it, a lore entry is searched only when it returns true. Comment it. `HostedSession`:
  - Build the predicate per turn from `_world.Lore()`, the turn's `observation.json` and the speaker's `knows` (from the observation's actors): visible when `condition` holds (if any) and `scopes` is empty or shares one with `knows`.
  - Pass it to the turn's `Retrieve`.
  - `Pass` and the decide prompt keep only the always-on entries whose condition holds on the observation they are built from (`inputs.observation` / `observation.json`).
- [ ] **Step 3:** API 1.6.0 (`Api.h`; `"1.5.0"` pins under tests/server become `"1.6.0"`). CHANGELOG `## API 1.6.0`:
  - lore `scopes` and `requires`, actor `knows`, with the visibility rule;
  - the review becomes a continuation of the speak prompt (Task 4; write the line now).

  README `morpheus_server`:
  - `api_version` 1.6.0 (the sentence and the SSE example);
  - a **Lore.** paragraph (scopes, knows, requires, the rule, always-on with requires but no scopes).

  `tests/hosted/fixtures/README.md`: the lore keys and `knows`. The hosted-world spec's lore and observation sections: one line each pointing to the host-feedback spec.
- [ ] **Step 4:** `cmake --build --preset tests --clean-first && ctest --preset tests`. Commit: "Hosted lore: scoped and gated retrieval, gated always-on lore; API 1.6.0".

---

