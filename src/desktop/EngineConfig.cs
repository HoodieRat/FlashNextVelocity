using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace FlashNextVelocity.Desktop;

internal static class JsonUtil
{
    public static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never
    };
}

internal sealed class EngineConfig
{
    [JsonPropertyName("model")] public string Model { get; set; } = "";
    [JsonPropertyName("mtp")] public string Mtp { get; set; } = "";
    [JsonPropertyName("mmproj")] public string Mmproj { get; set; } = "";
    [JsonPropertyName("host")] public string Host { get; set; } = "127.0.0.1";
    [JsonPropertyName("port")] public int Port { get; set; } = 8080;
    [JsonPropertyName("context")] public uint Context { get; set; } = 131117;
    [JsonPropertyName("draft_max")] public uint DraftMax { get; set; } = 6;
    [JsonPropertyName("draft_confidence")] public double DraftConfidence { get; set; } = 0;
    [JsonPropertyName("mtp_draft_vocabulary")] public string MtpDraftVocabulary { get; set; } = "";
    [JsonPropertyName("mtp_proposal_mode")] public string MtpProposalMode { get; set; } = "halo_greedy";
    [JsonPropertyName("prefill_batch")] public uint PrefillBatch { get; set; } = 2048;
    [JsonPropertyName("context_lookup")] public bool ContextLookup { get; set; } = true;
    [JsonPropertyName("context_lookup_min_ngram")] public uint ContextLookupMinNgram { get; set; } = 3;
    [JsonPropertyName("context_lookup_max_ngram")] public uint ContextLookupMaxNgram { get; set; } = 6;
    [JsonPropertyName("context_lookup_window")] public uint ContextLookupWindow { get; set; } = 32768;
    [JsonPropertyName("context_lookup_min_draft")] public uint ContextLookupMinDraft { get; set; } = 6;
    [JsonPropertyName("context_lookup_max_draft")] public uint ContextLookupMaxDraft { get; set; } = 16;
    [JsonPropertyName("context_lookup_policy")] public string ContextLookupPolicy { get; set; } = "sticky";
    [JsonPropertyName("context_lookup_capacity")] public uint ContextLookupCapacity { get; set; } = 16;
    [JsonIgnore] public uint LookupStartWidth => ContextLookupPolicy == "fixed16" ? 16U : 6U;
    [JsonIgnore] public uint LookupMaximumWidth => ContextLookupPolicy == "fixed6" ? 6U : 16U;
    [JsonIgnore] public string EffectiveReasoningEffort => Thinking ? ReasoningEffort : "OFF";
    [JsonIgnore] public string LookupDescription => !ContextLookup ? "OFF" : ContextLookupPolicy == "sticky"
        ? $"Sticky: start 6 -> promote 16; capacity {ContextLookupCapacity}. Reset to 6 each request; no demotion."
        : $"{ContextLookupPolicy}: start/max {LookupStartWidth}; capacity {ContextLookupCapacity}.";
    [JsonPropertyName("memory_guard")] public bool MemoryGuard { get; set; } = true;
    [JsonPropertyName("memory_guard_min_available_gib")] public double MemoryGuardMinAvailableGiB { get; set; } = 5.0;
    [JsonPropertyName("sessions")] public uint Sessions { get; set; } = 1;
    [JsonPropertyName("default_max_tokens")] public int DefaultMaxTokens { get; set; } = 4096;
    [JsonPropertyName("thinking")] public bool Thinking { get; set; } = false;
    [JsonPropertyName("preserve_thinking")] public bool PreserveThinking { get; set; } = false;
    [JsonPropertyName("reasoning_effort")] public string ReasoningEffort { get; set; } = "medium";
    [JsonPropertyName("sampling")] public SamplingConfig Sampling { get; set; } = new();

    [JsonIgnore] public string BaseUrl => $"http://{(Host == "0.0.0.0" ? "127.0.0.1" : Host)}:{Port}";

    public static EngineConfig LoadOrCreate()
    {
        EngineConfig cfg;
        if (File.Exists(AppPaths.Config))
            cfg = LoadFromFile(AppPaths.Config);
        else
        {
            cfg = new EngineConfig();
            cfg.AutoDiscover();
        }

        cfg.Normalize();
        cfg.Save();
        return cfg;
    }

