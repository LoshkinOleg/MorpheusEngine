using System.Text.RegularExpressions;

namespace MorpheusEngine
{
    /// <summary>
    /// Stateful post-processor for bundled Ollama child stdout/stderr before Morpheus logs it.
    /// Best-effort substring matching; Ollama log text may change between versions.
    /// </summary>
    internal sealed partial class OllamaLogNoiseFilter
    {
        private static readonly string[] Empty = [];
        private static readonly TimeSpan PrimeDedupeWindow = TimeSpan.FromSeconds(30);

        private int _tensorLoadLineCount = 0;
        private int _layerAssignmentCount = 0;
        private string _lastLayerDevice = "";
        private int _metadataTensorCount = 0;

        private bool _embeddingBurstActive = false;
        private int _embeddingChunkCount = 0;

        private bool _inVocabOnlyPass = false;
        private bool _inPrintInfoRealLoad = false;
        private bool _printInfoSummaryEmitted = false;
        private string _printInfoArch = "";
        private string _printInfoModelType = "";
        private string _printInfoFileType = "";
        private string _printInfoFileSize = "";
        private string _printInfoNCtxTrain = "";
        private string _printInfoGeneralName = "";

        private bool _llamaContextBurstActive = false;
        private string _llamaContextNCtx = "";
        private string _llamaContextBatch = "";
        private string _llamaContextFlashAttn = "";
        private string _llamaContextKvMiB = "";
        private string _llamaContextComputeMiB = "";

        private readonly HashSet<string> _dedupedMessages = new(StringComparer.Ordinal);
        private bool _cudaBackendSummaryEmitted = false;
        private bool _ggmlSystemCapsEmitted = false;
        private bool _cpuWindowsEmitted = false;

        private bool _nomicEmbedLoadActive = false;
        private string _nomicLayersOffloaded = "";
        private string _nomicTotalMemory = "";

        private bool _deviceMemoryBurstActive = false;
        private string _deviceMemoryModelLabel = "";
        private string _deviceWeights = "";
        private string _deviceKv = "";
        private string _deviceGraph = "";
        private string _deviceTotal = "";

        private string? _lastPrimeRequestJson = null;
        private int _primePassCount = 0;
        private long _primeTotalMs = 0;
        private DateTime _primeWindowStartUtc = DateTime.MinValue;

        public IReadOnlyList<string> ProcessLine(string rawLine)
        {
            if (string.IsNullOrWhiteSpace(rawLine))
            {
                return Empty;
            }

            var results = new List<string>();
            EndOpenBurstsForNewLine(rawLine, results);

            if (_inVocabOnlyPass)
            {
                if (lineEndsVocabOnlyPass(rawLine))
                {
                    _inVocabOnlyPass = false;
                }

                return Empty;
            }

            if (TryDropUnconditionally(rawLine))
            {
                return Empty;
            }

            if (TryHandleDedupedJsonMessage(rawLine))
            {
                return Empty;
            }

            if (TryHandleBootstrapRunnerSpawn(rawLine))
            {
                return Empty;
            }

            if (TryHandleRunnerSubprocessListen(rawLine))
            {
                return Empty;
            }

            if (TryHandleGgmlCudaInit(rawLine, results))
            {
                return results;
            }

            if (TryHandleGgmlSystemCaps(rawLine, results))
            {
                return results;
            }

            if (TryHandleCpuWindows(rawLine, results))
            {
                return results;
            }

            if (TryHandleNomicEmbedLoad(rawLine, results))
            {
                return results;
            }

            if (TryHandleDeviceMemoryLine(rawLine, results))
            {
                return results;
            }

            if (IsKvMetadataDetailLine(rawLine))
            {
                return Empty;
            }

            if (IsLoadedMetaDataLine(rawLine, out _))
            {
                return Empty;
            }

            if (rawLine.Contains("loading first model", StringComparison.Ordinal))
            {
                _printInfoSummaryEmitted = false;
            }

            if (TryHandlePrintInfoLine(rawLine, results))
            {
                return results;
            }

            if (ShouldDropAfterBurstSummary(rawLine))
            {
                return results;
            }

            if (TryHandleLlamaContextLine(rawLine, results))
            {
                return results;
            }

            var line = rawLine;
            if (IsSubprocessEnvLine(line))
            {
                line = RedactPathInSubprocessLine(line);
            }

            if (IsCreateTensorLine(line))
            {
                _tensorLoadLineCount++;
                return Empty;
            }

            if (TryCaptureLayerAssignment(line))
            {
                return Empty;
            }

            if (IsEmbeddingNoiseLine(line))
            {
                _embeddingBurstActive = true;
                if (line.Contains("loading cache slot", StringComparison.Ordinal))
                {
                    _embeddingChunkCount++;
                }

                return Empty;
            }

            results.Add(line);
            return results;
        }

