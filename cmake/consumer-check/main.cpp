// Consumer check: a host program that links Morpheus::turn includes headers from core and turn and calls into both.
// It needs no files and no model. It exits with 0 when both calls give the expected results.
#include <cstdio>

#include "core/Dice.h"
#include "turn/Review.h"

int main()
{
    using namespace morpheus;

    // core: the usual world roll, one d20 plus a stat of 3 against 10, with a fixed seed. The die lands in 1..20 and
    // the total is the die plus the stat.
    const int statValue = 3;
    const core::DiceRule rule = core::DiceRule::Parse("1d20+stat");
    const core::CheckResult check = core::RollCheck(rule, statValue, 10, core::CheckSeed(42, 1, 0));
    const bool diceOk = check.rolls.size() == 1 && check.rolls[0] >= 1 && check.rolls[0] <= 20 &&
                        check.total == check.rolls[0] + statValue;

    // turn: a review that finds the narration consistent, with no problems, reads back as exactly that.
    const turn::Review review = turn::ParseReviewOutput(R"({"consistent":true,"problems":[]})", true, false);
    const bool reviewOk = review.consistent == true && review.problems.empty() && review.error.empty();

    if (!diceOk || !reviewOk) {
        std::fprintf(stderr, "consumer_check: unexpected result (dice %s, review %s)\n", diceOk ? "ok" : "wrong",
                     reviewOk ? "ok" : "wrong");
        return 1;
    }
    std::printf("consumer_check: ok (rolled %d, total %lld)\n", check.rolls[0], check.total);
    return 0;
}
