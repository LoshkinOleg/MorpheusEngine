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
