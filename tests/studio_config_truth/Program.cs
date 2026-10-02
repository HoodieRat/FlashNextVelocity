using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using FlashNextVelocity.Desktop;

internal static class Program
{
    private static readonly List<object> Results = new();
    private static string Output = "";
    private static T Field<T>(MainForm form, string name) => (T)typeof(MainForm).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    private static JsonNode Node(object value) => JsonSerializer.SerializeToNode(value, JsonUtil.Options)!;
    private static void Write(string name, object value) => File.WriteAllText(Path.Combine(Output, name + ".json"), JsonSerializer.Serialize(value, JsonUtil.Options));

    private static void TestAgentPrompt(EngineConfig production)
    {
        using var form = new MainForm(Array.Empty<string>(), production.Clone(), false);
        var editor = Field<TextBox>(form, "_agentPrompt");
        Check(editor.Multiline && editor.AcceptsReturn && editor.AcceptsTab, "Agent prompt editor must support multiline instructions");
        Check(editor.Text == production.AgentPrompt, "Saved prompt not restored in editor");
        var original = form.CaptureSettings();
        var custom = "Coding instructions: \"quotes\", <svg>, and Unicode café.\r\n\r\nPreserve this exact text.";
        editor.Text = custom;
        var changed = form.CaptureSettings();
        Check(changed.AgentPrompt == custom, "Editor prompt lost during settings capture");
        var path = Path.Combine(Output, "agent-prompt-custom.json");
        changed.SaveToFile(path);
        var restored = EngineConfig.LoadFromFile(path);
        Check(restored.AgentPrompt == custom && changed.EquivalentTo(restored), "Prompt persistence mismatch");
        Check(!original.EquivalentTo(changed), "Prompt omitted from persistence verification");
        Check(original.EquivalentTo(changed, includeAgentPrompt: false), "Prompt-only edit requires a model restart");
        var payload = Node(ApiClient.BuildChatRequest(restored, "Make an SVG.", 32, true, false, 12345, true));
        Check(payload["agent_prompt"]!.GetValue<string>() == custom, "Custom prompt missing from outgoing request");
        Check(payload["messages"]![0]!["content"]!.GetValue<string>() == ApiClient.StandaloneChatInstruction, "Quick Chat capability instructions missing or duplicated");
        Check(payload["tools"] is null, "Prompt setting unexpectedly advertises tools");
        editor.Text = "";
        var empty = form.CaptureSettings();
        empty.SaveToFile(path);
        Check(EngineConfig.LoadFromFile(path).AgentPrompt == "", "Empty prompt was replaced with default");
        var legacy = Node(production).AsObject(); legacy.Remove("agent_prompt");
        File.WriteAllText(path, legacy.ToJsonString());
        Check(EngineConfig.LoadFromFile(path).AgentPrompt == AgentPromptDefaults.CodingAndSvg, "Legacy config missing default prompt");
        var defaults = production.Clone(); defaults.AgentPrompt = AgentPromptDefaults.CodingAndSvg;
        Write("agent-prompt-default", defaults);
        Write("agent-prompt-results", new { status = "PASS", multiline_editor = true, exact_persistence = true, legacy_default = true, blank_preserved = true, request_override = true, prompt_only_edit_no_reload = true });
        Console.WriteLine("PASS: agent prompt editor, persistence, defaults, blank opt-out, request payload, and no-reload comparison.");
    }