        public IReadOnlyList<string> FlushPending()
        {
            var results = new List<string>();
            TryAppendPrintInfoSummary(results);
            TryAppendLlamaContextSummary(results);
            TryAppendTensorSummary(results);
            TryAppendEmbeddingSummary(results);
            TryAppendNomicEmbedSummary(results);
            TryAppendDeviceMemorySummary(results);
            return results;
        }

        /// <summary>
        /// Records a Morpheus /api/generate priming attempt; may emit a collapsed summary when duplicate payloads arrive.
        /// </summary>
        public IReadOnlyList<string> RecordPrimeAttempt(string logTag, string requestJson, TimeSpan elapsed, string modelName)
        {
            var elapsedMs = (long)elapsed.TotalMilliseconds;
            var now = DateTime.UtcNow;

            if (_primePassCount > 0
                && string.Equals(_lastPrimeRequestJson, requestJson, StringComparison.Ordinal)
                && now - _primeWindowStartUtc <= PrimeDedupeWindow)
            {
                _primePassCount++;
                _primeTotalMs += elapsedMs;
                return
                [
                    $"primed ({_primePassCount} passes) in {_primeTotalMs}ms model={modelName} ({logTag})"
                ];
            }

            var pending = FlushPrimeSummary(modelName);
            _lastPrimeRequestJson = requestJson;
            _primePassCount = 1;
            _primeTotalMs = elapsedMs;
            _primeWindowStartUtc = now;
            return pending;
        }

        /// <summary>Emits a single-pass priming summary when no duplicate payload followed within the dedupe window.</summary>
        public IReadOnlyList<string> FlushPrimeSummary(string modelName)
        {
            if (_primePassCount == 0)
            {
                return Empty;
            }

            var summary = _primePassCount == 1
                ? $"primed (1 pass) in {_primeTotalMs}ms model={modelName}"
                : $"primed ({_primePassCount} passes) in {_primeTotalMs}ms model={modelName}";

            _lastPrimeRequestJson = null;
            _primePassCount = 0;
            _primeTotalMs = 0;
            _primeWindowStartUtc = DateTime.MinValue;
            return [summary];
        }

        private void EndOpenBurstsForNewLine(string line, List<string> results)
        {
            if (EndsTensorBurst(line))
            {
                TryAppendTensorSummary(results);
            }

            if (EndsEmbeddingBurst(line))
            {
                TryAppendEmbeddingSummary(results);
            }

            if (EndsPrintInfoBurst(line))
            {
                TryAppendPrintInfoSummary(results);
            }

            if (EndsLlamaContextBurst(line))
            {
                TryAppendLlamaContextSummary(results);
            }

            if (EndsDeviceMemoryBurst(line))
            {
                TryAppendDeviceMemorySummary(results);
            }
        }

        private static bool lineEndsVocabOnlyPass(string line) =>
            line.Contains("llama_model_load: vocab only", StringComparison.Ordinal);

        private bool TryDropUnconditionally(string line)
        {
            if (IsBootstrapDiscoveryIntermediate(line))
            {
                return true;
            }

            if (IsControlTokenEogWarning(line) || IsTokenDumpNoise(line) || IsMissingGgufKeyDebug(line))
            {
                return true;
            }

            if (line.Contains("print_info: vocab_only", StringComparison.Ordinal)
                && line.Contains("= 1", StringComparison.Ordinal))
            {
                _inVocabOnlyPass = true;
                _inPrintInfoRealLoad = false;
                return true;
            }

            return false;
        }

        private bool TryHandleDedupedJsonMessage(string line)
        {
            if (!TryExtractJsonLogMessage(line, out var msg))
            {
                return false;
            }

            if (!IsDedupedMessage(msg))
            {
                return false;
            }

            if (_dedupedMessages.Contains(msg))
            {
                return true;
            }

            _dedupedMessages.Add(msg);
            return false;
        }

