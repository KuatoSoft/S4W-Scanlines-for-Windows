namespace S4W.Models;

public class CrtPreset
{
    public string Name { get; set; } = "Default";

    // Blur
    public bool BlurEnabled { get; set; }
    public double BlurIntensity { get; set; }

    // Bloom
    public bool BloomEnabled { get; set; }
    public double BloomIntensity { get; set; }

    // Curvature
    public bool CurvatureEnabled { get; set; }
    public double CurvatureIntensity { get; set; }

    // Flicker
    public bool FlickerEnabled { get; set; }
    public double FlickerIntensity { get; set; }
    public double FlickerRate { get; set; } = 25;

    // Phosphor
    public bool PhosphorEnabled { get; set; }
    public double PhosphorIntensity { get; set; }

    // VHS + Film Grain + Tape Noise
    public bool VhsEnabled { get; set; }
    public double VhsIntensity { get; set; } = 50;
    public bool GrainEnabled { get; set; }
    public double GrainIntensity { get; set; } = 30;
    public bool TapeNoiseEnabled { get; set; }
    public double TapeNoiseIntensity { get; set; } = 50;

    // Luminosity Compensation
    public bool LumaEnabled { get; set; }
    public double BrightnessValue { get; set; }
    public double ContrastValue { get; set; }
    public double SaturationValue { get; set; }
    public double TemperatureValue { get; set; }
    public double BlackLevelValue { get; set; }
    public double GammaValue { get; set; } = 1.0;
}
