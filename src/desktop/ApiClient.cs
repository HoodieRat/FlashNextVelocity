using System.Text;
using System.Text.Json;
using System.Security.Cryptography;

namespace FlashNextVelocity.Desktop;

internal sealed record HealthInfo(string Status, string Model, bool Mtp, bool Vision, bool ContextLookup, double LoadSeconds, ulong Context, ulong DraftMax, double DraftConfidence, string MtpProposalMode, ulong PrefillBatch, string MtpDraftVocabulary, string LookupPolicy, ulong LookupStart, ulong LookupMaximum, ulong LookupCapacity, string Tuning);
internal sealed record BenchmarkResult(double PrefillTps, double DecodeTps, double MinDecodeTps, double MaxDecodeTps, IReadOnlyList<double> MeasuredDecodeTps, long Seed, bool CompletionsIdentical, bool EffectiveSettingsIdentical, double Acceptance, double LookupAcceptance, ulong Drafted, ulong Accepted, ulong LookupDrafted, ulong LookupAccepted, IReadOnlyList<double> Windows, string Report, double TtftMs, double AvgDraftDepth, double QsaPct, double PleWaitMs, double FallbackPct, double SyncPerToken, string RawJson);

internal sealed class ApiClient : IDisposable
{
    internal const string StandaloneChatInstruction = "You are a helpful assistant in a standalone chat. No tools, filesystem, shell, or project workspace are available. Answer the user directly. For code and SVG requests, write the complete code in your response.";
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

    internal sealed record ChatResult(string Text, string? FinishReason, long? CompletionTokens, int RequestedMaxTokens, long? EffectiveSeed);

    public async Task<string> ChatAsync(EngineConfig cfg, string prompt, int maxTokens)
        => (await ChatStreamAsync(cfg, prompt, maxTokens)).Text;

