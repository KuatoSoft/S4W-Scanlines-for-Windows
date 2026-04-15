namespace S4W.Models;

public class BackupData
{
    public string Version    { get; set; } = "1.4";
    public string ExportDate { get; set; } = "";
    public string Language   { get; set; } = "English";
    public List<ProfileData> Profiles   { get; set; } = new();
    public List<string>      AppHistory { get; set; } = new();
    public List<CrtPreset>   CrtPresets { get; set; } = new();
}
