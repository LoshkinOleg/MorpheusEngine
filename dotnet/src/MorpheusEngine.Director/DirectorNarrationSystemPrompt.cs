using System.Text;

namespace MorpheusEngine;

/// <summary>
/// Builds the Director narration system string from game project files (instructions + canon lore CSV).
/// </summary>
public static class DirectorNarrationSystemPrompt
{
    /// <summary>
    /// Reads game_projects/<paramref name="gameProjectId"/>/system/instructions.md and lore/default_lore_entries.csv.
    /// </summary>
    /// <exception cref="FileNotFoundException">Required file missing.</exception>
    /// <exception cref="InvalidOperationException">Lore CSV empty or invalid headers.</exception>
    public static string Build(string repositoryRoot, string gameProjectId)
    {
        var instructionsPath = Path.Combine(repositoryRoot, "game_projects", gameProjectId, "system", "instructions.md");
        var loreCsvPath = Path.Combine(repositoryRoot, "game_projects", gameProjectId, "lore", "default_lore_entries.csv");

        if (!File.Exists(instructionsPath))
        {
            throw new FileNotFoundException($"Narration system prompt requires instructions at '{instructionsPath}'.", instructionsPath);
        }

        if (!File.Exists(loreCsvPath))
        {
            throw new FileNotFoundException($"Narration system prompt requires lore CSV at '{loreCsvPath}'.", loreCsvPath);
        }

        var instructions = File.ReadAllText(instructionsPath).Trim();
        var loreSection = BuildCanonLoreSectionFromCsv(loreCsvPath);

        return instructions + Environment.NewLine + Environment.NewLine + loreSection;
    }

    /// <summary>
    /// Parses default_lore_entries.csv (subject + data columns) into a markdown bullet list under ## Canon Lore.
    /// Logical CSV rows follow RFC 4180 rules (newlines inside quoted fields do not end a row).
    /// </summary>
    private static string BuildCanonLoreSectionFromCsv(string csvPath)
    {
        var text = File.ReadAllText(csvPath);
        // Trim only for comment detection / blank rows; do not trim record text so quoted fields keep boundary spaces.
        var lines = CsvRfc4180.SplitRecords(text)
            .Where(static line => !string.IsNullOrWhiteSpace(line) && !line.TrimStart().StartsWith("#", StringComparison.Ordinal))
            .ToArray();

        if (lines.Length == 0)
        {
            throw new InvalidOperationException($"Lore CSV at '{csvPath}' is empty.");
        }

        var headers = CsvRfc4180.ParseRecordFields(lines[0]).Select(static h => h.ToLowerInvariant()).ToArray();
        var subjectIndex = Array.IndexOf(headers, "subject");
        var dataIndex = Array.FindIndex(
            headers,
            static h => h is "data" or "description" or "entry");
        if (subjectIndex < 0 || dataIndex < 0)
        {
            throw new InvalidOperationException(
                $"Lore CSV at '{csvPath}' must declare 'subject' and 'data' (or description/entry) columns.");
        }

        var sb = new StringBuilder();
        sb.AppendLine("## Canon Lore");
        sb.AppendLine();

        for (var i = 1; i < lines.Length; i++)
        {
            var columns = CsvRfc4180.ParseRecordFields(lines[i]);
            if (subjectIndex >= columns.Count || dataIndex >= columns.Count)
            {
                continue;
            }

            var subject = columns[subjectIndex];
            var data = columns[dataIndex];
            if (string.IsNullOrWhiteSpace(subject) || string.IsNullOrWhiteSpace(data))
            {
                continue;
            }

            sb.Append("- **");
            sb.Append(subject);
            sb.Append(":** ");
            sb.Append(data);
            sb.AppendLine();
        }

        return sb.ToString().TrimEnd();
    }
}
