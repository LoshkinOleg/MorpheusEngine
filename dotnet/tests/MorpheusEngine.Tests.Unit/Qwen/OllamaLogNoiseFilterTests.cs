using FluentAssertions;

namespace MorpheusEngine.Tests.Unit.Qwen;

[Trait("Category", "Unit")]
public sealed class OllamaLogNoiseFilterTests
{
    [Fact]
    // Verifies that per-runner bootstrap discovery lines are dropped while the final GPU summary is kept.
    public void ProcessLine_BootstrapDiscoveryIntermediate_DropsIntermediateKeepsGpuSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine(
                """time=2026-06-03T14:21:00.394+02:00 level=DEBUG source=runner.go:437 msg="bootstrap discovery took" duration=229.4489ms""")
            .Should()
            .BeEmpty();

        filter.ProcessLine(
                """time=2026-06-03T14:21:00.394+02:00 level=DEBUG source=runner.go:40 msg="GPU bootstrap discovery took" duration=706.7276ms""")
            .Should()
            .ContainSingle()
            .Which.Should().Contain("GPU bootstrap discovery took");
    }

    [Fact]
    // Verifies that subprocess env dumps redact PATH while preserving Ollama-specific variables.
    public void ProcessLine_SubprocessEnvLine_RedactsPathPreservesOllamaHost()
    {
        var filter = new OllamaLogNoiseFilter();
        const string line =
            """time=2026-06-03T14:20:59.713+02:00 level=DEBUG source=server.go:445 msg=subprocess OLLAMA_HOST=127.0.0.1:8795 PATH="C:\\repo\\ollama;C:\\Windows\\system32" OLLAMA_MODELS=C:\\repo\\models""";

        var emitted = filter.ProcessLine(line);

        emitted.Should().ContainSingle();
        emitted[0].Should().Contain("PATH=<redacted>");
        emitted[0].Should().Contain("OLLAMA_HOST=127.0.0.1:8795");
        emitted[0].Should().NotContain("Windows\\system32");
    }

    [Fact]
    // Verifies that tensor and layer assignment spam collapses to one summary when the burst ends.
    public void ProcessLine_TensorBurst_EndsWithSummaryLine()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine(
                "llama_model_loader: loaded meta data with 34 key-value pairs and 339 tensors from C:\\models\\blob (version GGUF V3 (latest))")
            .Should()
            .BeEmpty();

        filter.ProcessLine("create_tensor: loading tensor blk.0.attn_q.weight").Should().BeEmpty();
        filter.ProcessLine("create_tensor: loading tensor blk.0.attn_k.weight").Should().BeEmpty();
        filter.ProcessLine("load_tensors: layer   0 assigned to device CUDA0, is_swa = 0").Should().BeEmpty();
        filter.ProcessLine("load_tensors: layer   1 assigned to device CUDA0, is_swa = 0").Should().BeEmpty();

        var emitted = filter.ProcessLine("load_tensors: offloaded 29/29 layers to GPU");

        emitted.Should().ContainSingle().Which.Should().Be("loaded 339 tensors, 2 layers offloaded to CUDA0");
    }

    [Fact]
    // Verifies that KV metadata detail lines are suppressed and loaded-meta lines are not echoed verbatim.
    public void ProcessLine_KvMetadataDump_SuppressesDetailWithoutRawLoadedMeta()
    {
        var filter = new OllamaLogNoiseFilter();
        const string loadedMeta =
            "llama_model_loader: loaded meta data with 34 key-value pairs and 339 tensors from C:\\models\\blob (version GGUF V3 (latest))";

        filter.ProcessLine(loadedMeta).Should().BeEmpty();
        filter.ProcessLine("llama_model_loader: Dumping metadata keys/values. Note: KV overrides do not apply in this output.")
            .Should()
            .BeEmpty();
        filter.ProcessLine("llama_model_loader: - kv   0:                       general.architecture str              = qwen2")
            .Should()
            .BeEmpty();
        filter.ProcessLine(loadedMeta).Should().BeEmpty();
    }

    [Fact]
    // Verifies that the vocab-only pre-pass produces no log output.
    public void ProcessLine_VocabOnlyPass_DropsEntireSection()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("print_info: file format = GGUF V3 (latest)").Should().BeEmpty();
        filter.ProcessLine("print_info: vocab_only       = 1").Should().BeEmpty();
        filter.ProcessLine("print_info: general.name     = Qwen2.5 7B Instruct").Should().BeEmpty();
        filter.ProcessLine("load: printing all EOG tokens:").Should().BeEmpty();
        filter.ProcessLine("llama_model_load: vocab only - skipping tensors").Should().BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:25.674+02:00 level=INFO source=server.go:444 msg=\"starting runner\"").Should().BeEmpty();
    }

    [Fact]
    // Verifies that the real print_info block collapses to one model summary line.
    public void ProcessLine_PrintInfoRealLoad_EmitsCompactSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("print_info: file format = GGUF V3 (latest)").Should().BeEmpty();
        filter.ProcessLine("print_info: file type   = Q4_K - Medium").Should().BeEmpty();
        filter.ProcessLine("print_info: file size   = 4.36 GiB (4.91 BPW) ").Should().BeEmpty();
        filter.ProcessLine("print_info: vocab_only       = 0").Should().BeEmpty();
        filter.ProcessLine("print_info: arch             = qwen2").Should().BeEmpty();
        filter.ProcessLine("print_info: model type       = 7B").Should().BeEmpty();
        filter.ProcessLine("print_info: general.name     = Qwen2.5 7B Instruct").Should().BeEmpty();
        filter.ProcessLine("print_info: n_ctx_train      = 32768").Should().BeEmpty();
        filter.ProcessLine("print_info: n_rot            = 128").Should().BeEmpty();

        var emitted = filter.ProcessLine("load_tensors: loading model tensors, this can take a while... (mmap = false)");

        emitted.Should().ContainSingle();
        emitted[0].Should().Contain("qwen2");
        emitted[0].Should().Contain("7B");
        emitted[0].Should().Contain("Q4_K");
        emitted[0].Should().Contain("4.36 GiB");
        emitted[0].Should().Contain("n_ctx_train=32768");
    }

    [Fact]
    // Verifies that llama_context spam collapses but the n_ctx_train warning is preserved.
    public void ProcessLine_LlamaContext_KeepsWarningAndSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("llama_context: constructing llama_context").Should().BeEmpty();
        filter.ProcessLine("llama_context: n_ctx         = 8192").Should().BeEmpty();
        filter.ProcessLine("llama_context: n_batch       = 512").Should().BeEmpty();
        filter.ProcessLine("llama_context: flash_attn    = enabled").Should().BeEmpty();
        filter.ProcessLine("llama_context: n_ctx_seq (8192) < n_ctx_train (32768) -- the full capacity of the model will not be utilized")
            .Should()
            .ContainSingle();
        filter.ProcessLine("llama_kv_cache:      CUDA0 KV buffer size =   448.00 MiB").Should().BeEmpty();
        filter.ProcessLine("llama_context:      CUDA0 compute buffer size =   311.00 MiB").Should().BeEmpty();
        filter.ProcessLine("llama_context: graph splits = 2").Should().BeEmpty();

        var emitted = filter.ProcessLine("time=2026-06-03T14:31:27.879+02:00 level=INFO source=server.go:1402 msg=\"llama runner started in 2.20 seconds\"");

        emitted.Should().HaveCount(2);
        emitted[0].Should().StartWith("ctx=8192");
        emitted[0].Should().Contain("flash_attn=on");
        emitted[1].Should().Contain("llama runner started");
    }

    [Fact]
    // Verifies that duplicate llama runner status JSON lines are emitted only once.
    public void ProcessLine_DuplicateLlamaRunnerStarted_Dedupes()
    {
        var filter = new OllamaLogNoiseFilter();
        const string line =
            "time=2026-06-03T14:31:27.879+02:00 level=INFO source=server.go:1402 msg=\"llama runner started in 2.20 seconds\"";

        filter.ProcessLine(line).Should().ContainSingle();
        filter.ProcessLine(line).Should().BeEmpty();
    }

    [Fact]
    // Verifies that bootstrap runner spawns without --model are dropped while model loads are kept.
    public void ProcessLine_StartingRunner_DropsBootstrapKeepsModelLoad()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine(
                "time=2026-06-03T14:31:24.297+02:00 level=INFO source=server.go:444 msg=\"starting runner\" cmd=\"C:\\ollama\\ollama.exe runner --ollama-engine --port 63052\"")
            .Should()
            .BeEmpty();

        filter.ProcessLine(
                "time=2026-06-03T14:31:25.674+02:00 level=INFO source=server.go:444 msg=\"starting runner\" cmd=\"C:\\ollama\\ollama.exe runner --model C:\\models\\blob --port 63082\"")
            .Should()
            .ContainSingle();
    }

    [Fact]
    // Verifies that runner subprocess listen addresses are dropped while parent listen is kept.
    public void ProcessLine_RunnerSubprocessListen_DroppedParentKept()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("time=2026-06-03T14:31:25.868+02:00 level=INFO source=runner.go:1001 msg=\"Server listening on 127.0.0.1:63082\"")
            .Should()
            .BeEmpty();

        filter.ProcessLine("time=2026-06-03T14:31:24.272+02:00 level=INFO source=routes.go:1810 msg=\"Listening on 127.0.0.1:8795 (version 0.21.1)\"")
            .Should()
            .ContainSingle();
    }

    [Fact]
    // Verifies that device.go memory breakdown collapses to one line per model.
    public void ProcessLine_DeviceMemoryBurst_EmitsSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("time=2026-06-03T14:31:25.678+02:00 level=INFO source=device.go:240 msg=\"model weights\" device=CUDA0 size=\"4.1 GiB\"").Should().BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:25.678+02:00 level=INFO source=device.go:251 msg=\"kv cache\" device=CUDA0 size=\"448.0 MiB\"").Should().BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:25.678+02:00 level=INFO source=device.go:262 msg=\"compute graph\" device=CUDA0 size=\"478.0 MiB\"").Should().BeEmpty();

        var emitted = filter.ProcessLine("time=2026-06-03T14:31:25.678+02:00 level=INFO source=device.go:272 msg=\"total memory\" size=\"5.0 GiB\"");

        emitted.Should().ContainSingle();
        emitted[0].Should().Contain("4.1 GiB");
        emitted[0].Should().Contain("448.0 MiB");
        emitted[0].Should().Contain("5.0 GiB");
    }

    [Fact]
    // Verifies that nomic-embed fit/alloc/commit load requests collapse to one summary.
    public void ProcessLine_NomicEmbedLoad_EmitsSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("time=2026-06-03T14:31:30.279+02:00 level=INFO source=ggml.go:136 msg=\"\" architecture=nomic-bert file_type=F16 name=nomic-embed-text-v1.5")
            .Should()
            .BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:30.274+02:00 level=INFO source=runner.go:1290 msg=load request=\"{Operation:fit ...}\"")
            .Should()
            .BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:30.641+02:00 level=INFO source=runner.go:1290 msg=load request=\"{Operation:alloc ...}\"")
            .Should()
            .BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:30.660+02:00 level=INFO source=runner.go:1290 msg=load request=\"{Operation:commit ...}\"")
            .Should()
            .BeEmpty();
        filter.ProcessLine("time=2026-06-03T14:31:30.661+02:00 level=INFO source=ggml.go:494 msg=\"offloaded 13/13 layers to GPU\"").Should().BeEmpty();

        var emitted = filter.ProcessLine("time=2026-06-03T14:31:30.662+02:00 level=INFO source=device.go:272 msg=\"total memory\" size=\"567.6 MiB\"");

        emitted.Should().ContainSingle();
        emitted[0].Should().Be("nomic-embed: loaded 13/13 layers, 567.6 MiB");
    }

    [Fact]
    // Verifies that ggml system caps and cpu_windows lines are emitted only once per filter instance.
    public void ProcessLine_HardwareConstants_EmitOnce()
    {
        var filter = new OllamaLogNoiseFilter();
        const string ggml =
            """time=2026-06-03T14:31:25.867+02:00 level=INFO source=ggml.go:104 msg=system CPU.0.AVX2=1 CUDA.0.USE_GRAPHS=1""";
        const string cpu =
            """time=2026-06-03T14:31:25.337+02:00 level=INFO source=cpu_windows.go:148 msg=packages count=1""";

        filter.ProcessLine(ggml).Should().ContainSingle();
        filter.ProcessLine(ggml).Should().BeEmpty();
        filter.ProcessLine(cpu).Should().ContainSingle();
        filter.ProcessLine(cpu).Should().BeEmpty();
    }

    [Fact]
    // Verifies that ggml_cuda_init spam collapses to one CUDA backend summary.
    public void ProcessLine_GgmlCudaInit_EmitsSummaryOnce()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("ggml_cuda_init: GGML_CUDA_FORCE_MMQ:    no").Should().BeEmpty();
        filter.ProcessLine("ggml_cuda_init: GGML_CUDA_FORCE_CUBLAS: no").Should().BeEmpty();
        var first = filter.ProcessLine("ggml_cuda_init: found 1 CUDA devices:");
        first.Should().ContainSingle().Which.Should().Be("CUDA backend loaded (1 device)");
        filter.ProcessLine("  Device 0: NVIDIA GeForce RTX 3070 Laptop GPU, compute capability 8.6").Should().BeEmpty();

        filter.ProcessLine("ggml_cuda_init: GGML_CUDA_FORCE_MMQ:    no").Should().BeEmpty();
    }

    [Fact]
    // Verifies that embedding cache-slot and bos/eos spam collapses to a chunk count summary.
    public void ProcessLine_EmbeddingBurst_EndsWithChunkSummary()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("""time=2026-06-03T14:21:06.039+02:00 level=DEBUG source=vocabulary.go:52 msg="adding bos token to prompt" id=101""")
            .Should()
            .BeEmpty();
        filter.ProcessLine("""time=2026-06-03T14:21:06.039+02:00 level=DEBUG source=vocabulary.go:61 msg="adding eos token to prompt" id=102""")
            .Should()
            .BeEmpty();
        filter.ProcessLine("""time=2026-06-03T14:21:06.039+02:00 level=DEBUG source=cache.go:151 msg="loading cache slot" id=0 cache=53 prompt=62 used=0 remaining=62""")
            .Should()
            .BeEmpty();
        filter.ProcessLine("""time=2026-06-03T14:21:06.046+02:00 level=DEBUG source=cache.go:151 msg="loading cache slot" id=0 cache=62 prompt=64 used=0 remaining=64""")
            .Should()
            .BeEmpty();

        var emitted = filter.ProcessLine(
            "time=2026-06-03T14:21:06.050+02:00 level=INFO source=routes.go:100 msg=\"done\"");

        emitted.Should().HaveCount(2);
        emitted[0].Should().Be("embedded 2 chunks");
        emitted[1].Should().Contain("msg=\"done\"");
    }

    [Fact]
    // Verifies that harmless control-token EOG warnings and missing-key debug lines are dropped.
    public void ProcessLine_ControlTokenAndMissingKeyNoise_DropsLines()
    {
        var filter = new OllamaLogNoiseFilter();

        filter.ProcessLine("load: control token: 151644 '<|im_start|>' is not marked as EOG").Should().BeEmpty();
        filter.ProcessLine("load: printing all EOG tokens:").Should().BeEmpty();
        filter.ProcessLine("print_info: EOG token        = 151645 '<|im_end|>'").Should().BeEmpty();
        filter.ProcessLine("""time=2026-06-03T14:21:00.901+02:00 level=DEBUG source=ggml.go:325 msg="key with type not found" key=general.alignment default=32""")
            .Should()
            .BeEmpty();
    }

    [Fact]
    // Verifies that FlushPending emits summaries for bursts still open when the child process exits.
    public void FlushPending_OpenTensorBurst_EmitsSummary()
    {
        var filter = new OllamaLogNoiseFilter();
        filter.ProcessLine("create_tensor: loading tensor blk.0.attn_q.weight");

        filter.FlushPending().Should().ContainSingle().Which.Should().StartWith("loaded ");
    }

    [Fact]
    // Verifies that duplicate priming payloads collapse to one multi-pass summary.
    public void RecordPrimeAttempt_DuplicatePayload_EmitsTwoPassSummary()
    {
        var filter = new OllamaLogNoiseFilter();
        const string json = """{"model":"qwen2.5:7b-instruct","prompt":"."}""";

        filter.RecordPrimeAttempt("initial", json, TimeSpan.FromSeconds(2.9), "qwen2.5:7b-instruct").Should().BeEmpty();
        var second = filter.RecordPrimeAttempt("post_initialize_bind", json, TimeSpan.FromSeconds(2.8), "qwen2.5:7b-instruct");

        second.Should().ContainSingle();
        second[0].Should().Contain("primed (2 passes)");
        second[0].Should().Contain("5700");
    }

    [Fact]
    // Verifies that a single priming pass can be flushed as a one-line summary.
    public void FlushPrimeSummary_SinglePass_EmitsSummary()
    {
        var filter = new OllamaLogNoiseFilter();
        const string json = """{"model":"qwen2.5:7b-instruct","prompt":"."}""";

        filter.RecordPrimeAttempt("post_initialize_bind", json, TimeSpan.FromSeconds(3), "qwen2.5:7b-instruct").Should().BeEmpty();
        var flushed = filter.FlushPrimeSummary("qwen2.5:7b-instruct");

        flushed.Should().ContainSingle();
        flushed[0].Should().Be("primed (1 pass) in 3000ms model=qwen2.5:7b-instruct");
    }
}