        private static bool TryExtractJsonLogMessage(string line, out string msg)
        {
            msg = string.Empty;
            var match = JsonMsgRegex().Match(line);
            if (match.Success)
            {
                msg = match.Groups["msg"].Value;
                return true;
            }

            const string prefix = "msg=\"";
            var start = line.IndexOf(prefix, StringComparison.Ordinal);
            if (start < 0)
            {
                return false;
            }

            start += prefix.Length;
            var end = line.IndexOf('"', start);
            if (end <= start)
            {
                return false;
            }

            msg = line[start..end];
            return true;
        }

        private static bool IsDedupedMessage(string msg) =>
            msg.StartsWith("llama runner started in ", StringComparison.Ordinal)
            || string.Equals(msg, "waiting for llama runner to start responding", StringComparison.Ordinal);

        private static bool TryHandleBootstrapRunnerSpawn(string line)
        {
            if (!line.Contains("msg=\"starting runner\"", StringComparison.Ordinal))
            {
                return false;
            }

            return !line.Contains("--model", StringComparison.Ordinal);
        }

        private static bool TryHandleRunnerSubprocessListen(string line)
        {
            if (line.Contains("routes.go:1810", StringComparison.Ordinal)
                && line.Contains("Listening on 127.0.0.1:", StringComparison.Ordinal))
            {
                return false;
            }

            return (line.Contains("runner.go:1001", StringComparison.Ordinal)
                    || line.Contains("runner.go:1452", StringComparison.Ordinal))
                && line.Contains("Server listening on 127.0.0.1:", StringComparison.Ordinal);
        }

        private bool TryHandleGgmlCudaInit(string line, List<string> results)
        {
            if (line.Contains("ggml_cuda_init:", StringComparison.Ordinal)
                || (line.Contains("Device 0:", StringComparison.Ordinal) && line.Contains("compute capability", StringComparison.Ordinal)))
            {
                if (_cudaBackendSummaryEmitted)
                {
                    return true;
                }

                if (line.Contains("found 1 CUDA devices", StringComparison.Ordinal)
                    || line.Contains("loaded CUDA backend", StringComparison.Ordinal))
                {
                    _cudaBackendSummaryEmitted = true;
                    results.Add("CUDA backend loaded (1 device)");
                }

                return true;
            }

            return false;
        }

        private bool TryHandleGgmlSystemCaps(string line, List<string> results)
        {
            if (!line.Contains("source=ggml.go:104 msg=system", StringComparison.Ordinal))
            {
                return false;
            }

            if (_ggmlSystemCapsEmitted)
            {
                return true;
            }

            _ggmlSystemCapsEmitted = true;
            return false;
        }

        private bool TryHandleCpuWindows(string line, List<string> results)
        {
            if (!line.Contains("source=cpu_windows.go:", StringComparison.Ordinal))
            {
                return false;
            }

            if (_cpuWindowsEmitted)
            {
                return true;
            }

            _cpuWindowsEmitted = true;
            return false;
        }

        private bool TryHandleNomicEmbedLoad(string line, List<string> results)
        {
            if (line.Contains("architecture=nomic-bert", StringComparison.Ordinal))
            {
                _nomicEmbedLoadActive = true;
                return true;
            }

            if (!_nomicEmbedLoadActive)
            {
                return false;
            }

            if (line.Contains("runner.go:1290 msg=load request", StringComparison.Ordinal)
                && (line.Contains("Operation:fit", StringComparison.Ordinal)
                    || line.Contains("Operation:alloc", StringComparison.Ordinal)
                    || line.Contains("Operation:commit", StringComparison.Ordinal)))
            {
                return true;
            }

            var offloaded = OffloadedLayersRegex().Match(line);
            if (offloaded.Success)
            {
                _nomicLayersOffloaded = $"{offloaded.Groups["loaded"].Value}/{offloaded.Groups["total"].Value}";
                return true;
            }

            if (line.Contains("msg=\"total memory\"", StringComparison.Ordinal))
            {
                var mem = TotalMemoryRegex().Match(line);
                if (mem.Success)
                {
                    _nomicTotalMemory = mem.Groups["size"].Value;
                }

                TryAppendNomicEmbedSummary(results);
                _nomicEmbedLoadActive = false;
                return true;
            }

            return false;
        }

