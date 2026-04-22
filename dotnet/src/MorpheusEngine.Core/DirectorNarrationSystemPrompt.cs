using System.Text;

namespace MorpheusEngine;

/// <summary>
/// Builds the Director narration system string from game project files (instructions + canon lore CSV).
/// Shared by the Director module and the LLM provider warm-up so content cannot drift.
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
    /// </summary>
    private static string BuildCanonLoreSectionFromCsv(string csvPath)
    {
        var lines = File.ReadAllLines(csvPath)
            .Select(static line => line.Trim())
            .Where(static line => line.Length > 0 && !line.StartsWith("#", StringComparison.Ordinal))
            .ToArray();

        if (lines.Length == 0)
        {
            throw new InvalidOperationException($"Lore CSV at '{csvPath}' is empty.");
        }

        var headers = ParseCsvLine(lines[0]).Select(static h => h.ToLowerInvariant()).ToArray();
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
            var columns = ParseCsvLine(lines[i]);
            if (subjectIndex >= columns.Count || dataIndex >= columns.Count)
            {
                continue;
            }

            var subject = columns[subjectIndex].Trim();
            var data = columns[dataIndex].Trim();
            if (subject.Length == 0 || data.Length == 0)
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

    /// <summary>Minimal CSV line parser mirroring RunPersistence.ParseCsvLine (quoted fields, doubled quotes).</summary>
    private static List<string> ParseCsvLine(string line)
    {
        var values = new List<string>();
        var current = new StringBuilder();
        var inQuotes = false;
        for (var i = 0; i < line.Length; i++)
        {
            var ch = line[i];
            if (ch == '"')
            {
                if (inQuotes && i + 1 < line.Length && line[i + 1] == '"')
                {
                    current.Append('"');
                    i++;
                }
                else
                {
                    inQuotes = !inQuotes;
                }

                continue;
            }

            if (ch == ',' && !inQuotes)
            {
                values.Add(current.ToString().Trim());
                current.Clear();
                continue;
            }

            current.Append(ch);
        }

        values.Add(current.ToString().Trim());
        for (var v = 0; v < values.Count; v++)
        {
            var s = values[v];
            if (s.Length >= 2 && s[0] == '"' && s[^1] == '"')
            {
                values[v] = s.Substring(1, s.Length - 2).Trim();
            }
        }

        return values;
    }
}
