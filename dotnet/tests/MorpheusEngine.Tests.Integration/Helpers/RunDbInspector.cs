using Microsoft.Data.Sqlite;

namespace MorpheusEngine.Tests.Integration.Helpers;

internal static class RunDbInspector
{
    public sealed record ArchivalPassageRow(
        string Id,
        string Scope,
        string Source,
        string Content,
        string TagsJson,
        string? MetadataJson,
        string EmbeddingModel,
        int EmbeddingDimensions);

    public static string BuildDbPath(string repositoryRoot, string gameProjectId, string runId)
    {
        return Path.Combine(repositoryRoot, "game_projects", gameProjectId, "saved", runId, "world_state.db");
    }

    public static SqliteConnection OpenConnection(string repositoryRoot, string gameProjectId, string runId)
    {
        return OpenConnection(BuildDbPath(repositoryRoot, gameProjectId, runId));
    }

    public static SqliteConnection OpenConnection(string dbPath)
    {
        var connection = new SqliteConnection($"Data Source={dbPath}");
        connection.Open();
        return connection;
    }

    public static int CountRows(SqliteConnection connection, string tableName, string? whereClause = null)
    {
        using var command = connection.CreateCommand();
        command.CommandText = $"SELECT COUNT(*) FROM {tableName}" + (string.IsNullOrWhiteSpace(whereClause) ? ";" : $" WHERE {whereClause};");
        return Convert.ToInt32(command.ExecuteScalar(), System.Globalization.CultureInfo.InvariantCulture);
    }

    public static IReadOnlyList<(string Subject, string Data, string Source)> ReadLoreRows(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT subject, data, source
            FROM lore
            ORDER BY subject COLLATE NOCASE;
            """;

        var rows = new List<(string Subject, string Data, string Source)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetString(0), reader.GetString(1), reader.GetString(2)));
        }

        return rows;
    }

    public static IReadOnlyList<(int Turn, string EventType, string Payload)> ReadEvents(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, event_type, payload
            FROM events
            ORDER BY id ASC;
            """;

        var rows = new List<(int Turn, string EventType, string Payload)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetInt32(0), reader.GetString(1), reader.GetString(2)));
        }

        return rows;
    }

    public static IReadOnlyList<AgentMessageDto> ReadAgentMessages(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, step_number, role, message_type, content, tool_name, tool_call_id
            FROM agent_messages
            ORDER BY id ASC;
            """;

        var rows = new List<AgentMessageDto>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new AgentMessageDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                reader.GetString(2),
                reader.GetString(3),
                reader.GetString(4),
                reader.IsDBNull(5) ? null : reader.GetString(5),
                reader.IsDBNull(6) ? null : reader.GetString(6)));
        }

        return rows;
    }

    public static IReadOnlyList<(int Turn, int StepNumber, string PayloadJson)> ReadPipelineEvents(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT turn, step_number, payload
            FROM pipeline_events
            ORDER BY id ASC;
            """;

        var rows = new List<(int Turn, int StepNumber, string PayloadJson)>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add((reader.GetInt32(0), reader.GetInt32(1), reader.GetString(2)));
        }

        return rows;
    }

    public static (string WorldState, string ViewState) ReadSnapshotForTurn(SqliteConnection connection, int turn)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT world_state, view_state
            FROM snapshots
            WHERE turn = @turn
            ORDER BY id DESC
            LIMIT 1;
            """;
        command.Parameters.AddWithValue("@turn", turn);

        using var reader = command.ExecuteReader();
        if (!reader.Read())
        {
            throw new InvalidOperationException($"No snapshot row exists for turn {turn}.");
        }

        return (reader.GetString(0), reader.GetString(1));
    }

    public static IReadOnlyList<MemorySummaryDto> ReadConversationSummaries(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT start_turn, end_turn, summary, source_message_count, metadata_json
            FROM conversation_summaries
            ORDER BY end_turn ASC, start_turn ASC, id ASC;
            """;

        var rows = new List<MemorySummaryDto>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new MemorySummaryDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                reader.GetString(2),
                reader.GetInt32(3),
                reader.IsDBNull(4) ? null : reader.GetString(4)));
        }

        return rows;
    }

    public static IReadOnlyList<ArchivalPassageRow> ReadArchivalPassages(SqliteConnection connection)
    {
        using var command = connection.CreateCommand();
        command.CommandText =
            """
            SELECT id, scope, source, content, tags_json, metadata_json, embedding_model, embedding_dimensions
            FROM archival_passages
            ORDER BY created_at ASC, id ASC;
            """;

        var rows = new List<ArchivalPassageRow>();
        using var reader = command.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new ArchivalPassageRow(
                reader.GetString(0),
                reader.GetString(1),
                reader.GetString(2),
                reader.GetString(3),
                reader.GetString(4),
                reader.IsDBNull(5) ? null : reader.GetString(5),
                reader.GetString(6),
                reader.GetInt32(7)));
        }

        return rows;
    }
}
