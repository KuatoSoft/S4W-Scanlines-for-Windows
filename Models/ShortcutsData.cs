namespace S4W.Models;

/// <summary>
/// Global (non-profile) keyboard shortcuts. Stored in a dedicated JSON file
/// so that hotkey bindings survive profile switches.
/// </summary>
public class ShortcutsData
{
    // Switch Bezel
    public string SwitchBezelModifiers     { get; set; } = "";
    public string SwitchBezelKey           { get; set; } = "";
    public string SwitchBezelBackModifiers { get; set; } = "";
    public string SwitchBezelBackKey       { get; set; } = "";

    // Switch Profile
    public string SwitchProfileModifiers     { get; set; } = "";
    public string SwitchProfileKey           { get; set; } = "";
    public string SwitchProfileBackModifiers { get; set; } = "";
    public string SwitchProfileBackKey       { get; set; } = "";

    // CRT — Blur
    public string BlurUpModifiers   { get; set; } = "";
    public string BlurUpKey         { get; set; } = "";
    public string BlurDownModifiers { get; set; } = "";
    public string BlurDownKey       { get; set; } = "";

    // CRT — Bloom
    public string BloomUpModifiers   { get; set; } = "";
    public string BloomUpKey         { get; set; } = "";
    public string BloomDownModifiers { get; set; } = "";
    public string BloomDownKey       { get; set; } = "";

    // CRT — Curvature
    public string CurvatureUpModifiers   { get; set; } = "";
    public string CurvatureUpKey         { get; set; } = "";
    public string CurvatureDownModifiers { get; set; } = "";
    public string CurvatureDownKey       { get; set; } = "";

    // CRT — Flicker
    public string FlickerUpModifiers   { get; set; } = "";
    public string FlickerUpKey         { get; set; } = "";
    public string FlickerDownModifiers { get; set; } = "";
    public string FlickerDownKey       { get; set; } = "";

    // CRT — Phosphor
    public string PhosphorUpModifiers   { get; set; } = "";
    public string PhosphorUpKey         { get; set; } = "";
    public string PhosphorDownModifiers { get; set; } = "";
    public string PhosphorDownKey       { get; set; } = "";

    // Scanlines — Horizontal Opacity
    public string HOpacityUpModifiers   { get; set; } = "";
    public string HOpacityUpKey         { get; set; } = "";
    public string HOpacityDownModifiers { get; set; } = "";
    public string HOpacityDownKey       { get; set; } = "";

    // Scanlines — Vertical Opacity
    public string VOpacityUpModifiers   { get; set; } = "";
    public string VOpacityUpKey         { get; set; } = "";
    public string VOpacityDownModifiers { get; set; } = "";
    public string VOpacityDownKey       { get; set; } = "";

    // VHS — Intensity
    public string VhsUpModifiers   { get; set; } = "";
    public string VhsUpKey         { get; set; } = "";
    public string VhsDownModifiers { get; set; } = "";
    public string VhsDownKey       { get; set; } = "";

    // VHS — Grain Intensity
    public string GrainUpModifiers   { get; set; } = "";
    public string GrainUpKey         { get; set; } = "";
    public string GrainDownModifiers { get; set; } = "";
    public string GrainDownKey       { get; set; } = "";

    // VHS — Tape Noise Intensity
    public string TapeNoiseUpModifiers   { get; set; } = "";
    public string TapeNoiseUpKey         { get; set; } = "";
    public string TapeNoiseDownModifiers { get; set; } = "";
    public string TapeNoiseDownKey       { get; set; } = "";
}
