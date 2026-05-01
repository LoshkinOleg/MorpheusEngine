using System.Text;

namespace MorpheusEngine;

/// <summary>
/// Minimal RFC 4180–style CSV helpers: record boundaries honor quoted fields (newlines inside quotes do not end a row).
/// </summary>
public static class CsvRfc4180
{
    /// <summary>
    /// Splits full file text into logical CSV records. Newlines and CR/LF outside double quotes end a record; doubled quotes ("") stay inside the field.
    /// </summary>
    /// <exception cref="InvalidOperationException">Unclosed double quote before end of input.</exception>
    public static List<string> SplitRecords(string text)
    {
        if (text.Length == 0)
        {
            return [];
        }

        var records = new List<string>();
        var current = new StringBuilder();
        var inQuotes = false;

        for (var i = 0; i < text.Length; i++)
        {
            var ch = text[i];

            if (ch == '"')
            {
                if (inQuotes && i + 1 < text.Length && text[i + 1] == '"')
                {
                    current.Append('"');
                    current.Append('"');
                    i++;
                    continue;
                }

                inQuotes = !inQuotes;
                current.Append(ch);
                continue;
            }

            if (!inQuotes && (ch == '\n' || ch == '\r'))
            {
                records.Add(current.ToString());
                current.Clear();
                if (ch == '\r' && i + 1 < text.Length && text[i + 1] == '\n')
                {
                    i++;
                }

                continue;
            }

            current.Append(ch);
        }

        if (inQuotes)
        {
            throw new InvalidOperationException("CSV has an unclosed double-quoted field (record boundary cannot be determined).");
        }

        records.Add(current.ToString());
        return records;
    }

    /// <summary>
    /// Parses one logical CSV record into fields (quoted fields, commas, doubled quotes). Newlines inside the record string are literal field content.
    /// Leading and trailing whitespace are trimmed only for fields that were not opened with a delimiter quote (RFC-style quoted fields keep interior edge spaces).
    /// </summary>
    public static List<string> ParseRecordFields(string record)
    {
        var values = new List<string>();
        var current = new StringBuilder();
        var inQuotes = false;
        var fieldWasQuoted = false;

        void EndField()
        {
            var raw = current.ToString();
            values.Add(fieldWasQuoted ? raw : raw.Trim());
            current.Clear();
            fieldWasQuoted = false;
        }

        for (var i = 0; i < record.Length; i++)
        {
            var ch = record[i];
            if (ch == '"')
            {
                if (inQuotes && i + 1 < record.Length && record[i + 1] == '"')
                {
                    current.Append('"');
                    i++;
                }
                else
                {
                    if (!inQuotes && current.Length == 0)
                    {
                        fieldWasQuoted = true;
                    }

                    inQuotes = !inQuotes;
                }

                continue;
            }

            if (ch == ',' && !inQuotes)
            {
                EndField();
                continue;
            }

            current.Append(ch);
        }

        EndField();

        return values;
    }
}
