using System.IO;
using System.Text.Json;
using S4W.Models;

namespace S4W.Services;

public static class ProfileService
{
    private static readonly string ProfileDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "S4W", "Profiles");

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };

    static ProfileService()
    {
        Directory.CreateDirectory(ProfileDir);
    }

    public static void Save(ProfileData profile)
    {
        string path = GetPath(profile.ProfileName);
        File.WriteAllText(path, JsonSerializer.Serialize(profile, JsonOpts));
    }

    public static ProfileData? Load(string name)
    {
        string path = GetPath(name);
        if (!File.Exists(path)) return null;
        return JsonSerializer.Deserialize<ProfileData>(File.ReadAllText(path), JsonOpts);
    }

    public static void Delete(string name)
    {
        string path = GetPath(name);
        if (File.Exists(path)) File.Delete(path);
    }

    public static List<string> ListProfiles()
    {
        // One-time migration: rename legacy "Default" profile to "Desktop"
        string defaultPath = GetPath("Default");
        string desktopPath = GetPath("Desktop");
        if (File.Exists(defaultPath) && !File.Exists(desktopPath))
        {
            try
            {
                var data = JsonSerializer.Deserialize<ProfileData>(
                    File.ReadAllText(defaultPath), JsonOpts);
                if (data != null)
                {
                    data.ProfileName = "Desktop";
                    File.WriteAllText(desktopPath,
                        JsonSerializer.Serialize(data, JsonOpts));
                    File.Delete(defaultPath);
                }
            }
            catch { }
        }

        if (!Directory.Exists(ProfileDir))
            return ["Desktop"];

        var names = Directory.GetFiles(ProfileDir, "*.json")
            .Select(f => Path.GetFileNameWithoutExtension(f))
            .ToList();

        if (names.Count == 0) names.Add("Desktop");

        // Desktop is permanently first; the rest are alphabetically sorted.
        var sorted = names
            .Where(n => !string.Equals(n, "Desktop", StringComparison.OrdinalIgnoreCase))
            .OrderBy(n => n, StringComparer.OrdinalIgnoreCase)
            .ToList();
        sorted.Insert(0, "Desktop");
        return sorted;
    }

    private static string GetPath(string name)
    {
        foreach (char c in Path.GetInvalidFileNameChars())
            name = name.Replace(c, '_');
        return Path.Combine(ProfileDir, name + ".json");
    }
}
