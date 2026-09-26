using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal static class AppPaths
{
    public static string AppDir => AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    public static string ProjectRoot
    {
        get
        {
            var marker = Path.Combine(AppDir, "project-root.txt");
            if (File.Exists(marker))
            {
                var value = File.ReadAllText(marker).Trim();
                if (Directory.Exists(value)) return value;
            }
            return AppDir;
        }
    }
    public static string EngineExe => Path.Combine(AppDir, "engine", "FlashNextVelocity.Engine.exe");
    public static string Config => Path.Combine(AppDir, "config.json");
    public static string UiConfig => Path.Combine(AppDir, "ui.json");
    public static string RuntimeInfoFile => Path.Combine(AppDir, "runtime.json");
    public static string Logs => Path.Combine(AppDir, "logs");
    public static string Benchmarks => Path.Combine(AppDir, "benchmarks");

    public static RuntimeInfo LoadRuntimeInfo()
    {
        if (!File.Exists(RuntimeInfoFile)) return new RuntimeInfo();
        return JsonSerializer.Deserialize<RuntimeInfo>(File.ReadAllText(RuntimeInfoFile), JsonUtil.Options) ?? new RuntimeInfo();
    }
}

internal sealed class RuntimeInfo
{
    public string RocmPath { get; set; } = "";
    public string DeviceLibPath { get; set; } = "";
    public string RocblasTensileLibPath { get; set; } = "";
    public string HipblasltTensileLibPath { get; set; } = "";
    public string VcpkgBin { get; set; } = "";
}
