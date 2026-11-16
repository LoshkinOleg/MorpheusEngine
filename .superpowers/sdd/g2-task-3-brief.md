### Task 3: Thinking formats in the settings

**Files:**
- Create: `src/core/Thinking.h`, `src/core/Thinking.cpp`
- Modify: `src/core/CMakeLists.txt` (add `Thinking.cpp`), `src/core/Settings.h` (`Settings::thinking`), `src/core/Settings.cpp` (read, check, write)
- Test: `tests/core/ThinkingTests.cpp` (new), `tests/core/SettingsTests.cpp`

**Interfaces:**
- Produces (`src/core/Thinking.h`, namespace `morpheus::core`):
  ```cpp
  struct ThinkingFormat {
      std::string open = {};     // The prompt's end when the call may think.
      std::string skip = {};     // The prompt's end when it does not (may be empty).
      std::string end = {};      // The end marker the model writes.
      std::string afterEnd = {}; // What follows the end marker before the answer.
      std::string close = {};    // What closes a block that reached its budget: a nudge, then end and afterEnd.
      bool operator==(const ThinkingFormat&) const = default;
  };
  enum class ThinkingMode { Auto, Format, None };
  struct ThinkingSetting {
      ThinkingMode mode = ThinkingMode::Auto;
      ThinkingFormat format = {}; // Used by ThinkingMode::Format.
      bool operator==(const ThinkingSetting&) const = default;
  };
  inline const ThinkingFormat QWEN_THINKING = {...}; // Global Constraints' texts
  // The format a backend uses: Auto is QWEN_THINKING when `chatTemplate` has the enable_thinking switch and none
  // otherwise; Format is its format; None is none.
  std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate);
  ```
  `core::Settings::thinking` (`ThinkingSetting`, default Auto).

- [ ] **Step 1: Write the failing tests**

Create `tests/core/ThinkingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "core/Thinking.h"

using namespace morpheus::core;

TEST_CASE("The Qwen preset is the texts Qwen's chat template writes", "[thinking]")
{
    CHECK(QWEN_THINKING.open == "<think>\n");
    CHECK(QWEN_THINKING.skip == "<think>\n\n</think>\n\n");
    CHECK(QWEN_THINKING.end == "</think>");
    CHECK(QWEN_THINKING.afterEnd == "\n\n");
    CHECK(QWEN_THINKING.close == "\n\nConsidering the limited time, I have to give the solution based on the thinking "
                                 "directly now.\n</think>\n\n");
    CHECK(QWEN_THINKING.close.ends_with(QWEN_THINKING.end + QWEN_THINKING.afterEnd));
}

TEST_CASE("ResolveThinking: auto reads the template's switch, a format is used as given, none is none", "[thinking]")
{
    const std::string withSwitch = "{%- if enable_thinking is defined %}...{%- endif %}";
    const std::string without = "{% for message in messages %}{{ message.content }}{% endfor %}";
    CHECK(ResolveThinking({}, withSwitch) == std::optional<ThinkingFormat>(QWEN_THINKING));
    CHECK(ResolveThinking({}, without) == std::nullopt);
    const ThinkingFormat custom = {"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"};
    CHECK(ResolveThinking({ThinkingMode::Format, custom}, without) == std::optional<ThinkingFormat>(custom));
    CHECK(ResolveThinking({ThinkingMode::None, {}}, withSwitch) == std::nullopt);
}
```

`tests/core/SettingsTests.cpp`, append:

