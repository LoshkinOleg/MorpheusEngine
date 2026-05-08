using System.Data;
using System.Security.Cryptography;
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
    #region Nested types
    public sealed record ArchivalSeedCandidate(
        string Id,
        string Scope,
        string Source,
        string Content,
        IReadOnlyList<string> Tags,
        string MetadataJson);

    private sealed record LoreSeedEntry(string Subject, string Data);
    #endregion

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
        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId must be non-empty.", nameof(runId));
        }

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
        try
        {
            foreach (var entry in ReadLoreSeedEntries(gameProjectId))
            {
                UpsertLore(connection, entry.Subject, entry.Data, "lore/default_lore_entries.csv");
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
        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId must be non-empty.", nameof(runId));
        }

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

            var tracePayload = BuildModuleTracePayload(request.PlayerInput, request.DirectorResponseBody);
            InsertEvent(connection, transaction, request.Turn, "module_trace", tracePayload);

            var worldState = ReadLatestWorldState(connection, transaction);
            var viewState = BuildViewStateEnvelope(request.DirectorResponseBody);
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

    public MemoryLoadContextResponse LoadMemoryContext(string gameProjectId, string runId, MemoryLoadContextRequest request, MemoryBudgetDto budget)
    {
        if (request.Turn < 1)
        {
            throw new InvalidOperationException("Turn must be >= 1.");
        }

        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);

        return new MemoryLoadContextResponse(
            true,
            ReadMemoryBlocks(connection, includeReadOnly: true),
            ReadRecentMessages(connection, request.MaxFullMessages, roles: null),
            ReadLatestSnapshot(connection),
            budget,
            ReadRecentSummaries(connection, limit: 5));
    }

    public MemoryPersistStepResponse PersistMemoryStep(string gameProjectId, string runId, MemoryPersistStepRequest request)
    {
        if (request.Turn < 1)
        {
            throw new InvalidOperationException("Turn must be >= 1.");
        }

        if (request.StepNumber < 0)
        {
            throw new InvalidOperationException("stepNumber must be >= 0.");
        }

        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);

        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            foreach (var message in request.Messages)
            {
                InsertAgentMessage(connection, transaction, runId, message);
            }

            foreach (var mutation in request.Mutations)
            {
                InsertMemoryMutation(connection, transaction, runId, mutation);
            }

            foreach (var block in request.BlockUpdates)
            {
                UpsertMemoryBlock(connection, transaction, block);
            }

            if (request.ContextAccounting is not null)
            {
                InsertPipelineEvent(
                    connection,
                    transaction,
                    runId,
                    request.Turn,
                    request.StepNumber,
                    JsonSerializer.Serialize(new { eventType = "memory_context_budget", accounting = request.ContextAccounting }));
            }

            transaction.Commit();
        }
        catch
        {
            transaction.Rollback();
            throw;
        }

        return new MemoryPersistStepResponse(true);
    }

    public MemoryBlocksGetAllResponse GetMemoryBlocks(string gameProjectId, string runId, MemoryBlocksGetAllRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        return new MemoryBlocksGetAllResponse(true, ReadMemoryBlocks(connection, request.IncludeReadOnly));
    }

    public MemoryBlockUpsertResponse UpsertMemoryBlock(string gameProjectId, string runId, MemoryBlockUpsertRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            UpsertMemoryBlock(connection, transaction, request.Block);
            transaction.Commit();
        }
        catch
        {
            transaction.Rollback();
            throw;
        }

        return new MemoryBlockUpsertResponse(true);
    }

    public MemoryMessagesRecentResponse GetRecentMessages(string gameProjectId, string runId, MemoryMessagesRecentRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        return new MemoryMessagesRecentResponse(true, ReadRecentMessages(connection, request.Limit, request.Roles));
    }

    public MemoryMessageAppendResponse AppendMessage(string gameProjectId, string runId, MemoryMessageAppendRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            InsertAgentMessage(connection, transaction, runId, request.Message);
            transaction.Commit();
        }
        catch
        {
            transaction.Rollback();
            throw;
        }

        return new MemoryMessageAppendResponse(true);
    }

    public MemoryMutationAppendResponse AppendMutation(string gameProjectId, string runId, MemoryMutationAppendRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            InsertMemoryMutation(connection, transaction, runId, request.Mutation);
            transaction.Commit();
        }
        catch
        {
            transaction.Rollback();
            throw;
        }

        return new MemoryMutationAppendResponse(true);
    }

    public MemorySnapshotLatestResponse GetLatestSnapshot(string gameProjectId, string runId)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        return new MemorySnapshotLatestResponse(true, ReadLatestSnapshot(connection));
    }

    public MemoryPipelineEventsRecentResponse GetRecentPipelineEvents(string gameProjectId, string runId, MemoryPipelineEventsRecentRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        return new MemoryPipelineEventsRecentResponse(true, ReadRecentPipelineEvents(connection, request));
    }

    public MemoryRecallSearchResponse SearchRecall(string gameProjectId, string runId, MemoryRecallSearchRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        var query = request.Query?.Trim() ?? string.Empty;
        if (query.Length == 0)
        {
            return new MemoryRecallSearchResponse(true, []);
        }

        using var cmd = connection.CreateCommand();
        var roles = NormalizeRoles(request.Roles);
        var roleClause = string.Empty;
        if (roles.Length > 0)
        {
            var roleParameters = new List<string>();
            for (var i = 0; i < roles.Length; i++)
            {
                var parameterName = "@role" + i;
                roleParameters.Add(parameterName);
                cmd.Parameters.AddWithValue(parameterName, roles[i]);
            }

            roleClause = $" AND m.role IN ({string.Join(", ", roleParameters)})";
        }

        cmd.CommandText =
            $"""
            SELECT m.id, m.turn, m.step_number, m.role, m.message_type, m.content, m.tool_name, bm25(agent_messages_fts) AS score
            FROM agent_messages_fts f
            JOIN agent_messages m ON m.id = f.rowid
            WHERE agent_messages_fts MATCH @query{roleClause}
            ORDER BY score ASC, m.turn DESC, m.step_number DESC, m.id DESC
            LIMIT @limit;
            """;
        cmd.Parameters.AddWithValue("@query", BuildFtsQuery(query));
        cmd.Parameters.AddWithValue("@limit", Math.Clamp(request.Limit, 1, 50));

        var results = new List<MemorySearchResultDto>();
        using var reader = cmd.ExecuteReader();
        while (reader.Read())
        {
            var id = reader.GetInt64(0).ToString();
            var metadata = JsonSerializer.Serialize(new
            {
                turn = reader.GetInt32(1),
                stepNumber = reader.GetInt32(2),
                role = reader.GetString(3),
                messageType = reader.GetString(4),
                toolName = reader.IsDBNull(6) ? null : reader.GetString(6)
            });
            results.Add(new MemorySearchResultDto(id, reader.GetString(5), "recall", reader.GetDouble(7), metadata));
        }

        return new MemoryRecallSearchResponse(true, results);
    }

    public MemoryArchivalSearchResponse SearchArchival(string gameProjectId, string runId, MemoryArchivalSearchRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        var query = request.Query?.Trim() ?? string.Empty;
        if (query.Length == 0)
        {
            return new MemoryArchivalSearchResponse(true, []);
        }

        if (request.QueryEmbedding is null || request.QueryEmbedding.Count == 0)
        {
            throw new InvalidOperationException("Archival search requires a non-empty queryEmbedding.");
        }

        if (string.IsNullOrWhiteSpace(request.EmbeddingModel))
        {
            throw new InvalidOperationException("Archival search requires embeddingModel.");
        }

        var queryEmbedding = request.QueryEmbedding.ToArray();
        var tags = NormalizeTags(request.Tags);
        var topK = Math.Clamp(request.TopK, 1, 50);
        var results = new List<(MemorySearchResultDto Result, string CreatedAt)>();

        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            SELECT id, scope, source, content, tags_json, metadata_json, embedding_model, embedding_dimensions, embedding_json, created_at
            FROM archival_passages
            WHERE embedding_model = @embeddingModel
              AND embedding_dimensions = @dimensions;
            """;
        cmd.Parameters.AddWithValue("@embeddingModel", request.EmbeddingModel.Trim());
        cmd.Parameters.AddWithValue("@dimensions", queryEmbedding.Length);

        using var reader = cmd.ExecuteReader();
        while (reader.Read())
        {
            var passageTags = DeserializeStringArray(reader.GetString(4));
            if (tags.Length > 0 && !TagsContainAll(passageTags, tags))
            {
                continue;
            }

            var embedding = DeserializeFloatArray(reader.GetString(8));
            if (embedding.Length != queryEmbedding.Length)
            {
                continue;
            }

            var score = CosineSimilarity(queryEmbedding, embedding);
            var metadata = JsonSerializer.Serialize(new
            {
                scope = reader.GetString(1),
                source = reader.GetString(2),
                tags = passageTags,
                metadataJson = reader.IsDBNull(5) ? null : reader.GetString(5),
                embeddingModel = reader.GetString(6),
                embeddingDimensions = reader.GetInt32(7)
            });
            results.Add((
                new MemorySearchResultDto(reader.GetString(0), reader.GetString(3), "archival", score, metadata),
                reader.GetString(9)));
        }

        var ordered = results
            .OrderByDescending(static row => row.Result.Score ?? double.NegativeInfinity)
            .ThenByDescending(static row => row.CreatedAt, StringComparer.Ordinal)
            .ThenBy(static row => row.Result.Id, StringComparer.Ordinal)
            .Take(topK)
            .Select(static row => row.Result)
            .ToArray();
        return new MemoryArchivalSearchResponse(true, ordered);
    }

    public MemoryArchivalUpsertResponse UpsertArchivalPassage(string gameProjectId, string runId, MemoryArchivalUpsertRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            var passage = UpsertArchivalPassage(connection, transaction, request.Passage);
            transaction.Commit();
            return new MemoryArchivalUpsertResponse(true, passage);
        }
        catch
        {
            transaction.Rollback();
            throw;
        }
    }

    public IReadOnlyList<ArchivalSeedCandidate> BuildArchivalLoreSeedCandidates(string gameProjectId, string runId)
    {
        _ = RequireRunDatabase(gameProjectId, runId);
        return ReadLoreSeedEntries(gameProjectId)
            .Select(static entry =>
            {
                var id = CreateStableArchivalId("lore/default_lore_entries.csv", entry.Subject);
                var content = $"{entry.Subject}: {entry.Data}";
                var metadata = JsonSerializer.Serialize(new { subject = entry.Subject, source = "lore/default_lore_entries.csv" });
                return new ArchivalSeedCandidate(id, "project", "lore/default_lore_entries.csv", content, ["lore", "seed"], metadata);
            })
            .ToArray();
    }

    public MemorySummariesRecentResponse GetRecentSummaries(string gameProjectId, string runId, MemorySummariesRecentRequest request)
    {
        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        return new MemorySummariesRecentResponse(true, ReadRecentSummaries(connection, request.Limit));
    }

    public MemoryCompactRecallResponse CompactRecall(string gameProjectId, string runId, MemoryCompactRecallRequest request)
    {
        if (request.StartTurn < 1 || request.EndTurn < request.StartTurn)
        {
            throw new InvalidOperationException("Compaction range must start at turn >= 1 and end at or after startTurn.");
        }

        if (string.IsNullOrWhiteSpace(request.Summary))
        {
            throw new InvalidOperationException("Compaction summary must be non-empty.");
        }

        if (request.SourceMessageCount < 1)
        {
            throw new InvalidOperationException("sourceMessageCount must be >= 1.");
        }

        var dbPath = RequireRunDatabase(gameProjectId, runId);
        using var connection = OpenConnection(dbPath);
        InitializeSessionSchema(connection);
        using var transaction = connection.BeginTransaction(IsolationLevel.Serializable);
        try
        {
            var summary = InsertConversationSummary(connection, transaction, request);
            transaction.Commit();
            return new MemoryCompactRecallResponse(true, summary);
        }
        catch
        {
            transaction.Rollback();
            throw;
        }
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

            CREATE INDEX IF NOT EXISTS idx_pipeline_events_turn_step
            ON pipeline_events (turn, step_number, id);

            CREATE TABLE IF NOT EXISTS memory_blocks (
              label TEXT PRIMARY KEY,
              description TEXT NOT NULL,
              value TEXT NOT NULL,
              char_limit INTEGER NOT NULL,
              read_only INTEGER NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
              updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS agent_messages (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              run_id TEXT NOT NULL,
              turn INTEGER NOT NULL,
              step_number INTEGER NOT NULL,
              role TEXT NOT NULL,
              message_type TEXT NOT NULL,
              content TEXT NOT NULL,
              tool_name TEXT NULL,
              tool_call_id TEXT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE INDEX IF NOT EXISTS idx_agent_messages_turn_step
            ON agent_messages (turn, step_number, id);

            CREATE INDEX IF NOT EXISTS idx_agent_messages_role
            ON agent_messages (role);

            CREATE VIRTUAL TABLE IF NOT EXISTS agent_messages_fts USING fts5(
              content,
              role UNINDEXED,
              message_type UNINDEXED,
              tool_name UNINDEXED,
              content='agent_messages',
              content_rowid='id'
            );

            CREATE TRIGGER IF NOT EXISTS agent_messages_ai AFTER INSERT ON agent_messages BEGIN
              INSERT INTO agent_messages_fts(rowid, content, role, message_type, tool_name)
              VALUES (new.id, new.content, new.role, new.message_type, COALESCE(new.tool_name, ''));
            END;

            CREATE TRIGGER IF NOT EXISTS agent_messages_ad AFTER DELETE ON agent_messages BEGIN
              INSERT INTO agent_messages_fts(agent_messages_fts, rowid, content, role, message_type, tool_name)
              VALUES ('delete', old.id, old.content, old.role, old.message_type, COALESCE(old.tool_name, ''));
            END;

            CREATE TRIGGER IF NOT EXISTS agent_messages_au AFTER UPDATE ON agent_messages BEGIN
              INSERT INTO agent_messages_fts(agent_messages_fts, rowid, content, role, message_type, tool_name)
              VALUES ('delete', old.id, old.content, old.role, old.message_type, COALESCE(old.tool_name, ''));
              INSERT INTO agent_messages_fts(rowid, content, role, message_type, tool_name)
              VALUES (new.id, new.content, new.role, new.message_type, COALESCE(new.tool_name, ''));
            END;

            CREATE TABLE IF NOT EXISTS memory_mutations (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              run_id TEXT NOT NULL,
              turn INTEGER NOT NULL,
              step_number INTEGER NOT NULL,
              tool_name TEXT NOT NULL,
              target TEXT NOT NULL,
              before_json TEXT NULL,
              after_json TEXT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE INDEX IF NOT EXISTS idx_memory_mutations_turn_step
            ON memory_mutations (turn, step_number, id);

            CREATE TABLE IF NOT EXISTS conversation_summaries (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              run_id TEXT NOT NULL,
              start_turn INTEGER NOT NULL,
              end_turn INTEGER NOT NULL,
              summary TEXT NOT NULL,
              source_message_count INTEGER NOT NULL,
              metadata_json TEXT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
              UNIQUE(start_turn, end_turn)
            );

            CREATE INDEX IF NOT EXISTS idx_conversation_summaries_turn_range
            ON conversation_summaries (start_turn, end_turn, id);

            CREATE TABLE IF NOT EXISTS archival_passages (
              id TEXT PRIMARY KEY,
              scope TEXT NOT NULL,
              source TEXT NOT NULL,
              content TEXT NOT NULL,
              tags_json TEXT NOT NULL,
              metadata_json TEXT NULL,
              embedding_model TEXT NOT NULL,
              embedding_dimensions INTEGER NOT NULL,
              embedding_json TEXT NOT NULL,
              created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
              updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );

            CREATE INDEX IF NOT EXISTS idx_archival_passages_scope
            ON archival_passages (scope);

            CREATE INDEX IF NOT EXISTS idx_archival_passages_source
            ON archival_passages (source);

            CREATE INDEX IF NOT EXISTS idx_archival_passages_created_at
            ON archival_passages (created_at);
            """;
        cmd.ExecuteNonQuery();
        using var rebuild = connection.CreateCommand();
        rebuild.CommandText = "INSERT INTO agent_messages_fts(agent_messages_fts) VALUES ('rebuild');";
        rebuild.ExecuteNonQuery();
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

    private static IReadOnlyList<MemoryBlockDto> ReadMemoryBlocks(SqliteConnection connection, bool includeReadOnly)
    {
        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            SELECT label, description, value, char_limit, read_only
            FROM memory_blocks
            WHERE @includeReadOnly = 1 OR read_only = 0
            ORDER BY label COLLATE NOCASE;
            """;
        cmd.Parameters.AddWithValue("@includeReadOnly", includeReadOnly ? 1 : 0);

        var rows = new List<MemoryBlockDto>();
        using var reader = cmd.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new MemoryBlockDto(
                reader.GetString(0),
                reader.GetString(1),
                reader.GetString(2),
                reader.GetInt32(3),
                reader.GetInt32(4) != 0));
        }

        return rows;
    }

    private static IReadOnlyList<MemorySummaryDto> ReadRecentSummaries(SqliteConnection connection, int limit)
    {
        var normalizedLimit = Math.Clamp(limit, 0, 20);
        if (normalizedLimit == 0)
        {
            return [];
        }

        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            SELECT start_turn, end_turn, summary, source_message_count, metadata_json
            FROM conversation_summaries
            ORDER BY end_turn DESC, start_turn DESC, id DESC
            LIMIT @limit;
            """;
        cmd.Parameters.AddWithValue("@limit", normalizedLimit);

        var rows = new List<MemorySummaryDto>();
        using var reader = cmd.ExecuteReader();
        while (reader.Read())
        {
            rows.Add(new MemorySummaryDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                reader.GetString(2),
                reader.GetInt32(3),
                reader.IsDBNull(4) ? null : reader.GetString(4)));
        }

        rows.Reverse();
        return rows;
    }

    private static MemorySummaryDto InsertConversationSummary(
        SqliteConnection connection,
        SqliteTransaction transaction,
        MemoryCompactRecallRequest request)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO conversation_summaries
              (run_id, start_turn, end_turn, summary, source_message_count, metadata_json)
            VALUES
              ((SELECT value FROM meta WHERE key = 'run_id'), @startTurn, @endTurn, @summary, @sourceMessageCount, @metadataJson)
            ON CONFLICT(start_turn, end_turn) DO UPDATE SET
              summary = excluded.summary,
              source_message_count = excluded.source_message_count,
              metadata_json = excluded.metadata_json;
            """;
        cmd.Parameters.AddWithValue("@startTurn", request.StartTurn);
        cmd.Parameters.AddWithValue("@endTurn", request.EndTurn);
        cmd.Parameters.AddWithValue("@summary", request.Summary.Trim());
        cmd.Parameters.AddWithValue("@sourceMessageCount", request.SourceMessageCount);
        cmd.Parameters.AddWithValue("@metadataJson", request.MetadataJson is null ? DBNull.Value : request.MetadataJson);
        cmd.ExecuteNonQuery();

        return new MemorySummaryDto(
            request.StartTurn,
            request.EndTurn,
            request.Summary.Trim(),
            request.SourceMessageCount,
            request.MetadataJson);
    }

    private static IReadOnlyList<AgentMessageDto> ReadRecentMessages(
        SqliteConnection connection,
        int limit,
        IReadOnlyList<string>? roles)
    {
        var normalizedLimit = Math.Clamp(limit, 0, 200);
        if (normalizedLimit == 0)
        {
            return [];
        }

        var roleFilter = roles?
            .Where(static role => !string.IsNullOrWhiteSpace(role))
            .Select(static role => role.Trim())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        using var cmd = connection.CreateCommand();
        if (roleFilter is { Length: > 0 })
        {
            var parameterNames = new List<string>();
            for (var i = 0; i < roleFilter.Length; i++)
            {
                var parameterName = "@role" + i;
                parameterNames.Add(parameterName);
                cmd.Parameters.AddWithValue(parameterName, roleFilter[i]);
            }

            cmd.CommandText =
                $"""
                SELECT turn, step_number, role, message_type, content, tool_name, tool_call_id
                FROM agent_messages
                WHERE role IN ({string.Join(", ", parameterNames)})
                ORDER BY turn DESC, step_number DESC, id DESC
                LIMIT @limit;
                """;
        }
        else
        {
            cmd.CommandText =
                """
                SELECT turn, step_number, role, message_type, content, tool_name, tool_call_id
                FROM agent_messages
                ORDER BY turn DESC, step_number DESC, id DESC
                LIMIT @limit;
                """;
        }

        cmd.Parameters.AddWithValue("@limit", normalizedLimit);
        var rows = new List<AgentMessageDto>();
        using var reader = cmd.ExecuteReader();
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

        rows.Reverse();
        return rows;
    }

    private static IReadOnlyList<MemoryPipelineEventDto> ReadRecentPipelineEvents(
        SqliteConnection connection,
        MemoryPipelineEventsRecentRequest request)
    {
        var normalizedLimit = Math.Clamp(request.Limit, 0, 200);
        if (normalizedLimit == 0)
        {
            return [];
        }

        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            SELECT turn, step_number, payload, created_at
            FROM pipeline_events
            ORDER BY turn DESC, step_number DESC, id DESC
            LIMIT @limit;
            """;
        cmd.Parameters.AddWithValue("@limit", normalizedLimit * 4);

        var rows = new List<MemoryPipelineEventDto>();
        using var reader = cmd.ExecuteReader();
        while (reader.Read() && rows.Count < normalizedLimit)
        {
            var payload = reader.GetString(2);
            if (!PayloadMatchesEventType(payload, request.EventType))
            {
                continue;
            }

            rows.Add(new MemoryPipelineEventDto(
                reader.GetInt32(0),
                reader.GetInt32(1),
                payload,
                reader.GetString(3)));
        }

        rows.Reverse();
        return rows;
    }

    private static LatestSnapshotDto ReadLatestSnapshot(SqliteConnection connection)
    {
        using var cmd = connection.CreateCommand();
        cmd.CommandText =
            """
            SELECT turn, world_state, view_state
            FROM snapshots
            ORDER BY turn DESC, id DESC
            LIMIT 1;
            """;
        using var reader = cmd.ExecuteReader();
        if (reader.Read())
        {
            return new LatestSnapshotDto(reader.GetInt32(0), reader.GetString(1), reader.GetString(2));
        }

        return new LatestSnapshotDto(0, "{}", "{}");
    }

    private static void InsertAgentMessage(
        SqliteConnection connection,
        SqliteTransaction transaction,
        string runId,
        AgentMessageDto message)
    {
        if (message.Turn < 1 && message.Turn != 0)
        {
            throw new InvalidOperationException("Agent message turn must be 0 for setup or >= 1.");
        }

        if (message.StepNumber < 0)
        {
            throw new InvalidOperationException("Agent message stepNumber must be >= 0.");
        }

        if (string.IsNullOrWhiteSpace(message.Role)
            || string.IsNullOrWhiteSpace(message.MessageType)
            || string.IsNullOrWhiteSpace(message.Content))
        {
            throw new InvalidOperationException("Agent message must include non-empty role, messageType, and content.");
        }

        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO agent_messages
              (run_id, turn, step_number, role, message_type, content, tool_name, tool_call_id)
            VALUES
              (@runId, @turn, @stepNumber, @role, @messageType, @content, @toolName, @toolCallId);
            """;
        cmd.Parameters.AddWithValue("@runId", runId);
        cmd.Parameters.AddWithValue("@turn", message.Turn);
        cmd.Parameters.AddWithValue("@stepNumber", message.StepNumber);
        cmd.Parameters.AddWithValue("@role", message.Role.Trim());
        cmd.Parameters.AddWithValue("@messageType", message.MessageType.Trim());
        cmd.Parameters.AddWithValue("@content", message.Content);
        cmd.Parameters.AddWithValue("@toolName", string.IsNullOrWhiteSpace(message.ToolName) ? DBNull.Value : message.ToolName.Trim());
        cmd.Parameters.AddWithValue("@toolCallId", string.IsNullOrWhiteSpace(message.ToolCallId) ? DBNull.Value : message.ToolCallId.Trim());
        cmd.ExecuteNonQuery();
    }

    private static void InsertMemoryMutation(
        SqliteConnection connection,
        SqliteTransaction transaction,
        string runId,
        MemoryMutationDto mutation)
    {
        if (mutation.Turn < 1 && mutation.Turn != 0)
        {
            throw new InvalidOperationException("Memory mutation turn must be 0 for setup or >= 1.");
        }

        if (mutation.StepNumber < 0)
        {
            throw new InvalidOperationException("Memory mutation stepNumber must be >= 0.");
        }

        if (string.IsNullOrWhiteSpace(mutation.ToolName) || string.IsNullOrWhiteSpace(mutation.Target))
        {
            throw new InvalidOperationException("Memory mutation must include non-empty toolName and target.");
        }

        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO memory_mutations
              (run_id, turn, step_number, tool_name, target, before_json, after_json)
            VALUES
              (@runId, @turn, @stepNumber, @toolName, @target, @beforeJson, @afterJson);
            """;
        cmd.Parameters.AddWithValue("@runId", runId);
        cmd.Parameters.AddWithValue("@turn", mutation.Turn);
        cmd.Parameters.AddWithValue("@stepNumber", mutation.StepNumber);
        cmd.Parameters.AddWithValue("@toolName", mutation.ToolName.Trim());
        cmd.Parameters.AddWithValue("@target", mutation.Target.Trim());
        cmd.Parameters.AddWithValue("@beforeJson", mutation.BeforeJson is null ? DBNull.Value : mutation.BeforeJson);
        cmd.Parameters.AddWithValue("@afterJson", mutation.AfterJson is null ? DBNull.Value : mutation.AfterJson);
        cmd.ExecuteNonQuery();
    }

    private static void InsertPipelineEvent(
        SqliteConnection connection,
        SqliteTransaction transaction,
        string runId,
        int turn,
        int stepNumber,
        string payloadJson)
    {
        if (turn < 1 && turn != 0)
        {
            throw new InvalidOperationException("Pipeline event turn must be 0 for setup or >= 1.");
        }

        if (stepNumber < 0)
        {
            throw new InvalidOperationException("Pipeline event stepNumber must be >= 0.");
        }

        using var _ = JsonDocument.Parse(payloadJson);
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO pipeline_events (run_id, turn, step_number, payload)
            VALUES (@runId, @turn, @stepNumber, @payload);
            """;
        cmd.Parameters.AddWithValue("@runId", runId);
        cmd.Parameters.AddWithValue("@turn", turn);
        cmd.Parameters.AddWithValue("@stepNumber", stepNumber);
        cmd.Parameters.AddWithValue("@payload", payloadJson);
        cmd.ExecuteNonQuery();
    }

    private static void UpsertMemoryBlock(
        SqliteConnection connection,
        SqliteTransaction transaction,
        MemoryBlockDto block)
    {
        ValidateMemoryBlock(block);
        var existing = ReadMemoryBlock(connection, transaction, block.Label);
        if (existing is not null && existing.ReadOnly && existing != block)
        {
            throw new InvalidOperationException($"Memory block '{block.Label}' is read-only and cannot be changed.");
        }

        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO memory_blocks (label, description, value, char_limit, read_only)
            VALUES (@label, @description, @value, @charLimit, @readOnly)
            ON CONFLICT(label) DO UPDATE SET
              description = excluded.description,
              value = excluded.value,
              char_limit = excluded.char_limit,
              read_only = excluded.read_only,
              updated_at = CURRENT_TIMESTAMP;
            """;
        cmd.Parameters.AddWithValue("@label", block.Label.Trim());
        cmd.Parameters.AddWithValue("@description", block.Description.Trim());
        cmd.Parameters.AddWithValue("@value", block.Value);
        cmd.Parameters.AddWithValue("@charLimit", block.CharLimit);
        cmd.Parameters.AddWithValue("@readOnly", block.ReadOnly ? 1 : 0);
        cmd.ExecuteNonQuery();
    }

    private static ArchivalPassageDto UpsertArchivalPassage(
        SqliteConnection connection,
        SqliteTransaction transaction,
        ArchivalPassageDto passage)
    {
        ValidateArchivalPassage(passage);
        var normalized = passage with
        {
            Id = passage.Id.Trim(),
            Scope = passage.Scope.Trim().ToLowerInvariant(),
            Source = passage.Source.Trim(),
            Content = passage.Content.Trim(),
            Tags = NormalizeTags(passage.Tags),
            MetadataJson = string.IsNullOrWhiteSpace(passage.MetadataJson) ? null : passage.MetadataJson.Trim(),
            EmbeddingModel = passage.EmbeddingModel.Trim(),
            EmbeddingDimensions = passage.Embedding.Count,
            Embedding = passage.Embedding.ToArray()
        };

        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            INSERT INTO archival_passages
              (id, scope, source, content, tags_json, metadata_json, embedding_model, embedding_dimensions, embedding_json)
            VALUES
              (@id, @scope, @source, @content, @tagsJson, @metadataJson, @embeddingModel, @embeddingDimensions, @embeddingJson)
            ON CONFLICT(id) DO UPDATE SET
              scope = excluded.scope,
              source = excluded.source,
              content = excluded.content,
              tags_json = excluded.tags_json,
              metadata_json = excluded.metadata_json,
              embedding_model = excluded.embedding_model,
              embedding_dimensions = excluded.embedding_dimensions,
              embedding_json = excluded.embedding_json,
              updated_at = CURRENT_TIMESTAMP;
            """;
        cmd.Parameters.AddWithValue("@id", normalized.Id);
        cmd.Parameters.AddWithValue("@scope", normalized.Scope);
        cmd.Parameters.AddWithValue("@source", normalized.Source);
        cmd.Parameters.AddWithValue("@content", normalized.Content);
        cmd.Parameters.AddWithValue("@tagsJson", JsonSerializer.Serialize(normalized.Tags));
        cmd.Parameters.AddWithValue("@metadataJson", normalized.MetadataJson is null ? DBNull.Value : normalized.MetadataJson);
        cmd.Parameters.AddWithValue("@embeddingModel", normalized.EmbeddingModel);
        cmd.Parameters.AddWithValue("@embeddingDimensions", normalized.EmbeddingDimensions);
        cmd.Parameters.AddWithValue("@embeddingJson", JsonSerializer.Serialize(normalized.Embedding));
        cmd.ExecuteNonQuery();
        return normalized;
    }

    private static MemoryBlockDto? ReadMemoryBlock(SqliteConnection connection, SqliteTransaction transaction, string label)
    {
        using var cmd = connection.CreateCommand();
        cmd.Transaction = transaction;
        cmd.CommandText =
            """
            SELECT label, description, value, char_limit, read_only
            FROM memory_blocks
            WHERE label = @label;
            """;
        cmd.Parameters.AddWithValue("@label", label.Trim());
        using var reader = cmd.ExecuteReader();
        if (!reader.Read())
        {
            return null;
        }

        return new MemoryBlockDto(
            reader.GetString(0),
            reader.GetString(1),
            reader.GetString(2),
            reader.GetInt32(3),
            reader.GetInt32(4) != 0);
    }
    #endregion

    #region Helpers
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
    private static string BuildModuleTracePayload(string playerInput, string directorResponseBody)
    {
        static bool TryBuildNarrationFromDirectorResponse(string body, out string narration)
        {
            narration = string.Empty;
            try
            {
                var parsed = JsonSerializer.Deserialize<DirectorMessageResponse>(body);
                if (parsed is null || !parsed.Ok || string.IsNullOrWhiteSpace(parsed.Text))
                {
                    return false;
                }

                narration = parsed.Text.Trim();
                return true;
            }
            catch (JsonException)
            {
                return false;
            }
        }

        var narrationText = TryBuildNarrationFromDirectorResponse(directorResponseBody, out var narration)
            ? narration
            : "Director returned a non-standard response.";

        return JsonSerializer.Serialize(new
        {
            narrationText,
            directorRaw = directorResponseBody,
            playerInputEcho = playerInput
        });
    }
    private string GetGameProjectsRoot() => Path.Combine(_repositoryRoot, "game_projects");

    private string GetDbPath(string gameProjectId, string runId) =>
        Path.Combine(GetGameProjectsRoot(), gameProjectId, "saved", runId, "world_state.db");

    private string RequireRunDatabase(string gameProjectId, string runId)
    {
        if (string.IsNullOrWhiteSpace(gameProjectId))
        {
            throw new ArgumentException("gameProjectId must be non-empty.", nameof(gameProjectId));
        }

        if (string.IsNullOrWhiteSpace(runId))
        {
            throw new ArgumentException("runId must be non-empty.", nameof(runId));
        }

        var dbPath = GetDbPath(gameProjectId, runId);
        if (!File.Exists(dbPath))
        {
            throw new InvalidOperationException(
                "Run database not found; the host must bind the run before using memory endpoints.");
        }

        return dbPath;
    }

    private static void ValidateMemoryBlock(MemoryBlockDto block)
    {
        if (string.IsNullOrWhiteSpace(block.Label))
        {
            throw new InvalidOperationException("Memory block label must be non-empty.");
        }

        if (string.IsNullOrWhiteSpace(block.Description))
        {
            throw new InvalidOperationException($"Memory block '{block.Label}' description must be non-empty.");
        }

        if (block.CharLimit < 1)
        {
            throw new InvalidOperationException($"Memory block '{block.Label}' charLimit must be >= 1.");
        }

        if (block.Value.Length > block.CharLimit)
        {
            throw new InvalidOperationException(
                $"Memory block '{block.Label}' value length {block.Value.Length} exceeds charLimit {block.CharLimit}.");
        }
    }

    private static string[] NormalizeRoles(IReadOnlyList<string>? roles) =>
        roles?
            .Where(static role => !string.IsNullOrWhiteSpace(role))
            .Select(static role => role.Trim())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray()
        ?? [];

    private static string[] NormalizeTags(IReadOnlyList<string>? tags) =>
        tags?
            .Where(static tag => !string.IsNullOrWhiteSpace(tag))
            .Select(static tag => tag.Trim().ToLowerInvariant())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray()
        ?? [];

    private static bool TagsContainAll(IReadOnlyList<string> passageTags, IReadOnlyList<string> requestedTags)
    {
        var tagSet = passageTags.ToHashSet(StringComparer.OrdinalIgnoreCase);
        return requestedTags.All(tagSet.Contains);
    }

    private static IReadOnlyList<LoreSeedEntry> ReadLoreSeedEntries(string gameProjectId)
    {
        var loreDir = Path.Combine(GetGameProjectsRootStatic(), gameProjectId, "lore");
        var csvPath = Path.Combine(loreDir, "default_lore_entries.csv");
        if (!File.Exists(csvPath))
        {
            if (Directory.Exists(loreDir))
            {
                Console.WriteLine(
                    $"[SessionStore] WARNING: No default_lore_entries.csv under '{loreDir}' for game project '{gameProjectId}'. Lore table will not be seeded from disk.");
            }

            return [];
        }

        var lines = CsvRfc4180.SplitRecords(File.ReadAllText(csvPath))
            .Where(static line => !string.IsNullOrWhiteSpace(line) && !line.TrimStart().StartsWith('#'))
            .ToArray();
        if (lines.Length == 0)
        {
            return [];
        }

        var headers = CsvRfc4180.ParseRecordFields(lines[0]).Select(static h => h.ToLowerInvariant()).ToArray();
        var subjectIndex = Array.IndexOf(headers, "subject");
        var dataIndex = Array.FindIndex(headers, static h => h is "data" or "description" or "entry");
        if (subjectIndex < 0 || dataIndex < 0)
        {
            return [];
        }

        var rows = new List<LoreSeedEntry>();
        for (var i = 1; i < lines.Length; i++)
        {
            var columns = CsvRfc4180.ParseRecordFields(lines[i]);
            if (subjectIndex >= columns.Count || dataIndex >= columns.Count)
            {
                continue;
            }

            var subject = columns[subjectIndex].Trim();
            var data = columns[dataIndex].Trim();
            if (string.IsNullOrWhiteSpace(subject) || string.IsNullOrWhiteSpace(data))
            {
                continue;
            }

            rows.Add(new LoreSeedEntry(subject, data));
        }

        return rows;
    }

    private static string GetGameProjectsRootStatic()
    {
        var repositoryRoot = EngineConfigLoader.FindRepositoryRoot()
            ?? throw new InvalidOperationException("Repository root could not be resolved for lore seeding.");
        return Path.Combine(repositoryRoot, "game_projects");
    }

    private static string CreateStableArchivalId(string source, string subject)
    {
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(source + "\n" + subject.Trim().ToLowerInvariant()));
        return "lore:default:" + Convert.ToHexString(hash)[..24].ToLowerInvariant();
    }

    private static float[] DeserializeFloatArray(string json)
    {
        var values = JsonSerializer.Deserialize<float[]>(json);
        return values is { Length: > 0 } ? values : [];
    }

    private static string[] DeserializeStringArray(string json)
    {
        var values = JsonSerializer.Deserialize<string[]>(json);
        return NormalizeTags(values);
    }

    private static double CosineSimilarity(IReadOnlyList<float> left, IReadOnlyList<float> right)
    {
        if (left.Count != right.Count || left.Count == 0)
        {
            throw new InvalidOperationException("Cosine similarity requires non-empty vectors with matching dimensions.");
        }

        double dot = 0;
        double leftMagnitude = 0;
        double rightMagnitude = 0;
        for (var i = 0; i < left.Count; i++)
        {
            dot += left[i] * right[i];
            leftMagnitude += left[i] * left[i];
            rightMagnitude += right[i] * right[i];
        }

        if (leftMagnitude <= 0 || rightMagnitude <= 0)
        {
            return 0;
        }

        return dot / (Math.Sqrt(leftMagnitude) * Math.Sqrt(rightMagnitude));
    }

    private static void ValidateArchivalPassage(ArchivalPassageDto passage)
    {
        if (string.IsNullOrWhiteSpace(passage.Id))
        {
            throw new InvalidOperationException("Archival passage id must be non-empty.");
        }

        if (passage.Scope.Trim().ToLowerInvariant() is not ("project" or "run"))
        {
            throw new InvalidOperationException("Archival passage scope must be either 'project' or 'run'.");
        }

        if (string.IsNullOrWhiteSpace(passage.Source) || string.IsNullOrWhiteSpace(passage.Content))
        {
            throw new InvalidOperationException("Archival passage source and content must be non-empty.");
        }

        if (string.IsNullOrWhiteSpace(passage.EmbeddingModel))
        {
            throw new InvalidOperationException("Archival passage embeddingModel must be non-empty.");
        }

        if (passage.Embedding.Count == 0 || passage.EmbeddingDimensions != passage.Embedding.Count)
        {
            throw new InvalidOperationException("Archival passage embeddingDimensions must match the non-empty embedding vector.");
        }

        if (passage.MetadataJson is not null)
        {
            using var _ = JsonDocument.Parse(passage.MetadataJson);
        }
    }

    private static string BuildFtsQuery(string query)
    {
        var terms = query
            .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(static term => new string(term.Where(static ch => char.IsLetterOrDigit(ch) || ch == '_' || ch == '-').ToArray()))
            .Where(static term => term.Length > 0)
            .Select(static term => "\"" + term.Replace("\"", "\"\"") + "\"")
            .ToArray();

        if (terms.Length == 0)
        {
            return "\"\"";
        }

        return string.Join(" OR ", terms);
    }

    private static bool PayloadMatchesEventType(string payloadJson, string? eventType)
    {
        if (string.IsNullOrWhiteSpace(eventType))
        {
            return true;
        }

        try
        {
            using var doc = JsonDocument.Parse(payloadJson);
            return doc.RootElement.TryGetProperty("eventType", out var eventTypeElement)
                && eventTypeElement.ValueKind == JsonValueKind.String
                && string.Equals(eventTypeElement.GetString(), eventType.Trim(), StringComparison.OrdinalIgnoreCase);
        }
        catch (JsonException)
        {
            return false;
        }
    }

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

    private static string BuildViewStateEnvelope(string directorResponseBody)
    {
        try
        {
            using var parsed = JsonDocument.Parse(directorResponseBody);
            return JsonSerializer.Serialize(new { directorResponse = parsed.RootElement.Clone() });
        }
        catch (JsonException)
        {
            return JsonSerializer.Serialize(new { directorRawText = directorResponseBody });
        }
    }
    #endregion
}
