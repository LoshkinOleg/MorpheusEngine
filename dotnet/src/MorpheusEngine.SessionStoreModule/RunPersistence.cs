using System.Data;
using System.Text;
using System.Text.Json;
using Microsoft.Data.Sqlite;

namespace MorpheusEngine;

/// <summary>
/// Per-run SQLite file under game_projects/&lt;gameProjectId&gt;/saved/&lt;runId&gt;/world_state.db.
/// Mirrors the TypeScript sessionStore schema and bootstrap rules (WAL, idempotent DDL, turn-0 snapshot, optional lore seed from CSV only).
/// </summary>
internal sealed class RunPersistence
{
    private readonly string _repositoryRoot;

    // ctor
    public RunPersistence(string repositoryRoot)
    {
        _repositoryRoot = repositoryRoot;
    }

    #region Public methods
    /// <summary>
    /// Creates session directory, opens DB, applies schema, meta, turn-0 snapshot, and optional lore seed from lore/default_lore_entries.csv only.
    /// Called from SessionStoreHost when the host binds the run for this process.
    /// </summary>
    public InitializeModuleResponse InitializeRun(string gameProjectId, string runId)
    {
        RequireSafePathSegment(nameof(gameProjectId), gameProjectId);
        RequireSafePathSegment(nameof(runId), runId);

        var dbPath = GetDbPath(gameProjectId, runId);
        var sessionDir = Path.GetDirectoryName(dbPath) ?? throw new InvalidOperationException("Failed to resolve session directory.");

        Directory.CreateDirectory(sessionDir);

        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        SetMeta(connection, "run_id", runId);
        SetMeta(connection, "game_project_id", gameProjectId);

        // Q: why do we need a turn 0? Why can't the player's actual first turn be turn 0? Is it to make the engine generate an opening message to present to the player or something?
        using (var cmd = connection.CreateCommand())
        {
            cmd.CommandText =
                """
                INSERT OR IGNORE INTO snapshots (turn, world_state, view_state)
                VALUES (@turn, @world, @view);
                """;
            cmd.Parameters.AddWithValue("@turn", 0);
            cmd.Parameters.AddWithValue(
                "@world",
                JsonSerializer.Serialize(new
                {
                    gameProjectId,
                    entities = Array.Empty<object>(),
                    facts = Array.Empty<object>(),
                    anchors = Array.Empty<object>()
                }));
            cmd.Parameters.AddWithValue(
                "@view",
                JsonSerializer.Serialize(new { player = new { observations = Array.Empty<object>() } }));
            cmd.ExecuteNonQuery();
        }

        // Lore seed: default_lore_entries.csv under game_projects/&lt;id&gt;/lore/ only.
        var loreDir = Path.Combine(GetGameProjectsRoot(), gameProjectId, "lore");
        var csvPath = Path.Combine(loreDir, "default_lore_entries.csv");

        try
        {
            if (!File.Exists(csvPath))
            {
                if (Directory.Exists(loreDir))
                {
                    Console.WriteLine(
                        $"[SessionStore] WARNING: No default_lore_entries.csv under '{loreDir}' for game project '{gameProjectId}'. Lore table will not be seeded from disk.");
                }
            }
            else
            {
                var lines = File.ReadAllLines(csvPath)
                    .Select(static line => line.Trim())
                    .Where(static line => line.Length > 0 && !line.StartsWith('#'))
                    .ToArray();
                if (lines.Length > 0)
                {
                    var headers = ParseCsvLine(lines[0]).Select(static h => h.ToLowerInvariant()).ToArray();
                    var subjectIndex = Array.IndexOf(headers, "subject");
                    var dataIndex = Array.FindIndex(
                        headers,
                        static h => h is "data" or "description" or "entry");
                    if (subjectIndex >= 0 && dataIndex >= 0)
                    {
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

                            UpsertLore(connection, subject, data, "lore/default_lore_entries.csv");
                        }
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[SessionStore] Lore CSV seed failed: {ex.Message}");
        }

        return new InitializeModuleResponse(true);
    }

    /// <summary>
    /// Inserts player_input and module_trace events plus a snapshot row for this turn.
    /// Re-checks sequencing inside the transaction (fail fast).
    /// </summary>
    public TurnPersistResponse PersistTurn(string gameProjectId, string runId, TurnPersistRequest request)
    {
        RequireSafePathSegment(nameof(gameProjectId), gameProjectId);
        RequireSafePathSegment(nameof(runId), runId);
        if (request.Turn < 1)
        {
            throw new InvalidOperationException("Turn must be >= 1.");
        }

        var dbPath = GetDbPath(gameProjectId, runId);
        if (!File.Exists(dbPath))
        {
            throw new InvalidOperationException(
                "Run database not found; the host must bind the run before persisting turns.");
        }

        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);

        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            var maxSnapshotTurn = ReadMaxSnapshotTurn(connection, transaction);
            var expectedTurn = maxSnapshotTurn + 1;
            if (request.Turn != expectedTurn)
            {
                throw new InvalidOperationException(
                    $"Turn sequencing violation on persist: turn {request.Turn} but expected {expectedTurn}.");
            }

            var playerPayload = JsonSerializer.Serialize(new { text = request.PlayerInput });
            InsertEvent(connection, transaction, request.Turn, "player_input", playerPayload);

            var tracePayload = BuildModuleTracePayload(request.PlayerInput, request.IntentResponseBody);
            InsertEvent(connection, transaction, request.Turn, "module_trace", tracePayload);

            var worldState = ReadLatestWorldState(connection, transaction);
            var viewState = BuildViewStateEnvelope(request.IntentResponseBody);
            InsertSnapshot(connection, transaction, request.Turn, worldState, viewState);

            transaction.Commit();
        }
        catch
        {
            transaction.Rollback();
            throw;
        }

        return new TurnPersistResponse(true);
    }
    #endregion

