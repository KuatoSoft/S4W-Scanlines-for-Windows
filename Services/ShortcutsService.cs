using System.IO;
using System.Text.Json;
using S4W.Models;

namespace S4W.Services;

/// <summary>
/// Persists global keyboard shortcuts to %AppData%\S4W\shortcuts.json.
/// Shortcuts are independent of profiles — they survive profile switches.
/// </summary>
public static class ShortcutsService
{
    private static readonly string FilePath =
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "S4W", "shortcuts.json");

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true
    };

    public static ShortcutsData Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<ShortcutsData>(
                           File.ReadAllText(FilePath), JsonOpts)
                       ?? new ShortcutsData();
        }
        catch { /* corrupt file → fresh defaults */ }
        return new ShortcutsData();
    }

    public static void Save(ShortcutsData data)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(data, JsonOpts));
        }
        catch { /* non-fatal — shortcuts just won't persist this time */ }
    }
}
