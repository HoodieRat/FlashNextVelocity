using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal sealed class EngineManager : IDisposable
{
    private const string RequiredRuntimeRevision = "fnv-async-halo-pipeline-v12";
    private Process? _process;
    private StreamWriter? _logWriter;
    private EngineConfig? _appliedConfig;
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(2) };
    private readonly object _gate = new();

    public event Action<string>? LogLine;
    public event Action? StateChanged;
    public bool IsRunning => _process is { HasExited: false };
    public int? ProcessId => IsRunning ? _process!.Id : null;

    public bool RequiresRestart(EngineConfig cfg) =>
        !IsRunning || _appliedConfig is null || !_appliedConfig.EquivalentTo(cfg);

    public async Task StartAsync(EngineConfig cfg)
    {
        if (IsRunning) return;
        if (!File.Exists(AppPaths.EngineExe)) throw new FileNotFoundException("Native engine is not built.", AppPaths.EngineExe);
        if (string.IsNullOrWhiteSpace(cfg.Model) || !File.Exists(cfg.Model)) throw new InvalidOperationException("Configure a valid first model shard before starting the engine.");
        Directory.CreateDirectory(AppPaths.Logs);
        Emit($"Desktop executable: {Environment.ProcessPath ?? AppContext.BaseDirectory}");
        Emit($"Launching engine: {AppPaths.EngineExe}");
        Emit($"Live config: {AppPaths.Config}");

        foreach (var p in Process.GetProcessesByName("FlashNextVelocity.Engine"))
        {
            try { p.Kill(true); p.WaitForExit(3000); } catch { }
        }

        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
        var logPath = Path.Combine(AppPaths.Logs, $"engine-{stamp}.log");
        _logWriter?.Dispose();
        _logWriter = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false)) { AutoFlush = true };

        var psi = new ProcessStartInfo
        {
            FileName = AppPaths.EngineExe,
            Arguments = $"--config \"{AppPaths.Config}\"",
            WorkingDirectory = AppPaths.AppDir,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        ApplyRuntimeEnvironment(psi);

        var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        proc.Exited += (_, _) =>
        {
            try
            {
                proc.WaitForExit();
                Emit($"Engine exited with code {proc.ExitCode} ({FormatExitCode(proc.ExitCode)}).{ExplainExitCode(proc.ExitCode)}");
            }
            catch (Exception e) { Emit("Engine exit diagnostic failed: " + e.Message); }
            if (ReferenceEquals(_process, proc))
            {
                _process = null;
                _appliedConfig = null;
            }
            StateChanged?.Invoke();
        };
        if (!proc.Start()) throw new InvalidOperationException("Failed to start the native engine process.");
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
        _process = proc;
        _appliedConfig = null;
        StateChanged?.Invoke();

        var deadline = DateTime.UtcNow + TimeSpan.FromMinutes(3);
        Exception? last = null;
        while (DateTime.UtcNow < deadline)
        {
            if (proc.HasExited)
            {
                proc.WaitForExit();
                throw new InvalidOperationException($"Native engine exited during startup with code {proc.ExitCode} ({FormatExitCode(proc.ExitCode)}).{ExplainExitCode(proc.ExitCode)} Check the Logs tab.");
            }

            string? fatalMismatch = null;
            try
            {
                using var r = await _http.GetAsync(cfg.BaseUrl + "/health");
                if (r.IsSuccessStatusCode)
                {
                    var text = await r.Content.ReadAsStringAsync();
                    fatalMismatch = FindConfigMismatch(text, cfg);
                    if (fatalMismatch is null)
                    {
                        _appliedConfig = cfg.Clone();
                        Emit($"Engine health check passed; active config verified (draft_max={cfg.DraftMax}, proposal={cfg.MtpProposalMode}, prefill_batch={cfg.PrefillBatch}, context={cfg.Context}).");
                        StateChanged?.Invoke();
                        return;
                    }
                }
            }
            catch (Exception e) { last = e; }

            if (fatalMismatch is not null)
            {
                try { if (!proc.HasExited) proc.Kill(true); } catch { }
                _process = null;
                _appliedConfig = null;
                StateChanged?.Invoke();
                throw new InvalidOperationException("Engine started with settings that do not match the saved config: " + fatalMismatch);
            }

            await Task.Delay(1000);
        }
        throw new TimeoutException("Engine did not become healthy within three minutes." + (last == null ? "" : " " + last.Message));
    }

    public async Task StopAsync()
    {
        var p = _process;
        if (p == null)
        {
            _appliedConfig = null;
            return;
        }
        try
        {
            if (!p.HasExited)
            {
                p.Kill(true);
                await p.WaitForExitAsync();
            }
        }
        catch { }
        finally
        {
            _process = null;
            _appliedConfig = null;
            StateChanged?.Invoke();
        }
    }

    public async Task RestartAsync(EngineConfig cfg)
    {
        await StopAsync();
        await Task.Delay(250);
        await StartAsync(cfg);
    }

    internal static string? FindConfigMismatch(string json, EngineConfig expected)
    {
        using var d = JsonDocument.Parse(json);
        var root = d.RootElement;
        var problems = new List<string>();

        static bool EqPath(string? a, string? b) => string.Equals(a ?? "", b ?? "", StringComparison.OrdinalIgnoreCase);
        static bool Near(double a, double b) => Math.Abs(a - b) <= 1e-4;
        static string Str(JsonElement root, string name) => root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
        static long Int(JsonElement root, string name, long missing = long.MinValue) => root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var x) ? x : missing;
        static double Num(JsonElement root, string name, double missing = double.NaN) => root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : missing;
        static bool Bool(JsonElement root, string name, bool missing = false) => root.TryGetProperty(name, out var v) && (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False) ? v.GetBoolean() : missing;

        void Check(bool ok, string name, object expectedValue, object actualValue)
        {
            if (!ok) problems.Add($"{name}: expected {expectedValue}, engine reported {actualValue}");
        }

        Check(string.Equals(Str(root, "runtime_revision"), RequiredRuntimeRevision, StringComparison.Ordinal),
            "runtime_revision", RequiredRuntimeRevision, Str(root, "runtime_revision"));

        var activeModel = Str(root, "config_model");
        var activeMtp = Str(root, "config_mtp");
        var activeMmproj = Str(root, "config_mmproj");
        Check(EqPath(activeModel, expected.Model), "model", expected.Model, activeModel);
        Check(EqPath(activeMtp, expected.Mtp), "mtp", expected.Mtp, activeMtp);
        Check(EqPath(activeMmproj, expected.Mmproj), "mmproj", expected.Mmproj, activeMmproj);
        Check(string.Equals(Str(root, "host"), expected.Host, StringComparison.OrdinalIgnoreCase), "host", expected.Host, Str(root, "host"));
        Check(Int(root, "port") == expected.Port, "port", expected.Port, Int(root, "port"));
        Check(Int(root, "context") == expected.Context, "context", expected.Context, Int(root, "context"));
        Check(Int(root, "draft_max") == expected.DraftMax, "draft_max", expected.DraftMax, Int(root, "draft_max"));
        Check(Near(Num(root, "draft_confidence"), expected.DraftConfidence), "draft_confidence", expected.DraftConfidence.ToString("0.00"), Num(root, "draft_confidence"));
        Check(string.Equals(Str(root, "mtp_proposal_mode"), expected.MtpProposalMode, StringComparison.OrdinalIgnoreCase), "mtp_proposal_mode", expected.MtpProposalMode, Str(root, "mtp_proposal_mode"));
        Check(Str(root, "mtp_draft_vocabulary") == expected.MtpDraftVocabulary, "mtp_draft_vocabulary", expected.MtpDraftVocabulary, Str(root, "mtp_draft_vocabulary"));
        Check(Bool(root, "mtp") == !string.IsNullOrWhiteSpace(expected.Mtp), "mtp", !string.IsNullOrWhiteSpace(expected.Mtp), Bool(root, "mtp"));
        Check(Str(root, "context_lookup_policy") == expected.ContextLookupPolicy, "lookup policy", expected.ContextLookupPolicy, Str(root, "context_lookup_policy"));
        Check(Int(root, "context_lookup_start_width") == expected.LookupStartWidth, "lookup start", expected.LookupStartWidth, Int(root, "context_lookup_start_width"));
        Check(Int(root, "context_lookup_promotion_width") == expected.LookupMaximumWidth, "lookup maximum", expected.LookupMaximumWidth, Int(root, "context_lookup_promotion_width"));
        Check(Int(root, "context_lookup_capacity") == expected.ContextLookupCapacity, "lookup capacity", expected.ContextLookupCapacity, Int(root, "context_lookup_capacity"));
        Check(Str(root, "effective_reasoning_effort") == expected.EffectiveReasoningEffort, "effective reasoning", expected.EffectiveReasoningEffort, Str(root, "effective_reasoning_effort"));
        Check(Int(root, "prefill_batch") == expected.PrefillBatch, "prefill_batch", expected.PrefillBatch, Int(root, "prefill_batch"));
        Check(Bool(root, "context_lookup") == expected.ContextLookup, "context_lookup", expected.ContextLookup, Bool(root, "context_lookup"));
        Check(Int(root, "context_lookup_min_ngram") == expected.ContextLookupMinNgram, "context_lookup_min_ngram", expected.ContextLookupMinNgram, Int(root, "context_lookup_min_ngram"));
        Check(Int(root, "context_lookup_max_ngram") == expected.ContextLookupMaxNgram, "context_lookup_max_ngram", expected.ContextLookupMaxNgram, Int(root, "context_lookup_max_ngram"));
        Check(Int(root, "context_lookup_window") == expected.ContextLookupWindow, "context_lookup_window", expected.ContextLookupWindow, Int(root, "context_lookup_window"));
        Check(Int(root, "context_lookup_min_draft") == expected.ContextLookupMinDraft, "context_lookup_min_draft", expected.ContextLookupMinDraft, Int(root, "context_lookup_min_draft"));
        Check(Bool(root, "memory_guard") == expected.MemoryGuard, "memory_guard", expected.MemoryGuard, Bool(root, "memory_guard"));
        Check(Near(Num(root, "memory_guard_min_available_gib"), expected.MemoryGuardMinAvailableGiB), "memory_guard_min_available_gib", expected.MemoryGuardMinAvailableGiB, Num(root, "memory_guard_min_available_gib"));
        Check(Int(root, "sessions") == expected.Sessions, "sessions", expected.Sessions, Int(root, "sessions"));
        Check(Int(root, "default_max_tokens") == expected.DefaultMaxTokens, "default_max_tokens", expected.DefaultMaxTokens, Int(root, "default_max_tokens"));
        Check(Bool(root, "thinking") == expected.Thinking, "thinking", expected.Thinking, Bool(root, "thinking"));
        Check(Bool(root, "preserve_thinking") == expected.PreserveThinking, "preserve_thinking", expected.PreserveThinking, Bool(root, "preserve_thinking"));
        Check(string.Equals(Str(root, "reasoning_effort"), expected.ReasoningEffort, StringComparison.OrdinalIgnoreCase), "reasoning_effort", expected.ReasoningEffort, Str(root, "reasoning_effort"));

        if (!root.TryGetProperty("sampling", out var sampling) || sampling.ValueKind != JsonValueKind.Object)
        {
            problems.Add("sampling: engine health did not expose active sampling settings");
        }
        else
        {
            Check(Near(Num(sampling, "temperature"), expected.Sampling.Temperature), "sampling.temperature", expected.Sampling.Temperature, Num(sampling, "temperature"));
            Check(Near(Num(sampling, "top_p"), expected.Sampling.TopP), "sampling.top_p", expected.Sampling.TopP, Num(sampling, "top_p"));
            Check(Int(sampling, "top_k") == expected.Sampling.TopK, "sampling.top_k", expected.Sampling.TopK, Int(sampling, "top_k"));
            Check(Near(Num(sampling, "min_p"), expected.Sampling.MinP), "sampling.min_p", expected.Sampling.MinP, Num(sampling, "min_p"));
            Check(Near(Num(sampling, "repeat_penalty"), expected.Sampling.RepeatPenalty), "sampling.repeat_penalty", expected.Sampling.RepeatPenalty, Num(sampling, "repeat_penalty"));
            Check(Near(Num(sampling, "frequency_penalty"), expected.Sampling.FrequencyPenalty), "sampling.frequency_penalty", expected.Sampling.FrequencyPenalty, Num(sampling, "frequency_penalty"));
            Check(Near(Num(sampling, "presence_penalty"), expected.Sampling.PresencePenalty), "sampling.presence_penalty", expected.Sampling.PresencePenalty, Num(sampling, "presence_penalty"));
            Check(Int(sampling, "repeat_last_n") == expected.Sampling.RepeatLastN, "sampling.repeat_last_n", expected.Sampling.RepeatLastN, Int(sampling, "repeat_last_n"));
        }

        return problems.Count == 0 ? null : string.Join("; ", problems);
    }

    private static string FormatExitCode(int code) => $"0x{unchecked((uint)code):X8}";

    private static string ExplainExitCode(int code) => unchecked((uint)code) switch
    {
        0xC0000135u => " STATUS_DLL_NOT_FOUND: Windows could not locate a required DLL before main() started.",
        0xC000007Bu => " STATUS_INVALID_IMAGE_FORMAT: a DLL/EXE architecture or binary format is incompatible.",
        0xC000001Du => " STATUS_ILLEGAL_INSTRUCTION: an unsupported CPU instruction was executed.",
        0xC0000005u => " STATUS_ACCESS_VIOLATION: the process crashed on invalid memory access.",
        _ => ""
    };

    private void ApplyRuntimeEnvironment(ProcessStartInfo psi)
    {
        var r = AppPaths.LoadRuntimeInfo();
        void Set(string key, string value) { if (!string.IsNullOrWhiteSpace(value)) psi.Environment[key] = value; }
        Set("ROCM_PATH", r.RocmPath);
        Set("HIP_PATH", r.RocmPath);
        Set("HIP_DEVICE_LIB_PATH", r.DeviceLibPath);
        Set("DEVICE_LIB_PATH", r.DeviceLibPath);
        Set("ROCBLAS_TENSILE_LIBPATH", r.RocblasTensileLibPath);
        Set("HIPBLASLT_TENSILE_LIBPATH", r.HipblasltTensileLibPath);
        var parts = new[] { Path.GetDirectoryName(AppPaths.EngineExe) ?? "", Path.Combine(r.RocmPath ?? "", "bin"), r.VcpkgBin }
            .Where(x => !string.IsNullOrWhiteSpace(x) && Directory.Exists(x));
        psi.Environment["PATH"] = string.Join(";", parts) + ";" + (psi.Environment.TryGetValue("PATH", out var old) ? old : Environment.GetEnvironmentVariable("PATH"));
    }

    private void Emit(string line)
    {
        lock (_gate) { _logWriter?.WriteLine($"[{DateTime.Now:HH:mm:ss}] {line}"); }
        LogLine?.Invoke(line);
    }

    public void Dispose()
    {
        try { StopAsync().GetAwaiter().GetResult(); } catch { }
        _logWriter?.Dispose();
        _http.Dispose();
    }
}
