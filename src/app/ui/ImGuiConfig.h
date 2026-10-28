#pragma once

// Dear ImGui's build configuration (IMGUI_USER_CONFIG), set by the imgui_core target for everything that compiles
// or includes ImGui.

#include <stdexcept>
#include <string>

// A failed ImGui assertion is a UI bug (an unbalanced Begin/End, a broken ID stack). Throwing fails loud with the
// expression and location, and lets a headless test report it instead of aborting the test run.
#define IM_ASSERT(expr)                                                                                               \
    ((expr) ? static_cast<void>(0)                                                                                    \
            : throw std::logic_error(std::string("ImGui assertion failed: ") + #expr + " [" + __FILE__ + ":" +       \
                                     std::to_string(__LINE__) + "]"))

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
