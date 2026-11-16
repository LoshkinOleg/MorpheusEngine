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

TEST_CASE("HasThinkingSwitch: the chat template names Qwen's enable_thinking switch", "[thinking]")
{
    CHECK(HasThinkingSwitch("{%- if enable_thinking is defined %}...{%- endif %}"));
    CHECK_FALSE(HasThinkingSwitch("{% for message in messages %}{{ message.content }}{% endfor %}"));
    CHECK_FALSE(HasThinkingSwitch(""));
}
