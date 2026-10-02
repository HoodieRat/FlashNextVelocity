using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using FlashNextVelocity.Desktop;

internal static class LifecycleTests
{
    // This executable doubles as a tiny child engine. No model is loaded.
    internal static void FakeEngine(string[] args)
    {
        var path = args[Array.IndexOf(args, "--config") + 1];
        var cfg = EngineConfig.LoadFromFile(path);
        var mode = cfg.AgentPrompt;
        if (mode == "exit") Environment.Exit(31);
        if (mode == "timeout") { Thread.Sleep(30000); return; }
        if (mode == "delay") Thread.Sleep(1500);
        var health = JsonSerializer.SerializeToNode(cfg, JsonUtil.Options)!.AsObject();
        health["runtime_revision"] = EngineManager.RequiredRuntimeRevision;
        health["server_pid"] = Environment.ProcessId;
        if (mode == "wrong-pid") health["server_pid"] = -1;
        health["config_model"] = cfg.Model; health["config_mtp"] = cfg.Mtp; health["config_mmproj"] = cfg.Mmproj;
        health["mtp"] = cfg.Mtp.Length != 0;
        health["context_lookup_start_width"] = cfg.LookupStartWidth;
        health["context_lookup_promotion_width"] = cfg.LookupMaximumWidth;
        health["effective_reasoning_effort"] = cfg.EffectiveReasoningEffort;
        if (mode == "mismatch") health["draft_max"] = 1;
        using var listener = new HttpListener();
        listener.Prefixes.Add(cfg.BaseUrl + "/"); listener.Start();
        while (true)
        {
            var context = listener.GetContext();
            var bytes = Encoding.UTF8.GetBytes(health.ToJsonString());
            context.Response.ContentType = "application/json";
            context.Response.ContentLength64 = bytes.Length;
            context.Response.OutputStream.Write(bytes); context.Response.Close();
        }
    }

    private static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    private static int FreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port; listener.Stop(); return port;
    }

    internal static async Task Run(string output)
    {
        Directory.CreateDirectory(output);
        var model = Path.Combine(output, "fake.gguf"); File.WriteAllText(model, "fake engine fixture");
        var desiredPath = Path.Combine(output, "studio.json");
        var executable = Path.Combine(AppContext.BaseDirectory, "StudioConfigTruth.exe");
        EngineConfig Config(string mode) => new() { Model = model, Port = FreePort(), DraftMax = 7, AgentPrompt = mode };
        // Leave room for cold .NET process startup on a machine building the native candidate.
        EngineManager Manager(bool timeoutCase = false) => new(executable, desiredPath, output, TimeSpan.FromSeconds(timeoutCase ? 3 : 8));
        using (var manager = Manager())
        {
            var cfg = Config("delay"); cfg.SaveToFile(desiredPath);
            var start = manager.StartAsync(cfg);
            cfg.DraftMax = 4; cfg.SaveToFile(desiredPath);
            try { await manager.RestartAsync(cfg); throw new Exception("Duplicate restart was accepted"); }
            catch (InvalidOperationException error) { Check(error.Message.Contains("in progress"), error.Message); }
            await start;
            Check(manager.State == EngineManager.LifecycleState.Running && manager.AppliedConfig!.DraftMax == 7,
                "Launch did not use its immutable configuration");
            var first = manager.ProcessId;
            await manager.RestartAsync(cfg);
            Check(manager.ProcessId != first && manager.AppliedConfig!.DraftMax == 4, "Restart did not replace the owned launch");
            await manager.StopAsync();
            Check(manager.State == EngineManager.LifecycleState.Stopped && !manager.IsRunning, "Stop did not clear owned process");
        }
        using (var manager = Manager())
        {
            var cfg = Config("delay"); cfg.SaveToFile(desiredPath);
            var start = manager.StartAsync(cfg);
            await manager.StopAsync();
            try { await start; throw new Exception("Canceled initialization succeeded"); }
            catch (OperationCanceledException) { }
            Check(!manager.IsRunning && manager.State == EngineManager.LifecycleState.Stopped, "Cancellation leaked a process");
        }
        foreach (var mode in new[] { "exit", "mismatch", "wrong-pid", "timeout" })
        {
            using var manager = Manager(mode == "timeout"); var cfg = Config(mode); cfg.SaveToFile(desiredPath);
            try { await manager.StartAsync(cfg); throw new Exception(mode + " unexpectedly started"); }
            catch (Exception error) when (error is InvalidOperationException or TimeoutException)
            {
                Check(error.Message.Contains(mode == "exit" ? "31" : mode is "mismatch" or "wrong-pid" ? "do not match" : "healthy"), error.Message);
            }
            Check(!manager.IsRunning && manager.State == EngineManager.LifecycleState.Failed, mode + " failed to clean up");
            await manager.StopAsync();
        }
        using (var listener = new TcpListener(IPAddress.Loopback, 0))
        {
            listener.Start(); using var manager = Manager(); var cfg = Config("normal");
            cfg.Port = ((IPEndPoint)listener.LocalEndpoint).Port; cfg.SaveToFile(desiredPath);
            try { await manager.StartAsync(cfg); throw new Exception("Occupied port was accepted"); }
            catch (InvalidOperationException error) { Check(error.Message.Contains("already in use"), error.Message); }
            Check(!manager.IsRunning, "Occupied port launched an engine");
            await manager.StopAsync(); listener.Stop();
        }
        Check(!Directory.EnumerateFiles(output, "config.launch-*.json").Any(), "Launch snapshots were leaked");
        File.WriteAllText(Path.Combine(output, "lifecycle-results.json"), "{\"status\":\"PASS\",\"real_model_loaded\":false}");
        Console.WriteLine("PASS: immutable launch, duplicate restart, restart replacement, cancel/exit during initialization, early exit, settings/process health mismatch, timeout, occupied port, snapshot cleanup.");
    }
}