    public static EngineConfig LoadFromDisk() => LoadFromFile(AppPaths.Config);

    public EngineConfig Clone()
    {
        Normalize();
        return JsonSerializer.Deserialize<EngineConfig>(JsonSerializer.Serialize(this, JsonUtil.Options), JsonUtil.Options)
               ?? throw new InvalidOperationException("Could not clone engine configuration.");
    }

    public bool EquivalentTo(EngineConfig? other)
    {
        if (other is null) return false;
        Normalize();
        other.Normalize();
        return PathEquals(Model, other.Model)
            && PathEquals(Mtp, other.Mtp)
            && PathEquals(Mmproj, other.Mmproj)
            && string.Equals(Host, other.Host, StringComparison.OrdinalIgnoreCase)
            && Port == other.Port
            && Context == other.Context
            && DraftMax == other.DraftMax
            && Near(DraftConfidence, other.DraftConfidence)
            && string.Equals(MtpProposalMode, other.MtpProposalMode, StringComparison.OrdinalIgnoreCase)
            && MtpDraftVocabulary == other.MtpDraftVocabulary
            && ContextLookupPolicy == other.ContextLookupPolicy
            && ContextLookupCapacity == other.ContextLookupCapacity
            && PrefillBatch == other.PrefillBatch
            && ContextLookup == other.ContextLookup
            && ContextLookupMinNgram == other.ContextLookupMinNgram
            && ContextLookupMaxNgram == other.ContextLookupMaxNgram
            && ContextLookupWindow == other.ContextLookupWindow
            && ContextLookupMinDraft == other.ContextLookupMinDraft
            && ContextLookupMaxDraft == other.ContextLookupMaxDraft
            && MemoryGuard == other.MemoryGuard
            && Near(MemoryGuardMinAvailableGiB, other.MemoryGuardMinAvailableGiB)
            && Sessions == other.Sessions
            && DefaultMaxTokens == other.DefaultMaxTokens
            && Thinking == other.Thinking
            && PreserveThinking == other.PreserveThinking
            && string.Equals(ReasoningEffort, other.ReasoningEffort, StringComparison.OrdinalIgnoreCase)
            && Sampling.EquivalentTo(other.Sampling);
    }

    public void Save() => SaveToFile(AppPaths.Config);

    internal void SaveToFile(string path)
    {
        Normalize();
        Validate();
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? AppPaths.AppDir);
        var json = JsonSerializer.Serialize(this, JsonUtil.Options);
        var temp = path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllText(temp, json, new UTF8Encoding(false));
            var roundTrip = LoadFromFile(temp);
            if (!EquivalentTo(roundTrip))
                throw new InvalidOperationException("Configuration verification failed before replacing config.json.");

