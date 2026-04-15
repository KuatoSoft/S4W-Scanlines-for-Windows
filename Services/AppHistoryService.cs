using System.IO;
using System.Text.Json;

namespace S4W.Services;

public static class AppHistoryService
{
    private static readonly string HistoryFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "S4W", "app_history.json");

    private static readonly JsonSerializerOptions Opts = new() { WriteIndented = true };

    public static List<string> Load()
    {
        try
        {
            if (!File.Exists(HistoryFile)) return [];
            return JsonSerializer.Deserialize<List<string>>(
                File.ReadAllText(HistoryFile), Opts) ?? [];
        }
        catch { return []; }
    }

    public static void Save(List<string> paths)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(HistoryFile)!);
        File.WriteAllText(HistoryFile, JsonSerializer.Serialize(paths, Opts));
    }

    public static bool TryAdd(List<string> current, string fullPath)
    {
        if (current.Any(p => string.Equals(p, fullPath, StringComparison.OrdinalIgnoreCase)))
            return false;
        current.Add(fullPath);
        Save(current);
        return true;
    }
}
