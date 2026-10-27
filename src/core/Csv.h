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
