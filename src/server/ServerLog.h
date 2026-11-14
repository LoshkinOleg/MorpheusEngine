#pragma once

#include <functional>
#include <string>

namespace morpheus::server {

// Where the server's layers write a line for the operator: the executable writes them to stderr, tests keep them.
// Called from any thread, so the function must be thread-safe.
using Log = std::function<void(const std::string& line)>;

} // namespace morpheus::server
