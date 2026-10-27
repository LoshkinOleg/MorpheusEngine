#pragma once

#include <stdexcept>
#include <string>

namespace morpheus::core {

// Thrown when engine code breaks one of its own invariants. Always a bug in the engine, never bad input.
class ContractError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// Thrown when world files or saved data are malformed. The message names the file and field at fault.
class DataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace morpheus::core

// Fails fast and loud on a broken engine contract: throws ContractError carrying `message` and the source location.
#define MORPHEUS_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            throw ::morpheus::core::ContractError( \
                std::string(message) + " [" __FILE__ ":" + std::to_string(__LINE__) + "]"); \
        } \
    } while (false)
