using System.IO;
using System.Text.Json;
using S4W.Models;

namespace S4W.Services;

public static class CrtPresetService
{
    private static string DataDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "S4W");

    private static string PresetsPath => Path.Combine(DataDir, "crt_presets.json");

    public static List<CrtPreset> Load()
    {
        try
        {
            if (!File.Exists(PresetsPath)) return GetDefaults();
            var json = File.ReadAllText(PresetsPath);
            var list = JsonSerializer.Deserialize<List<CrtPreset>>(json);
            if (list == null || list.Count == 0) return GetDefaults();

            // Merge built-in presets that don't exist in the saved list
            foreach (var builtin in GetDefaults())
            {
                if (!list.Any(p => string.Equals(p.Name, builtin.Name, StringComparison.OrdinalIgnoreCase)))
                    list.Add(builtin);
            }
            return list;
        }
        catch
        {
            return GetDefaults();
        }
    }

    public static void Save(List<CrtPreset> presets)
    {
        Directory.CreateDirectory(DataDir);
        var json = JsonSerializer.Serialize(presets,
            new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(PresetsPath, json);
    }

    /// <summary>
    /// Returns the built-in presets used when no saved file exists.
    /// Index 0 = Default (all zeros), Index 1 = CRT Classic, Index 2 = VHS.
    /// </summary>
    public static List<CrtPreset> GetDefaults() => new()
    {
        new CrtPreset { Name = "Default" },
        new CrtPreset
        {
            Name = "CRT Classic",
            BlurEnabled      = true,  BlurIntensity      = 40,
            BloomEnabled     = true,  BloomIntensity     = 7,
            CurvatureEnabled = true,  CurvatureIntensity = 30,
            FlickerEnabled   = true,  FlickerIntensity   = 26, FlickerRate = 25,
            PhosphorEnabled  = true,  PhosphorIntensity  = 15,
            LumaEnabled      = true,
            BrightnessValue  = 14,    ContrastValue  = -2,
            SaturationValue  = 3,     TemperatureValue = 0,
            BlackLevelValue  = 0.019, GammaValue = 1.04
        },
        new CrtPreset
        {
            Name = "VHS",
            BlurEnabled      = true,  BlurIntensity      = 32,
            BloomEnabled     = true,  BloomIntensity     = 11,
            CurvatureEnabled = true,  CurvatureIntensity = 10,
            FlickerEnabled   = true,  FlickerIntensity   = 22, FlickerRate = 25,
            PhosphorEnabled  = true,  PhosphorIntensity  = 24,
            LumaEnabled      = true,
            BrightnessValue  = 13,    ContrastValue  = -2,
            SaturationValue  = 10,    TemperatureValue = 0,
            BlackLevelValue  = 0.019, GammaValue = 1.04,
            VhsEnabled       = true,  VhsIntensity       = 34,
            GrainEnabled     = true,  GrainIntensity     = 33,
            TapeNoiseEnabled = true,  TapeNoiseIntensity = 76
        },
        new CrtPreset
        {
            Name = "VHS 2",
            BlurEnabled      = true,  BlurIntensity      = 32,
            BloomEnabled     = true,  BloomIntensity     = 11,
            CurvatureEnabled = true,  CurvatureIntensity = 10,
            FlickerEnabled   = true,  FlickerIntensity   = 22, FlickerRate = 25,
            PhosphorEnabled  = true,  PhosphorIntensity  = 24,
            LumaEnabled      = true,
            BrightnessValue  = 13,    ContrastValue  = 13,
            SaturationValue  = 15,    TemperatureValue = 0,
            BlackLevelValue  = 0.019, GammaValue = 1.09,
            VhsEnabled       = true,  VhsIntensity       = 34,
            GrainEnabled     = true,  GrainIntensity     = 33,
            TapeNoiseEnabled = true,  TapeNoiseIntensity = 22
        }
    };
}
