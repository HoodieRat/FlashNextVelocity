using System.Text.Json;

namespace FlashNextVelocity.Desktop;

internal static class SettingsSnapshot
{
    // Serialize the configuration types themselves so newly added settings are
    // included automatically. Capture before any await or later UI edits.
    internal static JsonElement Capture(EngineConfig cfg, UiSettings? ui) =>
        JsonSerializer.SerializeToElement(new
        {
            active_config = AppPaths.Config,
            engine = cfg,
            studio = ui,
            lookup_description = cfg.LookupDescription,
            lookup_start_width = cfg.LookupStartWidth,
            lookup_maximum_width = cfg.LookupMaximumWidth,
            configured_effective_reasoning_effort = cfg.EffectiveReasoningEffort
        }, JsonUtil.Options);

    internal static string Format(JsonElement snapshot) =>
        "STUDIO SETTINGS SNAPSHOT (saved before benchmark; request overrides below)\n"
        + JsonSerializer.Serialize(snapshot, JsonUtil.Options) + "\n\n";

    internal static string FormatCurrentPreferences(UiSettings ui) =>
        "\nCURRENT STUDIO PREFERENCES (local app settings at refresh time)\n"
        + $"Active config: {AppPaths.Config}\n"
        + $"Start engine with app: {(ui.StartEngineOnLaunch ? "ON" : "OFF")}\n"
        + $"Start app with Windows: {(ui.StartWithWindows ? "ON" : "OFF")}\n"
        + $"Minimize to tray: {(ui.MinimizeToTray ? "ON" : "OFF")}\n";
}