    private static void TestQualityPresets(EngineConfig production)
    {
        using var form = new MainForm(Array.Empty<string>(), production.Clone(), false);
        Check(form.CaptureSettings().EquivalentTo(production), "Opening Studio changed current settings");
        foreach (var preset in new[] { "Thinking - Medium", "Thinking - Xhigh", "Non-thinking" })
        {
            form.ApplyQualityPreset(preset);
            var cfg = Capture(form, "preset-" + preset.Replace(' ', '_'));
            var thinking = preset != "Non-thinking";
            Check(cfg.Thinking == thinking && cfg.PreserveThinking == thinking, "Preset thinking settings");
            Check(cfg.Sampling.Temperature == (thinking ? 1 : .7) && cfg.Sampling.TopP == (thinking ? .95 : .8), "Preset sampling values");
            Check(cfg.Sampling.TopK == 20 && cfg.Sampling.MinP == 0 && cfg.Sampling.RepeatPenalty == 1 &&
                  cfg.Sampling.FrequencyPenalty == 0 && cfg.Sampling.PresencePenalty == (thinking ? 0 : 1.5), "Preset penalties/filters");
            Check(cfg.ReasoningEffort == (preset == "Thinking - Medium" ? "medium" : "xhigh"), "Preferred effort was lost");
            var unchanged = cfg.Clone(); unchanged.Sampling = production.Clone().Sampling;
            unchanged.Thinking = production.Thinking; unchanged.PreserveThinking = production.PreserveThinking;
            unchanged.ReasoningEffort = production.ReasoningEffort;
            Check(unchanged.EquivalentTo(production), "Preset changed unrelated configuration");
        }
        Console.WriteLine("PASS: opt-in presets, exact values, remembered effort, and unrelated settings preserved.");
    }

    private static void TestSettingsSnapshot(EngineConfig production)
    {
        using var form = new MainForm(Array.Empty<string>(), production.Clone(), false);
        var captured = Capture(form, "settings-current");
        Check(captured.EquivalentTo(production), "Settings capture changed the current production configuration");
        var ui = new UiSettings { StartEngineOnLaunch = false, StartWithWindows = true, MinimizeToTray = false };
        var snapshot = SettingsSnapshot.Capture(captured, ui);
        var expected = Node(captured).AsObject();
        var actual = JsonNode.Parse(snapshot.GetRawText())!.AsObject();
        foreach (var field in expected)
            Check(JsonNode.DeepEquals(field.Value, actual["engine"]![field.Key]), "Benchmark snapshot omitted or changed engine setting " + field.Key);
        foreach (var field in Node(ui).AsObject())
            Check(JsonNode.DeepEquals(field.Value, actual["studio"]![field.Key]), "Benchmark snapshot omitted Studio preference " + field.Key);
        foreach (var field in new[] { "active_config", "lookup_description", "lookup_start_width", "lookup_maximum_width", "configured_effective_reasoning_effort" })
            Check(actual[field] is not null, "Benchmark snapshot omitted derived field " + field);
        var before = snapshot.GetRawText();
        captured.Context = 16384; captured.Sampling.Seed = 54321; ui.StartWithWindows = false;
        Check(snapshot.GetRawText() == before, "Benchmark snapshot changed after later settings edits");
        var report = SettingsSnapshot.Format(snapshot);
        Check(report.Contains(before), "Benchmark text output omitted the complete snapshot");
        Write("settings-snapshot", snapshot);
        Write("settings-snapshot-results", new { status = "PASS", actual_current_settings_roundtrip = true,
            engine_fields = expected.Select(x => x.Key).ToArray(), studio_fields = Node(ui).AsObject().Select(x => x.Key).ToArray(),
            immutable_after_capture = true, report_includes_snapshot = true });
        Console.WriteLine("PASS: every Settings-page engine value and Studio preference, save/reload fidelity, immutable benchmark snapshot, and text export.");
    }

