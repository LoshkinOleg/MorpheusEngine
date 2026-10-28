#include <catch2/catch_test_macros.hpp>

#include "core/Settings.h"
#include "inference/Llm.h"

using namespace morpheus;

TEST_CASE("SamplingFrom converts settings and carries the seed", "[inference]")
{
    const core::SamplingSettings settings = {0.25, 30, 0.5};
    const inference::Sampling sampling = inference::SamplingFrom(settings, 77);
    CHECK(sampling.temperature == 0.25f);
    CHECK(sampling.topK == 30);
    CHECK(sampling.topP == 0.5f);
    CHECK(sampling.seed == 77u);
}