            File.Move(temp, path, true);
            var persisted = LoadFromFile(path);
            if (!EquivalentTo(persisted))
                throw new InvalidOperationException("Configuration verification failed after writing config.json.");
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { }
        }
    }

    internal static EngineConfig LoadFromFile(string path)
    {
        if (!File.Exists(path)) throw new FileNotFoundException("Configuration file does not exist.", path);
        var cfg = JsonSerializer.Deserialize<EngineConfig>(File.ReadAllText(path), JsonUtil.Options)
                  ?? throw new InvalidOperationException("Configuration file is empty or invalid.");
        cfg.Normalize();
        cfg.Validate();
        return cfg;
    }

    private void Normalize()
    {
        Model ??= "";
        Mtp ??= "";
        Mmproj ??= "";
        Host = string.IsNullOrWhiteSpace(Host) ? "127.0.0.1" : Host.Trim();
        MtpProposalMode = string.IsNullOrWhiteSpace(MtpProposalMode)
            ? "halo_greedy"
            : MtpProposalMode.Trim().ToLowerInvariant();
        MtpDraftVocabulary = string.IsNullOrWhiteSpace(MtpDraftVocabulary)
            ? (Sessions == 1 && MtpProposalMode == "halo_greedy" ? "latin" : "full")
            : MtpDraftVocabulary.Trim().ToLowerInvariant();
        ContextLookupPolicy = string.IsNullOrWhiteSpace(ContextLookupPolicy) ? "sticky" : ContextLookupPolicy.Trim().ToLowerInvariant();
        ReasoningEffort = string.IsNullOrWhiteSpace(ReasoningEffort)
            ? "medium"
            : ReasoningEffort.Trim().ToLowerInvariant();
        Sampling ??= new SamplingConfig();
    }

    private void Validate()
    {
        if (Port is < 1 or > 65535) throw new InvalidOperationException("Port must be from 1 to 65535.");
        if (Context == 0) throw new InvalidOperationException("Context must be greater than zero.");
        if (DraftMax is < 1 or > 7) throw new InvalidOperationException("MTP draft max must be from 1 to 7.");
        if (!double.IsFinite(DraftConfidence) || DraftConfidence is < 0 or > 1)
            throw new InvalidOperationException("MTP draft confidence must be from 0.00 to 1.00.");
        if (MtpProposalMode is not ("halo_greedy" or "distribution"))
            throw new InvalidOperationException("MTP proposal mode must be halo_greedy or distribution.");
        if (MtpDraftVocabulary is not ("latin" or "full"))
            throw new InvalidOperationException("MTP vocabulary must be latin or full.");
        if (ContextLookupPolicy is not ("sticky" or "fixed6" or "fixed16"))
            throw new InvalidOperationException("Lookup policy must be sticky, fixed6, or fixed16.");
        if (ContextLookupCapacity < LookupMaximumWidth || ContextLookupCapacity > 16 || ContextLookupCapacity < ContextLookupMaxDraft)
            throw new InvalidOperationException("Lookup capacity must cover the selected policy and configured maximum, up to 16.");
        if (ContextLookupMinDraft > LookupStartWidth)
            throw new InvalidOperationException("Lookup minimum draft cannot exceed the policy's starting width.");
        if (PrefillBatch is < 128 or > 4096)
            throw new InvalidOperationException("Prefill batch must be from 128 to 4096.");
        if (ContextLookup && (ContextLookupMinNgram < 2 || ContextLookupMaxNgram < ContextLookupMinNgram || ContextLookupMaxNgram > 32))
            throw new InvalidOperationException("Context lookup n-gram range is invalid.");
        if (ContextLookupMinDraft < 1 || ContextLookupMaxDraft > 16 || ContextLookupMinDraft > ContextLookupMaxDraft)
            throw new InvalidOperationException("Lookup draft range must satisfy 1 <= minimum <= maximum <= 16.");
        if (!double.IsFinite(MemoryGuardMinAvailableGiB) || MemoryGuardMinAvailableGiB is < 0 or > 128)
            throw new InvalidOperationException("Memory guard floor must be from 0 to 128 GiB.");
        if (Sessions != 1) throw new InvalidOperationException("This Windows runtime currently requires sessions=1.");
        if (DefaultMaxTokens < 1) throw new InvalidOperationException("Default max tokens must be positive.");
        if (ReasoningEffort is not ("low" or "medium" or "xhigh"))
            throw new InvalidOperationException("Reasoning effort must be low, medium, or xhigh.");
        Sampling.Validate();
    }

    private void AutoDiscover()
    {
        var roots = new[] { @"C:\FlashNextModels", Path.Combine(AppPaths.ProjectRoot, "models") };
        foreach (var root in roots)
        {
            if (!Directory.Exists(root)) continue;
            try
            {
                if (string.IsNullOrWhiteSpace(Model) || !File.Exists(Model))
                    Model = Directory.EnumerateFiles(root, "*.gguf", SearchOption.AllDirectories)
                        .FirstOrDefault(x => Path.GetFileName(x).Contains("Qwen3.8-Flash-Next", StringComparison.OrdinalIgnoreCase) && Path.GetFileName(x).Contains("00001-of-", StringComparison.OrdinalIgnoreCase)) ?? Model;
                if (string.IsNullOrWhiteSpace(Mtp) || !File.Exists(Mtp))
                    Mtp = Directory.EnumerateFiles(root, "*.gguf", SearchOption.AllDirectories)
                        .FirstOrDefault(x => Path.GetFileName(x).Contains("mtp-Qwen3.8-Flash-Next-shared-Q8_0", StringComparison.OrdinalIgnoreCase)) ?? Mtp;
                if (string.IsNullOrWhiteSpace(Mmproj) || !File.Exists(Mmproj))
                    Mmproj = Directory.EnumerateFiles(root, "mmproj-F16.gguf", SearchOption.AllDirectories).FirstOrDefault()
                        ?? Directory.EnumerateFiles(root, "mmproj-BF16.gguf", SearchOption.AllDirectories).FirstOrDefault() ?? Mmproj;
            }
            catch { }
        }
    }

    private static bool Near(double a, double b) => Math.Abs(a - b) <= 1e-9;
    private static bool PathEquals(string a, string b) => string.Equals(a ?? "", b ?? "", StringComparison.OrdinalIgnoreCase);
}