    private static EngineConfig Capture(MainForm form, string state, bool stream = false, string prompt = "Reply with the word ready.")
    {
        var cfg = form.CaptureSettings();
        var path = Path.Combine(Output, state + "-config.json");
        cfg.SaveToFile(path);
        var saved = EngineConfig.LoadFromFile(path);
        Check(cfg.EquivalentTo(saved), state + " persistence mismatch");
        var savedSeed = Node(saved)["sampling"]?["seed"]?.GetValue<long>() ?? -1;
        var payload = ApiClient.BuildChatRequest(saved, prompt, stream ? 64 : 8, stream, false, stream ? savedSeed : 12345, stream);
        Write(state + "-request", payload);
        var ui = new Dictionary<string, object?>();
        foreach (var item in new[] { "_context", "_draft", "_draftConfidence", "_temp", "_topP", "_topK", "_minP", "_repeatPenalty", "_frequencyPenalty", "_presencePenalty", "_repeatLastN" })
            ui[item] = Field<NumericUpDown>(form, item).Value;
        foreach (var item in new[] { "_thinking", "_contextLookup", "_preserveThinking" }) ui[item] = Field<CheckBox>(form, item).Checked;
        foreach (var item in new[] { "_reasoning", "_lookupPolicy", "_mtpVocabulary", "_mtpProposalMode" }) ui[item] = Field<ComboBox>(form, item).SelectedItem;
        ui["lookup_description"] = Field<Label>(form, "_lookupDescription").Text;
        ui["effective_reasoning"] = Field<Label>(form, "_effectiveReasoning").Text;
        ui["effort_selector_enabled"] = Field<ComboBox>(form, "_reasoning").Enabled;
        ui["confidence_control_enabled"] = Field<NumericUpDown>(form, "_draftConfidence").Enabled;
        ui["mtp_path"] = Field<TextBox>(form, "_mtpPath").Text;
        Results.Add(new { state, ui, persisted = Node(saved), outgoing = payload, status = "PASS" });
        return saved;
    }

    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--config") { LifecycleTests.FakeEngine(args); return; }
        if (args.Length == 2 && args[0] == "--lifecycle") { LifecycleTests.Run(Path.GetFullPath(args[1])).GetAwaiter().GetResult(); return; }
        Check(args.Length >= 2, "Usage: StudioConfigTruth production-config output-directory [health-json]");
        Output = Path.GetFullPath(args[1]); Directory.CreateDirectory(Output);
        var production = EngineConfig.LoadFromFile(Path.GetFullPath(args[0]));
        if (args.Length == 3 && args[2] == "--presets") { TestQualityPresets(production); TestAgentPrompt(production); return; }
        if (args.Length == 3 && args[2] == "--settings-snapshot") { TestSettingsSnapshot(production); return; }
        if (args.Length == 3 && args[2] == "--agent-prompt") { TestAgentPrompt(production); return; }
        if (args.Length == 3)
        {
            var health = File.ReadAllText(args[2]);
            Check(EngineManager.FindConfigMismatch(health, production) is null, "Production health mismatch: " + EngineManager.FindConfigMismatch(health, production));
            foreach (var name in new[] { "context_lookup_policy", "mtp_draft_vocabulary", "effective_reasoning_effort" })
            {
                var wrong = JsonNode.Parse(health)!; wrong[name] = "wrong";
                Check(EngineManager.FindConfigMismatch(wrong.ToJsonString(), production) is not null, "Missing mismatch check: " + name);
            }
            Write("health-verification", new { status = "PASS", startup_config_matches = true, mismatches_rejected = true });
            Console.WriteLine("PASS: actual runtime health verified by Studio's startup gate."); return;
        }
        using var form = new MainForm(Array.Empty<string>(), production.Clone(), false);
        var a = Capture(form, "A-production");
        Check(a.Context == 131117 && a.DraftMax == 6 && a.Mtp.Contains("Q8_0") && a.MtpDraftVocabulary == "latin", "Production model configuration");
        Check(a.ContextLookupPolicy == "sticky" && a.LookupStartWidth == 6 && a.LookupMaximumWidth == 16 && a.ContextLookupCapacity == 16, "Sticky configuration");
        Check(Field<Label>(form, "_lookupDescription").Text.Contains("Reset to 6 each request"), "Missing reset display");
        Check(!Field<NumericUpDown>(form, "_draftConfidence").Enabled, "Halo confidence must be inactive");