    public async Task<ChatResult> ChatStreamAsync(EngineConfig cfg, string prompt, int maxTokens, IProgress<string>? progress = null, CancellationToken ct = default)
    {
        if (maxTokens < 1) throw new ArgumentOutOfRangeException(nameof(maxTokens), "Quick Chat max tokens must be positive.");
        // Quick Chat has no tool-execution loop, so it must not advertise tools:
        // no "tools", no "tool_choice", no tool-use system instruction, no
        // "max_completion_tokens". max_tokens comes from the caller
        // (config.default_max_tokens), never a hardcoded cap.
        var payload = JsonSerializer.Serialize(BuildChatRequest(cfg, prompt, maxTokens,
            stream: true, profile: false, seed: cfg.Sampling.Seed, includeUsage: true));
        using var req = new HttpRequestMessage(HttpMethod.Post, cfg.BaseUrl + "/v1/chat/completions")
        {
            Content = new StringContent(payload, Encoding.UTF8, "application/json")
        };
        using var resp = await _http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct);
        if (!resp.IsSuccessStatusCode)
            throw new InvalidOperationException(ExtractError(await resp.Content.ReadAsStringAsync(ct)));
        // StreamReader incrementally decodes UTF-8, so a multibyte character
        // split across TCP reads is still reassembled correctly. ReadLineAsync
        // returns whole "data: ..." lines regardless of TCP fragmentation.
        using var stream = await resp.Content.ReadAsStreamAsync(ct);
        using var reader = new StreamReader(stream, Encoding.UTF8);
        var sb = new StringBuilder();
        string? finishReason = null;
        long? completionTokens = null;
        long? effectiveSeed = null;
        var sawDone = false;
        string? line;
        while ((line = await reader.ReadLineAsync(ct)) is not null)
        {
            if (line.Length == 0) continue;
            if (line[0] == ':') continue; // SSE comment / heartbeat
            if (!line.StartsWith("data:", StringComparison.Ordinal)) continue;
            var data = line.Substring(5).TrimStart();
            if (data.Length == 0) continue;
            if (data.Equals("[DONE]", StringComparison.Ordinal))
            {
                sawDone = true;
                break;
            }
            string? fragment;
            string? reason;
            long? usageTokens;
            long? chunkSeed;
            try
            {
                (fragment, reason, usageTokens, chunkSeed) = ParseChunk(data);
            }
            catch (JsonException)
            {
                // A non-JSON SSE data line is a stream error, not content.
                throw new InvalidOperationException("Quick Chat stream error: received a malformed SSE data line before [DONE].");
            }
            // Append every delta.content fragment verbatim: Markdown fences,
            // XML/SVG/HTML tags, "<...>" text and braces never affect control
            // flow. Assistant text is never parsed as tool markup here; only
            // the API's structured fields (finish_reason/usage) drive logging.
            if (!string.IsNullOrEmpty(fragment))
            {
                sb.Append(fragment);
                progress?.Report(sb.ToString());
            }
            if (!string.IsNullOrEmpty(reason)) finishReason = reason;
            if (usageTokens.HasValue) completionTokens = usageTokens;
            if (chunkSeed.HasValue) effectiveSeed = chunkSeed;
        }
        if (!sawDone && finishReason is null)
            throw new InvalidOperationException("Quick Chat stream error: connection ended before [DONE] and before any finish_reason; generation did not complete normally.");
        return new ChatResult(sb.ToString(), finishReason, completionTokens, maxTokens, effectiveSeed);
    }

    private static (string? Fragment, string? FinishReason, long? CompletionTokens, long? EffectiveSeed) ParseChunk(string data)
    {
        using var d = JsonDocument.Parse(data);
        var root = d.RootElement;
        if (root.TryGetProperty("error", out var error))
            throw new InvalidOperationException(error.TryGetProperty("message", out var errorMessage)
                ? errorMessage.GetString() ?? "Generation failed." : "Generation failed.");
        string? fragment = null;
        string? reason = null;
        if (root.TryGetProperty("choices", out var choices) && choices.ValueKind == JsonValueKind.Array && choices.GetArrayLength() > 0)
        {
            var choice = choices[0];
            if (choice.TryGetProperty("delta", out var delta) && delta.ValueKind == JsonValueKind.Object)
            {
                if (delta.TryGetProperty("content", out var content) && content.ValueKind == JsonValueKind.String)
                    fragment = content.GetString();
                else if (delta.TryGetProperty("text", out var text) && text.ValueKind == JsonValueKind.String)
                    fragment = text.GetString();
            }
            // Fallback for non-delta chunk shapes; still verbatim content only.
            if (fragment is null && choice.TryGetProperty("message", out var message) && message.ValueKind == JsonValueKind.Object)
            {
                if (message.TryGetProperty("content", out var content) && content.ValueKind == JsonValueKind.String)
                    fragment = content.GetString();
            }
            if (choice.TryGetProperty("finish_reason", out var fr) && fr.ValueKind == JsonValueKind.String)
                reason = fr.GetString();
        }
        long? usageTokens = null;
        long? effectiveSeed = null;
        if (root.TryGetProperty("usage", out var usage) && usage.ValueKind == JsonValueKind.Object
            && usage.TryGetProperty("completion_tokens", out var ct) && ct.ValueKind == JsonValueKind.Number
            && ct.TryGetInt64(out var n))
            usageTokens = n;
        if (usage.ValueKind == JsonValueKind.Object
            && usage.TryGetProperty("flashnext_velocity", out var velocity)
            && velocity.ValueKind == JsonValueKind.Object
            && velocity.TryGetProperty("effective_request_settings", out var settings)
            && settings.ValueKind == JsonValueKind.Object
            && settings.TryGetProperty("seed", out var seed)
            && seed.ValueKind == JsonValueKind.Number
            && seed.TryGetInt64(out var actualSeed))
            effectiveSeed = actualSeed;
        return (fragment, reason, usageTokens, effectiveSeed);
    }

    public async Task<BenchmarkResult> BenchmarkAsync(EngineConfig cfg, int maxTokens, UiSettings? ui = null)
    {
        if (maxTokens < 1) throw new ArgumentOutOfRangeException(nameof(maxTokens));
        cfg = cfg.Clone();
        var settingsSnapshot = SettingsSnapshot.Capture(cfg, ui);
        var seed = cfg.Sampling.Seed >= 0 ? cfg.Sampling.Seed : 12345;
        var prompt = string.Concat(Enumerable.Repeat("Explain speculative decoding, memory bandwidth, recurrent state, and long-context inference in technically precise, non-repetitive prose. ", 48));
        var warmupBody = BuildChatRequest(cfg, prompt, maxTokens, stream: false, profile: false, seed, includeUsage: false);
        var warmupJson = await PostAsync(cfg.BaseUrl + "/v1/chat/completions", warmupBody);
        using var warmupDoc = JsonDocument.Parse(warmupJson);
        var warmupContent = CompletionText(warmupDoc.RootElement);

        // Serialize once and send the byte-for-byte same request for each measured run.
        var measuredBody = BuildChatRequest(cfg, prompt, maxTokens, stream: false, profile: true, seed, includeUsage: false);
        var measuredPayload = JsonSerializer.Serialize(measuredBody);
        var runs = new List<BenchmarkMeasurement>(3);
        for (var i = 0; i < 3; i++)
        {
            var text = await PostJsonAsync(cfg.BaseUrl + "/v1/chat/completions", measuredPayload);
            using var doc = JsonDocument.Parse(text);
            runs.Add(ParseBenchmarkMeasurement(doc.RootElement, text));
        }

        if (runs.Any(x => x.ActualSeed != seed))
            throw new InvalidOperationException($"Benchmark seed mismatch: requested {seed}, effective seeds {string.Join(", ", runs.Select(x => x.ActualSeed))}.");

        var m = runs[^1].Metrics;
        var windows = new List<double>();
        if (m.TryGetProperty("speed_windows_64_tokens", out var wa)) foreach (var x in wa.EnumerateArray()) windows.Add(x.GetDouble());
        static double Num(JsonElement e, string name) => e.TryGetProperty(name, out var x) && x.ValueKind == JsonValueKind.Number ? x.GetDouble() : 0;
        var qsaPct = m.TryGetProperty("qsa", out var qsa) ? Num(qsa, "pct_of_decode") : 0;
        var ple = m.TryGetProperty("ple", out var pleEl) ? Num(pleEl, "wait_ms") : 0;
        var fallback = m.TryGetProperty("blas", out var blas) ? Num(blas, "fallback_pct") : 0;
        var syncPer = m.TryGetProperty("sync", out var sync) ? Num(sync, "ms_per_output_token") : 0;
        var depth = m.TryGetProperty("mtp", out var mtp) ? Num(mtp, "avg_draft_depth") : 0;
        var report = m.TryGetProperty("profile_text", out var textEl) ? textEl.GetString() ?? "" : "";
        if (string.IsNullOrWhiteSpace(report) && m.TryGetProperty("last_request_text", out var lastText))
            report = lastText.GetString() ?? "";
        report = SettingsSnapshot.Format(settingsSnapshot) + report;
        var decodeRates = runs.Select(x => x.DecodeTps).ToArray();
        var orderedRates = decodeRates.OrderBy(x => x).ToArray();
        var completionHashes = runs.Select(x => x.CompletionHash).ToArray();
        var effectiveSettings = runs.Select(x => x.EffectiveSettingsJson).ToArray();
        var allHashesEqual = completionHashes.Distinct(StringComparer.Ordinal).Count() == 1;
        var allSettingsEqual = effectiveSettings.Distinct(StringComparer.Ordinal).Count() == 1;
        var raw = JsonSerializer.Serialize(new
        {
            settings_snapshot = settingsSnapshot,
            report_text = report,
            measured_request = JsonSerializer.Deserialize<JsonElement>(measuredPayload),
            warmup = new { seed, completion_sha256 = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(warmupContent))) },
            requested_seed = cfg.Sampling.Seed,
            actual_seed = seed,
            measured_requests = runs.Select(x => new { x.DecodeTps, x.CompletionHash, effective_settings = x.EffectiveSettingsJson, response_json = x.RawJson }),
            median_decode_tps = orderedRates[1],
            min_decode_tps = orderedRates[0],
            max_decode_tps = orderedRates[^1],
            completion_hashes_identical = allHashesEqual,
            effective_settings_identical = allSettingsEqual
        }, JsonUtil.Options);
        return new BenchmarkResult(
            m.GetProperty("prefill_tokens_per_second").GetDouble(),
            orderedRates[1], orderedRates[0], orderedRates[^1], decodeRates, seed,
            allHashesEqual, allSettingsEqual,
            m.TryGetProperty("mtp_acceptance", out var mtpAcceptance) ? mtpAcceptance.GetDouble() : m.GetProperty("draft_acceptance").GetDouble(),
            Num(m, "lookup_acceptance"),
            m.TryGetProperty("mtp_draft_tokens", out var mtpDrafted) ? mtpDrafted.GetUInt64() : m.GetProperty("draft_tokens").GetUInt64(),
            m.TryGetProperty("mtp_draft_tokens_accepted", out var mtpAccepted) ? mtpAccepted.GetUInt64() : m.GetProperty("draft_tokens_accepted").GetUInt64(),
            m.TryGetProperty("lookup_draft_tokens", out var lookupDrafted) ? lookupDrafted.GetUInt64() : 0UL,
            m.TryGetProperty("lookup_draft_tokens_accepted", out var lookupAccepted) ? lookupAccepted.GetUInt64() : 0UL, windows,
            report, Num(m, "ttft_ms"), depth, qsaPct, ple, fallback, syncPer, raw);
    }

    private sealed record BenchmarkMeasurement(JsonElement Metrics, double DecodeTps, long ActualSeed, string CompletionHash, string EffectiveSettingsJson, string RawJson);

    private static BenchmarkMeasurement ParseBenchmarkMeasurement(JsonElement root, string raw)
    {
        var velocity = root.GetProperty("usage").GetProperty("flashnext_velocity");
        var settings = velocity.GetProperty("effective_request_settings");
        var actualSeed = settings.GetProperty("seed").GetInt64();
        var decode = velocity.GetProperty("completion_tokens_per_second").GetDouble();
        var content = CompletionText(root);
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(content)));
        return new BenchmarkMeasurement(velocity.Clone(), decode, actualSeed, hash, settings.GetRawText(), raw);
    }

    private static string CompletionText(JsonElement root)
    {
        var choice = root.GetProperty("choices")[0];
        if (choice.TryGetProperty("message", out var message)
            && message.TryGetProperty("content", out var content)
            && content.ValueKind == JsonValueKind.String)
            return content.GetString() ?? "";
        return "";
    }

    internal static Dictionary<string, object?> BuildChatRequest(EngineConfig cfg, string prompt,
        int maxTokens, bool stream, bool profile, long seed, bool includeUsage)
    {
        var body = new Dictionary<string, object?>
        {
            ["model"] = "local",
            ["agent_prompt"] = cfg.AgentPrompt,
            ["messages"] = new[] {
                new { role = "system", content = StandaloneChatInstruction },
                new { role = "user", content = prompt }
            },
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

    private async Task<string> PostJsonAsync(string url, string json)
    {
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