internal sealed class SamplingConfig
{
    [JsonPropertyName("temperature")] public double Temperature { get; set; } = 0.35;
    [JsonPropertyName("top_p")] public double TopP { get; set; } = 0.90;
    [JsonPropertyName("top_k")] public int TopK { get; set; } = 40;
    [JsonPropertyName("min_p")] public double MinP { get; set; } = 0.05;
    [JsonPropertyName("repeat_penalty")] public double RepeatPenalty { get; set; } = 1.05;
    [JsonPropertyName("frequency_penalty")] public double FrequencyPenalty { get; set; } = 0;
    [JsonPropertyName("presence_penalty")] public double PresencePenalty { get; set; } = 0;
    [JsonPropertyName("repeat_last_n")] public int RepeatLastN { get; set; } = 512;

    public bool EquivalentTo(SamplingConfig? other) => other is not null
        && Near(Temperature, other.Temperature)
        && Near(TopP, other.TopP)
        && TopK == other.TopK
        && Near(MinP, other.MinP)
        && Near(RepeatPenalty, other.RepeatPenalty)
        && Near(FrequencyPenalty, other.FrequencyPenalty)
        && Near(PresencePenalty, other.PresencePenalty)
        && RepeatLastN == other.RepeatLastN;

    public void Validate()
    {
        if (!double.IsFinite(Temperature) || Temperature < 0) throw new InvalidOperationException("Temperature must be finite and nonnegative.");
        if (!double.IsFinite(TopP) || TopP <= 0 || TopP > 1) throw new InvalidOperationException("Top P must be in (0, 1].");
        if (TopK < 0) throw new InvalidOperationException("Top K must be nonnegative.");
        if (!double.IsFinite(MinP) || MinP < 0 || MinP > 1) throw new InvalidOperationException("Min P must be in [0, 1].");
        if (!double.IsFinite(RepeatPenalty) || RepeatPenalty <= 0) throw new InvalidOperationException("Repeat penalty must be finite and positive.");
        if (!double.IsFinite(FrequencyPenalty) || !double.IsFinite(PresencePenalty)) throw new InvalidOperationException("Frequency and presence penalties must be finite.");
        if (RepeatLastN < 0) throw new InvalidOperationException("Repeat last N must be nonnegative.");
    }

    private static bool Near(double a, double b) => Math.Abs(a - b) <= 1e-9;
}

internal sealed class UiSettings
{
    public bool StartEngineOnLaunch { get; set; } = true;
    public bool StartWithWindows { get; set; } = false;
    public bool MinimizeToTray { get; set; } = true;

    public static UiSettings Load()
    {
        try { if (File.Exists(AppPaths.UiConfig)) return JsonSerializer.Deserialize<UiSettings>(File.ReadAllText(AppPaths.UiConfig), JsonUtil.Options) ?? new UiSettings(); }
        catch { }
        return new UiSettings();
    }
    public void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AppPaths.UiConfig) ?? AppPaths.AppDir);
        var temp = AppPaths.UiConfig + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            File.WriteAllText(temp, JsonSerializer.Serialize(this, JsonUtil.Options), new UTF8Encoding(false));
            File.Move(temp, AppPaths.UiConfig, true);
        }
        finally
        {
            try { if (File.Exists(temp)) File.Delete(temp); } catch { }
        }
    }
}