        private bool TryHandleDeviceMemoryLine(string line, List<string> results)
        {
            if (!line.Contains("source=device.go:", StringComparison.Ordinal))
            {
                return false;
            }

            if (line.Contains("msg=\"model weights\"", StringComparison.Ordinal))
            {
                _deviceMemoryBurstActive = true;
                _deviceWeights = ExtractQuotedSize(line);
                return true;
            }

            if (!_deviceMemoryBurstActive)
            {
                return false;
            }

            if (line.Contains("msg=\"kv cache\"", StringComparison.Ordinal))
            {
                _deviceKv = ExtractQuotedSize(line);
                return true;
            }

            if (line.Contains("msg=\"compute graph\"", StringComparison.Ordinal))
            {
                _deviceGraph = ExtractQuotedSize(line);
                return true;
            }

            if (line.Contains("msg=\"total memory\"", StringComparison.Ordinal))
            {
                _deviceTotal = ExtractQuotedSize(line);
                TryAppendDeviceMemorySummary(results);
                _deviceMemoryBurstActive = false;
                return true;
            }

            return false;
        }

        private bool TryHandlePrintInfoLine(string line, List<string> results)
        {
            if (!line.StartsWith("print_info:", StringComparison.Ordinal))
            {
                return false;
            }

            if (line.Contains("vocab_only", StringComparison.Ordinal))
            {
                if (line.Contains("= 0", StringComparison.Ordinal))
                {
                    _inPrintInfoRealLoad = true;
                }

                return true;
            }

            if (IsPrintInfoSummaryAnchor(line))
            {
                CapturePrintInfoField(line);
            }

            if (!_inPrintInfoRealLoad)
            {
                return true;
            }

            return true;
        }

        private static bool IsPrintInfoSummaryAnchor(string line) =>
            line.Contains("general.name", StringComparison.Ordinal)
            || line.Contains("print_info: arch", StringComparison.Ordinal)
            || line.Contains("model type", StringComparison.Ordinal)
            || line.Contains("file type", StringComparison.Ordinal)
            || line.Contains("file size", StringComparison.Ordinal)
            || line.Contains("n_ctx_train", StringComparison.Ordinal);

        private void CapturePrintInfoField(string line)
        {
            if (line.Contains("general.name", StringComparison.Ordinal))
            {
                _printInfoGeneralName = ExtractPrintInfoValue(line);
                _deviceMemoryModelLabel = ShortModelLabel(_printInfoGeneralName);
                return;
            }

            if (line.Contains("arch", StringComparison.Ordinal) && line.Contains("print_info: arch", StringComparison.Ordinal))
            {
                _printInfoArch = ExtractPrintInfoValue(line);
                return;
            }

            if (line.Contains("model type", StringComparison.Ordinal))
            {
                _printInfoModelType = ExtractPrintInfoValue(line);
                return;
            }

            if (line.Contains("file type", StringComparison.Ordinal))
            {
                _printInfoFileType = ExtractPrintInfoValue(line);
                return;
            }

            if (line.Contains("file size", StringComparison.Ordinal))
            {
                _printInfoFileSize = ExtractPrintInfoValue(line);
                return;
            }

            if (line.Contains("n_ctx_train", StringComparison.Ordinal))
            {
                _printInfoNCtxTrain = ExtractPrintInfoValue(line);
            }
        }

        private bool TryHandleLlamaContextLine(string line, List<string> results)
        {
            if (!line.StartsWith("llama_context:", StringComparison.Ordinal)
                && !line.StartsWith("llama_kv_cache:", StringComparison.Ordinal))
            {
                return false;
            }

            if (line.Contains("n_ctx_seq", StringComparison.Ordinal) && line.Contains("n_ctx_train", StringComparison.Ordinal))
            {
                results.Add(line);
                return true;
            }

            _llamaContextBurstActive = true;
            CaptureLlamaContextField(line);
            return true;
        }