```cpp
TEST_CASE("LoadSettings reads thinking_format: a preset or the five texts", "[settings][thinking]")
{
    CHECK(LoadSettings(WriteSettings("settings_thinking_absent", ValidSettingsJson())).thinking == ThinkingSetting{});
    auto load = [](const std::string& name, const nlohmann::json& format) {
        nlohmann::json json = ValidSettingsJson();
        json["thinking_format"] = format;
        return LoadSettings(WriteSettings(name, json));
    };
    CHECK(load("settings_thinking_auto", "auto").thinking == ThinkingSetting{});
    CHECK(load("settings_thinking_qwen", "qwen").thinking == ThinkingSetting{ThinkingMode::Format, QWEN_THINKING});
    CHECK(load("settings_thinking_none", "none").thinking.mode == ThinkingMode::None);
    const nlohmann::json custom = {{"open", "[T]"}, {"skip", ""}, {"end", "[/T]"}, {"after_end", "\n"},
                                   {"close", "Answer now.[/T]\n"}};
    const Settings customSettings = load("settings_thinking_custom", custom);
    CHECK(customSettings.thinking ==
          ThinkingSetting{ThinkingMode::Format, {"[T]", "", "[/T]", "\n", "Answer now.[/T]\n"}});

    // Each form round-trips through SettingsToJson: "qwen" for the preset, the object for a custom format.
    CHECK(SettingsToJson(load("settings_thinking_qwen_json", "qwen")).at("thinking_format") == "qwen");
    CHECK(SettingsToJson(customSettings).at("thinking_format") == custom);
    CHECK(SettingsToJson(load("settings_thinking_none_json", "none")).at("thinking_format") == "none");
    CHECK(LoadSettings(WriteSettings("settings_thinking_copy", SettingsToJson(customSettings))).thinking ==
          customSettings.thinking);

    // The faults.
    CHECK_THROWS_WITH(load("settings_thinking_bad_name", "deepseek"),
                      ContainsSubstring("'thinking_format' must be \"auto\", \"qwen\", \"none\" or an object with open, "
                                        "skip, end, after_end and close, got 'deepseek'"));
    nlohmann::json noOpen = custom;
    noOpen["open"] = "";
    CHECK_THROWS_WITH(load("settings_thinking_no_open", noOpen), ContainsSubstring("thinking_format 'open' must not be empty"));
    nlohmann::json noEnd = custom;
    noEnd["end"] = "";
    CHECK_THROWS_WITH(load("settings_thinking_no_end", noEnd), ContainsSubstring("thinking_format 'end' must not be empty"));
    nlohmann::json badClose = custom;
    badClose["close"] = "Answer now.";
    CHECK_THROWS_WITH(load("settings_thinking_bad_close", badClose),
                      ContainsSubstring("thinking_format 'close' must end with 'end' followed by 'after_end'"));
    nlohmann::json extra = custom;
    extra["nudge"] = "x";
    CHECK_THROWS_WITH(load("settings_thinking_extra", extra), ContainsSubstring("unknown key 'nudge' in thinking_format"));
    nlohmann::json missing = custom;
    missing.erase("skip");
    CHECK_THROWS_WITH(load("settings_thinking_missing", missing), ContainsSubstring("thinking_format needs 'skip'"));

    // With no thinking, every thinking budget must be 0.
    nlohmann::json none = ValidSettingsJson();
    none["thinking_format"] = "none";
    none["budget"]["narrate_thinking_tokens"] = 128;
    CHECK_THROWS_WITH(LoadSettings(WriteSettings("settings_thinking_none_budget", none)),
                      ContainsSubstring("thinking_format is \"none\": budget 'narrate_thinking_tokens' must be 0"));
}
```

(add `#include "core/Thinking.h"`).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: `core/Thinking.h` not found.

- [ ] **Step 3: Implement**

