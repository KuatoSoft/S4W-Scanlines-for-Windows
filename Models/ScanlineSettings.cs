namespace S4W.Models;

public class ScanlineSettings
{
    // Bezel
    public string BezelPath { get; set; } = "";
    public double BezelOpacity { get; set; } = 0;
    public bool BezelEnabled { get; set; } = false;

    // Horizontal scanlines
    public bool HorizontalEnabled { get; set; } = true;
    public int HGap { get; set; } = 3;
    public int HWidth { get; set; } = 0;
    public int HThickness { get; set; } = 3;
    public double HOpacity { get; set; } = 77;

    // Vertical scanlines
    public bool VerticalEnabled { get; set; }
    public int VGap { get; set; } = 2;
    public int VHeight { get; set; }
    public int VThickness { get; set; } = 1;
    public double VOpacity { get; set; } = 27;

    // CRT Effects (group master toggle)
    public bool CrtGroupEnabled { get; set; } = true;
    public bool BlurEnabled { get; set; } = false;
    public double BlurIntensity { get; set; } = 0;
    public bool BloomEnabled { get; set; } = false;
    public double BloomIntensity { get; set; } = 0;
    public bool CurvatureEnabled { get; set; } = false;
    public double CurvatureIntensity { get; set; } = 0;
    public bool VignetteEnabled { get; set; } = false;
    public bool FlickerEnabled { get; set; } = false;
    public double FlickerIntensity { get; set; } = 0;
    public double FlickerRate { get; set; } = 25;
    public bool PhosphorEnabled { get; set; } = false;
    public double PhosphorIntensity { get; set; } = 0;

    // VHS + Film Grain + Tape Noise effects
    public bool VhsEnabled { get; set; } = false;
    public double VhsIntensity { get; set; } = 50;
    public bool GrainEnabled { get; set; } = false;
    public double GrainIntensity { get; set; } = 30;
    public bool TapeNoiseEnabled { get; set; } = false;
    public double TapeNoiseIntensity { get; set; } = 50;

    // Image — luminosity compensation
    public bool LumaEnabled { get; set; } = false;
    public double BrightnessValue { get; set; } = 0;
    public double ContrastValue { get; set; } = 0;
    public double SaturationValue { get; set; } = 0;
    public double TemperatureValue { get; set; } = 0;
    public double BlackLevelValue  { get; set; } = 0.0;
    public double GammaValue       { get; set; } = 1.0;

    // Active CRT preset name (display only — restores the name label after profile reload)
    public string ActivePresetName { get; set; } = "";


    // General
    public bool StartMinimized { get; set; } = false;
    public bool CloseToTray { get; set; }
    public bool ApplyDesktopAtStart { get; set; } = false;
    public bool MultiSystemEnabled { get; set; } = false;
    public int MonitorIndex { get; set; }
    public string ApplicationPath { get; set; } = "";
    public bool BorderlessEnabled { get; set; } = false;

    // ROM Systems (emulator mode — multiple systems per profile)
    public List<RomSystem> RomSystems { get; set; } = new();

    // Hotkeys — main overlay
    public string HotkeyModifiers { get; set; } = "";
    public string HotkeyKey { get; set; } = "";

    // Hotkeys — switch bezel next / back
    public string SwitchBezelModifiers { get; set; } = "";
    public string SwitchBezelKey { get; set; } = "";
    public string SwitchBezelBackModifiers { get; set; } = "";
    public string SwitchBezelBackKey { get; set; } = "";

    // Hotkeys — switch profile next / back
    public string SwitchProfileModifiers { get; set; } = "";
    public string SwitchProfileKey { get; set; } = "";
    public string SwitchProfileBackModifiers { get; set; } = "";
    public string SwitchProfileBackKey { get; set; } = "";

    // Hotkeys — CRT effect adjustments (+/-)
    public string BlurUpModifiers { get; set; } = "";
    public string BlurUpKey { get; set; } = "";
    public string BlurDownModifiers { get; set; } = "";
    public string BlurDownKey { get; set; } = "";
    public string BloomUpModifiers { get; set; } = "";
    public string BloomUpKey { get; set; } = "";
    public string BloomDownModifiers { get; set; } = "";
    public string BloomDownKey { get; set; } = "";
    public string CurvatureUpModifiers { get; set; } = "";
    public string CurvatureUpKey { get; set; } = "";
    public string CurvatureDownModifiers { get; set; } = "";
    public string CurvatureDownKey { get; set; } = "";
    public string FlickerUpModifiers { get; set; } = "";
    public string FlickerUpKey { get; set; } = "";
    public string FlickerDownModifiers { get; set; } = "";
    public string FlickerDownKey { get; set; } = "";
    public string PhosphorUpModifiers { get; set; } = "";
    public string PhosphorUpKey { get; set; } = "";
    public string PhosphorDownModifiers { get; set; } = "";
    public string PhosphorDownKey { get; set; } = "";
}

public class RomSystem
{
    public string Name { get; set; } = "";
    public string Folder { get; set; } = "";
    public List<string> RomNames { get; set; } = new();
}

public class ProfileData
{
    public string ProfileName { get; set; } = "Desktop";
    public ScanlineSettings Settings { get; set; } = new();
}
