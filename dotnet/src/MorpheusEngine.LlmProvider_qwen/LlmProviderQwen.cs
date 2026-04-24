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
        /// <summary>Every outbound call to the bundled Ollama process (probe, warm-up, inference) uses this ceiling.</summary>
        private static readonly TimeSpan OllamaHttpTimeout = TimeSpan.FromSeconds(30);
        private const int MaxOllamaRestartAttempts = 3;
        private const int MaxCapturedOllamaErrorLines = 20;
        private const int OllamaRequestNumKeep = -1;

        /// <summary>Minimal user turn for Ollama warm-up only; not shown to players.</summary>
        private const string OllamaWarmupUserContent = "[morpheus_engine_warmup]";

        private const int WarmupResponseLogMaxChars = 500;
        private const int OllamaRequestPreviewMaxChars = 160;
        private const int OllamaResponseLogMaxChars = 500;

        private OllamaTrafficFile? _trafficFile;
        private readonly object _trafficFileSync = new();

        // Instance-owned HttpClient for Ollama (GET /, warm-up, /api/chat, /api/generate); disposed in Shutdown().
        // Model load is handled during InitializeAsync (warm-up) so normal inference stays within this bound.
        private readonly HttpClient _httpClient = new()
        {
            Timeout = OllamaHttpTimeout
        };

        private readonly HttpListener _listener = new HttpListener(); // Inbound listener for responding to http messages.
        private readonly SemaphoreSlim _ollamaRestartGate = new(1, 1);
        private readonly object _ollamaStateSync = new();
        private readonly Queue<string> _recentOllamaErrorLines = [];
        private bool _shutdownRequested = false;
        private bool _runBound = false;
        private string _boundGameProjectId = "";
        private string _boundRunId = "";
        private bool _ollamaHttpReady = false;
        private bool _ollamaReady = false;
        private bool _ollamaStopping = false;
        private int _ollamaRestartAttempts = 0;
        private Process? _ollamaProcess;

        /// <summary>Repository root resolved once at startup for locating bundled Ollama assets.</summary>
        private string _repositoryRoot = "";

        /// <summary>Qwen module-owned Ollama port from engine configuration.</summary>
        private int _ollamaPort = 0;

        /// <summary>Ollama model for /api/chat and /api/generate; resolved once in <see cref="InitializeAsync"/> from engine_config.json.</summary>
        private string _chatModel = "";

        /// <summary>Forwarded on every Ollama /api/chat and /api/generate request as options.num_ctx.</summary>
        private int _ollamaNumCtx = 0;

        /// <summary>Director narration system text (instructions + lore) for Ollama warm-up; built once per process.</summary>
        private string _narrationWarmupSystemContent = "";

        /// <summary>Configured game project id used to build <see cref="_narrationWarmupSystemContent"/> (logging only).</summary>
        private string _warmupGameProjectId = "";
        #endregion

        #region Public methods
        public async Task Run()
        {
            try
            {
                // Start the bundled Ollama child first so /health only becomes reachable after inference is actually ready.
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
        #endregion

        #region Private methods
        // Intentional single use method.
        private async Task InitializeAsync()
        {
            var configuration = EngineConfigLoader.GetConfiguration();
            _repositoryRoot = configuration.RepositoryRoot;
            _chatModel = configuration.LlmProviderDefaultChatModel.Trim();
            _ollamaPort = configuration.LlmProviderOllamaListenPort;
            _ollamaNumCtx = configuration.LlmProviderNumCtx;
            if (string.IsNullOrWhiteSpace(_chatModel))
            {
                throw new InvalidOperationException(
                    "llm_provider_qwen: LlmProviderDefaultChatModel from engine configuration is empty (check default_chat_model in engine_config.json).");
            }

            // Warm-up prompt content is built after host bind_run so the active run's gameProjectId is used.
            // warmup_game_project_id remains for logging/debug defaults only.
            _warmupGameProjectId = configuration.LlmProviderWarmupGameProjectId;
            _narrationWarmupSystemContent = string.Empty;
            // Traffic file is created only after POST /engine_log/activate.

            // Bundled Ollama inherits the host module job (see MorpheusEngine Run); no nested Job Object here.
            await StartManagedOllamaAsync("initial startup");

            var qwenListen = configuration.PortMap.GetRequiredPort("llm_provider_qwen");
            _listener.Prefixes.Add($"http://127.0.0.1:{qwenListen}/");
            _listener.Start();
            Console.WriteLine(
                $"ready listen=http://127.0.0.1:{qwenListen}/ model='{_chatModel}' ollama=http://127.0.0.1:{_ollamaPort}/ num_ctx={_ollamaNumCtx} "
                + $"warmup_game_project_id='{_warmupGameProjectId}' awaiting_bind_run=true");
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

                // /health endpoint mirrors whether the bundled Ollama child is currently ready to accept inference.
                if (path.Equals("/health", StringComparison.OrdinalIgnoreCase))
                {
                    var isHealthy = IsOllamaReady();
                    var reason = isHealthy
                        ? "healthy"
                        : !_ollamaHttpReady
                            ? "ollama_starting"
                            : !_runBound
                                ? "run_not_bound"
                                : "warming_up";
                    await Respond(context, isHealthy ? 200 : 503, new ModuleHealthResponse(isHealthy, reason));
                    return;
                }

                if (path.Equals("/engine_log/activate", StringComparison.OrdinalIgnoreCase))
                {
                    await ProcessRequest_engineLogActivate(context);
                    return;
                }

                if (path.Equals(EngineInternalRoutes.BindRunPath, StringComparison.OrdinalIgnoreCase))
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

        /// <summary>Loopback-only; binds on-disk traffic log for this run (same JSON as bind_run).</summary>
        private async Task ProcessRequest_engineLogActivate(HttpListenerContext context)
        {
            if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
                return;
            }

            if (!IsLoopbackRequest(context))
            {
                await Respond(context, 403, new ErrorResponse(false, "engine_log/activate is only allowed from loopback."));
                return;
            }

            string body;
            using (var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding))
            {
                body = await reader.ReadToEndAsync();
            }

            var result = EngineLogActivateCommands.TryActivateFromJsonBody(
                body,
                _repositoryRoot,
                primaryNotJoin: false,
                _jsonOptions);
            if (!result.Ok || result.GameProjectId is null || result.RunId is null)
            {
                await Respond(context, 400, new ErrorResponse(false, result.ErrorMessage ?? "Activation failed."));
                return;
            }

            var trafficPath = GameRunLogPaths.GetTrafficLogPath(_repositoryRoot, result.GameProjectId, result.RunId);
            lock (_trafficFileSync)
            {
                _trafficFile = new OllamaTrafficFile(trafficPath);
            }

            await Respond(context, 200, new InitializeModuleResponse(true));
        }

        private async Task ProcessRequest_bindRun(HttpListenerContext context)
        {
            if (!string.Equals(context.Request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
            {
                await Respond(context, 405, new ErrorResponse(false, "Method not allowed; use POST."));
                return;
            }

            if (!IsLoopbackRequest(context))
            {
                await Respond(context, 403, new ErrorResponse(false, "bind_run is only allowed from loopback."));
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

            await _ollamaRestartGate.WaitAsync();
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

                _warmupGameProjectId = _boundGameProjectId;
                _narrationWarmupSystemContent = DirectorNarrationSystemPrompt.Build(_repositoryRoot, _boundGameProjectId);

                var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(60);
                while (!_ollamaHttpReady && DateTime.UtcNow < deadline)
                {
                    await Task.Delay(250);
                }

                if (!_ollamaHttpReady)
                {
                    await Respond(context, 503, new ErrorResponse(false, "Bundled Ollama is not ready yet; try bind_run again."));
                    return;
                }

                await WarmUpBundledOllamaModelAsync();
                lock (_ollamaStateSync)
                {
                    _ollamaReady = true;
                    _ollamaStopping = false;
                    _ollamaRestartAttempts = 0;
                }

                Console.WriteLine($"[LlmProvider_qwen] Bound run runId={_boundRunId} gameProjectId={_boundGameProjectId} and completed warm-up.");
                await Respond(context, 200, new InitializeModuleResponse(true));
            }
            catch (FileNotFoundException e)
            {
                await Respond(context, 500, new ErrorResponse(false, e.Message, e.FileName));
            }
            catch (InvalidOperationException e)
            {
                await Respond(context, 500, new ErrorResponse(false, e.Message));
            }
            catch (Exception e)
            {
                await Respond(context, 500, new ErrorResponse(false, "Failed to bind run.", e.Message));
            }
            finally
            {
                _ollamaRestartGate.Release();
            }
        }

        private static bool IsLoopbackRequest(HttpListenerContext context)
        {
            var ep = context.Request.RemoteEndPoint;
            return ep is null || System.Net.IPAddress.IsLoopback(ep.Address);
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
                options = BuildOllamaOptionsPayload()
            };
            var promptTrimmed = request.Prompt.Trim();
            var systemTrimmed = request.System?.Trim();
            Console.WriteLine(
                $"OLLAMA_IO REQUEST model={model} promptChars={promptTrimmed.Length} systemChars={(systemTrimmed is null ? 0 : systemTrimmed.Length)} "
                + $"promptPreview='{TruncateMiddle(promptTrimmed, headChars: 80, tailChars: 60)}'");

            // Convert to json for transmission.
            var requestJson = JsonSerializer.Serialize(ollamaPayload);
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

            // Ollama /api/chat expects { model, messages, stream, truncate, options }; model is fixed at provider InitializeAsync() from engine_config.json.
            var ollamaPayload = new
            {
                model = _chatModel,
                messages = request.Messages,
                stream = false,
                truncate = false,
                options = BuildOllamaOptionsPayload()
            };
            Console.WriteLine(
                $"OLLAMA_IO CHAT_REQUEST model={_chatModel} messages={request.Messages.Count} {DescribeChatMessagesForLog(request.Messages)}");

            var requestJson = JsonSerializer.Serialize(ollamaPayload);
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

            Console.WriteLine($"OLLAMA_IO Starting bundled Ollama child ({reason}) on 127.0.0.1:{_ollamaPort}.");
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

                if (_runBound)
                {
                    // GET / only proves the HTTP server is up; the first real request still loads the GGUF into VRAM/RAM and can take tens of seconds.
                    await WarmUpBundledOllamaModelAsync();
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
                        : $"OLLAMA_IO (ollama) HTTP ready on http://127.0.0.1:{_ollamaPort}/ (awaiting bind_run).");
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

        /// <summary>
        /// Forces Ollama to load the configured model and the Director-grade system context (instructions + lore) before the module listens.
        /// </summary>
        private async Task WarmUpBundledOllamaModelAsync()
        {
            var sharedOptions = BuildOllamaOptionsPayload();
            var warmupPayload = new
            {
                model = _chatModel,
                messages = new object[]
                {
                    new { role = "system", content = _narrationWarmupSystemContent },
                    new { role = "user", content = OllamaWarmupUserContent }
                },
                stream = false,
                truncate = false,
                options = new { num_ctx = sharedOptions.num_ctx, num_keep = sharedOptions.num_keep, num_predict = 1 }
            };

            Console.WriteLine(
                $"OLLAMA_IO WARMUP_REQUEST model={_chatModel} warmup_game_project_id={_warmupGameProjectId} systemChars={_narrationWarmupSystemContent.Length} userToken={OllamaWarmupUserContent}");

            var json = JsonSerializer.Serialize(warmupPayload);
            WriteTrafficLine("OLLAMA_IO TRAFFIC WARMUP_REQUEST_JSON " + json);
            var content = new StringContent(json, Encoding.UTF8, "application/json");
            using var response = await _httpClient.PostAsync(BuildOllamaUri("/api/chat"), content);
            var body = await response.Content.ReadAsStringAsync();
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException(
                    $"Bundled Ollama model warm-up failed: {(int)response.StatusCode} {response.ReasonPhrase}. Body: {body}");
            }

            WriteTrafficLine("OLLAMA_IO TRAFFIC WARMUP_RESPONSE_BODY " + JsonSerializer.Serialize(body));

            var bodyForLog = TruncateMiddle(body, headChars: 240, tailChars: 120);
            Console.WriteLine($"OLLAMA_IO WARMUP_RESPONSE status={(int)response.StatusCode} bodySnippet={bodyForLog}");
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
                return _ollamaReady && _ollamaProcess is not null && !_ollamaProcess.HasExited;
            }
        }

        private void RememberOllamaErrorLine(string line)
        {
            lock (_ollamaStateSync)
            {
                _recentOllamaErrorLines.Enqueue(line);
                while (_recentOllamaErrorLines.Count > MaxCapturedOllamaErrorLines)
                {
                    _recentOllamaErrorLines.Dequeue();
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
            if (!string.IsNullOrWhiteSpace(e.Data))
            {
                // Keep the OLLAMA_IO prefix so the WPF monitor continues to pick up these lines.
                Console.WriteLine("OLLAMA_IO (ollama) " + e.Data);
                WriteTrafficLine("OLLAMA_IO TRAFFIC OLLAMA_STDOUT " + e.Data);
            }
        }

        private void OnOllamaErrorDataReceived(object sender, DataReceivedEventArgs e)
        {
            if (!string.IsNullOrWhiteSpace(e.Data))
            {
                RememberOllamaErrorLine(e.Data);
                Console.WriteLine("OLLAMA_IO (ollama:ERR) " + e.Data);
                WriteTrafficLine("OLLAMA_IO TRAFFIC OLLAMA_STDERR " + e.Data);
            }
        }

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

        private void WriteTrafficLine(string payload)
        {
            if (_trafficFile is null)
            {
                return;
            }

            _trafficFile.AppendFullLine(EngineLog.FormatLinePrefix(isError: false) + payload);
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