`src/core/Thinking.h`: the Interfaces block above, with includes `<optional>`, `<string>`, `<string_view>`, a header comment explaining the block (as `src/inference/Llm.h`'s THINKING comment does today), and:

```cpp
inline const ThinkingFormat QWEN_THINKING = {
    "<think>\n",
    "<think>\n\n</think>\n\n",
    "</think>",
    "\n\n",
    "\n\nConsidering the limited time, I have to give the solution based on the thinking directly now.\n</think>\n\n",
};
```

`src/core/Thinking.cpp`:

```cpp
#include "core/Thinking.h"

namespace morpheus::core {

std::optional<ThinkingFormat> ResolveThinking(const ThinkingSetting& setting, std::string_view chatTemplate)
{
    switch (setting.mode) {
    case ThinkingMode::Auto:
        // The chat template's own enable_thinking switch marks a model that reasons in Qwen's block.
        return chatTemplate.find("enable_thinking") != std::string_view::npos ? std::optional(QWEN_THINKING)
                                                                              : std::nullopt;
    case ThinkingMode::Format:
        return setting.format;
    case ThinkingMode::None:
        return std::nullopt;
    }
    return std::nullopt; // Unreachable: every mode is handled above.
}

} // namespace morpheus::core
```

(If MSVC warns about the unreachable return under /WX, replace it with `throw ContractError("ResolveThinking: unhandled mode");` and include `core/Errors.h`.) Add `Thinking.cpp` to `src/core/CMakeLists.txt`.

`src/core/Settings.h`: include `core/Thinking.h`; `Settings` gains, after `narrateSampling`: `ThinkingSetting thinking = {}; // thinking_format: how the backends write the thinking block (Auto: as the template says).` The `LoadSettings` comment names `thinking_format` among the optional keys.

`src/core/Settings.cpp`: the top-level `checkKeys` allows `"thinking_format"`. After the budget block (before sampling):

```cpp
    // The thinking format (generation-controls spec, section 3): a preset name, or the five texts. Absent is "auto".
    if (json.contains("thinking_format")) {
        const nlohmann::json& format = json.at("thinking_format");
        if (format.is_string()) {
            const std::string name = format.get<std::string>();
            if (name == "qwen") {
                settings.thinking = {ThinkingMode::Format, QWEN_THINKING};
            } else if (name == "none") {
                settings.thinking = {ThinkingMode::None, {}};
            } else if (name != "auto") {
                throw DataError(std::format("{}: 'thinking_format' must be \"auto\", \"qwen\", \"none\" or an object "
                                            "with open, skip, end, after_end and close, got '{}'",
                                            where, name));
            }
        } else {
            checkKeys(format, "thinking_format", {"open", "skip", "end", "after_end", "close"});
            ThinkingFormat custom;
            custom.open = requireString(format, "open", "thinking_format");
            custom.skip = requireString(format, "skip", "thinking_format");
            custom.end = requireString(format, "end", "thinking_format");
            custom.afterEnd = requireString(format, "after_end", "thinking_format");
            custom.close = requireString(format, "close", "thinking_format");
            for (const auto& [key, text] : {std::pair{"open", &custom.open}, std::pair{"end", &custom.end}}) {
                if (text->empty()) {
                    throw DataError(std::format("{}: thinking_format '{}' must not be empty", where, key));
                }
            }
            // A closed block ends as the model's own does: the end marker, then what follows it.
            if (!custom.close.ends_with(custom.end + custom.afterEnd)) {
                throw DataError(std::format("{}: thinking_format 'close' must end with 'end' followed by 'after_end'",
                                            where));
            }
            settings.thinking = {ThinkingMode::Format, std::move(custom)};
        }
    }
    // With no thinking, no call may think.
    if (settings.thinking.mode == ThinkingMode::None) {
        const std::pair<const char*, int> budgets[] = {{"resolve_thinking_tokens", settings.budget.resolveThinkingTokens},
                                                       {"narrate_thinking_tokens", settings.budget.narrateThinkingTokens},
                                                       {"review_thinking_tokens", settings.budget.reviewThinkingTokens}};
        for (const auto& [key, value] : budgets) {
            if (value > 0) {
                throw DataError(std::format("{}: thinking_format is \"none\": budget '{}' must be 0", where, key));
            }
        }
    }
```

(`requireString`'s missing-key message is "thinking_format needs '<key>'", matching the test.) `SettingsToJson`:

```cpp
    // The thinking format as settings.json spells it: a preset name, or the five texts.
    if (settings.thinking.mode == ThinkingMode::Auto) {
        json["thinking_format"] = "auto";
    } else if (settings.thinking.mode == ThinkingMode::None) {
        json["thinking_format"] = "none";
    } else if (settings.thinking.format == QWEN_THINKING) {
        json["thinking_format"] = "qwen";
    } else {
        const ThinkingFormat& format = settings.thinking.format;
        json["thinking_format"] = {{"open", format.open}, {"skip", format.skip}, {"end", format.end},
                                   {"after_end", format.afterEnd}, {"close", format.close}};
    }
```

`settings.example.json`: `"thinking_format": "auto"` after `"prose_turns"`.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests --clean-first && ./build/tests/bin/Debug/morpheus_tests.exe "[settings],[thinking]"`, then `ctest --preset tests`
Expected: all pass (the "settings.example.json loads" test included).

- [ ] **Step 5: Commit**

```bash
git add src/core/Thinking.h src/core/Thinking.cpp src/core/CMakeLists.txt src/core/Settings.h src/core/Settings.cpp settings.example.json tests/core/ThinkingTests.cpp tests/core/SettingsTests.cpp
git commit -m "Thinking formats as settings data: thinking_format (auto, qwen, none or the five texts)"
```

---

