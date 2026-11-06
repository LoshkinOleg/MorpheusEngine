### Task 2: RFC 4180 CSV parser

**Files:**
- Create: `src/core/Csv.h`, `src/core/Csv.cpp`, `tests/core/CsvTests.cpp`
- Modify: `src/core/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataError` (Task 1).
- Produces: `std::vector<std::vector<std::string>> morpheus::core::ParseCsv(std::string_view text)`.

- [ ] **Step 1: Write the failing test `tests/core/CsvTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "core/Csv.h"
#include "core/Errors.h"

using morpheus::core::DataError;
using morpheus::core::ParseCsv;
using Catch::Matchers::ContainsSubstring;
using Rows = std::vector<std::vector<std::string>>;

TEST_CASE("ParseCsv splits plain records and fields", "[csv]")
{
    CHECK(ParseCsv("a,b\nc,d\n") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv accepts CRLF endings and a missing final newline", "[csv]")
{
    CHECK(ParseCsv("a,b\r\nc,d") == Rows{{"a", "b"}, {"c", "d"}});
}

TEST_CASE("ParseCsv keeps empty fields", "[csv]")
{
    CHECK(ParseCsv("a,,c\n,\n") == Rows{{"a", "", "c"}, {"", ""}});
}

TEST_CASE("ParseCsv handles quoted fields", "[csv]")
{
    // Commas, doubled quotes and newlines inside quotes all belong to the field.
    CHECK(ParseCsv("\"x, y\",\"say \"\"hi\"\"\"\n\"line1\nline2\",z\n") ==
          Rows{{"x, y", "say \"hi\""}, {"line1\nline2", "z"}});
}

TEST_CASE("ParseCsv skips blank lines", "[csv]")
{
    CHECK(ParseCsv("a\n\n\r\nb\n") == Rows{{"a"}, {"b"}});
}

TEST_CASE("ParseCsv rejects malformed quoting with a line number", "[csv]")
{
    CHECK_THROWS_AS(ParseCsv("\"unterminated\n"), DataError);
    CHECK_THROWS_WITH(ParseCsv("a,b\nc\"d,e\n"), ContainsSubstring("line 2"));
    CHECK_THROWS_AS(ParseCsv("\"quoted\"tail\n"), DataError);
    CHECK_THROWS_AS(ParseCsv("a\rb\n"), DataError);
}
```

- [ ] **Step 2: Build. Expect failure.**

Run: `cmake --build --preset tests`
Expected: FAIL with `error C1083: Cannot open include file: 'core/Csv.h'`.

- [ ] **Step 3: Implement `src/core/Csv.h` and `src/core/Csv.cpp`, and register the source**

`src/core/Csv.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace morpheus::core {

// Parses RFC 4180 CSV text into records of fields.
// - Fields may be quoted; quoted fields can hold commas, newlines and doubled quotes ("" -> ").
// - Records end with LF or CRLF; the last record does not need a newline.
// - Blank lines are skipped, so a trailing newline never yields an empty record.
// Throws DataError with a 1-based line number on an unterminated quote, a quote inside an unquoted field,
// text after a closing quote, or a bare carriage return.
std::vector<std::vector<std::string>> ParseCsv(std::string_view text);

} // namespace morpheus::core
```

`src/core/Csv.cpp`:

```cpp
#include "core/Csv.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::core {

std::vector<std::vector<std::string>> ParseCsv(std::string_view text)
{
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> record;
    std::string field;
    bool inQuotes = false;       // Inside a quoted field: commas and newlines are literal.
    bool fieldWasQuoted = false; // Current field was quoted: only ',' or a line end may follow the closing quote.
    bool recordStarted = false;  // Current line has content, so the next line end completes a record.
    std::size_t line = 1;        // 1-based, for error messages.

    auto endField = [&]() {
        record.push_back(std::move(field));
        field.clear();
        fieldWasQuoted = false;
    };
    auto endRecord = [&]() {
        endField();
        records.push_back(std::move(record));
        record.clear();
        recordStarted = false;
    };

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (inQuotes) {
            if (c == '"') {
                // A doubled quote is an escaped quote; a single one closes the field.
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                if (c == '\n') {
                    ++line;
                }
                field.push_back(c);
            }
            continue;
        }

        switch (c) {
        case '"':
            if (!field.empty() || fieldWasQuoted) {
                throw DataError(std::format("CSV line {}: quote inside an unquoted field", line));
            }
            inQuotes = true;
            fieldWasQuoted = true;
            recordStarted = true;
            break;
        case ',':
            endField();
            recordStarted = true;
            break;
        case '\r':
            // Only valid as the first half of CRLF; the '\n' that follows ends the record.
            if (i + 1 >= text.size() || text[i + 1] != '\n') {
                throw DataError(std::format("CSV line {}: bare carriage return", line));
            }
            break;
        case '\n':
            // Blank lines are skipped rather than turned into empty records.
            if (recordStarted) {
                endRecord();
            }
            ++line;
            break;
        default:
            if (fieldWasQuoted) {
                throw DataError(std::format("CSV line {}: text after a closing quote", line));
            }
            field.push_back(c);
            recordStarted = true;
            break;
        }
    }

    if (inQuotes) {
        throw DataError(std::format("CSV line {}: unterminated quoted field", line));
    }
    if (recordStarted) {
        endRecord();
    }
    return records;
}

} // namespace morpheus::core
```

Replace `src/core/CMakeLists.txt` with:

```cmake
add_library(morpheus_core STATIC
    ContentHash.cpp
    Csv.cpp
)
target_include_directories(morpheus_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_core PUBLIC nlohmann_json::nlohmann_json)
morpheus_target_defaults(morpheus_core)
```

- [ ] **Step 4: Build and run the tests. Expect them to pass.**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass (2 hash + 6 csv).

- [ ] **Step 5: Commit**

```bash
git add src/core tests/core/CsvTests.cpp
git commit -m "Add RFC 4180 CSV parser" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