        private void CaptureLlamaContextField(string line)
        {
            if (line.Contains("n_ctx         =", StringComparison.Ordinal) || line.Contains("n_ctx=", StringComparison.Ordinal))
            {
                var m = LlamaContextFieldRegex("n_ctx").Match(line);
                if (m.Success)
                {
                    _llamaContextNCtx = m.Groups["value"].Value.Trim();
                }
            }
            else if (line.Contains("n_batch", StringComparison.Ordinal))
            {
                var m = LlamaContextFieldRegex("n_batch").Match(line);
                if (m.Success)
                {
                    _llamaContextBatch = m.Groups["value"].Value.Trim();
                }
            }
            else if (line.Contains("flash_attn", StringComparison.Ordinal))
            {
                _llamaContextFlashAttn = line.Contains("enabled", StringComparison.Ordinal) ? "on" : "off";
            }
            else if (line.StartsWith("llama_kv_cache:", StringComparison.Ordinal) && line.Contains("KV buffer size", StringComparison.Ordinal))
            {
                var m = Regex.Match(line, @"=\s*(.+?)\s*$", RegexOptions.CultureInvariant);
                if (m.Success)
                {
                    _llamaContextKvMiB = m.Groups[1].Value.Trim();
                }
            }
            else if (line.Contains("compute buffer size", StringComparison.Ordinal) && line.Contains("CUDA0", StringComparison.Ordinal))
            {
                var m = Regex.Match(line, @"=\s*(.+?)\s*$", RegexOptions.CultureInvariant);
                if (m.Success)
                {
                    _llamaContextComputeMiB = m.Groups[1].Value.Trim();
                }
            }
        }

        private bool EndsPrintInfoBurst(string line) =>
            _inPrintInfoRealLoad
            && !line.StartsWith("print_info:", StringComparison.Ordinal)
            && !line.Contains("load: printing all EOG", StringComparison.Ordinal)
            && !line.Contains("load: special tokens", StringComparison.Ordinal)
            && !line.Contains("load: token to piece", StringComparison.Ordinal);

        private bool EndsLlamaContextBurst(string line) =>
            _llamaContextBurstActive
            && !line.StartsWith("llama_context:", StringComparison.Ordinal)
            && !line.StartsWith("llama_kv_cache:", StringComparison.Ordinal);

        private bool EndsDeviceMemoryBurst(string line) =>
            _deviceMemoryBurstActive
            && !line.Contains("source=device.go:", StringComparison.Ordinal);

        private void TryAppendPrintInfoSummary(List<string> results)
        {
            if (!_inPrintInfoRealLoad || _printInfoSummaryEmitted)
            {
                return;
            }

            if (string.IsNullOrWhiteSpace(_printInfoArch) && string.IsNullOrWhiteSpace(_printInfoGeneralName))
            {
                return;
            }

            var arch = string.IsNullOrWhiteSpace(_printInfoArch) ? "model" : _printInfoArch;
            var sizeLabel = string.IsNullOrWhiteSpace(_printInfoModelType) ? "?" : _printInfoModelType;
            var quant = NormalizeQuantLabel(_printInfoFileType);
            var fileSize = string.IsNullOrWhiteSpace(_printInfoFileSize) ? "?" : _printInfoFileSize;
            var nCtxTrain = string.IsNullOrWhiteSpace(_printInfoNCtxTrain) ? "?" : _printInfoNCtxTrain;
            results.Add($"{arch} {sizeLabel}, {quant}, {fileSize}, n_ctx_train={nCtxTrain}");
            _printInfoSummaryEmitted = true;
            _inPrintInfoRealLoad = false;
        }

        private void TryAppendLlamaContextSummary(List<string> results)
        {
            if (!_llamaContextBurstActive || string.IsNullOrWhiteSpace(_llamaContextNCtx))
            {
                return;
            }

            var batch = string.IsNullOrWhiteSpace(_llamaContextBatch) ? "?" : _llamaContextBatch;
            var flash = string.IsNullOrWhiteSpace(_llamaContextFlashAttn) ? "?" : _llamaContextFlashAttn;
            var kv = string.IsNullOrWhiteSpace(_llamaContextKvMiB) ? "?" : _llamaContextKvMiB;
            var compute = string.IsNullOrWhiteSpace(_llamaContextComputeMiB) ? "?" : _llamaContextComputeMiB;
            results.Add($"ctx={_llamaContextNCtx}, batch={batch}, flash_attn={flash}, kv={kv}, compute={compute}");
            _llamaContextBurstActive = false;
        }

        private void TryAppendNomicEmbedSummary(List<string> results)
        {
            if (string.IsNullOrWhiteSpace(_nomicLayersOffloaded) && string.IsNullOrWhiteSpace(_nomicTotalMemory))
            {
                return;
            }

            var layers = string.IsNullOrWhiteSpace(_nomicLayersOffloaded) ? "?/?" : _nomicLayersOffloaded;
            var memory = string.IsNullOrWhiteSpace(_nomicTotalMemory) ? "?" : _nomicTotalMemory;
            results.Add($"nomic-embed: loaded {layers} layers, {memory}");
            _nomicLayersOffloaded = "";
            _nomicTotalMemory = "";
        }