        Field<CheckBox>(form, "_thinking").Checked = true;
        Field<ComboBox>(form, "_reasoning").SelectedItem = "xhigh";
        Field<CheckBox>(form, "_thinking").Checked = false;
        var off = Capture(form, "B-off");
        Check(!off.Thinking && off.ReasoningEffort == "xhigh" && off.EffectiveReasoningEffort == "OFF", "Remembered preference must not enable thinking");
        Check(!Field<ComboBox>(form, "_reasoning").Enabled, "OFF selector must be disabled");
        using (var restored = new MainForm(Array.Empty<string>(), off.Clone(), false))
        {
            Check(!Field<ComboBox>(restored, "_reasoning").Enabled && (string)Field<ComboBox>(restored, "_reasoning").SelectedItem! == "xhigh", "OFF restore");
            Field<CheckBox>(restored, "_thinking").Checked = true;
            var on = Capture(restored, "B-on");
            Check(on.Thinking && on.ReasoningEffort == "xhigh" && Field<ComboBox>(restored, "_reasoning").Enabled, "ON preference restoration");
        }
        Field<NumericUpDown>(form, "_temp").Value = 0;
        Field<NumericUpDown>(form, "_topP").Value = .7m;
        Field<NumericUpDown>(form, "_topK").Value = 0;
        Field<NumericUpDown>(form, "_minP").Value = 0;
        Field<NumericUpDown>(form, "_repeatPenalty").Value = 1.07m;
        Field<NumericUpDown>(form, "_frequencyPenalty").Value = 0;
        Field<NumericUpDown>(form, "_presencePenalty").Value = .7m;
        var c = Capture(form, "C-numeric");
        Check(c.Sampling.Temperature == 0 && c.Sampling.TopK == 0 && c.Sampling.MinP == 0 && c.Sampling.FrequencyPenalty == 0 && c.Sampling.PresencePenalty == .7 && c.Sampling.TopP == .7 && c.Sampling.RepeatPenalty == 1.07, "Deliberate numbers changed");
        var exact = production.Clone(); exact.Sampling.Temperature = .375; exact.Sampling.RepeatPenalty = 2.75; exact.Sampling.PresencePenalty = -3.125; exact.DraftConfidence = .7;
        using (var precision = new MainForm(Array.Empty<string>(), exact, false))
        {
            var saved = Capture(precision, "precision");
            Check(saved.Sampling.Temperature == .375 && saved.Sampling.RepeatPenalty == 2.75 && saved.Sampling.PresencePenalty == -3.125 && saved.DraftConfidence == .7, "UI rounded or clamped valid values");
        }
        var disabled = production.Clone(); disabled.Mtp = ""; disabled.ContextLookup = false; disabled.MtpDraftVocabulary = "full"; disabled.Sampling.Temperature = 0;
        disabled.SaveToFile(AppPaths.Config);
        var reload = EngineConfig.LoadOrCreate();
        Check(reload.Mtp == "" && !reload.ContextLookup && reload.Sampling.Temperature == 0 && reload.MtpDraftVocabulary == "full", "LoadOrCreate overwrote explicit values");
        foreach (var policy in new[] { "fixed6", "fixed16" })
        {
            Field<ComboBox>(form, "_lookupPolicy").SelectedItem = policy;
            var cfg = Capture(form, policy);
            Check(cfg.LookupStartWidth == (policy == "fixed6" ? 6 : 16) && cfg.DraftMax == 6, "Lookup and MTP widths conflated");
        }
        var legacy = Node(production).AsObject(); legacy.Remove("context_lookup_policy"); legacy.Remove("context_lookup_capacity"); legacy.Remove("mtp_draft_vocabulary"); legacy["sampling"]!.AsObject().Remove("frequency_penalty"); legacy["sampling"]!.AsObject().Remove("presence_penalty");
        File.WriteAllText(Path.Combine(Output, "legacy.json"), legacy.ToJsonString());
        var migrated = EngineConfig.LoadFromFile(Path.Combine(Output, "legacy.json"));
        Check(migrated.ContextLookupPolicy == "sticky" && migrated.ContextLookupCapacity == 16 && migrated.MtpDraftVocabulary == "latin" && !migrated.Thinking, "Legacy defaults");
        using var final = new MainForm(Array.Empty<string>(), production.Clone(), false);
        var fixture = Path.Combine(AppContext.BaseDirectory, "code.txt");
        Capture(final, "D-final", true, File.ReadAllText(fixture));
        Write("component-results", new { status = "PASS", states = Results, explicit_disabled_mtp_preserved = true, legacy_defaults = "PASS", precision_and_bounds = "PASS", live_production_config_untouched = true });
        Console.WriteLine("PASS: Studio controls, OFF/ON preference, persistence, payloads, zeros, precision, legacy defaults, independent lookup/MTP settings.");
    }
}
