### Task 1: min_p

**Files:**
- Modify: `src/core/Settings.h` (`SamplingSettings`), `src/core/Settings.cpp` (read, write), `src/inference/Llm.h` (`Sampling`), `src/turn/Session.cpp` (`SamplingFrom`), `src/llama/LlamaCppBackend.cpp` (the chain), `src/llamaserver/LlamaServerBackend.cpp` (the body), `src/app/model/SettingsEdit.cpp` (read, write), `src/app/ui/SettingsWindow.cpp` (the field), `settings.example.json`, `README.md`
- Test: `tests/core/SettingsTests.cpp`, `tests/turn/SamplingTests.cpp`, `tests/llamaserver/LlamaServerCompleteTests.cpp`, `tests/app/SettingsEditTests.cpp`

**Interfaces:**
- Produces: `core::SamplingSettings::minP` (`double`, default 0.0, last member); `inference::Sampling::minP` (`float`, default 0.0f, last member).

- [ ] **Step 1: Write the failing tests**

`tests/core/SettingsTests.cpp`, append:

```cpp
TEST_CASE("LoadSettings reads an optional min_p per call type", "[settings]")
{
    // Absent: 0, so existing settings files load as they did.
    const Settings plain = LoadSettings(WriteSettings("settings_min_p_absent", ValidSettingsJson()));
    CHECK(plain.resolveSampling.minP == 0.0);
    CHECK(plain.narrateSampling.minP == 0.0);

    nlohmann::json json = ValidSettingsJson();
    json["sampling"]["narrate"]["min_p"] = 0.05;
    const Settings set = LoadSettings(WriteSettings("settings_min_p_set", json));
    CHECK(set.narrateSampling.minP == 0.05);
    CHECK(set.resolveSampling.minP == 0.0);
    // It round-trips.
    CHECK(LoadSettings(WriteSettings("settings_min_p_copy", SettingsToJson(set))).narrateSampling.minP == 0.05);

    for (const double bad : {-0.1, 1.0, 1.5}) {
        INFO(bad);
        nlohmann::json wrong = ValidSettingsJson();
        wrong["sampling"]["resolve"]["min_p"] = bad;
        CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_min_p_bad", wrong)),
                          ContainsSubstring(std::format("sampling 'resolve': 'min_p' must be at least 0 and below 1, "
                                                        "not {}",
                                                        bad)));
    }
}
```

(add `#include <format>`).

`tests/turn/SamplingTests.cpp`, append (add includes `turn/Session.h`, `core/Settings.h` if missing):

```cpp
TEST_CASE("SamplingFrom copies min_p", "[sampling]")
{
    core::SamplingSettings settings{0.7, 30, 0.9, 0.05};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 99);
    CHECK(sampling.minP == 0.05f);
    CHECK(sampling.seed == 99u);
}
```

`tests/llamaserver/LlamaServerCompleteTests.cpp`, "LlamaServerBackend::Complete sends one streamed /completion request": the spec's sampling becomes `{0.5f, 20, 0.75f, 77, 0.05f}`; the samplers check becomes `CHECK(body.at("samplers") == nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"}));` and the min_p check `CHECK(body.at("min_p").get<double>() == Catch::Approx(0.05));` (include `catch2/catch_approx.hpp`); update the comment above it to name min-p in the chain.

`tests/app/SettingsEditTests.cpp`: in the test that reads an edit, add `CHECK(edit.narrateSampling.minP == 0.0);`; add a case that saves an edit with `edit.resolveSampling.minP = 0.1` and checks the file's `sampling.resolve.min_p == 0.1` and the loaded settings' `resolveSampling.minP == 0.1` (follow the file's existing save test for the setup).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests`
Expected: compile errors (`minP` is not a member).

- [ ] **Step 3: Implement**

`src/core/Settings.h`, `SamplingSettings`:

```cpp
struct SamplingSettings {
    double temperature = 0.0; // 0 means greedy decoding.
    int topK = 40;
    double topP = 0.95;
    double minP = 0.0;        // 0 means no min-p. Last, so positional initializers of the fields above stay valid.
};
```

`src/core/Settings.cpp`, `readSampling`: `checkKeys(object, callType, {"temperature", "top_k", "top_p", "min_p"});` and, after the top_p check:

```cpp
        // min_p is optional (0: none), and below 1, which would keep only the top token.
        if (object.contains("min_p")) {
            const nlohmann::json& minP = object.at("min_p");
            if (!minP.is_number() || minP.get<double>() < 0.0 || minP.get<double>() >= 1.0) {
                throw DataError(std::format("{}: sampling '{}': 'min_p' must be at least 0 and below 1, not {}", where,
                                            callType, minP.dump()));
            }
            result.minP = minP.get<double>();
        }
```

`SettingsToJson`'s `sampling` lambda adds `{"min_p", values.minP}`.

`src/inference/Llm.h`, `Sampling`: add after `seed`: `float minP = 0.0f; // 0 means no min-p. Last, so positional initializers stay valid.`

`src/turn/Session.cpp`, `SamplingFrom`: `sampling.minP = static_cast<float>(settings.minP);`

`src/llama/LlamaCppBackend.cpp`, the non-greedy chain: after the top-p line, `llama_sampler_chain_add(pChain.get(), llama_sampler_init_min_p(spec.sampling.minP, 1));`, and the comment names min-p.

`src/llamaserver/LlamaServerBackend.cpp`, the body: `{"samplers", nlohmann::json::array({"top_k", "top_p", "min_p", "temperature"})}` and `{"min_p", spec.sampling.minP}`; the comment above says the chain is top-k, top-p, min-p, temperature (min-p moves out of the neutral list).

`src/app/model/SettingsEdit.cpp`: `ReadSettingsEdit`'s `sampling` lambda reads `block.value("min_p", 0.0)` into the fourth member; `SaveSettingsEdit`'s `samplingJson` writes `{"min_p", sampling.minP}` after `top_p`.

`src/app/ui/SettingsWindow.cpp`, `samplingFields`: after Top P, `ImGui::InputDouble("Min P", &sampling.minP, 0.01, 0.05, "%.2f");`.

`tests/hosted/HostedTestSupport.h`, `HostedSettings()`: both samplings gain min_p 0.05 (`{0.3, 40, 0.9, 0.05}` and `{0.8, 40, 0.95, 0.05}`), so the live hosted and `[server]` runs, which start from these settings, sample with min_p (the fakes ignore sampling), and its comment says so.

`settings.example.json`: `"min_p": 0.0` in both sampling entries. `README.md`, the settings section's sampling description: name `min_p` (optional, 0 to below 1, default 0).

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ctest --preset tests`
Expected: all pass. Then build the dev tests target (`cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`): it compiles `src/llama`.

- [ ] **Step 5: Commit**

```bash
git add src/core/Settings.h src/core/Settings.cpp src/inference/Llm.h src/turn/Session.cpp src/llama/LlamaCppBackend.cpp src/llamaserver/LlamaServerBackend.cpp src/app/model/SettingsEdit.cpp src/app/ui/SettingsWindow.cpp settings.example.json README.md tests/hosted/HostedTestSupport.h tests/core/SettingsTests.cpp tests/turn/SamplingTests.cpp tests/llamaserver/LlamaServerCompleteTests.cpp tests/app/SettingsEditTests.cpp
git commit -m "Sampling: min_p, from settings.json to both backends' samplers"
```

---