        private void TryAppendDeviceMemorySummary(List<string> results)
        {
            if (string.IsNullOrWhiteSpace(_deviceWeights))
            {
                return;
            }

            var label = string.IsNullOrWhiteSpace(_deviceMemoryModelLabel) ? "model" : _deviceMemoryModelLabel;
            var kv = string.IsNullOrWhiteSpace(_deviceKv) ? "0" : _deviceKv;
            var graph = string.IsNullOrWhiteSpace(_deviceGraph) ? "0" : _deviceGraph;
            var total = string.IsNullOrWhiteSpace(_deviceTotal) ? "?" : _deviceTotal;
            results.Add($"{label}: {_deviceWeights} weights + {kv} KV + {graph} graph = {total}");
            _deviceWeights = "";
            _deviceKv = "";
            _deviceGraph = "";
            _deviceTotal = "";
            _deviceMemoryBurstActive = false;
        }

        private bool EndsTensorBurst(string line) =>
            (_tensorLoadLineCount > 0 || _layerAssignmentCount > 0)
            && !IsCreateTensorLine(line)
            && !IsLayerAssignmentLine(line);

        private static bool ShouldDropAfterBurstSummary(string line) =>
            line.Contains("load_tensors: loading model tensors", StringComparison.Ordinal)
            || line.Contains("load_tensors: offloaded", StringComparison.Ordinal);

        private bool EndsEmbeddingBurst(string line) =>
            _embeddingBurstActive && !IsEmbeddingNoiseLine(line);

        private static bool IsCreateTensorLine(string line) =>
            line.Contains("create_tensor: loading tensor", StringComparison.Ordinal);

        private static bool IsLayerAssignmentLine(string line) =>
            LayerAssignmentRegex().IsMatch(line);

        private bool TryCaptureLayerAssignment(string line)
        {
            var match = LayerAssignmentRegex().Match(line);
            if (!match.Success)
            {
                return false;
            }

            _layerAssignmentCount++;
            _lastLayerDevice = match.Groups["device"].Value;
            return true;
        }

        private bool IsLoadedMetaDataLine(string line, out int tensorCount)
        {
            tensorCount = 0;
            var match = LoadedMetaDataRegex().Match(line);
            if (!match.Success || !int.TryParse(match.Groups["tensors"].Value, out tensorCount))
            {
                return false;
            }

            _metadataTensorCount = tensorCount;
            return true;
        }

        private void TryAppendTensorSummary(List<string> results)
        {
            if (_tensorLoadLineCount == 0 && _layerAssignmentCount == 0)
            {
                return;
            }

            var tensorCount = _metadataTensorCount > 0 ? _metadataTensorCount : _tensorLoadLineCount;
            var layerCount = _layerAssignmentCount;
            var device = string.IsNullOrWhiteSpace(_lastLayerDevice) ? "CPU" : _lastLayerDevice;
            results.Add($"loaded {tensorCount} tensors, {layerCount} layers offloaded to {device}");
            _tensorLoadLineCount = 0;
            _layerAssignmentCount = 0;
            _lastLayerDevice = "";
        }

        private void TryAppendEmbeddingSummary(List<string> results)
        {
            if (!_embeddingBurstActive)
            {
                return;
            }

            if (_embeddingChunkCount > 0)
            {
                results.Add($"embedded {_embeddingChunkCount} chunks");
            }

            _embeddingBurstActive = false;
            _embeddingChunkCount = 0;
        }

        private static bool IsKvMetadataDetailLine(string line) =>
            line.Contains("Dumping metadata keys/values", StringComparison.Ordinal)
            || line.Contains("llama_model_loader: - kv", StringComparison.Ordinal)
            || (line.Contains("llama_model_loader: - type", StringComparison.Ordinal));

        private static bool IsControlTokenEogWarning(string line) =>
            line.Contains("load: control token:", StringComparison.Ordinal);

