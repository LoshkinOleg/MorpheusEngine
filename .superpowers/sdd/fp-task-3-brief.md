### Task 3: The server's `review`, API 1.5.0, docs

**Files:**
- Modify: `src/server/Api.h` (`API_VERSION`), `src/server/Api.cpp` (the turn body and `done`)
- Test: `tests/server/ApiReviewTests.cpp` (new), every `"1.4.0"` pin under `tests/server/`, `tests/server/ServerLiveTests.cpp` (one live case)
- Docs: `README.md`, `CHANGELOG.md`, `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, `docs/superpowers/specs/2026-11-16-prompt-pack-design.md`

**Interfaces:**
- Consumes: `HostedTurnRequest::review`; `test::HostedScript::reviews` (Task 2).

- [ ] **Step 1: Write the failing tests**

Create `tests/server/ApiReviewTests.cpp` (model it on `tests/server/ApiDecideTests.cpp`):
- "A review turn streams the action, then the line after the review, then done with timings.review": open, a conversation of Hulda and Aela, a turn body with `{"speaker": HULDA, "player_text": "Wait here.", "review": true, "observation": ...}`, `script.reviews = {R"({"consistent":true,"problems":[]})"}`; the events are `action`, then `token`s and `sentence`s whose text is the line, then `done` whose `timings` contain `review` (and no `respeak`), `api_version` "1.5.0".
- "A malformed review is a 400": `"review": "yes"` answers 400 with the message `review: expected true or false`.

Change every `"1.4.0"` under `tests/server/` to `"1.5.0"`.

`tests/server/ServerLiveTests.cpp`: in the conversation fixture case, send the fixture's last turn with `"review": true` (add the key to its body) and keep the existing checks (it completes and ends with done); say so in a comment.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: the review turn streams tokens during speak and `done` lacks `review`; the version pins fail; "yes" is accepted.

- [ ] **Step 3: Implement**

`src/server/Api.h`: `API_VERSION = "1.5.0"`. `src/server/Api.cpp`, the turn route, after `allowed_actions`:

```cpp
                    if (json.contains("review")) {
                        if (!json.at("review").is_boolean()) {
                            throw core::DataError("review: expected true or false");
                        }
                        request.review = json.at("review").get<bool>();
                    }
```

and the `done` event's `timings` gain `review` and `respeak` when the record's timings contain them (as `decide`).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all pass.

- [ ] **Step 5: Docs**

- `CHANGELOG.md`, above `## API 1.4.0`: `## API 1.5.0` "The review of a hosted turn (design: `docs/superpowers/specs/2026-11-16-hosted-review-design.md`). Only additions." with bullets: a turn's optional `review` (what it checks, one respeak, the line streams after the review, its cost); `done`'s `timings.review` and `timings.respeak`; the prompt pack's `review` text.
- `README.md`, `morpheus_server` section: `api_version` 1.5.0 (sentence and SSE example); the turn route's body names `review?`; a **Review.** paragraph (what is checked, that the line is held back and arrives after the review, one respeak, the timings, the cost: one review call and sometimes one more speak call, so measure it before enabling it widely); the Prompt pack paragraph lists `review` (no variable).
- Server spec: the turn route's body gains `review` (added in 1.5.0, G3); the `api_version` line 1.5.0; `done`'s timings line names `decide`, `review` and `respeak`; the Documentation section's CHANGELOG bullet adds 1.5.0.
- Prompt-pack spec, section 1's table: a `review` row (the review call's system message; no variable; added by G3).

- [ ] **Step 6: Commit**

```bash
git add src/server/Api.h src/server/Api.cpp tests/server/ApiReviewTests.cpp tests/server/*.cpp README.md CHANGELOG.md docs/superpowers/specs/2026-11-16-morpheus-server-design.md docs/superpowers/specs/2026-11-16-prompt-pack-design.md
git commit -m "Server: a turn's review, timings.review and respeak in done, API 1.5.0; docs"
```

---

