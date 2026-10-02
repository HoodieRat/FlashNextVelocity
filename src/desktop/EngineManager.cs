using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal sealed class EngineManager : IDisposable
{
    internal const string RequiredRuntimeRevision = "fnv-mtp-cache-replay-v13";
    internal enum LifecycleState { Stopped, Starting, Running, Stopping, Failed }
    private sealed class Launch(Process process, StreamWriter log, string configPath)
    {
        public readonly Process Process = process;
        public readonly StreamWriter Log = log;
        public readonly string ConfigPath = configPath;
        public bool Started;
        public bool IntentionalStop;
        public EngineConfig? AppliedConfig;
    }
    private Launch? _launch;
    private readonly SemaphoreSlim _transition = new(1, 1);
    private readonly object _gate = new();
    private CancellationTokenSource? _startup;
    private int _stopRequests;
    private bool _disposed;
    private readonly string _engineExe, _studioConfig, _logDirectory;
    private readonly TimeSpan _startupTimeout;
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(2) };
    public event Action<string>? LogLine;
    public event Action? StateChanged;
    public LifecycleState State { get; private set; } = LifecycleState.Stopped;
    public long Generation { get; private set; }
    public bool IsBusy => _transition.CurrentCount == 0;
    public bool IsRunning { get { lock (_gate) return _launch is { Started: true } l && !l.Process.HasExited; } }
    public int? ProcessId { get { lock (_gate) return _launch is { Started: true } l && !l.Process.HasExited ? l.Process.Id : null; } }
    public EngineConfig? AppliedConfig { get { lock (_gate) return _launch?.AppliedConfig?.Clone(); } }

    internal EngineManager(string? engineExe = null, string? studioConfig = null,
                           string? logDirectory = null, TimeSpan? startupTimeout = null)
    {
        _engineExe = engineExe ?? AppPaths.EngineExe;
        _studioConfig = studioConfig ?? AppPaths.Config;
        _logDirectory = logDirectory ?? AppPaths.Logs;
        _startupTimeout = startupTimeout ?? TimeSpan.FromMinutes(3);
    }

    public bool RequiresRestart(EngineConfig cfg) =>
        State != LifecycleState.Running || AppliedConfig is not { } applied || !applied.EquivalentTo(cfg, includeAgentPrompt: false);

    public Task StartAsync(EngineConfig cfg) => TransitionAsync(cfg.Clone(), false);
    public Task RestartAsync(EngineConfig cfg) => TransitionAsync(cfg.Clone(), true);

    private async Task TransitionAsync(EngineConfig snapshot, bool restart)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!await _transition.WaitAsync(0).ConfigureAwait(false))
            throw new InvalidOperationException("An engine transition is already in progress.");
        CancellationTokenSource startup;
        lock (_gate)
        {
            startup = new CancellationTokenSource();
            _startup = startup;
            if (_stopRequests != 0) startup.Cancel();
        }
        try
        {
            startup.Token.ThrowIfCancellationRequested();
            if (restart) await StopCoreAsync().ConfigureAwait(false);
            startup.Token.ThrowIfCancellationRequested();
            await StartCoreAsync(snapshot, startup.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (startup.IsCancellationRequested)
        {
            await StopCoreAsync().ConfigureAwait(false);
            throw;
        }
        catch
        {
            await StopCoreAsync().ConfigureAwait(false);
            SetState(LifecycleState.Failed);
            throw;
        }
        finally
        {
            lock (_gate) { if (ReferenceEquals(_startup, startup)) _startup = null; }
            startup.Dispose();
            _transition.Release();
            StateChanged?.Invoke();
        }
    }

    private async Task StartCoreAsync(EngineConfig cfg, CancellationToken cancellation)
    {
        if (IsRunning) return;
        await StopCoreAsync().ConfigureAwait(false);
        if (!File.Exists(_engineExe)) throw new FileNotFoundException("Native engine is not built.", _engineExe);
        if (string.IsNullOrWhiteSpace(cfg.Model) || !File.Exists(cfg.Model)) throw new InvalidOperationException("Configure a valid first model shard before starting the engine.");
        SetState(LifecycleState.Starting);
        // Do not attach to a different engine which happens to answer health.
        var endpoint = new Uri(cfg.BaseUrl);
        try
        {
            using var socket = new System.Net.Sockets.TcpClient();
            await socket.ConnectAsync(endpoint.Host, endpoint.Port, cancellation).ConfigureAwait(false);
            throw new InvalidOperationException($"Port {cfg.Port} is already in use. Stop the existing server or choose another port.");
        }
        catch (System.Net.Sockets.SocketException) { }
        cancellation.ThrowIfCancellationRequested();
        Directory.CreateDirectory(_logDirectory);
        var snapshotPath = Path.Combine(Path.GetDirectoryName(_studioConfig)!, $"config.launch-{Guid.NewGuid():N}.json");
        cfg.SaveToFile(snapshotPath);
        var logPath = Path.Combine(_logDirectory, $"engine-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.log");
        var psi = new ProcessStartInfo
        {
            FileName = _engineExe,
            WorkingDirectory = Path.GetDirectoryName(_engineExe)!,
            UseShellExecute = false, RedirectStandardOutput = true,
            RedirectStandardError = true, CreateNoWindow = true
        };
        psi.ArgumentList.Add("--config"); psi.ArgumentList.Add(snapshotPath);
        psi.ArgumentList.Add("--studio-config"); psi.ArgumentList.Add(_studioConfig);
        ApplyRuntimeEnvironment(psi);
        var launch = new Launch(new Process { StartInfo = psi, EnableRaisingEvents = true },
            new StreamWriter(new FileStream(logPath, FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false)) { AutoFlush = true }, snapshotPath);
        lock (_gate) { _launch = launch; ++Generation; }
        var proc = launch.Process;
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data, launch); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data, launch); };
        proc.Exited += (_, _) =>
        {
            try { Emit(launch.IntentionalStop ? "Engine stopped." : $"Engine exited with code {proc.ExitCode} ({FormatExitCode(proc.ExitCode)}).{ExplainExitCode(proc.ExitCode)}", launch); }
            catch (Exception e) { Emit("Engine exit diagnostic failed: " + e.Message, launch); }
            lock (_gate)
            {
                if (ReferenceEquals(_launch, launch))
                {
                    launch.AppliedConfig = null;
                    if (!launch.IntentionalStop) State = LifecycleState.Failed;
                }
            }
            StateChanged?.Invoke();
        };
        Emit($"Launching engine: {_engineExe}; immutable config: {snapshotPath}; Studio config: {_studioConfig}", launch);
        if (!proc.Start()) throw new InvalidOperationException("Failed to start the native engine process.");
        lock (_gate) launch.Started = true;
        proc.BeginOutputReadLine(); proc.BeginErrorReadLine();
        var deadline = DateTime.UtcNow + _startupTimeout;
        Exception? last = null;
        while (DateTime.UtcNow < deadline)
        {
            cancellation.ThrowIfCancellationRequested();
            if (proc.HasExited)
                throw new InvalidOperationException($"Native engine exited during startup with code {proc.ExitCode} ({FormatExitCode(proc.ExitCode)}).{ExplainExitCode(proc.ExitCode)} Check the Logs tab.");
            string? mismatch = null;
            try
            {
                using var response = await _http.GetAsync(cfg.BaseUrl + "/health", cancellation).ConfigureAwait(false);
                if (response.IsSuccessStatusCode)
                {
                    var healthJson = await response.Content.ReadAsStringAsync(cancellation).ConfigureAwait(false);
                    using var health = JsonDocument.Parse(healthJson);
                    mismatch = health.RootElement.TryGetProperty("server_pid", out var pid) && pid.TryGetInt32(out var serverPid) && serverPid == proc.Id
                        ? FindConfigMismatch(healthJson, cfg)
                        : "Health response came from a different process.";
                    if (mismatch is null)
                    {
                        cancellation.ThrowIfCancellationRequested();
                        lock (_gate)
                        {
                            cancellation.ThrowIfCancellationRequested();
                            if (!ReferenceEquals(_launch, launch) || proc.HasExited)
                                throw new InvalidOperationException("Native engine exited before startup health could be applied. Check the Logs tab.");
                            launch.AppliedConfig = cfg.Clone();
                            State = LifecycleState.Running;
                        }
                        StateChanged?.Invoke();
                        Emit($"Engine health passed; config verified (draft_max={cfg.DraftMax}, proposal={cfg.MtpProposalMode}, prefill_batch={cfg.PrefillBatch}, context={cfg.Context}).", launch);
                        return;
                    }
                }
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
            catch (Exception e) { last = e; }
            if (mismatch is not null) throw new InvalidOperationException("Engine settings do not match the launch snapshot: " + mismatch);
            await Task.Delay(250, cancellation).ConfigureAwait(false);
        }
        throw new TimeoutException("Engine did not become healthy within " + _startupTimeout.TotalSeconds + " seconds." + (last == null ? "" : " " + last.Message));
    }

    public async Task StopAsync()
    {
        lock (_gate) { ++_stopRequests; _startup?.Cancel(); }
        await _transition.WaitAsync().ConfigureAwait(false);
        try { await StopCoreAsync().ConfigureAwait(false); }
        finally
        {
            lock (_gate) --_stopRequests;
            _transition.Release();
            StateChanged?.Invoke();
        }
    }

    private async Task StopCoreAsync()
    {
        Launch? launch;
        lock (_gate) { launch = _launch; if (launch != null) launch.IntentionalStop = true; }
        if (launch is null) { SetState(LifecycleState.Stopped); return; }
        SetState(LifecycleState.Stopping);
        if (launch.Started)
        {
            if (!launch.Process.HasExited) launch.Process.Kill(true);
            await launch.Process.WaitForExitAsync().ConfigureAwait(false);
            // Drain asynchronous stdout before closing the launch's log.
            launch.Process.WaitForExit();
        }
        lock (_gate)
        {
            if (ReferenceEquals(_launch, launch)) _launch = null;
            launch.Log.Dispose();
        }
        launch.Process.Dispose();
        try { File.Delete(launch.ConfigPath); } catch (IOException) { }
        SetState(LifecycleState.Stopped);
    }

    private void SetState(LifecycleState state)
    {
        lock (_gate) State = state;
        StateChanged?.Invoke();
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
        Check(Int(root, "context_lookup_max_draft") == expected.ContextLookupMaxDraft, "context_lookup_max_draft", expected.ContextLookupMaxDraft, Int(root, "context_lookup_max_draft"));
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
            Check(Int(sampling, "seed") == expected.Sampling.Seed, "sampling.seed", expected.Sampling.Seed, Int(sampling, "seed"));
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

    private void Emit(string line, Launch? launch = null)
    {
        lock (_gate)
        {
            // Late callbacks belong to their own log, never the new process.
            try { (launch ?? _launch)?.Log.WriteLine($"[{DateTime.Now:HH:mm:ss}] {line}"); }
            catch (ObjectDisposedException) { }
        }
        LogLine?.Invoke(line);
    }

    public void Dispose()
    {
        if (_disposed) return;
        if (IsBusy || IsRunning) throw new InvalidOperationException("Await StopAsync before disposing the engine manager.");
        _disposed = true;
        _http.Dispose();
        _transition.Dispose();
    }
}