        private static bool IsTokenDumpNoise(string line) =>
            line.Contains("load: printing all EOG", StringComparison.Ordinal)
            || EogTokenListRegex().IsMatch(line)
            || line.Contains("print_info: BOS token", StringComparison.Ordinal)
            || line.Contains("print_info: EOS token", StringComparison.Ordinal)
            || line.Contains("print_info: EOT token", StringComparison.Ordinal)
            || line.Contains("print_info: PAD token", StringComparison.Ordinal)
            || line.Contains("print_info: LF token", StringComparison.Ordinal)
            || line.Contains("print_info: FIM ", StringComparison.Ordinal)
            || line.Contains("print_info: EOG token", StringComparison.Ordinal)
            || line.Contains("load: special tokens cache size", StringComparison.Ordinal)
            || line.Contains("load: token to piece cache size", StringComparison.Ordinal);

        private static bool IsMissingGgufKeyDebug(string line) =>
            line.Contains("key with type not found", StringComparison.Ordinal);

        private static bool IsEmbeddingNoiseLine(string line) =>
            line.Contains("loading cache slot", StringComparison.Ordinal)
            || line.Contains("adding bos token to prompt", StringComparison.Ordinal)
            || line.Contains("adding eos token to prompt", StringComparison.Ordinal);

        private static bool IsBootstrapDiscoveryIntermediate(string line) =>
            line.Contains("bootstrap discovery took", StringComparison.Ordinal)
            && !line.Contains("GPU bootstrap discovery took", StringComparison.Ordinal)
            && !line.Contains("overall device VRAM discovery took", StringComparison.Ordinal);

        private static bool IsSubprocessEnvLine(string line) =>
            line.Contains("msg=subprocess", StringComparison.Ordinal)
            && line.Contains("PATH=", StringComparison.Ordinal);

        private static string RedactPathInSubprocessLine(string line) =>
            PathRedactionRegex().Replace(line, "PATH=<redacted>");

        private static string ExtractPrintInfoValue(string line)
        {
            var idx = line.IndexOf('=');
            return idx < 0 ? string.Empty : line[(idx + 1)..].Trim();
        }

        private static string ExtractQuotedSize(string line)
        {
            var m = QuotedSizeRegex().Match(line);
            return m.Success ? m.Groups["size"].Value : string.Empty;
        }

        private static string NormalizeQuantLabel(string fileType)
        {
            if (string.IsNullOrWhiteSpace(fileType))
            {
                return "?";
            }

            if (fileType.Contains("Q4_K", StringComparison.OrdinalIgnoreCase))
            {
                return "Q4_K_M";
            }

            return fileType.Replace(" - Medium", "", StringComparison.Ordinal).Trim();
        }

        private static string ShortModelLabel(string generalName)
        {
            if (string.IsNullOrWhiteSpace(generalName))
            {
                return "model";
            }

            return generalName.Replace(" Instruct", "", StringComparison.OrdinalIgnoreCase).Trim();
        }

        private static Regex LlamaContextFieldRegex(string fieldName) =>
            new($@"{fieldName}\s*=\s*(?<value>[^\s]+)", RegexOptions.CultureInvariant);

        [GeneratedRegex(@"loaded meta data with \d+ key-value pairs and (?<tensors>\d+) tensors", RegexOptions.CultureInvariant)]
        private static partial Regex LoadedMetaDataRegex();

        [GeneratedRegex(@"load_tensors: layer\s+\d+\s+assigned to device (?<device>CUDA\d+|CPU)", RegexOptions.CultureInvariant)]
        private static partial Regex LayerAssignmentRegex();

        [GeneratedRegex(@"PATH=""[^""]*""", RegexOptions.CultureInvariant)]
        private static partial Regex PathRedactionRegex();

        [GeneratedRegex(@"msg=""(?<msg>[^""]+)""", RegexOptions.CultureInvariant)]
        private static partial Regex JsonMsgRegex();

        [GeneratedRegex(@"^load:\s+-\s+\d+", RegexOptions.CultureInvariant)]
        private static partial Regex EogTokenListRegex();

        [GeneratedRegex(@"offloaded\s+(?<loaded>\d+)/(?<total>\d+)\s+layers", RegexOptions.CultureInvariant)]
        private static partial Regex OffloadedLayersRegex();

        [GeneratedRegex(@"size=""(?<size>[^""]+)""", RegexOptions.CultureInvariant)]
        private static partial Regex QuotedSizeRegex();

        [GeneratedRegex(@"msg=""total memory""[^""]*size=""(?<size>[^""]+)""", RegexOptions.CultureInvariant)]
        private static partial Regex TotalMemoryRegex();
    }
}
