using System.Diagnostics;
using System.Net;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine
{
    public class LlmProviderQwen
    {
        #region Nested types
        private sealed record OllamaOptionsPayload(int num_ctx, int num_keep);
        #endregion

        #region Private data
        private static readonly JsonSerializerOptions _jsonOptions = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true // Allows either casing for json fields.
        };

        // Wall-clock budget for bundled ollama.exe to start answering GET /; independent of per-request HttpClient timeout.
        private static readonly TimeSpan OllamaReadyTimeout = TimeSpan.FromSeconds(90);
        private static readonly TimeSpan OllamaReadyPollInterval = TimeSpan.FromMilliseconds(200);
        private static readonly TimeSpan OllamaRestartBackoff = TimeSpan.FromMilliseconds(500);
        /// <summary>Every outbound call to the bundled Ollama process (GET /, model priming, inference) uses this ceiling; matches Director/Router LLM proxy budget.</summary>
        private static readonly TimeSpan OllamaHttpTimeout = TimeSpan.FromSeconds(60);
        private const int MaxOllamaRestartAttempts = 3;
        private const int MaxCapturedOllamaErrorLines = 20;
        private const int OllamaRequestNumKeep = -1;

        private const int OllamaRequestPreviewMaxChars = 160;
        private const int OllamaResponseLogMaxChars = 500;

        // Instance-owned HttpClient for Ollama (GET /, /api/chat, /api/generate); disposed in Shutdown().
        private readonly HttpClient _httpClient;
        private readonly EngineConfiguration _configuration;

        private readonly HttpListener _listener = new HttpListener(); // Inbound listener for responding to http messages.
        private readonly SemaphoreSlim _ollamaRestartGate = new(1, 1);
        private readonly object _ollamaStateSync = new();
        private readonly Queue<string> _recentOllamaErrorLines = [];
        private bool _shutdownRequested = false;
        private volatile bool _initializing;
        private volatile bool _runBound = false;
        private string _boundGameProjectId = "";
        private string _boundRunId = "";
        private bool _ollamaHttpReady = false;
        private bool _ollamaReady = false;
        /// <summary>Set when POST /initialize returned 202 but completion failed (host should fail fast via /health).</summary>
        private volatile bool _initializeBindFailedAfterAccepted = false;
        /// <summary>Set when background bundled Ollama bootstrap throws; host fails fast via /health status ollama_startup_failed.</summary>
        private volatile bool _ollamaBootstrapFailed = false;
        private Task? _bundledOllamaBootstrapTask;
        private bool _ollamaStopping = false;
        private int _ollamaRestartAttempts = 0;
        private Process? _ollamaProcess;
        private bool _disableBundledOllamaBootstrapForTesting = false;
        private bool _filterOffNoisyLogs = false;
        private OllamaLogNoiseFilter? _ollamaLogNoiseFilter = null;

        /// <summary>Repository root resolved once at startup for locating bundled Ollama assets.</summary>
        private string _repositoryRoot = "";

        /// <summary>Qwen module-owned Ollama port from engine configuration.</summary>
        private int _ollamaPort = 0;

        /// <summary>Ollama model for /api/chat and /api/generate; resolved once in <see cref="InitializeAsync"/> from engine_config.json.</summary>
        private string _chatModel = "";

        /// <summary>Forwarded on every Ollama /api/chat and /api/generate request as options.num_ctx.</summary>
        private int _ollamaNumCtx = 0;

        /// <summary>Forwarded as top-level think on Ollama /api/chat and /api/generate (from engine_config thinking).</summary>
        private bool _ollamaThink = false;

        /// <summary>System prompt for POST /summarize (loaded from prompts/summarize_system.md beside the module executable).</summary>
        private string _summarizeSystemPrompt = "";

        // Last Ollama wire JSON from POST /chat or POST /generate; read from GET /debug/last_llm_payload.
        private volatile LlmProviderLastPayloadResponse? _lastLlmPayloadSnapshot = null;
        #endregion

        #region Public methods
        public LlmProviderQwen()
            : this(
                EngineConfigLoader.GetConfiguration(),
                new HttpClient
                {
                    Timeout = OllamaHttpTimeout
                })
        {
        }

        internal LlmProviderQwen(EngineConfiguration configuration, HttpClient httpClient)
        {
            _configuration = configuration ?? throw new ArgumentNullException(nameof(configuration));
            _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
        }

        public async Task Run()
        {
            try
            {
                // HTTP listener starts before bundled Ollama is HTTP-ready so the host can poll GET /health (short listen timeout) while Ollama boots.
                await InitializeAsync();

                // Block until a request arrives, then handle it without awaiting (concurrent requests).
                while (!_shutdownRequested)
                {
                    HttpListenerContext context;
                    try
                    {
                        context = await _listener.GetContextAsync();
                    }
                    catch (HttpListenerException) when (_shutdownRequested)
                    {
                        break;
                    }
                    catch (ObjectDisposedException) when (_shutdownRequested)
                    {
                        break;
                    }

                    _ = ProcessQuery(context);
                }
            }
            catch (HttpListenerException e)
            {
                Console.WriteLine("LlmProvider_qwen error encountered: " + e.Message);
            }
            catch (Exception e)
            {
                Console.WriteLine("LlmProvider_qwen fatal startup/runtime error: " + e.Message);
            }
            finally
            {
                // Always release listener, child process, and outbound HTTP resources when the loop ends or faults.
                Shutdown();
            }
        }

        public void RequestShutdown() => _shutdownRequested = true;

        internal void DisableBundledOllamaBootstrapForTesting()
        {
            _disableBundledOllamaBootstrapForTesting = true;
        }

        internal void SetOllamaStateForTesting(bool httpReady, bool ready, bool bootstrapFailed)
        {
            lock (_ollamaStateSync)
            {
                _ollamaHttpReady = httpReady;
                _ollamaReady = ready;
                _ollamaBootstrapFailed = bootstrapFailed;
            }
        }

        #endregion

        #region Private methods
        // Intentional single use method: binds the HTTP listener immediately, then starts bundled Ollama on a background task so GET /health can answer during long Ollama cold start.
        private Task InitializeAsync()
        {
            _repositoryRoot = _configuration.RepositoryRoot;
            var providerRow = _configuration.GetRequiredGenericLlmProviderModule();
            var qwenOpts = providerRow.QwenOptions
                ?? throw new InvalidOperationException(
                    "llm_provider_qwen: generic_llm_provider target module has no qwen options (ollama_port, default_chat_model).");
            var genericOpts = providerRow.GenericLlmProviderOptions
                ?? throw new InvalidOperationException(
                    "llm_provider_qwen: generic_llm_provider target module has no num_ctx (generic_llm_provider options).");
            _chatModel = qwenOpts.OllamaModel.Trim();
            _ollamaPort = qwenOpts.OllamaPort;
            _ollamaNumCtx = genericOpts.NumCtx;
            _filterOffNoisyLogs = qwenOpts.FilterOffNoisyLogs;
            _ollamaThink = qwenOpts.Thinking;
            _summarizeSystemPrompt = LoadBundledSummarizeSystemPrompt();
            if (_filterOffNoisyLogs)
            {
                _ollamaLogNoiseFilter = new OllamaLogNoiseFilter();
            }
            if (string.IsNullOrWhiteSpace(_chatModel))
            {
                throw new InvalidOperationException(
                    "llm_provider_qwen: default_chat_model from engine configuration is empty (check engine_config.json).");
            }

            var qwenListen = _configuration.PortMap.GetRequiredPort("llm_provider_qwen");
            _listener.Prefixes.Add($"http://127.0.0.1:{qwenListen}/");
            _listener.Start();

            // Bundled Ollama inherits the host module job (see MorpheusEngine Run); no nested Job Object here.
            _ollamaBootstrapFailed = false;
            if (_disableBundledOllamaBootstrapForTesting)
            {
                _bundledOllamaBootstrapTask = Task.CompletedTask;
            }
            else
            {
                _bundledOllamaBootstrapTask = RunBundledOllamaBootstrapAsync();
            }

            Console.WriteLine(
                _disableBundledOllamaBootstrapForTesting
                    ? $"ready listen=http://127.0.0.1:{qwenListen}/ model='{_chatModel}' ollama=http://127.0.0.1:{_ollamaPort}/ num_ctx={_ollamaNumCtx} awaiting_initialize=true (test bootstrap disabled)"
                    : $"ready listen=http://127.0.0.1:{qwenListen}/ model='{_chatModel}' ollama=http://127.0.0.1:{_ollamaPort}/ num_ctx={_ollamaNumCtx} awaiting_initialize=true (ollama bootstrap in background)");
            return Task.CompletedTask;
        }

        // Swallows exceptions from StartManagedOllamaAsync so Run() always enters the accept loop; failures are reported on GET /health (ollama_startup_failed).
        private async Task RunBundledOllamaBootstrapAsync()
        {
            try
            {
                await StartManagedOllamaAsync("initial startup");
            }
            catch (Exception e)
            {
                _ollamaBootstrapFailed = true;
                Console.WriteLine("LlmProvider_qwen: bundled Ollama initial bootstrap failed: " + e.Message);
            }
        }

        // Intentional single use method.
        private async Task ProcessQuery(HttpListenerContext context)
        {
            try
            {
                // Contract checks.
                if (context.Request.Url is null)
                {
                    Console.WriteLine("LlmProvider_qwen received invalid request: Url is null.");
                    await Respond(context, 400, new ErrorResponse(false, "Invalid request URL."));
                    return;
                }

                var path = context.Request.Url.AbsolutePath;

                // /info endpoint.
                if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/info called.");
                    await Respond(context, 200, new
                    {
                        ok = true,
                        moduleName = "llm_provider_qwen",
                        provider = "ollama",
                        model = _chatModel
                    });
                    return;
                }

                // /health: awaiting_initialize (200) → initializing (503, includes one-shot model priming) → healthy (200, initialized).
                // POST /initialize may return 202 quickly while bind + priming continue; initialized stays false until priming succeeds.
                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    if (_initializing)
                    {
                        await Respond(context, 503, new ModuleHealthResponse(false, "initializing", false));
                        return;
                    }

                    if (IsOllamaReady())
                    {
                        await Respond(context, 200, new ModuleHealthResponse(true, "healthy", true));
                        return;
                    }

                    // Terminal bootstrap failure: 200 + fail-fast in host WaitForStartProcessCompletedAsync / WaitForInitializationToCompleteAsync.
                    if (_ollamaBootstrapFailed)
                    {
                        await Respond(context, 200, new ModuleHealthResponse(false, "ollama_startup_failed", false));
                        return;
                    }

                    // 200 (not 503): host WaitForStartProcessCompletedAsync only treats 2xx as "listening"; Ollama may still be warming for many seconds.
                    if (!_runBound && !_ollamaHttpReady)
                    {
                        await Respond(context, 200, new ModuleHealthResponse(false, "ollama_starting", false));
                        return;
                    }

                    if (!_runBound)
                    {
                        await Respond(context, 200, new ModuleHealthResponse(false, "awaiting_initialize", false));
                        return;
                    }

                    if (_initializeBindFailedAfterAccepted)
                    {
                        await Respond(context, 503, new ModuleHealthResponse(false, "initialize_failed", false));
                        return;
                    }

                    await Respond(context, 503, new ModuleHealthResponse(false, "warming_up", false));
                    return;
                }

                if (path.Equals("/initialize", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_bindRun(context);
                    return;
                }

                // /shutdown endpoint
                if (path.Equals("/shutdown", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_shutdown(context);
                    return;
                }

                // /generate endpoint.
                if (path.Equals("/generate", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/generate called.");
                    await ProcessRequest_generate(context);
                    return;
                }

                // /chat endpoint: Ollama /api/chat with explicit messages[] (Director and future chat flows).
                if (path.Equals("/chat", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/chat called.");
                    await ProcessRequest_chat(context);
                    return;
                }

                if (path.Equals("/summarize", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/summarize called.");
                    await ProcessRequest_summarize(context);
                    return;
                }

                if (path.Equals("/token_count", StringComparison.OrdinalIgnoreCase))
                {
                    Console.WriteLine("LlmProvider_qwen/token_count called.");
                    await ProcessRequest_tokenCount(context);
                    return;
                }

                if (path.Equals("/debug/last_llm_payload", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_lastLlmPayload(context);
                    return;
                }

                // Invalid endpoint specified.
                Console.WriteLine("LlmProvider_qwen called with an unknown path: " + path);
                await Respond(context, 404, new ErrorResponse(false, "Not found: " + path));
            }
            catch (Exception e)
            {
                Console.WriteLine("LlmProvider_qwen encountered unhandled request error: " + e.Message);
                if (context.Response.OutputStream.CanWrite)
                {
                    await Respond(context, 500, new ErrorResponse(false, "Unhandled llm provider error.", e.Message));
                }
            }
        }

        // Intentional single use method.
        private void Shutdown()
        {
            _shutdownRequested = true;

            // Let in-flight initial Ollama bootstrap finish if possible so we do not dispose HttpClient while it is still probing.
            if (_bundledOllamaBootstrapTask is { IsCompleted: false }
                && !_bundledOllamaBootstrapTask.Wait(TimeSpan.FromSeconds(15)))
            {
                Console.WriteLine("LlmProvider_qwen shutdown: bundled Ollama bootstrap still running; proceeding with teardown.");
            }

            // Stop taking new requests before tearing down the child process.
            try
            {
                _listener.Stop(); // Technically redundant as it's included in _listener.Close().
            }
            catch (ObjectDisposedException)
            {
            }
            catch (HttpListenerException)
            {
            }

            try
            {
                _listener.Close();
            }
            catch (ObjectDisposedException)
            {
            }

            StopCurrentOllamaProcess("provider shutdown");
            _httpClient.Dispose();
            Console.WriteLine("LlmProvider_qwen shut down.");
        }

        private async Task ProcessRequest_bindRun(HttpListenerContext context)
        {
            if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
                return;
            }

            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            InitializeModuleRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<InitializeModuleRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null
                || string.IsNullOrWhiteSpace(request.GameProjectId)
                || string.IsNullOrWhiteSpace(request.RunId))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include non-empty gameProjectId and runId."));
                return;
            }

            _initializing = true;
            try
            {
                await _ollamaRestartGate.WaitAsync();
                var acceptedInitialize = false;
                try
                {
                    if (_runBound)
                    {
                        await Respond(context, 409, new ErrorResponse(false, "LLM provider is already bound for this process; restart it to bind another run."));
                        return;
                    }

                    _boundGameProjectId = request.GameProjectId.Trim();
                    _boundRunId = request.RunId.Trim();
                    _runBound = true;
                    _initializeBindFailedAfterAccepted = false;

                    // Return 202 immediately so the host's short HttpClient timeout is not exceeded while waiting for Ollama HTTP readiness.
                    // Completion (or failure) is visible on GET /health; the host still calls WaitForInitializationToCompleteAsync afterward.
                    await Respond(context, 202, new InitializeModuleResponse(true));
                    acceptedInitialize = true;

                    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(60);
                    while (!_ollamaHttpReady && DateTime.UtcNow < deadline)
                    {
                        await Task.Delay(250);
                    }

                    if (!_ollamaHttpReady)
                    {
                        _initializeBindFailedAfterAccepted = true;
                        Console.WriteLine(
                            "[LlmProvider_qwen] POST /initialize accepted (202) but bundled Ollama HTTP did not become ready within the bind wait; "
                            + "GET /health will report initialize_failed.");
                        return;
                    }

                    // One minimal /api/generate so weights load before any external /chat; initialized=true only after this succeeds.
                    try
                    {
                        await PrimeBundledOllamaModelAsync("post_initialize_bind");
                        FlushPendingPrimeSummary();
                    }
                    catch (Exception e)
                    {
                        _initializeBindFailedAfterAccepted = true;
                        Console.WriteLine("[LlmProvider_qwen] Model priming failed after bind: " + e.Message);
                        return;
                    }

                    lock (_ollamaStateSync)
                    {
                        _ollamaReady = true;
                        _ollamaStopping = false;
                        _ollamaRestartAttempts = 0;
                    }

                    Console.WriteLine($"[LlmProvider_qwen] Bound run runId={_boundRunId} gameProjectId={_boundGameProjectId}.");
                }
                catch (FileNotFoundException e)
                {
                    if (!acceptedInitialize)
                    {
                        await Respond(context, 500, new ErrorResponse(false, e.Message, e.FileName));
                    }
                    else
                    {
                        _initializeBindFailedAfterAccepted = true;
                        Console.WriteLine($"[LlmProvider_qwen] Bind failed after POST /initialize 202: {e.Message}");
                    }
                }
                catch (InvalidOperationException e)
                {
                    if (!acceptedInitialize)
                    {
                        await Respond(context, 500, new ErrorResponse(false, e.Message));
                    }
                    else
                    {
                        _initializeBindFailedAfterAccepted = true;
                        Console.WriteLine($"[LlmProvider_qwen] Bind failed after POST /initialize 202: {e.Message}");
                    }
                }
                catch (Exception e)
                {
                    if (!acceptedInitialize)
                    {
                        await Respond(context, 500, new ErrorResponse(false, "Failed to bind run.", e.Message));
                    }
                    else
                    {
                        _initializeBindFailedAfterAccepted = true;
                        Console.WriteLine($"[LlmProvider_qwen] Bind failed after POST /initialize 202: {e.Message}");
                    }
                }
                finally
                {
                    _ollamaRestartGate.Release();
                }
            }
            finally
            {
                _initializing = false;
            }
        }

        // Exception to "extract only when >1 use": kept as a named handler parallel to ProcessRequest_generate for /shutdown routing clarity.
        private async Task ProcessRequest_shutdown(HttpListenerContext context)
        {
            await Respond(context, 200, new ModuleShutdownResponse(true, "Shutdown requested."));
            _shutdownRequested = true;

            try
            {
                _listener.Stop();
            }
            catch (ObjectDisposedException)
            {
            }
            catch (HttpListenerException)
            {
            }
        }

        // Intentional single use method.
        private async Task ProcessRequest_generate(HttpListenerContext context)
        {
            // Parse caller's request.
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            LlmGenerateRequest? request = null;
            try
            {
                request = JsonSerializer.Deserialize<LlmGenerateRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.Prompt))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include a non-empty 'prompt' field."));
                return;
            }

            if (!await RespondIfOllamaUnavailableAsync(context))
            {
                return;
            }

            // Model is owned by this provider (engine_config llm_provider_qwen.default_chat_model), not the HTTP caller.
            var model = _chatModel;

            // Construct an Ollama payload from the internal generic payload (shape matches Ollama /api/generate expectations).
            var ollamaPayload = new
            {
                model,
                prompt = request.Prompt,
                system = request.System,
                stream = false, // Generate whole response in one go and return it.
                truncate = false,
                think = _ollamaThink,
                options = BuildOllamaOptionsPayload()
            };
            var promptTrimmed = request.Prompt.Trim();
            var systemTrimmed = request.System?.Trim();
            Console.WriteLine(
                $"OLLAMA_IO REQUEST model={model} promptChars={promptTrimmed.Length} systemChars={(systemTrimmed is null ? 0 : systemTrimmed.Length)} "
                + $"promptPreview='{TruncateMiddle(promptTrimmed, headChars: 80, tailChars: 60)}'");

            // Convert to json for transmission.
            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            PublishLastLlmPayload("generate", requestJson);
            WriteTrafficLine("OLLAMA_IO TRAFFIC GENERATE_REQUEST_JSON " + requestJson);
            var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

            // Send message to the bundled Ollama child.
            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/generate"), content);
            }
            catch (Exception e)
            {
                Console.WriteLine("OLLAMA_IO ERROR Failed to reach Ollama: " + e.Message);
                await Respond(
                    context,
                    IsOllamaReady() ? 502 : 503,
                    new ErrorResponse(false, "Bundled Ollama is unavailable.", BuildOllamaUnavailableDetails(e.Message)));
                return;
            }

            // Relay the Ollama response back to caller.
            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            WriteTrafficLine("OLLAMA_IO TRAFFIC GENERATE_RESPONSE_BODY " + JsonSerializer.Serialize(ollamaBody));
            Console.WriteLine(
                $"OLLAMA_IO RESPONSE status={(int)ollamaResponse.StatusCode} bodySnippet={TruncateMiddle(ollamaBody, headChars: 240, tailChars: 120)}");
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new
                {
                    ok = false,
                    error = "Ollama returned an error.",
                    model,
                    ollama_status = (int)ollamaResponse.StatusCode,
                    ollama_response = ollamaBody
                });
                return;
            }

            // Successful /api/generate: text is under "response".
            string? responseText = null;
            try
            {
                using var doc = JsonDocument.Parse(ollamaBody);
                if (doc.RootElement.TryGetProperty("response", out var responseElement))
                {
                    responseText = responseElement.GetString();
                }
            }
            catch (JsonException)
            {
                // keep raw body when parsing fails
            }

            await Respond(context, 200, new LlmProviderGenerateResponse(true, responseText, ollamaBody));
        }

        // Intentional single use method: mirrors ProcessRequest_generate but targets Ollama /api/chat.
        private async Task ProcessRequest_chat(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            ChatGenerateRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<ChatGenerateRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null || request.Messages is null || request.Messages.Count == 0)
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include a non-empty 'messages' array."));
                return;
            }

            if (!await RespondIfOllamaUnavailableAsync(context))
            {
                return;
            }

            // Ollama /api/chat expects { model, messages, stream, truncate, options } plus optional format/keep_alive.
            // The model is fixed at provider InitializeAsync() from engine_config.json.
            var ollamaPayload = BuildOllamaChatPayload(
                request.Messages,
                request.Format,
                request.KeepAlive);
            Console.WriteLine(
                $"OLLAMA_IO CHAT_REQUEST model={_chatModel} messages={request.Messages.Count} {DescribeChatMessagesForLog(request.Messages)}");

            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            PublishLastLlmPayload("chat", requestJson);
            WriteTrafficLine("OLLAMA_IO TRAFFIC CHAT_REQUEST_JSON " + requestJson);
            var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/chat"), content);
            }
            catch (Exception e)
            {
                Console.WriteLine("OLLAMA_IO ERROR Failed to reach Ollama (chat): " + e.Message);
                await Respond(
                    context,
                    IsOllamaReady() ? 502 : 503,
                    new ErrorResponse(false, "Bundled Ollama is unavailable.", BuildOllamaUnavailableDetails(e.Message)));
                return;
            }

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            WriteTrafficLine("OLLAMA_IO TRAFFIC CHAT_RESPONSE_BODY " + JsonSerializer.Serialize(ollamaBody));
            Console.WriteLine(
                $"OLLAMA_IO CHAT_RESPONSE status={(int)ollamaResponse.StatusCode} bodySnippet={TruncateMiddle(ollamaBody, headChars: 240, tailChars: 120)}");
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new
                {
                    ok = false,
                    error = "Ollama returned an error.",
                    model = _chatModel,
                    ollamaStatus = (int)ollamaResponse.StatusCode,
                    ollamaResponse = ollamaBody
                });
                return;
            }

            // Successful /api/chat: assistant text is under message.content (not "response" like /api/generate).
            string? assistantText = null;
            try
            {
                using var doc = JsonDocument.Parse(ollamaBody);
                if (doc.RootElement.TryGetProperty("message", out var messageElement)
                    && messageElement.TryGetProperty("content", out var contentElement))
                {
                    assistantText = contentElement.GetString();
                }
            }
            catch (JsonException)
            {
                // keep assistantText null; raw body still returned
            }

            await Respond(context, 200, new ChatGenerateResponse(true, assistantText, ollamaBody));
        }

        // Intentional single use method: episodic recall summarization via Ollama /api/generate (not /api/chat).
        private async Task ProcessRequest_summarize(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            SummarizeRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<SummarizeRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null || string.IsNullOrWhiteSpace(request.Content))
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include non-empty 'content'."));
                return;
            }

            if (!await RespondIfOllamaUnavailableAsync(context))
            {
                return;
            }

            var model = _chatModel;
            var userPrompt = BuildSummarizeUserPrompt(request);
            var ollamaPayload = new Dictionary<string, object?>
            {
                ["model"] = model,
                ["prompt"] = userPrompt,
                ["system"] = _summarizeSystemPrompt,
                ["stream"] = false,
                ["truncate"] = false,
                ["think"] = _ollamaThink,
                ["options"] = BuildOllamaOptionsPayload()
            };

            if (!string.IsNullOrWhiteSpace(request.KeepAlive))
            {
                ollamaPayload["keep_alive"] = request.KeepAlive.Trim();
            }

            Console.WriteLine(
                $"OLLAMA_IO SUMMARIZE_REQUEST model={model} promptChars={userPrompt.Length} systemChars={_summarizeSystemPrompt.Length} "
                + $"promptPreview='{TruncateMiddle(userPrompt, headChars: 80, tailChars: 60)}'");

            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            PublishLastLlmPayload("generate", requestJson);
            WriteTrafficLine("OLLAMA_IO TRAFFIC SUMMARIZE_REQUEST_JSON " + requestJson);
            var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/generate"), content);
            }
            catch (Exception e)
            {
                Console.WriteLine("OLLAMA_IO ERROR Failed to reach Ollama (summarize): " + e.Message);
                await Respond(
                    context,
                    IsOllamaReady() ? 502 : 503,
                    new ErrorResponse(false, "Bundled Ollama is unavailable.", BuildOllamaUnavailableDetails(e.Message)));
                return;
            }

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            WriteTrafficLine("OLLAMA_IO TRAFFIC SUMMARIZE_RESPONSE_BODY " + JsonSerializer.Serialize(ollamaBody));
            Console.WriteLine(
                $"OLLAMA_IO SUMMARIZE_RESPONSE status={(int)ollamaResponse.StatusCode} bodySnippet={TruncateMiddle(ollamaBody, headChars: 240, tailChars: 120)}");
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new
                {
                    ok = false,
                    error = "Ollama returned an error.",
                    model,
                    ollama_status = (int)ollamaResponse.StatusCode,
                    ollama_response = ollamaBody
                });
                return;
            }

            string? summaryText = null;
            try
            {
                using var doc = JsonDocument.Parse(ollamaBody);
                if (doc.RootElement.TryGetProperty("response", out var responseElement))
                {
                    summaryText = responseElement.GetString();
                }
            }
            catch (JsonException)
            {
                // keep summaryText null; fail below
            }

            if (string.IsNullOrWhiteSpace(summaryText))
            {
                await Respond(
                    context,
                    502,
                    new ErrorResponse(false, "Ollama summarize did not return non-empty response text.", ollamaBody));
                return;
            }

            await Respond(context, 200, new SummarizeResponse(true, summaryText.Trim(), ollamaBody));
        }

        private async Task ProcessRequest_lastLlmPayload(HttpListenerContext context)
        {
            var snapshot = _lastLlmPayloadSnapshot;
            if (snapshot is null)
            {
                await Respond(
                    context,
                    200,
                    new LlmProviderLastPayloadResponse(true, false, string.Empty, string.Empty, string.Empty));
                return;
            }

            await Respond(context, 200, snapshot);
        }

        private void PublishLastLlmPayload(string endpoint, string ollamaRequestJson)
        {
            _lastLlmPayloadSnapshot = new LlmProviderLastPayloadResponse(
                true,
                true,
                endpoint,
                DateTime.UtcNow.ToString("O"),
                ollamaRequestJson);
        }

        private async Task ProcessRequest_tokenCount(HttpListenerContext context)
        {
            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            TokenCountRequest? request;
            try
            {
                request = JsonSerializer.Deserialize<TokenCountRequest>(body, _jsonOptions);
            }
            catch (JsonException e)
            {
                await Respond(context, 400, new ErrorResponse(false, "Invalid JSON payload.", e.Message));
                return;
            }

            if (request is null)
            {
                await Respond(context, 400, new ErrorResponse(false, "Request body is required."));
                return;
            }

            var hasText = !string.IsNullOrWhiteSpace(request.Text);
            var hasMessages = request.Messages is { Count: > 0 };
            if (hasText == hasMessages)
            {
                await Respond(context, 400, new ErrorResponse(false, "Request must include exactly one of non-empty 'text' or non-empty 'messages'."));
                return;
            }

            if (!string.IsNullOrWhiteSpace(request.Model) && !string.Equals(request.Model.Trim(), _chatModel, StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 400, new ErrorResponse(false, $"llm_provider_qwen token_count uses configured model '{_chatModel}', not caller model '{request.Model.Trim()}'."));
                return;
            }

            if (!await RespondIfOllamaUnavailableAsync(context))
            {
                return;
            }

            if (hasMessages)
            {
                await ProcessTokenCountChatProbeAsync(context, request.Messages!, request.Format, request.KeepAlive);
                return;
            }

            await ProcessTokenCountGenerateProbeAsync(context, request.Text!.Trim());
        }

        // Chat-aligned token probe: same /api/chat wire as POST /chat but num_predict=0; does not update last_llm_payload.
        private async Task ProcessTokenCountChatProbeAsync(
            HttpListenerContext context,
            IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages,
            JsonElement? format,
            string? keepAlive)
        {
            var ollamaPayload = BuildOllamaChatPayload(messages, format, keepAlive, numPredict: 0);
            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            using var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/chat"), content);
            }
            catch (Exception e)
            {
                await Respond(
                    context,
                    IsOllamaReady() ? 502 : 503,
                    new ErrorResponse(false, "Bundled Ollama is unavailable.", BuildOllamaUnavailableDetails(e.Message)));
                return;
            }

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new ErrorResponse(false, "Ollama returned an error during token_count.", ollamaBody));
                return;
            }

            if (TryReadPromptEvalCount(ollamaBody, out var exactTokens))
            {
                Console.WriteLine($"OLLAMA_IO TOKEN_COUNT chat_probe exact=true model={_chatModel} messages={messages.Count} tokens={exactTokens}");
                await Respond(context, 200, new TokenCountResponse(true, _chatModel, exactTokens, true));
                return;
            }

            Console.WriteLine(
                $"OLLAMA_IO TOKEN_COUNT chat_probe exact=false model={_chatModel} messages={messages.Count} ollamaBodySnippet={TruncateMiddle(ollamaBody, headChars: 240, tailChars: 120)}");
            await Respond(
                context,
                502,
                new ErrorResponse(false, "Ollama token_count did not return prompt_eval_count.", ollamaBody));
        }

        // Raw generate probe for non-chat callers (e.g. embeddings); does not update last_llm_payload.
        private async Task ProcessTokenCountGenerateProbeAsync(HttpListenerContext context, string text)
        {
            var options = BuildOllamaOptionsPayload();
            var ollamaPayload = new
            {
                model = _chatModel,
                prompt = text,
                stream = false,
                raw = true,
                truncate = false,
                think = _ollamaThink,
                options = new { options.num_ctx, options.num_keep, num_predict = 0 }
            };
            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            using var content = new StringContent(requestJson, Encoding.UTF8, "application/json");

            HttpResponseMessage ollamaResponse;
            try
            {
                ollamaResponse = await _httpClient.PostAsync(BuildOllamaUri("/api/generate"), content);
            }
            catch (Exception e)
            {
                await Respond(
                    context,
                    IsOllamaReady() ? 502 : 503,
                    new ErrorResponse(false, "Bundled Ollama is unavailable.", BuildOllamaUnavailableDetails(e.Message)));
                return;
            }

            var ollamaBody = await ollamaResponse.Content.ReadAsStringAsync();
            if (!ollamaResponse.IsSuccessStatusCode)
            {
                await Respond(context, (int)ollamaResponse.StatusCode, new ErrorResponse(false, "Ollama returned an error during token_count.", ollamaBody));
                return;
            }

            if (TryReadPromptEvalCount(ollamaBody, out var exactTokens))
            {
                Console.WriteLine($"OLLAMA_IO TOKEN_COUNT generate_probe exact=true model={_chatModel} chars={text.Length} tokens={exactTokens}");
                await Respond(context, 200, new TokenCountResponse(true, _chatModel, exactTokens, true));
                return;
            }

            Console.WriteLine(
                $"OLLAMA_IO TOKEN_COUNT generate_probe exact=false model={_chatModel} chars={text.Length} ollamaBodySnippet={TruncateMiddle(ollamaBody, headChars: 240, tailChars: 120)}");
            await Respond(
                context,
                502,
                new ErrorResponse(false, "Ollama token_count did not return prompt_eval_count.", ollamaBody));
        }

        // Intentional extraction: this sequence is used by initial startup and restart recovery.
        private async Task StartManagedOllamaAsync(string reason)
        {
            var ollamaExecutable = GetBundledOllamaExecutablePath();
            var ollamaModelsDirectory = GetBundledOllamaModelsDirectory();
            Directory.CreateDirectory(ollamaModelsDirectory);

            if (!File.Exists(ollamaExecutable))
            {
                throw new FileNotFoundException(
                    $"Bundled Ollama executable not found at '{ollamaExecutable}'.",
                    ollamaExecutable);
            }

            var processStartInfo = new ProcessStartInfo
            {
                FileName = ollamaExecutable,
                Arguments = "serve",
                WorkingDirectory = Path.GetDirectoryName(ollamaExecutable) ?? _repositoryRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            processStartInfo.Environment["OLLAMA_HOST"] = $"127.0.0.1:{_ollamaPort}";
            processStartInfo.Environment["OLLAMA_FLASH_ATTENTION"] = "1";
            processStartInfo.Environment["OLLAMA_MODELS"] = ollamaModelsDirectory;
            // processStartInfo.Environment["OLLAMA_DEBUG"] = "1";
            processStartInfo.Environment["OLLAMA_LOG_FORMAT"] = "json";

            Console.WriteLine($"OLLAMA_IO Starting bundled Ollama child ({reason}) on 127.0.0.1:{_ollamaPort}.");
            if (_filterOffNoisyLogs)
            {
                _ollamaLogNoiseFilter = new OllamaLogNoiseFilter();
            }

            var process = Process.Start(processStartInfo)
                ?? throw new InvalidOperationException("Failed to start bundled Ollama child process.");
            process.EnableRaisingEvents = true;
            process.OutputDataReceived += OnOllamaOutputDataReceived;
            process.ErrorDataReceived += OnOllamaErrorDataReceived;
            process.Exited += OnOllamaExited;
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            // Replace any exited handle we were keeping before exposing the new process as the current child.
            Process? staleProcessToDispose = null;
            lock (_ollamaStateSync)
            {
                if (_ollamaProcess is not null && !_ollamaProcess.HasExited)
                {
                    throw new InvalidOperationException("Attempted to start a new Ollama child while the previous child is still running.");
                }

                staleProcessToDispose = _ollamaProcess;
                _ollamaProcess = process;
                _ollamaReady = false;
                _ollamaHttpReady = false;
                _ollamaStopping = false;
            }

            DisposeProcessHandle(staleProcessToDispose);

            try
            {
                await WaitForOllamaReadyAsync(process);
                lock (_ollamaStateSync)
                {
                    if (ReferenceEquals(_ollamaProcess, process))
                    {
                        _ollamaHttpReady = true;
                    }
                }

                // After an unexpected exit, bind may still be active: reload weights before marking inference-ready (initial bind primes in ProcessRequest_bindRun instead).
                var shouldPrimeAfterHttp = false;
                lock (_ollamaStateSync)
                {
                    shouldPrimeAfterHttp = _runBound && ReferenceEquals(_ollamaProcess, process);
                }

                if (shouldPrimeAfterHttp)
                {
                    await PrimeBundledOllamaModelAsync(reason);
                    FlushPendingPrimeSummary();
                }

                lock (_ollamaStateSync)
                {
                    if (ReferenceEquals(_ollamaProcess, process))
                    {
                        _ollamaReady = _runBound;
                        _ollamaStopping = false;
                        _ollamaRestartAttempts = 0;
                    }
                }

                Console.WriteLine(
                    _runBound
                        ? $"OLLAMA_IO (ollama) Ready on http://127.0.0.1:{_ollamaPort}/"
                        : $"OLLAMA_IO (ollama) HTTP ready on http://127.0.0.1:{_ollamaPort}/ (awaiting POST /initialize).");
            }
            catch (Exception e)
            {
                lock (_ollamaStateSync)
                {
                    if (ReferenceEquals(_ollamaProcess, process))
                    {
                        _ollamaStopping = true;
                        _ollamaReady = false;
                        _ollamaHttpReady = false;
                        _ollamaProcess = null;
                    }
                }

                StopOllamaProcess(process, "startup failure");
                throw new InvalidOperationException(
                    $"Bundled Ollama failed to become ready on port {_ollamaPort}. {e.Message}{DescribeRecentOllamaErrors()}");
            }
        }

        // Single minimal /api/generate so Ollama loads the configured model before external /chat traffic (lazy load otherwise).
        private async Task PrimeBundledOllamaModelAsync(string logTag)
        {
            if (!_disableBundledOllamaBootstrapForTesting)
            {
                Process? proc;
                lock (_ollamaStateSync)
                {
                    proc = _ollamaProcess;
                }

                if (proc is null || proc.HasExited)
                {
                    throw new InvalidOperationException("Bundled Ollama process is not running; cannot prime model.");
                }
            }

            var options = BuildOllamaOptionsPayload();
            var ollamaPayload = new
            {
                model = _chatModel,
                prompt = ".",
                stream = false,
                truncate = false,
                think = _ollamaThink,
                options = new { options.num_ctx, options.num_keep, num_predict = 1 }
            };

            var requestJson = JsonSerializer.Serialize(ollamaPayload);
            if (!_filterOffNoisyLogs)
            {
                WriteTrafficLine("OLLAMA_IO TRAFFIC PRIME_REQUEST_JSON " + requestJson);
            }

            using var content = new StringContent(requestJson, Encoding.UTF8, "application/json");
            var primeStartedUtc = DateTime.UtcNow;

            HttpResponseMessage response;
            try
            {
                response = await _httpClient.PostAsync(BuildOllamaUri("/api/generate"), content);
            }
            catch (Exception e)
            {
                throw new InvalidOperationException($"Ollama priming request failed ({logTag}). {e.Message}", e);
            }

            var responseBody = await response.Content.ReadAsStringAsync();
            if (!_filterOffNoisyLogs)
            {
                WriteTrafficLine("OLLAMA_IO TRAFFIC PRIME_RESPONSE_BODY " + JsonSerializer.Serialize(responseBody));
            }

            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException(
                    $"Ollama priming returned {(int)response.StatusCode} ({logTag}). Body: {TruncateMiddle(responseBody, headChars: 200, tailChars: 120)}");
            }

            EmitPrimeSummaryIfFiltered(logTag, requestJson, responseBody, DateTime.UtcNow - primeStartedUtc);
        }

        private void EmitPrimeSummaryIfFiltered(string logTag, string requestJson, string responseBody, TimeSpan elapsed)
        {
            if (_filterOffNoisyLogs && _ollamaLogNoiseFilter is not null)
            {
                foreach (var line in _ollamaLogNoiseFilter.RecordPrimeAttempt(logTag, requestJson, elapsed, _chatModel))
                {
                    Console.WriteLine("OLLAMA_IO (ollama:summary) " + line);
                }

                return;
            }

            Console.WriteLine(
                $"OLLAMA_IO Model priming succeeded ({logTag}) model={_chatModel} logSnippet={TruncateMiddle(responseBody, headChars: 120, tailChars: 80)}");
        }

        private void FlushPendingPrimeSummary()
        {
            if (!_filterOffNoisyLogs || _ollamaLogNoiseFilter is null)
            {
                return;
            }

            foreach (var line in _ollamaLogNoiseFilter.FlushPrimeSummary(_chatModel))
            {
                Console.WriteLine("OLLAMA_IO (ollama:summary) " + line);
            }
        }

        // Intentional extraction: shared by startup and crash-recovery restart paths.
        private async Task WaitForOllamaReadyAsync(Process process)
        {
            Exception? lastError = null;
            var deadline = DateTime.UtcNow + OllamaReadyTimeout;

            while (DateTime.UtcNow < deadline)
            {
                if (process.HasExited)
                {
                    throw new InvalidOperationException($"Bundled Ollama exited before becoming ready (exit code {process.ExitCode}).");
                }

                try
                {
                    using var response = await _httpClient.GetAsync(BuildOllamaUri("/"));
                    if (response.IsSuccessStatusCode)
                    {
                        return;
                    }

                    lastError = new InvalidOperationException($"Health probe returned {(int)response.StatusCode}.");
                }
                catch (Exception e)
                {
                    lastError = e;
                }

                await Task.Delay(OllamaReadyPollInterval);
            }

            throw new TimeoutException($"Timed out waiting for bundled Ollama readiness. {lastError?.Message}");
        }

        // Intentional extraction: one place manages retry limits and keeps only a single restart loop active.
        private async Task RestartOllamaAfterUnexpectedExitAsync()
        {
            await _ollamaRestartGate.WaitAsync();
            try
            {
                if (_shutdownRequested || IsOllamaReady())
                {
                    return;
                }

                while (!_shutdownRequested && !IsOllamaReady())
                {
                    int attemptNumber;
                    lock (_ollamaStateSync)
                    {
                        if (_ollamaRestartAttempts >= MaxOllamaRestartAttempts)
                        {
                            Console.WriteLine($"OLLAMA_IO (ollama:ERR) Restart limit reached ({MaxOllamaRestartAttempts}); leaving provider unavailable.");
                            return;
                        }

                        _ollamaRestartAttempts++;
                        attemptNumber = _ollamaRestartAttempts;
                    }

                    Console.WriteLine($"OLLAMA_IO Restarting bundled Ollama child ({attemptNumber}/{MaxOllamaRestartAttempts}).");
                    try
                    {
                        await Task.Delay(OllamaRestartBackoff);
                        await StartManagedOllamaAsync($"restart {attemptNumber}/{MaxOllamaRestartAttempts}");
                        Console.WriteLine("OLLAMA_IO (ollama) Restart succeeded.");
                        return;
                    }
                    catch (Exception e)
                    {
                        Console.WriteLine($"OLLAMA_IO (ollama:ERR) Restart {attemptNumber}/{MaxOllamaRestartAttempts} failed: {e.Message}");
                    }
                }
            }
            finally
            {
                _ollamaRestartGate.Release();
            }
        }
        #endregion

        #region Helper methods
        private async Task<bool> RespondIfOllamaUnavailableAsync(HttpListenerContext context)
        {
            if (IsOllamaReady())
            {
                return true;
            }

            await Respond(context, 503, new ErrorResponse(false, "Bundled Ollama is not ready.", BuildOllamaUnavailableDetails()));
            return false;
        }

        private static void DisposeProcessHandle(Process? process)
        {
            if (process is null)
            {
                return;
            }

            try
            {
                process.Dispose();
            }
            catch (Exception)
            {
            }
        }

        private void StopCurrentOllamaProcess(string reason)
        {
            Process? processToStop;
            lock (_ollamaStateSync)
            {
                _ollamaReady = false;
                _ollamaStopping = true;
                processToStop = _ollamaProcess;
                _ollamaProcess = null;
            }

            StopOllamaProcess(processToStop, reason);
        }

        private void StopOllamaProcess(Process? process, string reason)
        {
            if (process is null)
            {
                return;
            }

            try
            {
                if (!process.HasExited)
                {
                    // A hard kill is acceptable here: Ollama does not own engine-critical persisted state.
                    Console.WriteLine($"OLLAMA_IO Stopping bundled Ollama child ({reason}).");
                    process.Kill(entireProcessTree: true);
                    process.WaitForExit(3000);
                }
            }
            catch (Exception e)
            {
                Console.WriteLine($"OLLAMA_IO (ollama:ERR) Error while stopping child process: {e.Message}");
            }
            finally
            {
                DisposeProcessHandle(process);
            }
        }

        private bool IsOllamaReady()
        {
            lock (_ollamaStateSync)
            {
                if (_disableBundledOllamaBootstrapForTesting)
                {
                    return _ollamaReady;
                }

                return _ollamaReady && _ollamaProcess is not null && !_ollamaProcess.HasExited;
            }
        }

        private void RememberOllamaErrorLine(string line)
        {
            var linesToRemember = _filterOffNoisyLogs && _ollamaLogNoiseFilter is not null
                ? _ollamaLogNoiseFilter.ProcessLine(line)
                : [line];

            lock (_ollamaStateSync)
            {
                foreach (var remembered in linesToRemember)
                {
                    _recentOllamaErrorLines.Enqueue(remembered);
                    while (_recentOllamaErrorLines.Count > MaxCapturedOllamaErrorLines)
                    {
                        _recentOllamaErrorLines.Dequeue();
                    }
                }
            }
        }

        private string DescribeRecentOllamaErrors()
        {
            lock (_ollamaStateSync)
            {
                if (_recentOllamaErrorLines.Count == 0)
                {
                    return string.Empty;
                }

                return " Recent Ollama stderr: " + string.Join(" | ", _recentOllamaErrorLines);
            }
        }

        private void OnOllamaOutputDataReceived(object sender, DataReceivedEventArgs e)
        {
            if (string.IsNullOrWhiteSpace(e.Data))
            {
                return;
            }

            EmitFilteredOllamaLines(e.Data, isStderr: false);
        }

        private void OnOllamaErrorDataReceived(object sender, DataReceivedEventArgs e)
        {
            if (string.IsNullOrWhiteSpace(e.Data))
            {
                return;
            }

            RememberOllamaErrorLine(e.Data);
            EmitFilteredOllamaLines(e.Data, isStderr: true);
        }

        private void EmitFilteredOllamaLines(string rawLine, bool isStderr)
        {
            if (_filterOffNoisyLogs && _ollamaLogNoiseFilter is not null)
            {
                foreach (var line in _ollamaLogNoiseFilter.ProcessLine(rawLine))
                {
                    // Keep the OLLAMA_IO prefix so the WPF monitor continues to pick up these lines.
                    Console.WriteLine(FormatFilteredOllamaConsoleLine(line, isStderr));
                }

                return;
            }

            var channelPrefix = isStderr ? "OLLAMA_IO (ollama:ERR) " : "OLLAMA_IO (ollama) ";
            var trafficKind = isStderr ? "OLLAMA_STDERR " : "OLLAMA_STDOUT ";
            Console.WriteLine(channelPrefix + rawLine);
            WriteTrafficLine("OLLAMA_IO TRAFFIC " + trafficKind + rawLine);
        }

        private static string FormatFilteredOllamaConsoleLine(string line, bool isStderr)
        {
            if (IsFilterSummaryLine(line))
            {
                return "OLLAMA_IO (ollama:summary) " + line;
            }

            return isStderr ? "OLLAMA_IO (ollama:ERR) " + line : "OLLAMA_IO (ollama) " + line;
        }

        private static bool IsFilterSummaryLine(string line) =>
            line.StartsWith("loaded ", StringComparison.Ordinal)
            || line.StartsWith("embedded ", StringComparison.Ordinal)
            || line.StartsWith("primed ", StringComparison.Ordinal)
            || line.StartsWith("ctx=", StringComparison.Ordinal)
            || line.StartsWith("CUDA backend loaded", StringComparison.Ordinal)
            || line.StartsWith("nomic-embed:", StringComparison.Ordinal)
            || line.Contains("n_ctx_train=", StringComparison.Ordinal)
            || (line.Contains(" weights + ", StringComparison.Ordinal) && line.Contains(" graph = ", StringComparison.Ordinal));

        private void OnOllamaExited(object? sender, EventArgs e)
        {
            if (sender is not Process exitedProcess)
            {
                return;
            }

            int exitCode;
            try
            {
                exitCode = exitedProcess.ExitCode;
            }
            catch (Exception)
            {
                exitCode = int.MinValue;
            }

            var shouldRestart = false;
            lock (_ollamaStateSync)
            {
                if (!ReferenceEquals(_ollamaProcess, exitedProcess))
                {
                    return;
                }

                _ollamaReady = false;
                shouldRestart = !_shutdownRequested && !_ollamaStopping;
            }

            if (_filterOffNoisyLogs && _ollamaLogNoiseFilter is not null)
            {
                foreach (var line in _ollamaLogNoiseFilter.FlushPending())
                {
                    Console.WriteLine(FormatFilteredOllamaConsoleLine(line, isStderr: true));
                }
            }

            Console.WriteLine($"OLLAMA_IO (ollama:ERR) Child process exited with code {exitCode}.");
            if (shouldRestart)
            {
                _ = RestartOllamaAfterUnexpectedExitAsync();
            }
        }

        private string GetBundledOllamaExecutablePath() =>
            Path.Combine(_repositoryRoot, "third_party", "ollama", "ollama.exe");

        private string GetBundledOllamaModelsDirectory() =>
            Path.Combine(_repositoryRoot, "third_party", "ollama", "models");

        private string BuildOllamaUri(string path) =>
            $"http://127.0.0.1:{_ollamaPort}{EngineConfiguration.NormalizePath(path)}";

        private OllamaOptionsPayload BuildOllamaOptionsPayload() =>
            new(_ollamaNumCtx, OllamaRequestNumKeep);

        private Dictionary<string, object?> BuildOllamaChatPayload(
            IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages,
            JsonElement? format,
            string? keepAlive,
            int? numPredict = null)
        {
            var baseOptions = BuildOllamaOptionsPayload();
            object options = numPredict is int predict
                ? new { baseOptions.num_ctx, baseOptions.num_keep, num_predict = predict }
                : baseOptions;

            var payload = new Dictionary<string, object?>
            {
                ["model"] = _chatModel,
                ["messages"] = messages,
                ["stream"] = false,
                ["truncate"] = false,
                ["think"] = _ollamaThink,
                ["options"] = options
            };

            if (format.HasValue)
            {
                payload["format"] = format.Value;
            }

            if (!string.IsNullOrWhiteSpace(keepAlive))
            {
                payload["keep_alive"] = keepAlive.Trim();
            }

            return payload;
        }

        private string LoadBundledSummarizeSystemPrompt()
        {
            var path = Path.Combine(AppContext.BaseDirectory, "prompts", "summarize_system.md");
            if (!File.Exists(path))
            {
                throw new InvalidOperationException($"llm_provider_qwen: missing bundled summarize system prompt at '{path}'.");
            }

            var text = File.ReadAllText(path).Trim();
            if (string.IsNullOrWhiteSpace(text))
            {
                throw new InvalidOperationException($"llm_provider_qwen: bundled summarize system prompt at '{path}' is empty.");
            }

            return text;
        }

        private static string BuildSummarizeUserPrompt(SummarizeRequest request)
        {
            var builder = new StringBuilder();
            builder.AppendLine("Summarize the following transcript for durable episodic recall.");
            if (request.StartTurn is int startTurn && request.EndTurn is int endTurn)
            {
                builder.AppendLine($"Turn range: {startTurn}-{endTurn}");
            }

            builder.AppendLine();
            builder.Append(request.Content.Trim());
            return builder.ToString();
        }

        private static bool TryReadPromptEvalCount(string ollamaBody, out int promptEvalCount)
        {
            promptEvalCount = 0;
            try
            {
                using var doc = JsonDocument.Parse(ollamaBody);
                if (!doc.RootElement.TryGetProperty("prompt_eval_count", out var countElement)
                    || countElement.ValueKind != JsonValueKind.Number
                    || !countElement.TryGetInt32(out var count)
                    || count <= 0)
                {
                    return false;
                }

                promptEvalCount = count;
                return true;
            }
            catch (JsonException)
            {
                return false;
            }
        }

        private string BuildOllamaUnavailableDetails(string? extraDetail = null)
        {
            var baseDetail = $"LlmProvider_qwen is waiting for its bundled Ollama child on port {_ollamaPort}.";
            var recentErrors = DescribeRecentOllamaErrors();
            if (string.IsNullOrWhiteSpace(extraDetail))
            {
                return baseDetail + recentErrors;
            }

            return $"{baseDetail} {extraDetail}{recentErrors}";
        }

        private async Task Respond(HttpListenerContext context, int statusCode, object payload)
        {
            var response = context.Response;
            response.StatusCode = statusCode;
            response.ContentType = "application/json";
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload)); // using the "object" type to avoid having to define a type for every kind of communication.
            response.ContentLength64 = bytes.LongLength;
            await response.OutputStream.WriteAsync(bytes);
            response.OutputStream.Close();
        }

        private static string TruncateForLog(string text, int maxLen = OllamaRequestPreviewMaxChars)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            return text.Length <= maxLen ? text : text[..maxLen] + "…";
        }

        private static string TruncateMiddle(string text, int headChars, int tailChars)
        {
            if (string.IsNullOrEmpty(text))
            {
                return text;
            }

            if (headChars < 0 || tailChars < 0)
            {
                throw new ArgumentOutOfRangeException(nameof(headChars), "headChars and tailChars must be >= 0.");
            }

            var max = headChars + tailChars + 5;
            if (text.Length <= max || headChars == 0 || tailChars == 0)
            {
                return TruncateForLog(text, maxLen: max);
            }

            return text[..headChars] + " ... " + text[^tailChars..];
        }

        private static void WriteTrafficLine(string payload)
        {
            Console.WriteLine(payload);
        }

        private static string DescribeChatMessagesForLog(IReadOnlyList<ChatGenerateRequest.ChatMessageDto> messages)
        {
            var parts = new List<string>(messages.Count);
            foreach (var m in messages)
            {
                var role = string.IsNullOrWhiteSpace(m.Role) ? "(unknown)" : m.Role.Trim();
                var content = m.Content ?? string.Empty;
                parts.Add($"{role}({content.Length} chars)='{TruncateMiddle(content, headChars: 80, tailChars: 60)}'");
            }

            return "messagesPreview=" + string.Join(" | ", parts);
        }
        #endregion
    }
}
