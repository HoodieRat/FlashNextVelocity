using Microsoft.Win32;
using System.Diagnostics;
using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal sealed class MainForm : Form
{
    private EngineConfig _cfg = EngineConfig.LoadOrCreate();
    private UiSettings _ui = UiSettings.Load();
    private readonly EngineManager _engine = new();
    private readonly ApiClient _api = new();
    private readonly NotifyIcon _tray = null!;
    private readonly System.Windows.Forms.Timer _statusTimer = new() { Interval = 3000 };
    private bool _reallyExit;

    private readonly Label _status = new() { AutoSize = true, Text = "Engine stopped" };
    private readonly Label _model = new() { AutoSize = true };
    private readonly Label _mtp = new() { AutoSize = true };
    private readonly Label _vision = new() { AutoSize = true };
    private readonly Label _apiUrl = new() { AutoSize = true };
    private readonly RichTextBox _logs = new() { ReadOnly = true, Dock = DockStyle.Fill, BackColor = System.Drawing.Color.FromArgb(18,24,30), ForeColor = System.Drawing.Color.Gainsboro };
    private readonly RichTextBox _chatOutput = new() { ReadOnly = true, Dock = DockStyle.Fill };
    private readonly TextBox _chatPrompt = new() { Multiline = true, Dock = DockStyle.Fill, Text = "Write a short Python function that returns the nth Fibonacci number." };
    private readonly RichTextBox _benchOutput = new() { ReadOnly = true, Dock = DockStyle.Fill };
    private readonly RichTextBox _lastRequest = new() { ReadOnly = true, Dock = DockStyle.Fill, Text = "No request yet." };
    private readonly Label _benchPrefill = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 16, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchDecode = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 16, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchMtp = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 16, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchLookup = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 16, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchTtft = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchDepth = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchQsa = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchPle = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchBlas = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };
    private readonly Label _benchSync = new() { AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 14, System.Drawing.FontStyle.Bold) };

    private TextBox _modelPath = null!, _mtpPath = null!, _mmprojPath = null!, _host = null!;
    private NumericUpDown _port = null!, _context = null!, _draft = null!, _draftConfidence = null!, _prefillBatch = null!, _lookupMinNgram = null!, _lookupMaxNgram = null!, _lookupWindow = null!, _lookupMinDraft = null!, _memoryFloor = null!, _defaultTokens = null!, _temp = null!, _topP = null!, _topK = null!, _minP = null!, _repeatPenalty = null!, _repeatLastN = null!;
    private CheckBox _contextLookup = null!, _memoryGuard = null!, _thinking = null!, _preserveThinking = null!, _autoEngine = null!, _startWindows = null!;
    private ComboBox _reasoning = null!, _mtpProposalMode = null!;

    public MainForm(string[] args)
    {
        Text = "FlashNextVelocity";
        Width = 1180; Height = 900; MinimumSize = new System.Drawing.Size(980, 700);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new System.Drawing.Font("Segoe UI", 9F);
        BuildUi();

        var menu = new ContextMenuStrip();
        menu.Items.Add("Open Dashboard", null, (_, _) => ShowDashboard());
        menu.Items.Add("Open Browser Dashboard", null, (_, _) => OpenBrowser());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Start Engine", null, async (_, _) => await Safe(StartEngine));
        menu.Items.Add("Restart Engine", null, async (_, _) => await Safe(RestartEngine));
        menu.Items.Add("Stop Engine", null, async (_, _) => await Safe(StopEngine));
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, async (_, _) => { _reallyExit = true; await _engine.StopAsync(); _tray.Visible = false; Application.Exit(); });
        var trayIcon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? System.Drawing.SystemIcons.Application;
        Icon = trayIcon;
        _tray = new NotifyIcon { Text = "FlashNextVelocity", Icon = trayIcon, ContextMenuStrip = menu, Visible = true };
        _tray.DoubleClick += (_, _) => ShowDashboard();

        _engine.LogLine += line => BeginInvoke(new Action(() => AppendLog(line)));
        _engine.StateChanged += () => BeginInvoke(new Action(UpdateStatusLabels));
        _statusTimer.Tick += async (_, _) => await RefreshHealth();
        _statusTimer.Start();

        FormClosing += OnClosing;
        Shown += async (_, _) =>
        {
            LoadSettingsControls();
            if (args.Any(x => x.Equals("--tray", StringComparison.OrdinalIgnoreCase))) Hide();
            if (_ui.StartEngineOnLaunch && File.Exists(_cfg.Model)) await Safe(StartEngine);
        };
    }

    private void BuildUi()
    {
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2, ColumnCount = 1 };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize)); root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        var top = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Padding = new Padding(10), WrapContents = false };
        var start = new Button { Text = "Start" }; start.Click += async (_, _) => await Safe(StartEngine);
        var stop = new Button { Text = "Stop" }; stop.Click += async (_, _) => await Safe(StopEngine);
        var restart = new Button { Text = "Restart" }; restart.Click += async (_, _) => await Safe(RestartEngine);
        var web = new Button { Text = "Browser Dashboard" }; web.Click += (_, _) => OpenBrowser();
        top.Controls.AddRange(new Control[] { _status, start, stop, restart, web });
        root.Controls.Add(top, 0, 0);

        var tabs = new TabControl { Dock = DockStyle.Fill };
        tabs.TabPages.Add(BuildDashboardTab());
        tabs.TabPages.Add(BuildSettingsTab());
        tabs.TabPages.Add(BuildBenchmarkTab());
        tabs.TabPages.Add(BuildLogsTab());
        root.Controls.Add(tabs, 0, 1);
        Controls.Add(root);
    }

    private TabPage BuildDashboardTab()
    {
        var tab = new TabPage("Dashboard");
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3, Padding = new Padding(12) };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50)); layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 42)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 58));
        var info = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, AutoSize = true };
        AddInfo(info, 0, "Model", _model); AddInfo(info, 1, "MTP", _mtp); AddInfo(info, 2, "Vision", _vision); AddInfo(info, 3, "API Base", _apiUrl);
        layout.SetColumnSpan(info, 2); layout.Controls.Add(info, 0, 0);

        var promptGroup = new GroupBox { Text = "Quick Chat / Code Test", Dock = DockStyle.Fill };
        var promptLayout = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2 }; promptLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); promptLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        promptLayout.Controls.Add(_chatPrompt, 0, 0);
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        var send = new Button { Text = "Send" }; send.Click += async (_, _) => await Safe(async () => { _chatOutput.Text = "Working..."; _chatOutput.Text = await _api.ChatAsync(_cfg, _chatPrompt.Text, 512); });
        var clear = new Button { Text = "Clear" }; clear.Click += (_, _) => _chatOutput.Clear();
        buttons.Controls.AddRange(new Control[] { send, clear }); promptLayout.Controls.Add(buttons, 0, 1); promptGroup.Controls.Add(promptLayout);
        layout.Controls.Add(promptGroup, 0, 1);

        var outputGroup = new GroupBox { Text = "Response", Dock = DockStyle.Fill }; outputGroup.Controls.Add(_chatOutput); layout.Controls.Add(outputGroup, 1, 1);
        var note = new Label { Dock = DockStyle.Fill, AutoSize = false, Text = "Other projects use the same server exactly like an OpenAI-compatible local provider. Base URL: http://127.0.0.1:8080/v1\r\n\r\nOpen the Browser Dashboard button to chat and benchmark from your browser too.", Padding = new Padding(12) };
        layout.SetColumnSpan(note, 2); layout.Controls.Add(note, 0, 2);
        tab.Controls.Add(layout); return tab;
    }

    private TabPage BuildSettingsTab()
    {
        var tab = new TabPage("Settings");
        var panel = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        var grid = new TableLayoutPanel { AutoSize = true, Dock = DockStyle.Top, ColumnCount = 3, Padding = new Padding(12) };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180)); grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        int r = 0;
        _modelPath = AddPath(grid, ref r, "First model shard", "Qwen3.8-Flash-Next...");
        _mtpPath = AddPath(grid, ref r, "MTP GGUF", "shared Q8_0 MTP");
        _mmprojPath = AddPath(grid, ref r, "Vision mmproj", "F16 or BF16 mmproj");
        _host = AddText(grid, ref r, "Host");
        _port = AddNumber(grid, ref r, "Port", 1, 65535, 0);
        _context = AddNumber(grid, ref r, "Context", 1024, 1048576, 0);
        _draft = AddNumber(grid, ref r, "MTP draft max", 1, 7, 0);
        _draftConfidence = AddNumber(grid, ref r, "MTP draft confidence", 0, 1, 2, .01m);
        grid.Controls.Add(new Label { Text = "MTP proposal mode", AutoSize = true, Anchor = AnchorStyles.Left }, 0, r);
        _mtpProposalMode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
        _mtpProposalMode.Items.AddRange(new object[] { "halo_greedy", "distribution" });
        grid.Controls.Add(_mtpProposalMode, 1, r++);
        _prefillBatch = AddNumber(grid, ref r, "Prefill batch (real Gufo)", 128, 4096, 0, 128);
        _contextLookup = AddCheck(grid, ref r, "Context lookup drafting");
        _lookupMinNgram = AddNumber(grid, ref r, "Lookup min n-gram", 2, 32, 0);
        _lookupMaxNgram = AddNumber(grid, ref r, "Lookup max n-gram", 2, 32, 0);
        _lookupWindow = AddNumber(grid, ref r, "Lookup search window", 0, 1048576, 0);
        _lookupMinDraft = AddNumber(grid, ref r, "Lookup minimum draft", 1, 7, 0);
        _memoryGuard = AddCheck(grid, ref r, "Windows memory guard");
        _memoryFloor = AddNumber(grid, ref r, "Memory guard floor GiB", 0, 128, 1, .5m);
        _contextLookup.CheckedChanged += (_, _) =>
        {
            var enabled = _contextLookup.Checked;
            _lookupMinNgram.Enabled = enabled; _lookupMaxNgram.Enabled = enabled;
            _lookupWindow.Enabled = enabled; _lookupMinDraft.Enabled = enabled;
        };
        _memoryGuard.CheckedChanged += (_, _) => _memoryFloor.Enabled = _memoryGuard.Checked;
        _defaultTokens = AddNumber(grid, ref r, "Default max tokens", 1, 65536, 0);
        _temp = AddNumber(grid, ref r, "Temperature", 0, 2, 2, .05m);
        _topP = AddNumber(grid, ref r, "Top P", .01m, 1, 2, .01m);
        _topK = AddNumber(grid, ref r, "Top K", 0, 1000, 0);
        _minP = AddNumber(grid, ref r, "Min P", 0, 1, 2, .01m);
        _repeatPenalty = AddNumber(grid, ref r, "Repeat penalty", 0.5m, 2, 2, .01m);
        _repeatLastN = AddNumber(grid, ref r, "Repeat last N", 0, 65536, 0);
        _thinking = AddCheck(grid, ref r, "Thinking enabled");
        _preserveThinking = AddCheck(grid, ref r, "Preserve thinking");
        var reasoningLabel = new Label { Text = "Reasoning effort", AutoSize = true, Anchor = AnchorStyles.Left };
        grid.Controls.Add(reasoningLabel, 0, r);
        _reasoning = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
        _reasoning.Items.AddRange(new object[] { "low", "medium", "xhigh" });
        grid.Controls.Add(_reasoning, 1, r++);
        _thinking.CheckedChanged += (_, _) =>
        {
            reasoningLabel.Enabled = _thinking.Checked;
            _reasoning.Enabled = _thinking.Checked;
            _preserveThinking.Enabled = _thinking.Checked;
        };
        reasoningLabel.Enabled = _thinking.Checked;
        _reasoning.Enabled = _thinking.Checked;
        _preserveThinking.Enabled = _thinking.Checked;
        grid.Controls.Add(new Label { Text = "Active config", AutoSize = true, Anchor = AnchorStyles.Left }, 0, r);
        var configPath = new TextBox { ReadOnly = true, Dock = DockStyle.Fill, Text = AppPaths.Config };
        var openConfig = new Button { Text = "Open Config" };
        openConfig.Click += (_, _) => SafeSync(OpenConfig);
        grid.Controls.Add(configPath, 1, r);
        grid.Controls.Add(openConfig, 2, r++);
        _autoEngine = AddCheck(grid, ref r, "Start engine with app");
        _startWindows = AddCheck(grid, ref r, "Start app with Windows");
        var action = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        var save = new Button { Text = "Save" }; save.Click += (_, _) => SafeSync(SaveSettings);
        var saveRestart = new Button { Text = "Save + Restart Engine" }; saveRestart.Click += async (_, _) => await Safe(RestartEngine);
        action.Controls.AddRange(new Control[] { save, saveRestart }); grid.Controls.Add(action, 1, r++);
        panel.Controls.Add(grid); tab.Controls.Add(panel); return tab;
    }

    private TabPage BuildBenchmarkTab()
    {
        var tab = new TabPage("Benchmark");
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 4, Padding = new Padding(12) };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize)); layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 46)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 54));
        var controls = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        controls.Controls.Add(new Label { Text = "Completion tokens", AutoSize = true, Padding = new Padding(0,8,0,0) });
        var tokens = new NumericUpDown { Minimum = 64, Maximum = 4096, Value = 512, Width = 100 };
        var run = new Button { Text = "Run Benchmark" };
        controls.Controls.Add(tokens); controls.Controls.Add(run); layout.Controls.Add(controls, 0, 0);
        var metrics = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, Padding = new Padding(0,10,0,10) };
        metrics.Controls.AddRange(new Control[] {
            MetricBox("Prefill", _benchPrefill), MetricBox("Decode", _benchDecode), MetricBox("MTP acceptance", _benchMtp),
            MetricBox("Lookup acceptance", _benchLookup), MetricBox("TTFT", _benchTtft), MetricBox("Avg draft depth", _benchDepth), MetricBox("QSA of decode", _benchQsa),
            MetricBox("PLE wait", _benchPle), MetricBox("hipBLASLt fallback", _benchBlas), MetricBox("Sync / token", _benchSync)
        });
        _benchOutput.Font = new System.Drawing.Font("Consolas", 9F);
        _benchOutput.WordWrap = false;
        _lastRequest.Font = new System.Drawing.Font("Consolas", 9F);
        _lastRequest.WordWrap = false;
        var last = new GroupBox { Text = "Last request", Dock = DockStyle.Fill }; last.Controls.Add(_lastRequest);
        layout.Controls.Add(metrics, 0, 1); layout.Controls.Add(last, 0, 2); layout.Controls.Add(_benchOutput, 0, 3);
        run.Click += async (_, _) => await Safe(async () =>
        {
            _benchOutput.Text = "Saving/applying settings...";
            SaveSettings();
            if (_engine.RequiresRestart(_cfg))
            {
                AppendLog("Benchmark settings differ from the running engine; restarting once so the benchmark uses the saved config.");
                await _engine.RestartAsync(_cfg);
                await RefreshHealth();
            }
            _benchOutput.Text = "Running warmup and benchmark...";
            var result = await _api.BenchmarkAsync(_cfg, (int)tokens.Value);
            _benchPrefill.Text = $"{result.PrefillTps:N2} tok/s"; _benchDecode.Text = $"{result.DecodeTps:N2} tok/s"; _benchMtp.Text = $"{result.Acceptance * 100:N1}%"; _benchLookup.Text = result.LookupDrafted > 0 ? $"{result.LookupAcceptance * 100:N1}%" : "n/a";
            _benchTtft.Text = $"{result.TtftMs:N0} ms"; _benchDepth.Text = $"{result.AvgDraftDepth:N2}"; _benchQsa.Text = $"{result.QsaPct:N1}%";
            _benchPle.Text = $"{result.PleWaitMs:N1} ms"; _benchBlas.Text = $"{result.FallbackPct:N1}%"; _benchSync.Text = $"{result.SyncPerToken:N2} ms";
            _benchOutput.Text = string.IsNullOrWhiteSpace(result.Report)
                ? $"MTP accepted: {result.Accepted}/{result.Drafted}\r\nLookup accepted: {result.LookupAccepted}/{result.LookupDrafted}\r\n64-token windows: {string.Join(", ", result.Windows.Select(x => x.ToString("N2")))}"
                : result.Report.Replace("\n", "\r\n");
            Directory.CreateDirectory(AppPaths.Benchmarks); File.WriteAllText(Path.Combine(AppPaths.Benchmarks, $"benchmark-{DateTime.Now:yyyyMMdd-HHmmss}.json"), result.RawJson);
        });
        tab.Controls.Add(layout); return tab;
    }

    private TabPage BuildLogsTab()
    {
        var tab = new TabPage("Logs");
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, RowCount = 2 }; layout.RowStyles.Add(new RowStyle(SizeType.AutoSize)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        var bar = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
        var clear = new Button { Text = "Clear View" }; clear.Click += (_, _) => _logs.Clear();
        var folder = new Button { Text = "Open Logs Folder" }; folder.Click += (_, _) => { Directory.CreateDirectory(AppPaths.Logs); Process.Start(new ProcessStartInfo(AppPaths.Logs) { UseShellExecute = true }); };
        bar.Controls.AddRange(new Control[] { clear, folder }); layout.Controls.Add(bar, 0, 0); layout.Controls.Add(_logs, 0, 1); tab.Controls.Add(layout); return tab;
    }

    private static Control MetricBox(string title, Label value)
    {
        var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Padding = new Padding(14), Margin = new Padding(5), BorderStyle = BorderStyle.FixedSingle };
        p.Controls.Add(new Label { Text = title, AutoSize = true }); p.Controls.Add(value); return p;
    }

    private static void AddInfo(TableLayoutPanel p, int row, string title, Label value)
    {
        p.RowStyles.Add(new RowStyle(SizeType.AutoSize)); p.Controls.Add(new Label { Text = title, AutoSize = true, Font = new System.Drawing.Font("Segoe UI", 9, System.Drawing.FontStyle.Bold) }, 0, row); p.Controls.Add(value, 1, row);
    }
    private TextBox AddPath(TableLayoutPanel g, ref int r, string label, string hint)
    {
        g.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, r); var t = new TextBox { Dock = DockStyle.Fill, PlaceholderText = hint }; var b = new Button { Text = "Browse" };
        b.Click += (_, _) => { using var d = new OpenFileDialog { Filter = "GGUF files (*.gguf)|*.gguf|All files (*.*)|*.*" }; if (d.ShowDialog(this) == DialogResult.OK) t.Text = d.FileName; };
        g.Controls.Add(t, 1, r); g.Controls.Add(b, 2, r++); return t;
    }
    private TextBox AddText(TableLayoutPanel g, ref int r, string label) { g.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, r); var t = new TextBox { Dock = DockStyle.Fill }; g.Controls.Add(t, 1, r++); return t; }
    private NumericUpDown AddNumber(TableLayoutPanel g, ref int r, string label, decimal min, decimal max, int decimals, decimal increment = 1) { g.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left }, 0, r); var n = new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = decimals, Increment = increment, Dock = DockStyle.Left, Width = 160, ThousandsSeparator = true }; g.Controls.Add(n, 1, r++); return n; }
    private CheckBox AddCheck(TableLayoutPanel g, ref int r, string text) { var c = new CheckBox { Text = text, AutoSize = true }; g.Controls.Add(c, 1, r++); return c; }

    private void LoadSettingsControls()
    {
        _modelPath.Text = _cfg.Model; _mtpPath.Text = _cfg.Mtp; _mmprojPath.Text = _cfg.Mmproj; _host.Text = _cfg.Host;
        Set(_port, _cfg.Port); Set(_context, _cfg.Context); Set(_draft, _cfg.DraftMax); Set(_draftConfidence, (decimal)_cfg.DraftConfidence); Set(_prefillBatch, _cfg.PrefillBatch);
        _mtpProposalMode.SelectedItem = _cfg.MtpProposalMode;
        if (_mtpProposalMode.SelectedIndex < 0) _mtpProposalMode.SelectedItem = "halo_greedy";
        _contextLookup.Checked = _cfg.ContextLookup; Set(_lookupMinNgram, _cfg.ContextLookupMinNgram); Set(_lookupMaxNgram, _cfg.ContextLookupMaxNgram); Set(_lookupWindow, _cfg.ContextLookupWindow); Set(_lookupMinDraft, _cfg.ContextLookupMinDraft);
        _lookupMinNgram.Enabled = _cfg.ContextLookup; _lookupMaxNgram.Enabled = _cfg.ContextLookup; _lookupWindow.Enabled = _cfg.ContextLookup; _lookupMinDraft.Enabled = _cfg.ContextLookup;
        _memoryGuard.Checked = _cfg.MemoryGuard; Set(_memoryFloor, (decimal)_cfg.MemoryGuardMinAvailableGiB); _memoryFloor.Enabled = _cfg.MemoryGuard; Set(_defaultTokens, _cfg.DefaultMaxTokens);
        Set(_temp, (decimal)_cfg.Sampling.Temperature); Set(_topP, (decimal)_cfg.Sampling.TopP); Set(_topK, _cfg.Sampling.TopK); Set(_minP, (decimal)_cfg.Sampling.MinP); Set(_repeatPenalty, (decimal)_cfg.Sampling.RepeatPenalty); Set(_repeatLastN, _cfg.Sampling.RepeatLastN);
        _thinking.Checked = _cfg.Thinking;
        _preserveThinking.Checked = _cfg.PreserveThinking;
        _reasoning.SelectedItem = _cfg.ReasoningEffort;
        if (_reasoning.SelectedIndex < 0) _reasoning.SelectedItem = "medium";
        _reasoning.Enabled = _thinking.Checked;
        _preserveThinking.Enabled = _thinking.Checked;
        _autoEngine.Checked = _ui.StartEngineOnLaunch; _startWindows.Checked = _ui.StartWithWindows; UpdateStatusLabels();
    }
    private static void Set(NumericUpDown n, decimal v) => n.Value = Math.Min(n.Maximum, Math.Max(n.Minimum, v));

    private void SaveSettings()
    {
        _cfg.Model = _modelPath.Text.Trim();
        _cfg.Mtp = _mtpPath.Text.Trim();
        _cfg.Mmproj = _mmprojPath.Text.Trim();
        _cfg.Host = _host.Text.Trim();
        _cfg.Port = (int)_port.Value;
        _cfg.Context = (uint)_context.Value;
        _cfg.DraftMax = (uint)_draft.Value;
        _cfg.DraftConfidence = (double)_draftConfidence.Value;
        _cfg.MtpProposalMode = _mtpProposalMode.SelectedItem?.ToString() ?? "halo_greedy";
        _cfg.PrefillBatch = (uint)_prefillBatch.Value;
        _cfg.ContextLookup = _contextLookup.Checked;
        _cfg.ContextLookupMinNgram = (uint)_lookupMinNgram.Value;
        _cfg.ContextLookupMaxNgram = (uint)_lookupMaxNgram.Value;
        _cfg.ContextLookupWindow = (uint)_lookupWindow.Value;
        _cfg.ContextLookupMinDraft = (uint)_lookupMinDraft.Value;
        _cfg.MemoryGuard = _memoryGuard.Checked;
        _cfg.MemoryGuardMinAvailableGiB = (double)_memoryFloor.Value;
        _cfg.DefaultMaxTokens = (int)_defaultTokens.Value;
        _cfg.Sampling.Temperature = (double)_temp.Value;
        _cfg.Sampling.TopP = (double)_topP.Value;
        _cfg.Sampling.TopK = (int)_topK.Value;
        _cfg.Sampling.MinP = (double)_minP.Value;
        _cfg.Sampling.RepeatPenalty = (double)_repeatPenalty.Value;
        _cfg.Sampling.RepeatLastN = (int)_repeatLastN.Value;
        _cfg.Thinking = _thinking.Checked;
        _cfg.PreserveThinking = _preserveThinking.Checked;
        _cfg.ReasoningEffort = _reasoning.SelectedItem?.ToString() ?? "medium";

        // Save atomically, then read the exact live dist\config.json back. If a
        // field failed to persist, do not continue with a misleading in-memory
        // configuration.
        var intended = _cfg.Clone();
        intended.Save();
        var persisted = EngineConfig.LoadFromDisk();
        if (!intended.EquivalentTo(persisted))
            throw new InvalidOperationException("Saved config.json does not match the Settings controls.");
        _cfg = persisted;

        _ui.StartEngineOnLaunch = _autoEngine.Checked;
        _ui.StartWithWindows = _startWindows.Checked;
        _ui.Save();
        ConfigureWindowsStartup(_ui.StartWithWindows);
        UpdateStatusLabels();
        AppendLog($"ALL settings saved and read-back verified: draft_max={_cfg.DraftMax}, draft_confidence={_cfg.DraftConfidence:0.00}, proposal={_cfg.MtpProposalMode}, prefill_batch={_cfg.PrefillBatch}, lookup={_cfg.ContextLookup}, lookup_ngram={_cfg.ContextLookupMinNgram}-{_cfg.ContextLookupMaxNgram}, memory_guard={_cfg.MemoryGuard}, thinking={_cfg.Thinking}, effort={(_cfg.Thinking ? _cfg.ReasoningEffort : "OFF")}, config={AppPaths.Config}");
        if (_engine.IsRunning && _engine.RequiresRestart(_cfg))
            AppendLog("Saved settings differ from the running engine. Use Restart/Save + Restart; Benchmark will apply them automatically.");
    }

    private void OpenConfig()
    {
        if (!File.Exists(AppPaths.Config)) _cfg.Save();
        Process.Start(new ProcessStartInfo(AppPaths.Config) { UseShellExecute = true });
    }

    private void ConfigureWindowsStartup(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
        if (enabled) key.SetValue("FlashNextVelocity", $"\"{Application.ExecutablePath}\" --tray"); else key.DeleteValue("FlashNextVelocity", false);
    }

    private async Task StartEngine() { SaveSettings(); await _engine.StartAsync(_cfg); await RefreshHealth(); }
    private async Task StopEngine() { await _engine.StopAsync(); UpdateStatusLabels(); }
    private async Task RestartEngine() { SaveSettings(); await _engine.RestartAsync(_cfg); await RefreshHealth(); }

    private async Task RefreshHealth()
    {
        if (!_engine.IsRunning) { UpdateStatusLabels(); return; }
        try
        {
            var h = await _api.HealthAsync(_cfg);
            _status.Text = $"Online  PID {_engine.ProcessId}  loaded {h.LoadSeconds:N1}s"; _status.ForeColor = System.Drawing.Color.ForestGreen;
            _model.Text = h.Model; _mtp.Text = h.Mtp ? $"ON · {h.MtpProposalMode} · max {h.DraftMax} · conf {h.DraftConfidence:0.00} · lookup {(h.ContextLookup ? "ON" : "OFF")}" : "OFF"; _vision.Text = h.Vision ? "ON" : "OFF"; _apiUrl.Text = _cfg.BaseUrl + "/v1";
            try { _lastRequest.Text = (await _api.LastRequestAsync(_cfg)).Replace("\n", "\r\n"); } catch { }
            _tray.Text = $"FlashNextVelocity - online - {(h.Mtp ? "MTP" : "no MTP")} - {(h.Vision ? "vision" : "text")}";
        }
        catch { UpdateStatusLabels(); }
    }

    private void UpdateStatusLabels()
    {
        if (!_engine.IsRunning) { _status.Text = "Engine stopped"; _status.ForeColor = System.Drawing.Color.Firebrick; _tray.Text = "FlashNextVelocity - stopped"; }
        _apiUrl.Text = _cfg.BaseUrl + "/v1"; if (string.IsNullOrWhiteSpace(_model.Text)) _model.Text = Path.GetFileName(_cfg.Model); if (string.IsNullOrWhiteSpace(_mtp.Text)) _mtp.Text = File.Exists(_cfg.Mtp) ? "configured" : "OFF"; if (string.IsNullOrWhiteSpace(_vision.Text)) _vision.Text = File.Exists(_cfg.Mmproj) ? "configured" : "OFF";
    }

    private void OpenBrowser()
    {
        try { Process.Start(new ProcessStartInfo(_cfg.BaseUrl + "/") { UseShellExecute = true }); } catch (Exception e) { MessageBox.Show(e.Message); }
    }
    private void ShowDashboard() { Show(); WindowState = FormWindowState.Normal; Activate(); }
    private void AppendLog(string line) { _logs.AppendText($"[{DateTime.Now:HH:mm:ss}] {line}\r\n"); _logs.SelectionStart = _logs.TextLength; _logs.ScrollToCaret(); }
    private async Task Safe(Func<Task> action) { try { await action(); } catch (Exception e) { AppendLog("ERROR: " + e.Message); MessageBox.Show(this, e.Message, "FlashNextVelocity", MessageBoxButtons.OK, MessageBoxIcon.Error); } }
    private void SafeSync(Action action) { try { action(); } catch (Exception e) { AppendLog("ERROR: " + e.Message); MessageBox.Show(this, e.Message, "FlashNextVelocity", MessageBoxButtons.OK, MessageBoxIcon.Error); } }

    private void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if (!_reallyExit && _ui.MinimizeToTray && e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); return; }
        _tray.Visible = false; _statusTimer.Stop(); _engine.Dispose(); _api.Dispose();
    }
}