    #region db I/O
    // First migration.
    private static void InitializeSessionSchema(SqliteConnection connection)
    {
        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            CREATE TABLE IF NOT EXISTS meta (
              key TEXT PRIMARY KEY,
              value TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS events (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              turn INTEGER NOT NULL,
              event_type TEXT NOT NULL,
              payload TEXT NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS snapshots (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              turn INTEGER NOT NULL,
              world_state TEXT NOT NULL,
              view_state TEXT NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS lore (
              subject TEXT PRIMARY KEY,
              data TEXT NOT NULL,
              source TEXT NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS turn_execution (
              run_id TEXT NOT NULL,
              turn INTEGER NOT NULL,
              mode TEXT NOT NULL,
              cursor INTEGER NOT NULL DEFAULT 0,
              completed INTEGER NOT NULL DEFAULT 0,
              player_input TEXT NOT NULL,
              player_id TEXT NOT NULL,
              request_id TEXT NOT NULL,
              game_project_id TEXT NOT NULL,
              checkpoint TEXT NOT NULL DEFAULT '{}',
              result TEXT NOT NULL DEFAULT '{}',
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
              updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
              PRIMARY KEY (run_id, turn)
            );

            CREATE TABLE IF NOT EXISTS pipeline_events (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              run_id TEXT NOT NULL,
              turn INTEGER NOT NULL,
              step_number INTEGER NOT NULL,
              payload TEXT NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );
            """;
        cmd.ExecuteNonQuery();
    }
    private static void SetMeta(SqliteConnection connection, string key, string value)
    {
        using var cmd = connection.CreateCommand();
        cmd.CommandText = "INSERT OR REPLACE INTO meta (key, value) VALUES (@k, @v);";
        cmd.Parameters.AddWithValue("@k", key);
        cmd.Parameters.AddWithValue("@v", value);
        cmd.ExecuteNonQuery();
    }
    private static void InsertEvent(
        SqliteConnection connection,
        SqliteTransaction transaction,
        int turn,
        string eventType,
        string payload)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText = "INSERT INTO events (turn, event_type, payload) VALUES (@t, @type, @payload);";
        cmd.Parameters.AddWithValue("@t", turn);
        cmd.Parameters.AddWithValue("@type", eventType);
        cmd.Parameters.AddWithValue("@payload", payload);
        cmd.ExecuteNonQuery();
    }

    private static string ReadLatestWorldState(SqliteConnection connection, SqliteTransaction transaction)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            SELECT world_state FROM snapshots
            ORDER BY turn DESC, id DESC
            LIMIT 1;
            """;
        var result = cmd.ExecuteScalar();
        if (result is string s && s.Length > 0)
        {
            return s;
        }

        return "{}";
    }
    private static void InsertSnapshot(
        SqliteConnection connection,
        SqliteTransaction transaction,
        int turn,
        string worldState,
        string viewState)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText = "INSERT INTO snapshots (turn, world_state, view_state) VALUES (@turn, @w, @v);";
        cmd.Parameters.AddWithValue("@turn", turn);
        cmd.Parameters.AddWithValue("@w", worldState);
        cmd.Parameters.AddWithValue("@v", viewState);
        cmd.ExecuteNonQuery();
    }
    private static void UpsertLore(SqliteConnection connection, string subject, string data, string source)
    {
        using var cmd = connection.CreateCommand();
        cmd.CommandText = "INSERT OR REPLACE INTO lore (subject, data, source) VALUES (@s, @d, @src);";
        cmd.Parameters.AddWithValue("@s", subject);
        cmd.Parameters.AddWithValue("@d", data);
        cmd.Parameters.AddWithValue("@src", source);
        cmd.ExecuteNonQuery();
    }
    #endregion

    #region Helpers
    private static void RequireSafePathSegment(string name, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            throw new ArgumentException($"{name} must be non-empty.");
        }

        var trimmed = value.Trim();
        if (!string.Equals(trimmed, value, StringComparison.Ordinal))
        {
            throw new ArgumentException($"{name} must not have leading or trailing whitespace.");
        }

        if (value.Contains("..", StringComparison.Ordinal))
        {
            throw new ArgumentException($"{name} must not contain '..'.");
        }

        if (value.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar, '/', '\\']) >= 0)
        {
            throw new ArgumentException($"{name} must not contain path separators.");
        }
    }
    private static int ReadMaxSnapshotTurn(SqliteConnection connection, SqliteTransaction? transaction = null)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText = "SELECT MAX(turn) FROM snapshots;";
        var scalar = cmd.ExecuteScalar();
        if (scalar is null || scalar is DBNull)
        {
            return 0;
        }

