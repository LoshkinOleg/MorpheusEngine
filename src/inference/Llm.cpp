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

} // namespace morpheus::inference
