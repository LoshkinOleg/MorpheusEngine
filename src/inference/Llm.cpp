#include "inference/Llm.h"

#include "core/Errors.h"

namespace morpheus::inference {

std::string RoleName(Role role)
{
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    }
    throw core::ContractError("RoleName: unhandled Role");
}

Sampling SamplingFrom(const core::SamplingSettings& settings, std::uint32_t seed)
{
    Sampling sampling;
    sampling.temperature = static_cast<float>(settings.temperature);
    sampling.topK = settings.topK;
    sampling.topP = static_cast<float>(settings.topP);
    sampling.seed = seed;
    return sampling;
}

} // namespace morpheus::inference