        var asLong = Convert.ToInt64(scalar);
        return (int)asLong;
    }
    private static string BuildModuleTracePayload(string playerInput, string intentResponseBody)
    {
        static bool tryBuildNarrationFromIntent(string body, out string narration)
        {
            narration = string.Empty;
            try
            {
                var parsed = JsonSerializer.Deserialize<IntentResponse>(body);
                if (parsed is null || !parsed.Ok)
                {
                    return false;
                }

                var lines = new List<string> { $"Intent: {parsed.Intent}" };
                var parameters = parsed.Parameters ?? new Dictionary<string, string>();
                if (parameters.Count == 0)
                {
                    lines.Add("Params: (none)");
                }
                else
                {
                    lines.Add("Params:");
                    foreach (var parameter in parameters)
                    {
                        lines.Add($"- {parameter.Key}: {parameter.Value}");
                    }
                }

                narration = string.Join(Environment.NewLine, lines);
                return true;
            }
            catch (JsonException)
            {
                return false;
            }
        }

        var narrationText = tryBuildNarrationFromIntent(intentResponseBody, out var narration)
            ? narration
            : "Intent extractor returned a non-standard response.";

        return JsonSerializer.Serialize(new
        {
            narrationText,
            intentExtractorRaw = intentResponseBody,
            playerInputEcho = playerInput
        });
    }
    private string GetGameProjectsRoot() => Path.Combine(_repositoryRoot, "game_projects");

    private string GetDbPath(string gameProjectId, string runId) =>
        Path.Combine(GetGameProjectsRoot(), gameProjectId, "saved", runId, "world_state.db");

    private static SqliteConnection OpenConnection(string dbPath)
    {
        var builder = new SqliteConnectionStringBuilder
        {
            DataSource = dbPath,
            Mode = SqliteOpenMode.ReadWriteCreate
        };
        var connection = new SqliteConnection(builder.ConnectionString);
        connection.Open();

        // Q (answered): This is not redundant with the connection string — SQLite applies journal_mode per connection.
        // We set WAL here on every open so the file always uses WAL even if an older build created it with a different mode (mirrors TS openSessionDb).
        using (var pragma = connection.CreateCommand())
        {
            pragma.CommandText = "PRAGMA journal_mode=WAL;";
            pragma.ExecuteNonQuery();
        }

        return connection;
    }

    private static string BuildViewStateEnvelope(string intentResponseBody)
    {
        try
        {
            using var parsed = JsonDocument.Parse(intentResponseBody);
            return JsonSerializer.Serialize(new { intentExtractorResponse = parsed.RootElement.Clone() });
        }
        catch (JsonException)
        {
            return JsonSerializer.Serialize(new { intentExtractorRawText = intentResponseBody });
        }
    }
    /// <summary>Minimal CSV line parser mirroring TS parseCsvLine (quoted fields, doubled quotes).</summary>
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
    #endregion
}
