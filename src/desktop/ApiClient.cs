using System.Text;
using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal sealed record HealthInfo(string Status, string Model, bool Mtp, bool Vision, bool ContextLookup, double LoadSeconds, ulong Context, ulong DraftMax, double DraftConfidence, string MtpProposalMode, ulong PrefillBatch, string MtpDraftVocabulary, string LookupPolicy, ulong LookupStart, ulong LookupMaximum, ulong LookupCapacity, string Tuning);
internal sealed record BenchmarkResult(double PrefillTps, double DecodeTps, double Acceptance, double LookupAcceptance, ulong Drafted, ulong Accepted, ulong LookupDrafted, ulong LookupAccepted, IReadOnlyList<double> Windows, string Report, double TtftMs, double AvgDraftDepth, double QsaPct, double PleWaitMs, double FallbackPct, double SyncPerToken, string RawJson);

internal sealed class ApiClient : IDisposable
{
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromMinutes(30) };

    public async Task<HealthInfo> HealthAsync(EngineConfig cfg)
    {
        using var r = await _http.GetAsync(cfg.BaseUrl + "/health");
        var text = await r.Content.ReadAsStringAsync();
        r.EnsureSuccessStatusCode();
        using var d = JsonDocument.Parse(text); var x = d.RootElement;
        return new HealthInfo(
            x.TryGetProperty("status", out var s) ? s.GetString() ?? "" : "",
            x.TryGetProperty("model", out var m) ? m.GetString() ?? "" : "",
            x.TryGetProperty("mtp", out var mtp) && mtp.GetBoolean(),
            x.TryGetProperty("vision", out var v) && v.GetBoolean(),
            x.TryGetProperty("context_lookup_active", out var lookupActive) ? lookupActive.GetBoolean() :
                (x.TryGetProperty("context_lookup", out var lookup) && lookup.GetBoolean()),
            x.TryGetProperty("load_seconds", out var ls) ? ls.GetDouble() : 0,
            x.TryGetProperty("context", out var c) ? c.GetUInt64() : 0,
            x.TryGetProperty("draft_max", out var dm) ? dm.GetUInt64() : 0,
            x.TryGetProperty("draft_confidence", out var dc) ? dc.GetDouble() : 0,
            x.TryGetProperty("mtp_proposal_mode", out var pm) ? pm.GetString() ?? "halo_greedy" : "halo_greedy",
            x.TryGetProperty("prefill_batch", out var pb) ? pb.GetUInt64() : 2048,
            x.GetProperty("mtp_draft_vocabulary").GetString() ?? "unknown",
            x.GetProperty("context_lookup_policy").GetString() ?? "unknown",
            x.GetProperty("context_lookup_start_width").GetUInt64(),
            x.GetProperty("context_lookup_promotion_width").GetUInt64(),
            x.GetProperty("context_lookup_capacity").GetUInt64(),
            string.Join(", ", x.GetProperty("tuning").EnumerateObject().Select(p => $"{p.Name}={(p.Value.GetBoolean() ? "ON" : "OFF")}")));
    }

    public async Task<string> LastRequestAsync(EngineConfig cfg)
    {
        using var r = await _http.GetAsync(cfg.BaseUrl + "/metrics");
        var text = await r.Content.ReadAsStringAsync();
        r.EnsureSuccessStatusCode();
        using var d = JsonDocument.Parse(text);
        return d.RootElement.TryGetProperty("last_request_text", out var t) ? t.GetString() ?? "No request yet." : "No request yet.";
    }

    public async Task<string> ChatAsync(EngineConfig cfg, string prompt, int maxTokens)
    {
        var payload = JsonSerializer.Serialize(BuildChatRequest(cfg, prompt, maxTokens,
            stream: false, profile: false, seed: -1, includeUsage: false));
        using var r = await _http.PostAsync(cfg.BaseUrl + "/v1/chat/completions", new StringContent(payload, Encoding.UTF8, "application/json"));
        var text = await r.Content.ReadAsStringAsync();
        if (!r.IsSuccessStatusCode) throw new InvalidOperationException(ExtractError(text));
        using var d = JsonDocument.Parse(text);
        return d.RootElement.GetProperty("choices")[0].GetProperty("message").GetProperty("content").GetString() ?? "";
    }

    public async Task<BenchmarkResult> BenchmarkAsync(EngineConfig cfg, int maxTokens)
    {
        await PostAsync(cfg.BaseUrl + "/v1/chat/completions", new
        {
            model = "local",
            messages = new[] { new { role = "user", content = "Reply in one short sentence about speculative decoding." } },
            max_tokens = 24,
            temperature = cfg.Sampling.Temperature,
            top_p = cfg.Sampling.TopP,
            top_k = cfg.Sampling.TopK,
            min_p = cfg.Sampling.MinP,
            repeat_penalty = cfg.Sampling.RepeatPenalty,
            repeat_last_n = cfg.Sampling.RepeatLastN,
            frequency_penalty = cfg.Sampling.FrequencyPenalty,
            presence_penalty = cfg.Sampling.PresencePenalty,
            context_lookup_policy = cfg.ContextLookupPolicy,
            stream = false,
            chat_template_kwargs = ReasoningArgs(cfg)
        });
        var prompt = string.Concat(Enumerable.Repeat("Explain speculative decoding, memory bandwidth, recurrent state, and long-context inference in technically precise, non-repetitive prose. ", 48));
        var text = await PostAsync(cfg.BaseUrl + "/v1/chat/completions", new
        {
            model = "local",
            messages = new[] { new { role = "user", content = prompt } },
            max_tokens = maxTokens,
            temperature = cfg.Sampling.Temperature,
            top_p = cfg.Sampling.TopP,
            top_k = cfg.Sampling.TopK,
            min_p = cfg.Sampling.MinP,
            repeat_penalty = cfg.Sampling.RepeatPenalty,
            repeat_last_n = cfg.Sampling.RepeatLastN,
            frequency_penalty = cfg.Sampling.FrequencyPenalty,
            presence_penalty = cfg.Sampling.PresencePenalty,
            context_lookup_policy = cfg.ContextLookupPolicy,
            stream = false,
            flashnext_profile = true,
            chat_template_kwargs = ReasoningArgs(cfg)
        });
        using var d = JsonDocument.Parse(text);
        var m = d.RootElement.GetProperty("usage").GetProperty("flashnext_velocity");
        var windows = new List<double>();
        if (m.TryGetProperty("speed_windows_64_tokens", out var wa)) foreach (var x in wa.EnumerateArray()) windows.Add(x.GetDouble());
        static double Num(JsonElement e, string name) => e.TryGetProperty(name, out var x) && x.ValueKind == JsonValueKind.Number ? x.GetDouble() : 0;
        var qsaPct = m.TryGetProperty("qsa", out var qsa) ? Num(qsa, "pct_of_decode") : 0;
        var ple = m.TryGetProperty("ple", out var pleEl) ? Num(pleEl, "wait_ms") : 0;
        var fallback = m.TryGetProperty("blas", out var blas) ? Num(blas, "fallback_pct") : 0;
        var syncPer = m.TryGetProperty("sync", out var sync) ? Num(sync, "ms_per_output_token") : 0;
        var depth = m.TryGetProperty("mtp", out var mtp) ? Num(mtp, "avg_draft_depth") : 0;
        var report = m.TryGetProperty("profile_text", out var textEl) ? textEl.GetString() ?? "" : "";
        return new BenchmarkResult(
            m.GetProperty("prefill_tokens_per_second").GetDouble(),
            m.GetProperty("completion_tokens_per_second").GetDouble(),
            m.TryGetProperty("mtp_acceptance", out var mtpAcceptance) ? mtpAcceptance.GetDouble() : m.GetProperty("draft_acceptance").GetDouble(),
            Num(m, "lookup_acceptance"),
            m.TryGetProperty("mtp_draft_tokens", out var mtpDrafted) ? mtpDrafted.GetUInt64() : m.GetProperty("draft_tokens").GetUInt64(),
            m.TryGetProperty("mtp_draft_tokens_accepted", out var mtpAccepted) ? mtpAccepted.GetUInt64() : m.GetProperty("draft_tokens_accepted").GetUInt64(),
            m.TryGetProperty("lookup_draft_tokens", out var lookupDrafted) ? lookupDrafted.GetUInt64() : 0UL,
            m.TryGetProperty("lookup_draft_tokens_accepted", out var lookupAccepted) ? lookupAccepted.GetUInt64() : 0UL, windows,
            report, Num(m, "ttft_ms"), depth, qsaPct, ple, fallback, syncPer, text);
    }

    internal static Dictionary<string, object?> BuildChatRequest(EngineConfig cfg, string prompt,
        int maxTokens, bool stream, bool profile, long seed, bool includeUsage)
    {
        var body = new Dictionary<string, object?>
        {
            ["model"] = "local",
            ["messages"] = new[] { new { role = "user", content = prompt } },
            ["max_tokens"] = maxTokens,
            ["temperature"] = cfg.Sampling.Temperature,
            ["top_p"] = cfg.Sampling.TopP,
            ["top_k"] = cfg.Sampling.TopK,
            ["min_p"] = cfg.Sampling.MinP,
            ["repeat_penalty"] = cfg.Sampling.RepeatPenalty,
            ["repeat_last_n"] = cfg.Sampling.RepeatLastN,
            ["frequency_penalty"] = cfg.Sampling.FrequencyPenalty,
            ["presence_penalty"] = cfg.Sampling.PresencePenalty,
            ["context_lookup_policy"] = cfg.ContextLookupPolicy,
            ["seed"] = seed,
            ["stream"] = stream,
            ["flashnext_profile"] = profile,
            ["chat_template_kwargs"] = ReasoningArgs(cfg)
        };
        if (stream && includeUsage) body["stream_options"] = new { include_usage = true };
        return body;
    }

    private static object ReasoningArgs(EngineConfig cfg) => new
    {
        enable_thinking = cfg.Thinking,
        preserve_thinking = cfg.Thinking && cfg.PreserveThinking,
        reasoning_effort = cfg.EffectiveReasoningEffort.ToLowerInvariant()
    };

    private async Task<string> PostAsync(string url, object body)
    {
        var json = JsonSerializer.Serialize(body);
        using var r = await _http.PostAsync(url, new StringContent(json, Encoding.UTF8, "application/json"));
        var text = await r.Content.ReadAsStringAsync();
        if (!r.IsSuccessStatusCode) throw new InvalidOperationException(ExtractError(text));
        return text;
    }

    private static string ExtractError(string text)
    {
        try { using var d = JsonDocument.Parse(text); return d.RootElement.GetProperty("error").GetProperty("message").GetString() ?? text; }
        catch { return text; }
    }
    public void Dispose() => _http.Dispose();
}
