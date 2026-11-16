#include <catch2/catch_test_macros.hpp>

#include "core/Settings.h"
#include "turn/Session.h"

using namespace morpheus;

TEST_CASE("SamplingFrom converts settings and carries the seed", "[turn]")
{
    const core::SamplingSettings settings = {0.25, 30, 0.5};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 77);
    CHECK(sampling.temperature == 0.25f);
    CHECK(sampling.topK == 30);
    CHECK(sampling.topP == 0.5f);
    CHECK(sampling.seed == 77u);
}

TEST_CASE("SamplingFrom copies min_p", "[sampling]")
{
    core::SamplingSettings settings{0.7, 30, 0.9, 0.05};
    const inference::Sampling sampling = turn::SamplingFrom(settings, 99);
    CHECK(sampling.minP == 0.05f);
    CHECK(sampling.seed == 99u);
}
