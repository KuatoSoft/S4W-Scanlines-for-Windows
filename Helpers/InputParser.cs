using System.Text.RegularExpressions;

namespace S4W.Helpers;

public static partial class InputParser
{
    [GeneratedRegex(@"^(-?\d+)")]
    private static partial Regex LeadingDigits();

    public static int ParsePixelValue(string text)
    {
        var match = LeadingDigits().Match(text.Trim());
        return match.Success ? int.Parse(match.Groups[1].Value) : 0;
    }

    public static string FormatPixelValue(int value) => $"{value} px";

    public static string FormatPlainValue(int value) => value.ToString();
}
