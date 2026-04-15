using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using S4W.Helpers;
using S4W.Models;
using S4W.Services;

namespace S4W;

public partial class MainWindow : Window
{
    // ── overlay ──
    private OverlayWindow? _overlay;
    private bool _overlayActive;

    // ── hotkey services ──
    private HotkeyService? _switchBezelHotkeyService;
    private HotkeyService? _switchBezelBackHotkeyService;
    private HotkeyService? _switchProfileHotkeyService;
    private HotkeyService? _switchProfileBackHotkeyService;
    private HotkeyService? _blurUpHotkeyService;
    private HotkeyService? _blurDownHotkeyService;
    private HotkeyService? _bloomUpHotkeyService;
    private HotkeyService? _bloomDownHotkeyService;
    private HotkeyService? _curvatureUpHotkeyService;
    private HotkeyService? _curvatureDownHotkeyService;
    private HotkeyService? _flickerUpHotkeyService;
    private HotkeyService? _flickerDownHotkeyService;
    private HotkeyService? _phosphorUpHotkeyService;
    private HotkeyService? _phosphorDownHotkeyService;
    private HotkeyService? _hOpacityUpHotkeyService;
    private HotkeyService? _hOpacityDownHotkeyService;
    private HotkeyService? _vOpacityUpHotkeyService;
    private HotkeyService? _vOpacityDownHotkeyService;
    private HotkeyService? _vhsUpHotkeyService;
    private HotkeyService? _vhsDownHotkeyService;
    private HotkeyService? _grainUpHotkeyService;
    private HotkeyService? _grainDownHotkeyService;
    private HotkeyService? _tapeNoiseUpHotkeyService;
    private HotkeyService? _tapeNoiseDownHotkeyService;

    // ── captured keys ──
    private ModifierKeys _switchBezelMods;          private Key _switchBezelKey;
    private ModifierKeys _switchBezelBackMods;      private Key _switchBezelBackKey;
    private ModifierKeys _switchProfileMods;        private Key _switchProfileKey;
    private ModifierKeys _switchProfileBackMods;    private Key _switchProfileBackKey;
    private ModifierKeys _blurUpMods;               private Key _blurUpKey;
    private ModifierKeys _blurDownMods;             private Key _blurDownKey;
    private ModifierKeys _bloomUpMods;              private Key _bloomUpKey;
    private ModifierKeys _bloomDownMods;            private Key _bloomDownKey;
    private ModifierKeys _curvatureUpMods;          private Key _curvatureUpKey;
    private ModifierKeys _curvatureDownMods;        private Key _curvatureDownKey;
    private ModifierKeys _flickerUpMods;            private Key _flickerUpKey;
    private ModifierKeys _flickerDownMods;          private Key _flickerDownKey;
    private ModifierKeys _phosphorUpMods;           private Key _phosphorUpKey;
    private ModifierKeys _phosphorDownMods;         private Key _phosphorDownKey;
    private ModifierKeys _hOpacityUpMods;           private Key _hOpacityUpKey;
    private ModifierKeys _hOpacityDownMods;         private Key _hOpacityDownKey;
    private ModifierKeys _vOpacityUpMods;           private Key _vOpacityUpKey;
    private ModifierKeys _vOpacityDownMods;         private Key _vOpacityDownKey;
    private ModifierKeys _vhsUpMods;                private Key _vhsUpKey;
    private ModifierKeys _vhsDownMods;              private Key _vhsDownKey;
    private ModifierKeys _grainUpMods;              private Key _grainUpKey;
    private ModifierKeys _grainDownMods;            private Key _grainDownKey;
    private ModifierKeys _tapeNoiseUpMods;          private Key _tapeNoiseUpKey;
    private ModifierKeys _tapeNoiseDownMods;        private Key _tapeNoiseDownKey;

    // ── cycling state ──
    private string[] _bezelImages = [];
    private int _bezelImageIndex;
    private string _bezelFullPath = "";   // full path stored separately; TxtBezelPath shows filename only
    private string[] _profileNames = [];
    private int _profileCycleIndex;

    // ── UI helpers ──
    private DispatcherTimer? _updateTimer;
    private System.Windows.Forms.NotifyIcon? _trayIcon;
    private System.Windows.Forms.ToolStripItem? _trayMenuShow;
    private System.Windows.Forms.ToolStripItem? _trayMenuExit;
    private Action<ModifierKeys, Key>? _pendingHotkeyCallback;
    private Action? _pendingHotkeyCancelCallback;
    private Button? _capturingButton;
    private string? _capturingButtonOriginalContent;
    private bool _isLoaded;
    private bool _applyingSettings;
    private int _activeTab = 1;

    // ── process monitoring ──
    private DispatcherTimer? _processCheckTimer;
    private string? _autoLoadedProfile;
    private string? _previousProfileName;
    private string? _monitoredProcessName;
    private int _monitoredWindowStyle;       // tracks monitored process's Win32 style for fullscreen detection

    // ── capture mode (borderless forcing) ──
    private IntPtr _gameHwnd;                // HWND of the monitored game window
    private int _savedGameStyle;             // original window style before borderless forcing
    private NativeMethods.RECT _savedGameRect; // original window position/size before borderless forcing

    // ── Hook injection ──
    private HookSharedMemory? _hookSharedMem;
    private bool _hookInjected;
    private readonly HashSet<int> _injectedPids = [];
    private int _hookedProcessId;
    private bool _hookedIs32bit;              // true if hooked process is x86
    private DateTime _injectRetryStart;       // when we first deferred injection (for HWND-less games)
    private int _diagTick;                    // counter for periodic diagnostic logging


    // ── language ──  0=EN, 1=FR, 2=DE, 3=IT, 4=ES, 5=PT-BR
    private int _langIndex = 0;

    private string L(string en, string fr, string de, string it, string es, string pt)
        => new[] { en, fr, de, it, es, pt }[_langIndex];

    private string NoneText => L("None", "Aucun", "Keine", "Nessuno", "Ninguno", "Nenhum");

    // ── CRT presets ──
    private List<S4W.Models.CrtPreset> _crtPresets = new();
    private int _crtPresetIndex = 0;

    // ── CRT OSD (on-screen display shown during hotkey slider adjustments) ──
    // WPF overlay OSD removed — all OSD is now rendered by the hook DLL directly
    private DispatcherTimer? _crtOsdTimer;

    // ── CRT key-repeat acceleration ──
    private DateTime _lastCrtAdjust = DateTime.MinValue;
    private int _crtRepeatCount;

    // ── app history (exe name → full path) ──
    private List<string> _appHistory = [];

    // ── close control ──
    private bool _forceClose;

    // ── ROM systems (current profile in memory) ──
    private List<RomSystem> _currentRomSystems = [];
    // Global cache of every ROM system ever added — survives Reset, cleared only by Delete
    private readonly List<RomSystem> _knownRomSystems = [];

    // ── ROM state during monitoring ──
    private bool _monitoredRomMode;
    private List<RomSystem> _monitoredRomSystems = [];

    // ── profile monitor data ──
    private record ProfileMonitorEntry(string AppPath, List<RomSystem> RomSystems);
    private Dictionary<string, ProfileMonitorEntry> _profileMonitorData =
        new(StringComparer.OrdinalIgnoreCase);

    // ── emulator warmup: poll count per process before ROM-title check ──
    private readonly Dictionary<string, int> _processRunningPolls =
        new(StringComparer.OrdinalIgnoreCase);
    private const int EmulatorWarmupPolls = 2;   // ~4 s at 2-s interval

    // ── suppress CmbSystems SelectionChanged while rebuilding items ──
    private bool _refreshingComboBox;

    // ── UI scale (x1…x4) ──
    private static readonly double[] UiScaleFactors = [0.70, 0.80, 0.90, 1.00];
    private static readonly string UiScaleFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "S4W", "uiscale.txt");

    // ── ROM SYSTEMS button handlers ──

    private void BtnRomSet_Click(object sender, RoutedEventArgs e)
    {
        var btn = (Button)sender;
        btn.ContextMenu.PlacementTarget = btn;
        btn.ContextMenu.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom;
        btn.ContextMenu.IsOpen = true;
    }

    private void MenuCreateFolder_Click(object sender, RoutedEventArgs e)
    {
        string systemName = CmbSystems.Text.Trim();
        if (string.IsNullOrWhiteSpace(systemName))
        {
            MessageBox.Show(
                L("Please enter a system name first.",
                  "Veuillez d'abord saisir un nom de système.",
                  "Bitte geben Sie zuerst einen Systemnamen ein.",
                  "Inserisci prima un nome di sistema.",
                  "Por favor, ingrese primero un nombre de sistema.",
                  "Por favor, insira primeiro um nome de sistema."),
                "S4W", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        using var dlg = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = L("Select a parent folder — a subfolder will be created inside it",
                "Sélectionnez le dossier parent — un sous-dossier y sera créé",
                "Wählen Sie einen übergeordneten Ordner — ein Unterordner wird darin erstellt",
                "Seleziona una cartella principale — verrà creata una sottocartella al suo interno",
                "Seleccione una carpeta principal — se creará una subcarpeta dentro",
                "Selecione uma pasta principal — uma subpasta será criada dentro dela")
        };
        if (dlg.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;

        string newFolder = Path.Combine(dlg.SelectedPath, systemName);
        try
        {
            Directory.CreateDirectory(newFolder);
            AddOrUpdateRomSystem(systemName, newFolder);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                L("Failed to create folder: ", "Échec de la création du dossier : ",
                  "Ordner konnte nicht erstellt werden: ", "Impossibile creare la cartella: ",
                  "Error al crear la carpeta: ", "Falha ao criar a pasta: ")
                    + ex.Message,
                "S4W", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void MenuUseFolder_Click(object sender, RoutedEventArgs e)
    {
        using var dlg = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = L("Select your ROM folder",
                "Sélectionnez votre dossier ROM",
                "Wählen Sie Ihren ROM-Ordner",
                "Seleziona la cartella ROM",
                "Seleccione su carpeta ROM",
                "Selecione sua pasta ROM")
        };
        if (dlg.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;

        // Auto-fill system name from the chosen folder name
        string folderName = Path.GetFileName(dlg.SelectedPath);
        if (!string.IsNullOrEmpty(folderName))
            CmbSystems.Text = folderName;

        string systemName = CmbSystems.Text.Trim();
        if (string.IsNullOrWhiteSpace(systemName))
            systemName = folderName;

        AddOrUpdateRomSystem(systemName, dlg.SelectedPath);
    }

    /// <summary>Reset: dissociates the selected system from this profile only → shows None.</summary>
    private void BtnResetRomSystem_Click(object sender, RoutedEventArgs e)
        => MenuRemoveSystem_Click(sender, e);

    private void MenuRemoveSystem_Click(object sender, RoutedEventArgs e)
    {
        string selectedName = CmbSystems.Text.Trim();
        if (string.IsNullOrEmpty(selectedName) || selectedName == NoneText) return;

        _currentRomSystems.RemoveAll(s =>
            string.Equals(s.Name, selectedName, StringComparison.OrdinalIgnoreCase));

        if (_currentRomSystems.Count == 0)
            ToggleMultiSystemEnabled.IsChecked = false;

        UpdateMultiSystemContent();
        RefreshSystemsComboBox(_currentRomSystems.Count > 0 ? _currentRomSystems[0].Name : null);
    }

    /// <summary>Delete: removes this system entry from the dropdown (all profiles) — does NOT touch disk.</summary>
    private void BtnDeleteRomSystem_Click(object sender, RoutedEventArgs e)
    {
        string selectedName = CmbSystems.Text.Trim();
        if (string.IsNullOrEmpty(selectedName) || selectedName == NoneText) return;

        // Remove from current profile in-memory
        _currentRomSystems.RemoveAll(s =>
            string.Equals(s.Name, selectedName, StringComparison.OrdinalIgnoreCase));
        if (_currentRomSystems.Count == 0)
            ToggleMultiSystemEnabled.IsChecked = false;

        // Purge from every saved profile on disk (no folder deletion)
        foreach (var profileName in ProfileService.ListProfiles())
        {
            var pd = ProfileService.Load(profileName);
            if (pd == null) continue;
            int before = pd.Settings.RomSystems.Count;
            pd.Settings.RomSystems.RemoveAll(s =>
                string.Equals(s.Name, selectedName, StringComparison.OrdinalIgnoreCase));
            if (pd.Settings.RomSystems.Count != before)
                ProfileService.Save(pd);
        }

        // Remove from persistent cache so it disappears from the dropdown permanently
        _knownRomSystems.RemoveAll(s =>
            string.Equals(s.Name, selectedName, StringComparison.OrdinalIgnoreCase));

        // Rebuild monitor data so dropdown no longer shows this system
        RefreshProfileAppPaths();

        UpdateMultiSystemContent();
        RefreshSystemsComboBox(_currentRomSystems.Count > 0 ? _currentRomSystems[0].Name : null);
    }

    /// <summary>Add a new system or update its folder in _currentRomSystems, then refresh the UI.</summary>
    private void AddOrUpdateRomSystem(string name, string folder)
    {
        var existing = _currentRomSystems.FirstOrDefault(s =>
            string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
        if (existing != null)
        {
            existing.Folder = folder;
            existing.RomNames.Clear(); // folder changed — invalidate cached ROMs
        }
        else
        {
            _currentRomSystems.Add(new RomSystem { Name = name, Folder = folder });
        }

        // Keep global cache up-to-date (survives Reset, cleared only by Delete)
        var known = _knownRomSystems.FirstOrDefault(s =>
            string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
        if (known != null)
            known.Folder = folder;
        else
            _knownRomSystems.Add(new RomSystem { Name = name, Folder = folder });

        // Auto-enable the toggle whenever a ROM folder is assigned
        ToggleMultiSystemEnabled.IsChecked = true;
        UpdateMultiSystemContent();
        RefreshSystemsComboBox(name);
    }

    /// <summary>
    /// Returns all unique ROM systems known across every saved profile.
    /// Current-profile systems come first; global cache follows (de-duplicated by name).
    /// </summary>
    private IEnumerable<RomSystem> GetAllKnownRomSystems()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var sys in _currentRomSystems)
            if (seen.Add(sys.Name)) yield return sys;
        foreach (var sys in _knownRomSystems)
            if (seen.Add(sys.Name)) yield return sys;
    }

    /// <summary>Find a ROM system by name from any available source.</summary>
    private RomSystem? FindRomSystemByName(string name)
    {
        return _currentRomSystems.FirstOrDefault(s =>
                   string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase))
            ?? _knownRomSystems.FirstOrDefault(s =>
                   string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Ensure the dropdown-selected ROM system is linked to the current profile.
    /// Called before Save so that picking a system from the dropdown and pressing Save works.
    /// If the system is already linked but the global cache has a different (newer) folder,
    /// the linked entry is updated in-place so folder corrections are persisted on Save.</summary>
    private void LinkSelectedRomSystem()
    {
        if (ToggleMultiSystemEnabled.IsChecked != true) return;

        string selectedName = CmbSystems.SelectedItem?.ToString()?.Trim()
                           ?? CmbSystems.Text.Trim();
        if (string.IsNullOrEmpty(selectedName) || selectedName == NoneText) return;

        // Find the system data (current profile first, then global cache)
        var source = FindRomSystemByName(selectedName);
        if (source == null) return;

        // The dropdown-selected system becomes THE system for this profile.
        // Clear any previously linked systems so the user's selection is exact —
        // this lets the user correct a wrong assignment by simply selecting the
        // right system from the dropdown and clicking Save.
        _currentRomSystems.Clear();
        _currentRomSystems.Add(new RomSystem
        {
            Name     = source.Name,
            Folder   = source.Folder,
            RomNames = new List<string>(source.RomNames)
        });
    }

    /// <summary>Rebuild the CmbSystems item list and select <paramref name="selectName"/>.
    /// Items include systems from all profiles so the user can reuse previously imported folders.</summary>
    private void RefreshSystemsComboBox(string? selectName = null)
    {
        _refreshingComboBox = true;
        try
        {
            CmbSystems.Items.Clear();
            foreach (var sys in GetAllKnownRomSystems())
                CmbSystems.Items.Add(sys.Name);

            if (selectName != null)
            {
                // Try to find the item by name and select it (more reliable than .Text alone)
                int idx = -1;
                for (int i = 0; i < CmbSystems.Items.Count; i++)
                {
                    if (string.Equals(CmbSystems.Items[i]?.ToString(), selectName,
                        StringComparison.OrdinalIgnoreCase))
                    {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0)
                    CmbSystems.SelectedIndex = idx;
                else
                    CmbSystems.Text = selectName;
            }
            else if (_currentRomSystems.Count > 0 && CmbSystems.Items.Count > 0)
            {
                // Profile has systems — select the first one
                CmbSystems.SelectedIndex = 0;
            }
            else
            {
                // Profile has no systems — show None (global items stay in dropdown for re-linking)
                CmbSystems.SelectedIndex = -1;
                CmbSystems.Text = NoneText;
            }
        }
        finally
        {
            _refreshingComboBox = false;
        }
        SyncRomFolderUI();
    }

    /// <summary>Update LblRomFolder and LblScanResult to reflect the currently selected system.
    /// Shows a preview for unlinked systems using global cache data.</summary>
    private void SyncRomFolderUI()
    {
        // Use SelectedItem first (reliable), fall back to Text for typed names
        string selectedName = CmbSystems.SelectedItem?.ToString()?.Trim()
                           ?? CmbSystems.Text.Trim();

        if (string.IsNullOrEmpty(selectedName) || selectedName == NoneText)
        {
            LblRomFolder.Text = NoneText;
            LblRomFolder.Foreground = NoneGray;
            LblScanResult.Text = "";
            return;
        }

        var sys = FindRomSystemByName(selectedName);

        bool isLinked = _currentRomSystems.Any(s =>
            string.Equals(s.Name, selectedName, StringComparison.OrdinalIgnoreCase));

        if (sys == null || string.IsNullOrEmpty(sys.Folder))
        {
            LblRomFolder.Text = NoneText;
            LblRomFolder.Foreground = NoneGray;
            LblScanResult.Text = "";
        }
        else
        {
            string folderName = Path.GetFileName(sys.Folder.TrimEnd('\\', '/')) is { Length: > 0 } n ? n : sys.Folder;
            LblRomFolder.Text = isLinked ? folderName : folderName + "  \u25BA";
            LblRomFolder.Foreground = isLinked
                ? System.Windows.Media.Brushes.White
                : new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0x9C, 0xA3, 0xAF));
            LblScanResult.Text = sys.RomNames.Count > 0
                ? $"{sys.RomNames.Count} ROM(s) {L("found", "trouvée(s)", "gefunden", "trovate", "encontradas", "encontradas")}"
                : "";
        }
    }

    private void CmbSystems_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_isLoaded || _refreshingComboBox) return;
        // Preview only — the system is linked to the profile when the user clicks Save.
        SyncRomFolderUI();
    }

    private void BtnScanSystem_Click(object sender, RoutedEventArgs e)
    {
        string selectedName = CmbSystems.SelectedItem?.ToString()?.Trim()
                           ?? CmbSystems.Text.Trim();

        // Look in current profile first, then global cache
        var sys = FindRomSystemByName(selectedName);

        if (sys == null || string.IsNullOrEmpty(sys.Folder) || !Directory.Exists(sys.Folder))
        {
            MessageBox.Show(
                L("Please set a valid ROM folder first using the Set button.",
                  "Veuillez d'abord définir un dossier ROM valide via le bouton Définir.",
                  "Bitte legen Sie zuerst einen gültigen ROM-Ordner über die Schaltfläche Setzen fest.",
                  "Imposta prima una cartella ROM valida usando il pulsante Imposta.",
                  "Primero defina una carpeta ROM válida usando el botón Definir.",
                  "Primeiro defina uma pasta ROM válida usando o botão Definir."),
                "S4W", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var romNames = Directory.GetFiles(sys.Folder)
            .Select(f => Path.GetFileNameWithoutExtension(f))
            .Where(n => !string.IsNullOrWhiteSpace(n))
            .OrderBy(n => n, StringComparer.OrdinalIgnoreCase)
            .ToList();

        // Update the system in-place (both local and cached)
        sys.RomNames = romNames;
        var cached = _knownRomSystems.FirstOrDefault(k =>
            string.Equals(k.Name, selectedName, StringComparison.OrdinalIgnoreCase));
        if (cached != null && cached != sys)
            cached.RomNames = [.. romNames];

        LblScanResult.Text = $"{romNames.Count} ROM(s) {L("found", "trouvée(s)", "gefunden", "trovate", "encontradas", "encontradas")}";
    }


    // ══════════════════════════════════════════════════════════════
    //  UI SCALING  (x1 = 0.70 · x2 = 1.00 · x3 = 1.40 · x4 = 1.85)
    // ══════════════════════════════════════════════════════════════

    private void RadioScale_Click(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        int idx = GetScaleIndex();
        ApplyUiScale(idx);
        SaveUiScalePreference(idx + 1); // store as 1-based (1-4)
    }

    private int GetScaleIndex()
    {
        if (RadioScale1.IsChecked == true) return 0;
        if (RadioScale3.IsChecked == true) return 2;
        if (RadioScale4.IsChecked == true) return 3;
        return 1; // default x2
    }

    private void ApplyUiScale(int idx)
    {
        double scale = UiScaleFactors[Math.Clamp(idx, 0, 3)];
        if (Content is FrameworkElement root)
            root.LayoutTransform = new System.Windows.Media.ScaleTransform(scale, scale);

        // Constrain each tab ScrollViewer so the window never exceeds screen height.
        // SizeToContent="WidthAndHeight" handles width/height automatically after LayoutTransform.
        // Formula: visual_window = scale × (nonScrollH + scrollMaxH) ≤ screenH
        // → scrollMaxH = screenH/scale − nonScrollH
        // nonScrollH ≈ 200 logical px (header + tab bar + bottom rows)
        double screenH    = SystemParameters.PrimaryScreenHeight - 40;
        double scrollMaxH = screenH / scale - 200;
        Tab1Content.MaxHeight = scrollMaxH;
        Tab2Content.MaxHeight = scrollMaxH;
        Tab3Content.MaxHeight = scrollMaxH;

        // Force the window to measure all tabs so SizeToContent picks the widest one.
        // Reset MinWidth first so the new scale can shrink freely, then lock it to
        // the widest tab at this scale. Collapsed tabs don't participate in layout
        // measurement — reveal all, UpdateLayout (measure/arrange, no render), restore.
        MinWidth = 0;
        var v1 = Tab1Content.Visibility;
        var v2 = Tab2Content.Visibility;
        var v3 = Tab3Content.Visibility;
        Tab1Content.Visibility = Visibility.Visible;
        Tab2Content.Visibility = Visibility.Visible;
        Tab3Content.Visibility = Visibility.Visible;
        UpdateLayout();
        MinWidth = ActualWidth;
        Tab1Content.Visibility = v1;
        Tab2Content.Visibility = v2;
        Tab3Content.Visibility = v3;
    }

    private static void SaveUiScalePreference(int scaleNum)
    {
        try { File.WriteAllText(UiScaleFile, scaleNum.ToString()); } catch { }
    }

    private static int LoadUiScalePreference()
    {
        try
        {
            if (File.Exists(UiScaleFile) &&
                int.TryParse(File.ReadAllText(UiScaleFile).Trim(), out int v))
                return Math.Clamp(v, 1, 4);
        }
        catch { }
        return 2; // default x2
    }

    private void RestoreScaleRadioButton(int scaleNum)
    {
        RadioScale1.IsChecked = scaleNum == 1;
        RadioScale2.IsChecked = scaleNum == 2;
        RadioScale3.IsChecked = scaleNum == 3;
        RadioScale4.IsChecked = scaleNum == 4;
    }

    // ══════════════════════════════════════════════════════════════

    public MainWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    // ── Focus-steal prevention ───────────────────────────────────────────────
    // ── No-focus-steal: S4W stays on top without grabbing game focus ────────
    // Two complementary mechanisms:
    //   1. WS_EX_NOACTIVATE (extended style) — OS-level: mouse clicks never
    //      activate this window, protecting fullscreen-exclusive games from
    //      minimising when the user drags a slider or clicks a toggle.
    //   2. WM_MOUSEACTIVATE → MA_NOACTIVATE (WndProc hook) — belt-and-suspenders
    //      fallback for cases the style alone doesn't cover.
    // Programmatic Activate() calls (ShowFromTray, explicit focus requests) still
    // work because WS_EX_NOACTIVATE only blocks user-interaction activation.
    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        var hwnd = new WindowInteropHelper(this).Handle;
        HwndSource.FromHwnd(hwnd)?.AddHook(NoActivateWndProc);

        // Set WS_EX_NOACTIVATE so the OS never makes this window the foreground
        // window when the user clicks it (sliders, toggles, buttons all still work).
        int exStyle = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE);
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE,
            exStyle | NativeMethods.WS_EX_NOACTIVATE);

        // Route A: extend the DWM frame over the entire client area so the window
        // is composited by DWM (visible to DXGI-based capture tools such as OBS,
        // NVIDIA Overlay, Xbox Game Bar).  Pure-black pixels (#000000) rendered by
        // WPF become transparent through the DWM glass effect.
        var margins = new NativeMethods.MARGINS
        {
            cxLeftWidth   = -1,
            cxRightWidth  = -1,
            cyTopHeight   = -1,
            cyBottomHeight = -1
        };
        NativeMethods.DwmExtendFrameIntoClientArea(hwnd, ref margins);
    }

    private static IntPtr NoActivateWndProc(
        IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == NativeMethods.WM_MOUSEACTIVATE)
        {
            handled = true;
            return new IntPtr(NativeMethods.MA_NOACTIVATE);
        }
        return IntPtr.Zero;
    }

    // When the user clicks a text-input control (TextBox / ComboBox), temporarily
    // activate the window so keyboard events reach the control.
    // All other controls (sliders, checkboxes, buttons) work without activation.
    private void Window_PreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is DependencyObject src && NeedsKeyboardFocus(src))
            Activate();
    }

    private static bool NeedsKeyboardFocus(DependencyObject obj)
    {
        while (obj != null)
        {
            if (obj is TextBox or ComboBox) return true;
            obj = VisualTreeHelper.GetParent(obj);
        }
        return false;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _isLoaded = true;
        SetupTrayIcon();
        _appHistory = AppHistoryService.Load();
        RefreshAppComboBox();   // populate before LoadProfileList selects a profile
        LoadProfileList();
        UpdateCrtTabAvailability();
        InitializeCrtPresets();
        LoadShortcuts();          // global shortcuts — loaded once, not per-profile
        WireRealTimeEvents();
        UpdateMonitorAvailability();

        // Refresh monitor buttons automatically when the user connects/disconnects a display
        MonitorService.MonitorCountChanged += _ => Dispatcher.Invoke(UpdateMonitorAvailability);
        MonitorService.StartMonitoring();

        StartProcessMonitoring();
        UpdateScanlineCardBorders();

        // Initialize Start-with-Windows toggle from registry
        using (var regKey = Registry.CurrentUser.OpenSubKey(StartupRegistryKey))
            ToggleStartWithWindows.IsChecked = regKey?.GetValue(StartupRegistryName) != null;

        ApplyLanguage();   // initialise tous les labels dans la langue courante

        // Restore saved UI scale
        int savedScale = LoadUiScalePreference();
        RestoreScaleRadioButton(savedScale);
        ApplyUiScale(savedScale - 1); // convert to 0-based index

        // Auto-apply Desktop profile at startup if the option is on
        if (ToggleApplyDesktopAtStart.IsChecked == true)
        {
            // Make sure Desktop profile is selected, then apply it
            SelectProfile("Desktop");
            Dispatcher.BeginInvoke(DispatcherPriority.Background, () =>
            {
                if (!_overlayActive)
                    ToggleOverlayVisibility();
                BtnApplyBtn.Tag = "active";
            });
        }

        if (ToggleStartMinimized.IsChecked == true)
        {
            Dispatcher.BeginInvoke(DispatcherPriority.Background, MinimizeToTray);
        }

        // Check for updates in background — non-blocking
        _ = CheckForUpdatesAsync();
    }

    // ── Update check ──────────────────────────────────────────────────────────

    private async Task CheckForUpdatesAsync()
    {
        try
        {
            using var client = new HttpClient();
            client.Timeout = TimeSpan.FromSeconds(8);
            client.DefaultRequestHeaders.UserAgent.ParseAdd($"S4W/{AppVersion}");
            var json = await client.GetStringAsync(GitHubReleasesApi).ConfigureAwait(false);
            var match = Regex.Match(json, "\"tag_name\"\\s*:\\s*\"v?([^\"]+)\"");
            if (!match.Success) return;
            var latest = match.Groups[1].Value.Trim();
            if (!IsNewerVersion(latest, AppVersion)) return;
            Dispatcher.Invoke(() =>
            {
                LblUpdateAvailable.Text       = $"● v{latest} AVAILABLE";
                BtnUpdateAvailable.Visibility = Visibility.Visible;
            });
        }
        catch { /* silently ignore — no internet, firewall, etc. */ }
    }

    private static bool IsNewerVersion(string latest, string current)
    {
        if (Version.TryParse(latest, out var l) && Version.TryParse(current, out var c))
            return l > c;
        return string.Compare(latest, current, StringComparison.OrdinalIgnoreCase) > 0;
    }

    private void BtnUpdateAvailable_Click(object sender, RoutedEventArgs e)
    {
        try { Process.Start(new ProcessStartInfo(ItchIoUrl) { UseShellExecute = true }); }
        catch { }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB SWITCHING
    // ══════════════════════════════════════════════════════════════

    private void TabBtn1_Click(object sender, MouseButtonEventArgs e)
    {
        SwitchToTab(1);
    }

    private void TabBtn2_Click(object sender, MouseButtonEventArgs e)
    {
        SwitchToTab(2);
    }

    private void TabBtn3_Click(object sender, MouseButtonEventArgs e)
    {
        SwitchToTab(3);
    }

    private void SwitchToTab(int tab)
    {
        _activeTab = tab;
        var white = (Brush)new BrushConverter().ConvertFrom("White")!;
        var muted = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;

        Tab1Content.Visibility = tab == 1 ? Visibility.Visible : Visibility.Collapsed;
        Tab3Content.Visibility = tab == 2 ? Visibility.Visible : Visibility.Collapsed; // tab 2 button = CRT Effects (Tab3Content)
        Tab2Content.Visibility = tab == 3 ? Visibility.Visible : Visibility.Collapsed; // tab 3 button = System Settings (Tab2Content)

        TabBtn1Text.Foreground = tab == 1 ? white : muted;
        TabBtn1Icon.Foreground = tab == 1 ? white : muted;
        TabBtn1Indicator.Visibility = tab == 1 ? Visibility.Visible : Visibility.Collapsed;

        TabBtn2Text.Foreground = tab == 2 ? white : muted;
        TabBtn2Icon.Foreground = tab == 2 ? white : muted;
        TabBtn2Indicator.Visibility = tab == 2 ? Visibility.Visible : Visibility.Collapsed;

        TabBtn3Text.Foreground = tab == 3 ? white : muted;
        TabBtn3Icon.Foreground = tab == 3 ? white : muted;
        TabBtn3Indicator.Visibility = tab == 3 ? Visibility.Visible : Visibility.Collapsed;
    }

    // ══════════════════════════════════════════════════════════════
    //  SYSTEM TRAY
    // ══════════════════════════════════════════════════════════════

    private static System.Drawing.Icon LoadAppIcon()
    {
        try
        {
            var exePath = Process.GetCurrentProcess().MainModule?.FileName;
            if (!string.IsNullOrEmpty(exePath) && File.Exists(exePath))
            {
                var icon = System.Drawing.Icon.ExtractAssociatedIcon(exePath);
                if (icon != null) return icon;
            }
        }
        catch { }
        return System.Drawing.SystemIcons.Application;
    }

    private void SetupTrayIcon()
    {
        _trayIcon = new System.Windows.Forms.NotifyIcon
        {
            Icon = LoadAppIcon(),
            Text = "S4W",
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => ShowFromTray();

        var menu = new System.Windows.Forms.ContextMenuStrip();
        _trayMenuShow = menu.Items.Add("Show S4W", null, (_, _) => ShowFromTray());
        _trayMenuExit = menu.Items.Add("Exit", null, (_, _) =>
        {
            _forceClose = true;
            Close();
        });
        _trayIcon.ContextMenuStrip = menu;
    }

    private void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private void MinimizeToTray()
    {
        Hide();
    }

    // ══════════════════════════════════════════════════════════════
    //  WINDOW CHROME & CLOSE BEHAVIOR
    // ══════════════════════════════════════════════════════════════

private void Header_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
{
    if (e.ClickCount == 2)
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal : WindowState.Maximized;
    else
        DragMove();
}

private void BtnMinimize_Click(object sender, RoutedEventArgs e)
    => WindowState = WindowState.Minimized;

private void BtnClose_Click(object sender, RoutedEventArgs e)
{
    if (ToggleCloseToTray?.IsChecked == true)
    {
        MinimizeToTray();
    }
    else
    {
        _forceClose = true;
        Close();
    }
}

protected override void OnClosing(CancelEventArgs e)
{
    if (!_forceClose && ToggleCloseToTray?.IsChecked == true)
    {
        e.Cancel = true;
        MinimizeToTray();
        return;
    }
    base.OnClosing(e);
}

protected override void OnClosed(EventArgs e)
{
    MonitorService.StopMonitoring();
    _processCheckTimer?.Stop();
    _updateTimer?.Stop();

    _switchBezelHotkeyService?.Dispose();
    _switchBezelBackHotkeyService?.Dispose();
    _switchProfileHotkeyService?.Dispose();
    _switchProfileBackHotkeyService?.Dispose();
    _blurUpHotkeyService?.Dispose();
    _blurDownHotkeyService?.Dispose();
    _bloomUpHotkeyService?.Dispose();
    _bloomDownHotkeyService?.Dispose();
    _curvatureUpHotkeyService?.Dispose();
    _curvatureDownHotkeyService?.Dispose();
    _flickerUpHotkeyService?.Dispose();
    _flickerDownHotkeyService?.Dispose();
    _phosphorUpHotkeyService?.Dispose();
    _phosphorDownHotkeyService?.Dispose();
    _hOpacityUpHotkeyService?.Dispose();
    _hOpacityDownHotkeyService?.Dispose();
    _vOpacityUpHotkeyService?.Dispose();
    _vOpacityDownHotkeyService?.Dispose();
    _vhsUpHotkeyService?.Dispose();
    _vhsDownHotkeyService?.Dispose();
    _grainUpHotkeyService?.Dispose();
    _grainDownHotkeyService?.Dispose();
    _tapeNoiseUpHotkeyService?.Dispose();
    _tapeNoiseDownHotkeyService?.Dispose();

    if (_trayIcon != null)
    {
        _trayIcon.Visible = false;
        _trayIcon.Dispose();
    }

    // Stop hook and restore game window before closing
    CleanupHook();
    RestoreGameWindow();

    // Kill the monitored game process (Alt+F4 exits everything)
    TerminateGameProcess();

    _crtOsdTimer?.Stop();
    _hookSharedMem?.ClearOsd();
    _overlay?.Close();
    base.OnClosed(e);
}


    // ══════════════════════════════════════════════════════════════
    //  REAL-TIME EVENT WIRING
    // ══════════════════════════════════════════════════════════════

    private void WireRealTimeEvents()
    {
        TextBox[] inputs = [TxtHGap, TxtHThickness,
                            TxtVGap, TxtVThickness];
        foreach (var tb in inputs)
        {
            tb.TextChanged += (_, _) => ScheduleOverlayUpdate();
            tb.PreviewKeyDown += TextBox_ArrowKeyHandler;
        }

        // Clamp to monitor limits on focus loss + re-format with "px" suffix
        TextBox[] clampTargets = [TxtHThickness, TxtHGap,
                                   TxtVThickness, TxtVGap];
        foreach (var tb in clampTargets)
        {
            var box = tb; // capture for closure
            box.LostFocus += (_, _) =>
            {
                ClampInputsToMonitor();
                box.Text = InputParser.FormatPixelValue(InputParser.ParsePixelValue(box.Text));
            };
        }

        SliderHOpacity.ValueChanged += (_, _) => ScheduleOverlayUpdate();
        SliderVOpacity.ValueChanged += (_, _) => ScheduleOverlayUpdate();
        SliderBlur.ValueChanged += (_, _) => ScheduleOverlayUpdate();
        SliderBezelOpacity.ValueChanged += (_, _) => ScheduleOverlayUpdate();
        TxtBezelPath.TextChanged  += (_, _) => { ScheduleOverlayUpdate(); UpdateNoneColors(); };
        CmbAppPath.SelectionChanged += (_, _) => UpdateNoneColors();

        // Detect when the user renames the profile by typing in the ComboBox editable field.
        // When they type away from "Desktop" → unlock; back to "Desktop" → re-lock.
        CmbProfile.AddHandler(
            System.Windows.Controls.TextBox.TextChangedEvent,
            new TextChangedEventHandler((_, _) => { if (_isLoaded) { UpdateDesktopProfileLock(); UpdateCrtTabAvailability(); } }));

        Monitor1.Checked += (_, _) => { ClampInputsToMonitor(); RepositionOverlay(); };
        Monitor2.Checked += (_, _) => { ClampInputsToMonitor(); RepositionOverlay(); };
        Monitor3.Checked += (_, _) => { ClampInputsToMonitor(); RepositionOverlay(); };
        Monitor4.Checked += (_, _) => { ClampInputsToMonitor(); RepositionOverlay(); };

        _updateTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _updateTimer.Tick += (_, _) =>
        {
            _updateTimer.Stop();
            if (_overlayActive) ApplyOverlay();
            // Update hook shared memory or capture renderer settings
            if (_overlayActive)
            {
                var s = BuildSettingsFromUI();
                if (_hookInjected)
                    UpdateHookSettings(s);
            }
        };
    }

    private void ScheduleOverlayUpdate()
    {
        _updateTimer?.Stop();
        _updateTimer?.Start();
    }

    // ══════════════════════════════════════════════════════════════
    //  "NONE" PLACEHOLDER COLOR (gray when empty, white when assigned)
    // ══════════════════════════════════════════════════════════════

    private static readonly System.Windows.Media.SolidColorBrush NoneGray =
        new(System.Windows.Media.Color.FromRgb(0x6B, 0x72, 0x80));

    /// <summary>Gray out the 3 "None" placeholder controls when nothing is assigned.</summary>
    private void UpdateNoneColors()
    {
        // 1. Bezel file path TextBox
        TxtBezelPath.Foreground = string.IsNullOrEmpty(_bezelFullPath)
            ? NoneGray : System.Windows.Media.Brushes.White;

        // 2. App path ComboBox (index 0 = "None" item)
        CmbAppPath.Foreground = CmbAppPath.SelectedIndex == 0
            ? NoneGray : System.Windows.Media.Brushes.White;

    }

    // ══════════════════════════════════════════════════════════════
    //  SPINNER BUTTONS ▲/▼
    // ══════════════════════════════════════════════════════════════

    private void SpinUp_Click(object sender, RoutedEventArgs e)   => SpinTextBox(sender, +1);
    private void SpinDown_Click(object sender, RoutedEventArgs e) => SpinTextBox(sender, -1);

    private void SpinTextBox(object sender, int delta)
    {
        // Layout: RepeatButton → StackPanel → Grid → TextBox (col 0)
        if (sender is FrameworkElement btn &&
            btn.Parent is System.Windows.Controls.StackPanel sp &&
            sp.Parent is Grid g)
        {
            var tb = g.Children.OfType<TextBox>().FirstOrDefault();
            if (tb == null || !tb.IsEnabled) return;
            int current = InputParser.ParsePixelValue(tb.Text);
            int newVal  = Math.Max(0, current + delta);
            tb.Text = InputParser.FormatPixelValue(newVal);
            tb.CaretIndex = tb.Text.Length;
            ClampInputsToMonitor();
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  ARROW KEY ±1
    // ══════════════════════════════════════════════════════════════

    private void TextBox_ArrowKeyHandler(object sender, KeyEventArgs e)
    {
        if (sender is not TextBox tb) return;
        if (e.Key != Key.Up && e.Key != Key.Down) return;

        int current = InputParser.ParsePixelValue(tb.Text);
        int newVal = Math.Max(0, current + (e.Key == Key.Up ? 1 : -1));
        tb.Text = InputParser.FormatPixelValue(newVal);
        tb.CaretIndex = tb.Text.Length;
        e.Handled = true;

        // Re-clamp after arrow-key adjustment
        ClampInputsToMonitor();
    }

    // ══════════════════════════════════════════════════════════════
    //  TOGGLES
    // ══════════════════════════════════════════════════════════════

    private void ToggleHorizontal_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleHorizontal.IsChecked == true;
        var active = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray   = (Brush)new BrushConverter().ConvertFrom("#555555")!;

        LblHorizontal.Foreground  = active;
        LblHGap.Foreground        = on ? active : gray;
        LblHThickness.Foreground  = on ? active : gray;
        LblHOpacity.Foreground    = on ? active : gray;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;
        TxtHOpacityPct.Foreground = on ? magenta : gray;

        TxtHGap.IsEnabled       = on;  TxtHGap.Opacity       = on ? 1 : 0.4;
        TxtHThickness.IsEnabled = on;  TxtHThickness.Opacity = on ? 1 : 0.4;
        SliderHOpacity.IsEnabled = on;
        SliderHOpacity.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        UpdateScanlineCardBorders();
        ScheduleOverlayUpdate();
    }

    private void ToggleVertical_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleVertical.IsChecked == true;
        var active = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray = (Brush)new BrushConverter().ConvertFrom("#555555")!;

        LblVertical.Foreground   = active;
        LblVGap.Foreground       = on ? active : gray;
        LblVThickness.Foreground = on ? active : gray;
        LblVOpacity.Foreground   = on ? active : gray;
        var magenta2 = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;
        TxtVOpacityPct.Foreground = on ? magenta2 : gray;

        TxtVGap.IsEnabled       = on;  TxtVGap.Opacity       = on ? 1 : 0.4;
        TxtVThickness.IsEnabled = on;  TxtVThickness.Opacity = on ? 1 : 0.4;
        SliderVOpacity.IsEnabled = on;
        SliderVOpacity.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        UpdateScanlineCardBorders();
        ScheduleOverlayUpdate();
    }

    private void ToggleBlur_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleBlur.IsChecked == true;
        var active = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray   = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white  = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblBlurTitle.Foreground     = on ? white : gray;
        LblBlurIntensity.Foreground = on ? active : gray;
        TxtBlurPct.Foreground       = on ? magenta : gray;

        SliderBlur.IsEnabled = on;
        SliderBlur.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void ToggleBloom_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleBloom.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblBloomTitle.Foreground     = on ? white : gray;
        LblBloomIntensity.Foreground = on ? active : gray;
        TxtBloomPct.Foreground       = on ? magenta : gray;

        SliderBloom.IsEnabled = on;
        SliderBloom.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void SliderBloom_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void ToggleCurvature_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleCurvature.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblCurvatureTitle.Foreground  = on ? white : gray;
        LblCurvatureAmount.Foreground = on ? active : gray;
        TxtCurvaturePct.Foreground    = on ? magenta : gray;

        SliderCurvature.IsEnabled = on;
        SliderCurvature.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        // Edge fade toggle is only active when curvature is on
        ToggleVignette.IsEnabled = on;
        LblVignetteTitle.Foreground = (on && ToggleVignette.IsChecked == true) ? white : gray;

        ScheduleOverlayUpdate();
    }

    private void ToggleVignette_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleVignette.IsChecked == true;
        var white = (Brush)new BrushConverter().ConvertFrom("White")!;
        var gray  = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        LblVignetteTitle.Foreground = on ? white : gray;
        ScheduleOverlayUpdate();
    }

    private void SliderCurvature_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void ToggleFlicker_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleFlicker.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblFlickerTitle.Foreground     = on ? white : gray;
        LblFlickerIntensity.Foreground = on ? active : gray;
        TxtFlickerPct.Foreground       = on ? magenta : gray;
        LblFlickerRate.Foreground      = on ? active : gray;
        TxtFlickerRatePct.Foreground   = on ? magenta : gray;

        SliderFlicker.IsEnabled     = on;
        SliderFlicker.Style         = (Style)FindResource(on ? "PinkSlider" : "GraySlider");
        SliderFlickerRate.IsEnabled = on;
        SliderFlickerRate.Style     = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void SliderFlicker_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void SliderFlickerRate_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void TogglePhosphor_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = TogglePhosphor.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblPhosphorTitle.Foreground     = on ? white : gray;
        LblPhosphorIntensity.Foreground = on ? active : gray;
        TxtPhosphorPct.Foreground       = on ? magenta : gray;

        SliderPhosphor.IsEnabled = on;
        SliderPhosphor.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void SliderPhosphor_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void ToggleLuma_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleLuma.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblLumaTitle.Foreground      = on ? white   : gray;
        LblBrightnessLbl.Foreground  = on ? active  : gray;
        LblContrastLbl.Foreground    = on ? active  : gray;
        LblSaturationLbl.Foreground  = on ? active  : gray;
        LblTemperatureLbl.Foreground = on ? active  : gray;
        LblBlackLevelLbl.Foreground  = on ? active  : gray;
        LblGammaLbl.Foreground       = on ? active  : gray;
        TxtBrightnessPct.Foreground  = on ? magenta : gray;
        TxtContrastPct.Foreground    = on ? magenta : gray;
        TxtSaturationPct.Foreground  = on ? magenta : gray;
        TxtTemperaturePct.Foreground = on ? magenta : gray;
        TxtBlackLevelVal.Foreground  = on ? magenta : gray;
        TxtGammaVal.Foreground       = on ? magenta : gray;

        SliderBrightness.IsEnabled  = on;
        SliderContrast.IsEnabled    = on;
        SliderSaturation.IsEnabled  = on;
        SliderTemperature.IsEnabled = on;
        SliderBlackLevel.IsEnabled  = on;
        SliderGamma.IsEnabled       = on;
        SliderBrightness.Style  = (Style)FindResource(on ? "CenterPinkSlider" : "GraySlider");
        SliderContrast.Style    = (Style)FindResource(on ? "CenterPinkSlider" : "GraySlider");
        SliderSaturation.Style  = (Style)FindResource(on ? "CenterPinkSlider" : "GraySlider");
        SliderTemperature.Style = (Style)FindResource(on ? "CenterPinkSlider" : "GraySlider");
        // Black Level (0 = default/min) and Gamma (1.0 = default) use PinkSlider (left-fill)
        SliderBlackLevel.Style  = (Style)FindResource(on ? "PinkSlider" : "GraySlider");
        SliderGamma.Style       = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        if (on)
        {
            Dispatcher.InvokeAsync(() => {
                UpdateCenterFill(SliderBrightness);
                UpdateCenterFill(SliderContrast);
                UpdateCenterFill(SliderSaturation);
                UpdateCenterFill(SliderTemperature);
            }, System.Windows.Threading.DispatcherPriority.Loaded);
        }

        ScheduleOverlayUpdate();
    }

    private void SliderBrightness_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        UpdateCenterFill(SliderBrightness);
        ScheduleOverlayUpdate();
    }

    private void SliderContrast_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        UpdateCenterFill(SliderContrast);
        ScheduleOverlayUpdate();
    }

    private void SliderSaturation_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        UpdateCenterFill(SliderSaturation);
        ScheduleOverlayUpdate();
    }

    private void SliderTemperature_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        UpdateCenterFill(SliderTemperature);
        ScheduleOverlayUpdate();
    }

    private void SliderBlackLevel_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void SliderGamma_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    // ── Double-click any slider thumb → reset to 0 ──
    // Uses PreviewMouseLeftButtonDown + ClickCount==2 because PreviewMouseDoubleClick
    // is often absorbed by the Thumb. We also walk up the visual tree because
    // OriginalSource may be an inner element (Ellipse, Border…) inside the Thumb.
    private void Slider_ResetOnDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (e.ClickCount != 2) return;
        if (sender is not Slider s) return;

        // Walk up from the clicked element until we find a Thumb
        var el = e.OriginalSource as System.Windows.DependencyObject;
        while (el != null)
        {
            if (el is System.Windows.Controls.Primitives.Thumb)
            {
                // Tag holds the reset target (e.g. Gamma → 100, others → 0)
                double resetVal = 0;
                if (s.Tag is string tag && double.TryParse(tag,
                        System.Globalization.NumberStyles.Any,
                        System.Globalization.CultureInfo.InvariantCulture, out double tv))
                    resetVal = tv;
                s.Value = resetVal;
                e.Handled = true;
                return;
            }
            el = System.Windows.Media.VisualTreeHelper.GetParent(el);
        }
    }

    // ── Black Level: slider [0, 100]% ↔ raw [0.0, 0.3] ──
    static double BlackLevelToSlider(double raw) => Math.Round(raw / 0.3 * 100.0);
    static double SliderToBlackLevel(double v)   => v / 100.0 * 0.3;

    // ── Gamma: slider [50, 200]% ↔ raw [0.5, 2.0] (raw × 100) ──
    static double GammaToSlider(double raw) => Math.Round(raw * 100.0);
    static double SliderToGamma(double v)   => v / 100.0;

    private void ToggleVhs_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleVhs.IsChecked == true;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;

        LblVhsTitle.Foreground = on ? white : gray;

        // Cascade to all sub-effects
        ToggleVhsEffect.IsEnabled = on;
        ToggleGrain.IsEnabled     = on;
        ToggleTapeNoise.IsEnabled = on;
        if (on)
        {
            ToggleVhsEffect_Changed(ToggleVhsEffect, new RoutedEventArgs());
            ToggleGrain_Changed(ToggleGrain, new RoutedEventArgs());
            ToggleTapeNoise_Changed(ToggleTapeNoise, new RoutedEventArgs());
        }
        else
        {
            var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
            var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

            LblVhsEffectTitle.Foreground = gray;
            LblVhsIntensity.Foreground   = gray;
            TxtVhsPct.Foreground         = gray;
            SliderVhs.IsEnabled          = false;
            SliderVhs.Style              = (Style)FindResource("GraySlider");

            LblGrainTitle.Foreground     = gray;
            LblGrainIntensity.Foreground = gray;
            TxtGrainPct.Foreground       = gray;
            SliderGrain.IsEnabled        = false;
            SliderGrain.Style            = (Style)FindResource("GraySlider");

            LblTapeNoiseTitle.Foreground     = gray;
            LblTapeNoiseIntensity.Foreground = gray;
            TxtTapeNoisePct.Foreground       = gray;
            SliderTapeNoise.IsEnabled        = false;
            SliderTapeNoise.Style            = (Style)FindResource("GraySlider");
        }

        ScheduleOverlayUpdate();
    }

    private void ToggleVhsEffect_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleVhsEffect.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var white   = (Brush)new BrushConverter().ConvertFrom("White")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblVhsEffectTitle.Foreground = on ? white   : gray;
        LblVhsIntensity.Foreground   = on ? active  : gray;
        TxtVhsPct.Foreground         = on ? magenta : gray;

        SliderVhs.IsEnabled = on;
        SliderVhs.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void ToggleGrain_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleGrain.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblGrainTitle.Foreground     = on ? (Brush)new BrushConverter().ConvertFrom("White")! : gray;
        LblGrainIntensity.Foreground = on ? active  : gray;
        TxtGrainPct.Foreground       = on ? magenta : gray;

        SliderGrain.IsEnabled = on;
        SliderGrain.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void SliderVhs_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void SliderGrain_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    private void ToggleTapeNoise_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleTapeNoise.IsChecked == true;
        var active  = (Brush)new BrushConverter().ConvertFrom("#6B7280")!;
        var gray    = (Brush)new BrushConverter().ConvertFrom("#555555")!;
        var magenta = (Brush)new BrushConverter().ConvertFrom("#FF2AFF")!;

        LblTapeNoiseTitle.Foreground     = on ? (Brush)new BrushConverter().ConvertFrom("White")! : gray;
        LblTapeNoiseIntensity.Foreground = on ? active  : gray;
        TxtTapeNoisePct.Foreground       = on ? magenta : gray;

        SliderTapeNoise.IsEnabled = on;
        SliderTapeNoise.Style = (Style)FindResource(on ? "PinkSlider" : "GraySlider");

        ScheduleOverlayUpdate();
    }

    private void SliderTapeNoise_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        ScheduleOverlayUpdate();
    }

    // ══════════════════════════════════════════════════════════════
    //  CRT PRESETS
    // ══════════════════════════════════════════════════════════════

    private void InitializeCrtPresets()
    {
        BtnPresetDelete.Content = MakeTrashIcon();
        _crtPresets = S4W.Services.CrtPresetService.Load();
        if (_crtPresets.Count == 0)
            _crtPresets = S4W.Services.CrtPresetService.GetDefaults();
        _crtPresetIndex = 0;
        TxtCrtPresetName.Text = _crtPresets[0].Name;
        // Don't apply preset on init — the profile's own CRT values are already loaded
    }

    private void BtnPresetPrev_Click(object sender, RoutedEventArgs e)
    {
        if (_crtPresets.Count == 0) return;
        _crtPresetIndex = (_crtPresetIndex - 1 + _crtPresets.Count) % _crtPresets.Count;
        TxtCrtPresetName.Text = _crtPresets[_crtPresetIndex].Name;
        ApplyCrtPreset(_crtPresets[_crtPresetIndex]);
    }

    private void BtnPresetNext_Click(object sender, RoutedEventArgs e)
    {
        if (_crtPresets.Count == 0) return;
        _crtPresetIndex = (_crtPresetIndex + 1) % _crtPresets.Count;
        TxtCrtPresetName.Text = _crtPresets[_crtPresetIndex].Name;
        ApplyCrtPreset(_crtPresets[_crtPresetIndex]);
    }

    private void BtnPresetSave_Click(object sender, RoutedEventArgs e)
    {
        string name = TxtCrtPresetName.Text.Trim();
        if (string.IsNullOrEmpty(name)) return;

        var preset = CrtPresetFromUI();
        preset.Name = name;

        // Update existing preset if same name, otherwise add new
        int existing = _crtPresets.FindIndex(p =>
            string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));
        if (existing >= 0)
        {
            _crtPresets[existing] = preset;
            _crtPresetIndex = existing;
        }
        else
        {
            _crtPresets.Add(preset);
            _crtPresetIndex = _crtPresets.Count - 1;
        }

        S4W.Services.CrtPresetService.Save(_crtPresets);
    }

    private void BtnPresetDelete_Click(object sender, RoutedEventArgs e)
    {
        if (_crtPresets.Count == 0) return;
        var current = _crtPresets[_crtPresetIndex];
        if (string.Equals(current.Name, "Default", StringComparison.OrdinalIgnoreCase))
            return; // Cannot delete Default preset

        _crtPresets.RemoveAt(_crtPresetIndex);
        if (_crtPresets.Count == 0)
            _crtPresets = S4W.Services.CrtPresetService.GetDefaults();
        _crtPresetIndex = Math.Max(0, _crtPresetIndex - 1);
        TxtCrtPresetName.Text = _crtPresets[_crtPresetIndex].Name;
        ApplyCrtPreset(_crtPresets[_crtPresetIndex]);
        S4W.Services.CrtPresetService.Save(_crtPresets);
    }

    /// <summary>
    /// Applies a CRT preset to the UI controls only.
    /// Does NOT save the global profile — user must click Save at the bottom for that.
    /// </summary>
    private void ApplyCrtPreset(S4W.Models.CrtPreset p)
    {
        bool prev = _isLoaded;
        _isLoaded = false; // Suppress ScheduleOverlayUpdate during bulk set

        ToggleBlur.IsChecked      = p.BlurEnabled;
        SliderBlur.Value          = p.BlurIntensity;
        ToggleBloom.IsChecked     = p.BloomEnabled;
        SliderBloom.Value         = p.BloomIntensity;
        ToggleCurvature.IsChecked = p.CurvatureEnabled;
        SliderCurvature.Value     = p.CurvatureIntensity;
        ToggleFlicker.IsChecked   = p.FlickerEnabled;
        SliderFlicker.Value = p.FlickerIntensity;
        SliderFlickerRate.Value      = p.FlickerRate;
        TogglePhosphor.IsChecked  = p.PhosphorEnabled;
        SliderPhosphor.Value      = p.PhosphorIntensity;
        ToggleLuma.IsChecked      = p.LumaEnabled;
        SliderBrightness.Value    = p.BrightnessValue;
        SliderContrast.Value      = p.ContrastValue;
        SliderSaturation.Value    = p.SaturationValue;
        SliderTemperature.Value   = p.TemperatureValue;
        SliderBlackLevel.Value    = BlackLevelToSlider(p.BlackLevelValue);
        SliderGamma.Value         = GammaToSlider(p.GammaValue);
        ToggleVhs.IsChecked           = p.VhsEnabled || p.GrainEnabled || p.TapeNoiseEnabled;
        ToggleVhsEffect.IsChecked     = p.VhsEnabled;
        SliderVhs.Value               = p.VhsIntensity;
        ToggleGrain.IsChecked         = p.GrainEnabled;
        SliderGrain.Value             = p.GrainIntensity;
        ToggleTapeNoise.IsChecked     = p.TapeNoiseEnabled;
        SliderTapeNoise.Value         = p.TapeNoiseIntensity;

        _isLoaded = prev;

        // Fire all toggle handlers to update colors/enabled states
        if (_isLoaded)
        {
            ToggleCrtGroup_Changed(ToggleCrtGroup, new RoutedEventArgs());
            ToggleBlur_Changed(ToggleBlur, new RoutedEventArgs());
            ToggleBloom_Changed(ToggleBloom, new RoutedEventArgs());
            ToggleCurvature_Changed(ToggleCurvature, new RoutedEventArgs());
            ToggleFlicker_Changed(ToggleFlicker, new RoutedEventArgs());
            TogglePhosphor_Changed(TogglePhosphor, new RoutedEventArgs());
            ToggleLuma_Changed(ToggleLuma, new RoutedEventArgs());
            ToggleVhs_Changed(ToggleVhs, new RoutedEventArgs());
            ToggleVhsEffect_Changed(ToggleVhsEffect, new RoutedEventArgs());
            ScheduleOverlayUpdate();
        }
    }

    /// <summary>Reads current CRT UI values into a new CrtPreset (name not set).</summary>
    private S4W.Models.CrtPreset CrtPresetFromUI() => new()
    {
        BlurEnabled      = ToggleBlur.IsChecked == true,
        BlurIntensity    = SliderBlur.Value,
        BloomEnabled     = ToggleBloom.IsChecked == true,
        BloomIntensity   = SliderBloom.Value,
        CurvatureEnabled = ToggleCurvature.IsChecked == true,
        CurvatureIntensity = SliderCurvature.Value,
        FlickerEnabled   = ToggleFlicker.IsChecked == true,
        FlickerIntensity = SliderFlicker.Value,
        FlickerRate      = SliderFlickerRate.Value,
        PhosphorEnabled  = TogglePhosphor.IsChecked == true,
        PhosphorIntensity = SliderPhosphor.Value,
        LumaEnabled      = ToggleLuma.IsChecked == true,
        BrightnessValue  = SliderBrightness.Value,
        ContrastValue    = SliderContrast.Value,
        SaturationValue  = SliderSaturation.Value,
        TemperatureValue = SliderTemperature.Value,
        BlackLevelValue  = SliderToBlackLevel(SliderBlackLevel.Value),
        GammaValue       = SliderToGamma(SliderGamma.Value),
        VhsEnabled           = ToggleVhs.IsChecked == true,
        VhsIntensity         = SliderVhs.Value,
        GrainEnabled         = ToggleGrain.IsChecked == true,
        GrainIntensity       = SliderGrain.Value,
        TapeNoiseEnabled     = ToggleTapeNoise.IsChecked == true,
        TapeNoiseIntensity   = SliderTapeNoise.Value,
    };

    // ══════════════════════════════════════════════════════════════
    //  CRT SECTION COLLAPSE BUTTONS
    // ══════════════════════════════════════════════════════════════

    private void ToggleCrtGroup_Changed(object sender, RoutedEventArgs e)
    {
        if (!_isLoaded) return;
        bool on = ToggleCrtGroup.IsChecked == true;
        LblCrtGroupTitle.Foreground = on
            ? (Brush)new BrushConverter().ConvertFrom("White")!
            : (Brush)new BrushConverter().ConvertFrom("#555555")!;
        CrtGroupContentPanel.IsEnabled = on;
        CrtGroupContentPanel.Opacity   = on ? 1.0 : 0.35;
        if (on)
        {
            ToggleCurvature_Changed(ToggleCurvature, new RoutedEventArgs());
            ToggleBlur_Changed(ToggleBlur, new RoutedEventArgs());
            ToggleBloom_Changed(ToggleBloom, new RoutedEventArgs());
            ToggleFlicker_Changed(ToggleFlicker, new RoutedEventArgs());
            TogglePhosphor_Changed(TogglePhosphor, new RoutedEventArgs());
            ToggleLuma_Changed(ToggleLuma, new RoutedEventArgs());
        }
        ScheduleOverlayUpdate();
    }

    private void BtnCollapseCrtGroup_Click(object sender, RoutedEventArgs e)
    {
        bool collapsed = CrtGroupBody.Visibility == Visibility.Collapsed;
        CrtGroupBody.Visibility = collapsed ? Visibility.Visible : Visibility.Collapsed;
        BtnCollapseCrtGroup.Content = collapsed ? "▲" : "▼";
    }

    private void BtnCollapseVhs_Click(object sender, RoutedEventArgs e)
    {
        bool collapsed = VhsBody.Visibility == Visibility.Collapsed;
        VhsBody.Visibility = collapsed ? Visibility.Visible : Visibility.Collapsed;
        BtnCollapseVhs.Content = collapsed ? "▲" : "▼";
    }

    // ── Bezel enable/disable toggle ──────────────────────────────────────────
    private void ToggleBezelEnabled_Changed(object sender, RoutedEventArgs e)
    {
        bool enabled = ToggleBezelEnabled.IsChecked == true;
        BezelContentPanel.IsEnabled = enabled;
        BezelContentPanel.Opacity   = enabled ? 1.0 : 0.35;
        ScheduleOverlayUpdate();
    }

    // ── Multi-system emulator toggle ─────────────────────────────────────────
    private void ToggleMultiSystemEnabled_Changed(object sender, RoutedEventArgs e)
    {
        UpdateMultiSystemContent();
    }

    private void UpdateMultiSystemContent()
    {
        bool enabled = ToggleMultiSystemEnabled?.IsChecked == true;
        RomSystemsContent.IsEnabled = enabled;
        RomSystemsContent.Opacity   = enabled ? 1.0 : 0.35;
    }

    // ── Software behavior toggles — auto-save immediately ────────────────────
    private void SaveBehaviorSettings()
    {
        if (!_isLoaded) return;
        if (_applyingSettings) return;
        string name = GetCurrentProfileName();
        ProfileService.Save(new ProfileData { ProfileName = name, Settings = BuildSettingsFromUI() });
    }

    private void ToggleStartMinimized_Changed(object sender, RoutedEventArgs e) => SaveBehaviorSettings();
    private void ToggleCloseToTray_Changed(object sender, RoutedEventArgs e)    => SaveBehaviorSettings();

    // ── Apply Desktop profile at start toggle ────────────────────────────────
    private void ToggleApplyDesktopAtStart_Changed(object sender, RoutedEventArgs e)
    {
        SaveBehaviorSettings();
    }

    // ── Start with Windows toggle ─────────────────────────────────────────────
    private const string StartupRegistryKey  = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string StartupRegistryName = "S4W";

    // ── Update check ──────────────────────────────────────────────────────────
    private const string GitHubReleasesApi = "https://api.github.com/repos/KuatoSoft/S4W-Scanlines-for-Windows/releases/latest";
    private const string ItchIoUrl         = "https://s4windows.itch.io/scanlines-for-windows";
    private static readonly string AppVersion =
        Assembly.GetExecutingAssembly().GetName().Version is { } v
            ? $"{v.Major}.{v.Minor}"
            : "1.2";

    private void ToggleStartWithWindows_Changed(object sender, RoutedEventArgs e)
    {
        bool enable = ToggleStartWithWindows.IsChecked == true;
        using var key = Registry.CurrentUser.OpenSubKey(StartupRegistryKey, writable: true);
        if (key == null) return;
        if (enable)
            key.SetValue(StartupRegistryName, $"\"{Environment.ProcessPath}\"");
        else
            key.DeleteValue(StartupRegistryName, throwOnMissingValue: false);
    }

    private void CenterSlider_Loaded(object sender, RoutedEventArgs e)
    {
        if (sender is Slider s)
            Dispatcher.InvokeAsync(() => UpdateCenterFill(s), System.Windows.Threading.DispatcherPriority.Loaded);
    }

    /// <summary>Updates the pink center-fill rectangle in a CenterPinkSlider template.</summary>
    private static void UpdateCenterFill(Slider slider)
    {
        if (!slider.IsLoaded) return;
        var canvas = slider.Template.FindName("PART_FillCanvas", slider) as System.Windows.Controls.Canvas;
        var fill   = slider.Template.FindName("PART_CenterFill",  slider) as System.Windows.Shapes.Rectangle;
        if (canvas == null || fill == null) return;

        double canvasWidth = canvas.ActualWidth;
        if (canvasWidth <= 0) return;

        double range      = slider.Maximum - slider.Minimum;
        double centerRatio = (0.0 - slider.Minimum) / range;
        double valueRatio  = (slider.Value  - slider.Minimum) / range;

        double centerX = centerRatio * canvasWidth;
        double valueX  = valueRatio  * canvasWidth;

        double left  = Math.Min(centerX, valueX);
        double width = Math.Abs(valueX - centerX);

        System.Windows.Controls.Canvas.SetLeft(fill, left);
        fill.Width = Math.Max(1, width);
    }

    private void UpdateScanlineCardBorders()
    {
    }


    // ══════════════════════════════════════════════════════════════
    //  MONITOR RESOLUTION GUARDS
    // ══════════════════════════════════════════════════════════════

    private (int W, int H) GetSelectedMonitorPixelSize()
    {
        var info = MonitorService.GetMonitor(GetSelectedMonitorIndex());
        return ((int)info.Width, (int)info.Height);
    }

    private bool _isClampingInputs;

    private void ClampInputsToMonitor()
    {
        if (_isClampingInputs) return;
        _isClampingInputs = true;
        try
        {
            var (monW, monH) = GetSelectedMonitorPixelSize();

            // HWidth and VHeight are auto (0 = full screen) — no clamping needed
        }
        finally
        {
            _isClampingInputs = false;
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  BUILD SETTINGS FROM UI
    // ══════════════════════════════════════════════════════════════

    private ScanlineSettings BuildSettingsFromUI()
    {
        return new ScanlineSettings
        {
            BezelPath         = _bezelFullPath,
            BezelOpacity      = SliderBezelOpacity.Value,
            BezelEnabled      = ToggleBezelEnabled.IsChecked == true,
            HorizontalEnabled = ToggleHorizontal.IsChecked == true,
            HGap              = InputParser.ParsePixelValue(TxtHGap.Text),
            HWidth            = 0, // auto: full screen width
            HThickness        = InputParser.ParsePixelValue(TxtHThickness.Text),
            HOpacity          = SliderHOpacity.Value,
            VerticalEnabled   = ToggleVertical.IsChecked == true,
            VGap              = InputParser.ParsePixelValue(TxtVGap.Text),
            VHeight           = 0, // auto: full screen height
            VThickness        = InputParser.ParsePixelValue(TxtVThickness.Text),
            VOpacity          = SliderVOpacity.Value,
            CrtGroupEnabled   = ToggleCrtGroup.IsChecked == true,
            BlurEnabled       = ToggleCrtGroup.IsChecked == true && ToggleBlur.IsChecked == true,
            BlurIntensity     = SliderBlur.Value,
            BloomEnabled      = ToggleCrtGroup.IsChecked == true && ToggleBloom.IsChecked == true,
            BloomIntensity    = SliderBloom.Value,
            CurvatureEnabled  = ToggleCrtGroup.IsChecked == true && ToggleCurvature.IsChecked == true,
            VignetteEnabled   = ToggleCrtGroup.IsChecked == true && ToggleCurvature.IsChecked == true && ToggleVignette.IsChecked == true,
            CurvatureIntensity = SliderCurvature.Value,
            FlickerEnabled    = ToggleCrtGroup.IsChecked == true && ToggleFlicker.IsChecked == true,
            FlickerIntensity  = SliderFlicker.Value,
            FlickerRate       = SliderFlickerRate.Value,
            PhosphorEnabled   = ToggleCrtGroup.IsChecked == true && TogglePhosphor.IsChecked == true,
            PhosphorIntensity = SliderPhosphor.Value,
            VhsEnabled          = ToggleVhs.IsChecked == true && ToggleVhsEffect.IsChecked == true,
            VhsIntensity        = SliderVhs.Value,
            GrainEnabled        = ToggleVhs.IsChecked == true && ToggleGrain.IsChecked == true,
            GrainIntensity      = SliderGrain.Value,
            TapeNoiseEnabled    = ToggleVhs.IsChecked == true && ToggleTapeNoise.IsChecked == true,
            TapeNoiseIntensity  = SliderTapeNoise.Value,
            LumaEnabled         = ToggleCrtGroup.IsChecked == true && ToggleLuma.IsChecked == true,
            BrightnessValue   = SliderBrightness.Value,
            ContrastValue     = SliderContrast.Value,
            SaturationValue   = SliderSaturation.Value,
            TemperatureValue  = SliderTemperature.Value,
            BlackLevelValue   = SliderToBlackLevel(SliderBlackLevel.Value),
            GammaValue        = SliderToGamma(SliderGamma.Value),
            ActivePresetName  = TxtCrtPresetName.Text.Trim(),
            StartMinimized      = ToggleStartMinimized.IsChecked == true,
            CloseToTray         = ToggleCloseToTray.IsChecked == true,
            ApplyDesktopAtStart = ToggleApplyDesktopAtStart.IsChecked == true,
            MultiSystemEnabled  = ToggleMultiSystemEnabled.IsChecked == true,
            MonitorIndex      = GetSelectedMonitorIndex(),
            ApplicationPath   = GetSelectedAppPath(),
            BorderlessEnabled  = ToggleBorderless.IsChecked == true,
            RomSystems        = _currentRomSystems
                                    .Select(s => new RomSystem
                                    {
                                        Name     = s.Name,
                                        Folder   = s.Folder,
                                        RomNames = [.. s.RomNames],
                                    })
                                    .ToList(),
            SwitchBezelModifiers        = _switchBezelMods.ToString(),
            SwitchBezelKey              = _switchBezelKey.ToString(),
            SwitchBezelBackModifiers    = _switchBezelBackMods.ToString(),
            SwitchBezelBackKey          = _switchBezelBackKey.ToString(),
            SwitchProfileModifiers      = _switchProfileMods.ToString(),
            SwitchProfileKey            = _switchProfileKey.ToString(),
            SwitchProfileBackModifiers  = _switchProfileBackMods.ToString(),
            SwitchProfileBackKey        = _switchProfileBackKey.ToString(),
            BlurUpModifiers             = _blurUpMods.ToString(),
            BlurUpKey                   = _blurUpKey.ToString(),
            BlurDownModifiers           = _blurDownMods.ToString(),
            BlurDownKey                 = _blurDownKey.ToString(),
            BloomUpModifiers            = _bloomUpMods.ToString(),
            BloomUpKey                  = _bloomUpKey.ToString(),
            BloomDownModifiers          = _bloomDownMods.ToString(),
            BloomDownKey                = _bloomDownKey.ToString(),
            CurvatureUpModifiers        = _curvatureUpMods.ToString(),
            CurvatureUpKey              = _curvatureUpKey.ToString(),
            CurvatureDownModifiers      = _curvatureDownMods.ToString(),
            CurvatureDownKey            = _curvatureDownKey.ToString(),
            FlickerUpModifiers          = _flickerUpMods.ToString(),
            FlickerUpKey                = _flickerUpKey.ToString(),
            FlickerDownModifiers        = _flickerDownMods.ToString(),
            FlickerDownKey              = _flickerDownKey.ToString(),
            PhosphorUpModifiers         = _phosphorUpMods.ToString(),
            PhosphorUpKey               = _phosphorUpKey.ToString(),
            PhosphorDownModifiers       = _phosphorDownMods.ToString(),
            PhosphorDownKey             = _phosphorDownKey.ToString(),
        };
    }

    private int GetSelectedMonitorIndex()
    {
        if (Monitor2.IsChecked == true) return 1;
        if (Monitor3.IsChecked == true) return 2;
        if (Monitor4.IsChecked == true) return 3;
        return 0;
    }

    // ══════════════════════════════════════════════════════════════
    //  OVERLAY MANAGEMENT
    // ══════════════════════════════════════════════════════════════

    /// <summary>
    /// Finds the first visible top-level window belonging to the given PID.
    /// Works for SDL2/FNA/Unity games that don't set Process.MainWindowHandle.
    /// </summary>
    private static IntPtr GetWindowHandleByPid(int pid)
    {
        IntPtr found = IntPtr.Zero;
        NativeMethods.EnumWindows((hwnd, _) =>
        {
            if (!NativeMethods.IsWindowVisible(hwnd)) return true;
            NativeMethods.GetWindowThreadProcessId(hwnd, out uint wpid);
            if (wpid == (uint)pid) { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    private void UpdateStatusBar()
    {
        Dispatcher.Invoke(() =>
        {
            if (_overlayActive)
            {
                if (_hookInjected)
                {
                    // Hook active — cyan, show process name and architecture
                    var br = (Brush)new BrushConverter().ConvertFrom("#00D4FF")!;
                    StatusDot.Fill = br;
                    LblStatus.Foreground = br;
                    string arch = _hookedIs32bit ? "x86" : "x64";
                    string procLabel = _monitoredProcessName ?? "?";
                    LblStatus.Text = $"HOOK {arch} : {procLabel}";

                    // Gray out Width/Height in hook mode (full viewport auto)
                    SetHookModeUI(true);
                }
                else if (IsDesktopProfile())
                {
                    // Desktop overlay active (no hook needed) — green
                    var br = (Brush)new BrushConverter().ConvertFrom("#22C55E")!;
                    StatusDot.Fill = br;
                    LblStatus.Foreground = br;
                    LblStatus.Text = L("DESKTOP OVERLAY",
                        "OVERLAY BUREAU",
                        "DESKTOP OVERLAY",
                        "OVERLAY DESKTOP",
                        "OVERLAY ESCRITORIO",
                        "OVERLAY ÁREA DE TRABALHO");
                    SetHookModeUI(false);
                }
                else
                {
                    // Waiting for hook injection (HWND not yet available) — orange
                    var br = (Brush)new BrushConverter().ConvertFrom("#FFA500")!;
                    StatusDot.Fill = br;
                    LblStatus.Foreground = br;
                    LblStatus.Text = L("WAITING FOR GAME WINDOW...",
                        "EN ATTENTE DE LA FENÊTRE...",
                        "WARTE AUF SPIELFENSTER...",
                        "IN ATTESA DELLA FINESTRA...",
                        "ESPERANDO VENTANA...",
                        "AGUARDANDO JANELA...");
                }
            }
            else
            {
                var br = (Brush)new BrushConverter().ConvertFrom("#22C55E")!;
                StatusDot.Fill = br;
                LblStatus.Foreground = br;
                LblStatus.Text = "READY";

                // Restore Width/Height controls
                SetHookModeUI(false);
            }
        });
    }

    private bool IsDesktopProfile() =>
        GetCurrentProfileName().Equals("Desktop", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Grays out sections irrelevant to the Desktop profile (Application path, ROM systems,
    /// Display Monitor) and re-enables them when the profile is renamed away from "Desktop".
    /// Also shows/hides the Desktop info hint text.
    /// </summary>
    private void UpdateDesktopProfileLock()
    {
        bool isDesktop = IsDesktopProfile();

        // App path + ROM sections: grayed for Desktop (no game to associate)
        AppPathSection.IsEnabled = !isDesktop;
        AppPathSection.Opacity   = isDesktop ? 0.35 : 1.0;

        // For Desktop: disable entire section including toggle.
        // For game profiles: the header/toggle is enabled; the content panel
        // follows the ToggleMultiSystemEnabled state.
        RomSystemsSection.IsEnabled = !isDesktop;
        RomSystemsSection.Opacity   = isDesktop ? 0.35 : 1.0;
        if (!isDesktop) UpdateMultiSystemContent();

        // Display Monitor: only meaningful for Desktop profile (game windows use their own monitor)
        MonitorSection.IsEnabled = isDesktop;
        MonitorSection.Opacity   = isDesktop ? 1.0 : 0.35;

        // Borderless: only meaningful for game profiles (no process to force for Desktop)
        BorderlessRow.IsEnabled = !isDesktop;
        BorderlessRow.Opacity   = isDesktop ? 0.35 : 1.0;

        // Info hint under the app path section
    }

    /// <summary>
    /// HWidth/VHeight are hidden (auto = full screen). This method is kept as a no-op
    /// so call sites don't need to be changed.
    /// </summary>
    private void SetHookModeUI(bool hookActive) { }

    /// <summary>
    /// Grays out the CRT Effects controls when the Desktop profile is active
    /// (the Desktop overlay uses legacy GPU scanlines — CRT effects are not supported there).
    /// </summary>
    private void UpdateCrtTabAvailability(bool? isDesktopOverride = null)
    {
        bool isDesktop = isDesktopOverride ?? IsDesktopProfile();
        CrtEffectsBody.IsEnabled = !isDesktop;
        CrtEffectsBody.Opacity   = isDesktop ? 0.35 : 1.0;

        // Sections stay expanded regardless of Desktop/game profile —
        // the gray-out (IsEnabled / Opacity) is enough to communicate unavailability.
    }

    private void ApplyOverlay()
    {
        var settings = BuildSettingsFromUI();
        if (_overlay == null)
        {
            _overlay = new OverlayWindow();
            _overlay.Show();
            // Never Activate() here — the game must keep its focus.
            // S4W stays visible (Topmost) but non-active.
        }
        else if (!_overlay.IsVisible)
        {
            _overlay.Show();
        }
        // Desktop profile = no game process = use legacy WPF scanlines on the overlay.
        // Game/emulator profiles = hook DLL renders scanlines; overlay is bezel-only.
        _overlay.EnableLegacyScanlines = IsDesktopProfile();
        _overlay.PositionOnMonitor(settings.MonitorIndex);
        string effectiveBezelPath = settings.BezelEnabled ? settings.BezelPath : "";
        _overlay.UpdateBezel(effectiveBezelPath, settings.BezelOpacity);
        _overlay.UpdateScanlines(settings);
    }

    private void RepositionOverlay()
    {
        _overlay?.PositionOnMonitor(GetSelectedMonitorIndex());
        ScheduleOverlayUpdate();
    }

    private void ToggleOverlayVisibility()
    {
        if (_overlay == null)
        {
            _overlayActive = true;
            ApplyOverlay();
        }
        else if (_overlay.IsVisible)
        {
            _overlay.Hide();
            _overlayActive = false;
        }
        else
        {
            _overlay.Show();
            _overlayActive = true;
            ApplyOverlay();
        }
        UpdateStatusBar();
    }

    // ══════════════════════════════════════════════════════════════
    //  SWITCH BEZEL (cycle images in folder) — NEXT + BACK
    // ══════════════════════════════════════════════════════════════

    private string[] RefreshBezelImageList()
    {
        string path = _bezelFullPath;
        string? folder = null;

        if (Directory.Exists(path))
            folder = path;
        else if (File.Exists(path))
            folder = Path.GetDirectoryName(path);

        if (folder == null) return [];

        return Directory.GetFiles(folder, "*.*")
            .Where(f => f.EndsWith(".png", StringComparison.OrdinalIgnoreCase)
                     || f.EndsWith(".jpg", StringComparison.OrdinalIgnoreCase)
                     || f.EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase)
                     || f.EndsWith(".bmp", StringComparison.OrdinalIgnoreCase)
                     || f.EndsWith(".gif", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f)
            .ToArray();
    }

    private void CycleBezelImage()
    {
        _bezelImages = RefreshBezelImageList();
        if (_bezelImages.Length == 0) return;
        _bezelImageIndex = (_bezelImageIndex + 1) % _bezelImages.Length;
        Dispatcher.Invoke(() =>
        {
            _bezelFullPath = _bezelImages[_bezelImageIndex];
            TxtBezelPath.Text = Path.GetFileName(_bezelFullPath);
            // OSD: show bezel filename only when bezel feature is in use
            if (!string.IsNullOrEmpty(_bezelFullPath))
            {
                string osd = "BEZEL   " + Path.GetFileNameWithoutExtension(_bezelFullPath);
                ShowGameOsd(osd);
            }
        });
    }

    private void CycleBezelImageBack()
    {
        _bezelImages = RefreshBezelImageList();
        if (_bezelImages.Length == 0) return;
        _bezelImageIndex = (_bezelImageIndex - 1 + _bezelImages.Length) % _bezelImages.Length;
        Dispatcher.Invoke(() =>
        {
            _bezelFullPath = _bezelImages[_bezelImageIndex];
            TxtBezelPath.Text = Path.GetFileName(_bezelFullPath);
            if (!string.IsNullOrEmpty(_bezelFullPath))
            {
                string osd = "BEZEL   " + Path.GetFileNameWithoutExtension(_bezelFullPath);
                ShowGameOsd(osd);
            }
        });
    }

    // ══════════════════════════════════════════════════════════════
    //  PROFILE TRANSITION (fade out → apply → fade in)
    // ══════════════════════════════════════════════════════════════

    /// <summary>
    /// Fades the overlay out (150 ms), runs <paramref name="applyAction"/>,
    /// then fades back in (150 ms).  Falls through immediately when the overlay
    /// is not visible.
    /// </summary>
    private void ApplyProfileWithTransition(Action applyAction)
    {
        if (_overlay == null || !_overlay.IsVisible)
        {
            applyAction();
            return;
        }

        _updateTimer?.Stop();   // suppress the 50 ms auto-update during the transition

        var dur = new System.Windows.Duration(TimeSpan.FromMilliseconds(150));

        var fadeOut = new System.Windows.Media.Animation.DoubleAnimation(1.0, 0.0, dur);
        fadeOut.Completed += (_, _) =>
        {
            applyAction();

            var fadeIn = new System.Windows.Media.Animation.DoubleAnimation(0.0, 1.0, dur);
            fadeIn.Completed += (_, _) =>
            {
                // Clear the animation and pin opacity at 1 so normal updates aren't blocked
                _overlay.BeginAnimation(System.Windows.UIElement.OpacityProperty, null);
                _overlay.Opacity = 1.0;
            };
            _overlay.BeginAnimation(System.Windows.UIElement.OpacityProperty, fadeIn);
        };

        _overlay.BeginAnimation(System.Windows.UIElement.OpacityProperty, fadeOut);
    }

    // ══════════════════════════════════════════════════════════════
    //  SWITCH PROFILE (cycle saved profiles) — NEXT + BACK
    // ══════════════════════════════════════════════════════════════

    private void CycleScanlineProfile()
    {
        _profileNames = ProfileService.ListProfiles().ToArray();
        if (_profileNames.Length == 0) return;
        _profileCycleIndex = (_profileCycleIndex + 1) % _profileNames.Length;
        Dispatcher.Invoke(() =>
        {
            ShowGameOsd("PROFILE   " + _profileNames[_profileCycleIndex]);
            ApplyProfileWithTransition(() =>
            {
                SelectProfile(_profileNames[_profileCycleIndex]);
                if (_overlayActive) ApplyOverlay();
            });
        });
    }

    private void CycleScanlineProfileBack()
    {
        _profileNames = ProfileService.ListProfiles().ToArray();
        if (_profileNames.Length == 0) return;
        _profileCycleIndex = (_profileCycleIndex - 1 + _profileNames.Length) % _profileNames.Length;
        Dispatcher.Invoke(() =>
        {
            ShowGameOsd("PROFILE   " + _profileNames[_profileCycleIndex]);
            ApplyProfileWithTransition(() =>
            {
                SelectProfile(_profileNames[_profileCycleIndex]);
                if (_overlayActive) ApplyOverlay();
            });
        });
    }

    // ══════════════════════════════════════════════════════════════
    //  PROCESS MONITORING
    // ══════════════════════════════════════════════════════════════

    private void StartProcessMonitoring()
    {
        RefreshProfileAppPaths();
        _processCheckTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _processCheckTimer.Tick += (_, _) => CheckProcesses();
        _processCheckTimer.Start();
    }

    private void RefreshProfileAppPaths()
    {
        _profileMonitorData.Clear();
        var allProfiles = ProfileService.ListProfiles().ToList();
        InjLog($"[INIT] RefreshProfileAppPaths — total profiles: {allProfiles.Count}");
        foreach (var name in allProfiles)
        {
            var profile = ProfileService.Load(name);
            if (profile == null) continue;

            string appPath = profile.Settings.ApplicationPath ?? "";
            if (appPath.Length > 0)
            {
                _profileMonitorData[name] = new ProfileMonitorEntry(
                    appPath,
                    profile.Settings.RomSystems);
                InjLog($"[INIT]   profile '{name}' → process '{Path.GetFileNameWithoutExtension(appPath)}' ({appPath}) romSystems={profile.Settings.RomSystems.Count}");
                foreach (var sys in profile.Settings.RomSystems)
                    InjLog($"[INIT]     system '{sys.Name}' folder='{sys.Folder}' romNames={sys.RomNames.Count} first='{(sys.RomNames.Count > 0 ? sys.RomNames[0] : "")}'");
            }
            else
            {
                InjLog($"[INIT]   profile '{name}' → no ApplicationPath (skipped)");
            }

            // Merge ROM systems from ALL profiles into persistent cache (including profiles without app path)
            foreach (var sys in profile.Settings.RomSystems)
            {
                var existing = _knownRomSystems.FirstOrDefault(k =>
                    string.Equals(k.Name, sys.Name, StringComparison.OrdinalIgnoreCase));
                if (existing == null)
                    _knownRomSystems.Add(new RomSystem { Name = sys.Name, Folder = sys.Folder, RomNames = [.. sys.RomNames] });
                else if (sys.RomNames.Count > existing.RomNames.Count)
                {
                    existing.Folder = sys.Folder;
                    existing.RomNames = [.. sys.RomNames];
                }
            }
        }
        InjLog($"[INIT] Auto-injection watching {_profileMonitorData.Count}/{allProfiles.Count} profiles.");
    }

    private void CheckProcesses()
    {
        if (_autoLoadedProfile != null && _monitoredProcessName != null)
        {
            bool emulatorRunning = IsProcessRunning(_monitoredProcessName);
            bool romStillVisible = !_monitoredRomMode ||
                                   IsRomDetected(_monitoredProcessName, _monitoredRomSystems, verbose: true);

            if (!emulatorRunning || !romStillVisible)
            {
                if (!emulatorRunning)
                    _processRunningPolls.Remove(_monitoredProcessName);

                // ── Soft vs Hard cleanup ──
                // If the emulator is still running but the ROM changed, keep the
                // hook DLL and shared memory alive — just deactivate and let the
                // next profile-match tick update settings in-place.  This avoids
                // the stale-g_Shared bug (DLL already loaded → LoadLibraryW is a
                // no-op → DllMain never re-runs → shared memory never re-opened).
                bool softCleanup = emulatorRunning && !romStillVisible;

                string savedProcessName = _monitoredProcessName;
                string revertTo = "Desktop";
                _autoLoadedProfile = null;
                _monitoredProcessName = null;
                _previousProfileName = null;
                _monitoredRomMode = false;
                _monitoredRomSystems = [];

                if (softCleanup)
                {
                    // Deactivate scanlines but keep shared memory + injection state
                    _hookSharedMem?.Deactivate();
                    InjLog($"[SOFT-CLEANUP] ROM gone but '{savedProcessName}' still running — deactivating, keeping state alive");
                    // Re-arm polling so the next ROM match fires quickly
                    _processRunningPolls[savedProcessName] = EmulatorWarmupPolls;
                }
                else
                {
                    // Emulator exited — full cleanup
                    CleanupHook();
                    InjLog($"[CLEANUP] Emulator '{savedProcessName}' exited — full cleanup");
                }

                RestoreGameWindow();

                // Hide overlay and reset UI to READY state
                if (_overlayActive)
                {
                    _overlay?.Hide();
                    _overlayActive = false;
                }
                BtnApplyBtn.Tag = null;

                _monitoredWindowStyle = 0;

                SelectProfile(revertTo);
                UpdateStatusBar();
            }
            else if (_overlayActive)
            {
                // ── Retry hook injection if not yet done ──
                {
                    var settings = BuildSettingsFromUI();
                    var captureProcs = Process.GetProcessesByName(_monitoredProcessName);
                    bool anyUninjected = captureProcs.Length > 0 && captureProcs.Any(p => !_injectedPids.Contains(p.Id));
                    if (!_hookInjected || anyUninjected)
                    {
                        if (captureProcs.Length > 0)
                        {
                            Process? target = null;
                            IntPtr gameHwnd = IntPtr.Zero;
                            foreach (var p in captureProcs)
                            {
                                IntPtr h = GetWindowHandleByPid(p.Id);
                                if (h == IntPtr.Zero) { p.Refresh(); h = p.MainWindowHandle; }
                                if (h != IntPtr.Zero) { target = p; gameHwnd = h; break; }
                            }
                            bool hwndTimeout = (DateTime.UtcNow - _injectRetryStart).TotalSeconds >= 5.0;
                            if (gameHwnd != IntPtr.Zero && target != null)
                            {
                                InjLog($"[RETRY] Injecting into '{_monitoredProcessName}' PID={target.Id} HWND=0x{gameHwnd:X}");
                                InjectHook(target.Id, settings);
                                UpdateStatusBar();
                            }
                            else if (hwndTimeout)
                            {
                                foreach (var p in captureProcs)
                                {
                                    InjLog($"[RETRY] no-HWND timeout — injecting into ALL: PID={p.Id}");
                                    InjectHook(p.Id, settings);
                                }
                                UpdateStatusBar();
                            }
                        }
                    }
                    foreach (var p in captureProcs) p.Dispose();
                }

                // Detect fullscreen transition via WS_POPUP style flag.
                // When it appears, the game entered borderless-fullscreen which can activate
                // DXGI Independent Flip / MPO, physically hiding our overlay.
                // ForceToFront() sends an activation attempt (without SWP_NOACTIVATE) that
                // forces DWM to exit MPO and re-composite.  WS_EX_NOACTIVATE on the overlay
                // ensures the game never actually loses focus.
                const int WS_POPUP = unchecked((int)0x80000000);
                var procs = System.Diagnostics.Process.GetProcessesByName(_monitoredProcessName);
                if (procs.Length > 0)
                {
                    procs[0].Refresh();
                    IntPtr monHwnd = procs[0].MainWindowHandle;
                    if (monHwnd != IntPtr.Zero)
                    {
                        int currentStyle = NativeMethods.GetWindowLong(monHwnd, NativeMethods.GWL_STYLE);
                        bool enteredFullscreen = (currentStyle & WS_POPUP) != 0
                                              && (_monitoredWindowStyle & WS_POPUP) == 0;
                        _monitoredWindowStyle = currentStyle;

                        if (enteredFullscreen)
                        {
                            // Transition detected: force DWM out of flip/MPO then give focus back.
                            // The main S4W window is never involved — fix goes through overlay only.
                            _overlay?.ForceToFrontWithFocusReturn(monHwnd);
                            return;
                        }
                    }
                }

                // No transition — periodic silent maintenance
                _overlay?.BringToFront();
            }
            return;
        }

        // ── Periodic diagnostic log (every ~30 s) ─────────────────────────────
        _diagTick++;
        if (_diagTick % 15 == 1) // first tick + every 15 ticks (×2 s = 30 s)
        {
            var monNames = _profileMonitorData.Values
                .Select(e => Path.GetFileNameWithoutExtension(e.AppPath))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();

            InjLog($"[DIAG] profiles={_profileMonitorData.Count} monitored=[{string.Join(", ", monNames)}]");
            foreach (string pn in monNames)
            {
                var ps = System.Diagnostics.Process.GetProcessesByName(pn);
                InjLog($"[DIAG]   process '{pn}' running={ps.Length > 0}" +
                       (ps.Length > 0 ? $" PIDs=[{string.Join(",", ps.Select(p => p.Id))}]" : ""));
                foreach (var p in ps) p.Dispose();
            }
            if (_profileMonitorData.Count == 0)
                InjLog("[DIAG] No profiles with ApplicationPath configured — auto-injection disabled.");
        }

        // ── Update running-poll counters for every monitored process ──
        var monitoredProcessNames = _profileMonitorData.Values
            .Select(e => Path.GetFileNameWithoutExtension(e.AppPath))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (string pn in monitoredProcessNames)
        {
            if (IsProcessRunning(pn))
                _processRunningPolls[pn] = _processRunningPolls.GetValueOrDefault(pn) + 1;
            else
                _processRunningPolls.Remove(pn);
        }

        // Log any newly detected running processes
        foreach (var kv in _processRunningPolls)
            if (kv.Value == 1) // first tick it appeared
                InjLog($"[POLL] Process '{kv.Key}' detected running (poll=1)");

        // ── Try to find a matching profile ──
        foreach (var (profileName, entry) in _profileMonitorData)
        {
            string processName = Path.GetFileNameWithoutExtension(entry.AppPath);
            if (!_processRunningPolls.ContainsKey(processName)) continue;

            // Emulator mode requires at least one system with scanned ROMs
            bool hasScannedRoms = entry.RomSystems.Any(s => s.RomNames.Count > 0);
            bool isEmulatorMode = entry.RomSystems.Count > 0 && hasScannedRoms;

            if (isEmulatorMode)
            {
                // Wait for the emulator window title to update before checking ROM name
                if (_processRunningPolls.GetValueOrDefault(processName) < EmulatorWarmupPolls)
                    continue;

                InjLog($"[MATCH-TRY] Checking profile '{profileName}' (emulatorMode) process='{processName}' systems={entry.RomSystems.Count} totalRoms={entry.RomSystems.Sum(s => s.RomNames.Count)}");
                if (!IsRomDetected(processName, entry.RomSystems, verbose: true))
                    continue;
            }
            else
            {
                InjLog($"[MATCH-TRY] Checking profile '{profileName}' (NON-emulator) process='{processName}'");
            }

            InjLog($"[MATCH] Profile '{profileName}' matched '{processName}' — calling SelectProfile...");
            _previousProfileName = GetCurrentProfileName();
            _autoLoadedProfile = profileName;
            _monitoredProcessName = processName;
            _monitoredRomMode = isEmulatorMode;
            _monitoredRomSystems = entry.RomSystems;
            _processRunningPolls.Remove(processName); // reset counter after loading
            try { SelectProfile(profileName); }
            catch (Exception ex) { InjLog($"[MATCH] SelectProfile EXCEPTION: {ex.Message}"); }
            if (!_overlayActive)
            {
                _overlayActive = true;
                try { ApplyOverlay(); }
                catch (Exception ex) { InjLog($"[MATCH] ApplyOverlay EXCEPTION: {ex.Message}"); }
            }

            // ── Hook injection ──
            ScanlineSettings settings;
            try { settings = BuildSettingsFromUI(); }
            catch (Exception ex) { InjLog($"[MATCH] BuildSettingsFromUI EXCEPTION: {ex.Message}"); break; }
            InjLog($"[AUTO] Profile '{profileName}' matched process '{processName}'. _hookInjected={_hookInjected}");
            {
                var procs = Process.GetProcessesByName(processName);
                if (procs.Length > 0)
                {
                    // Multiple processes may share the same name (e.g. launcher + game).
                    // Prefer the one with a visible window (the real game process).
                    Process? target = null;
                    IntPtr gameHwnd = IntPtr.Zero;
                    foreach (var p in procs)
                    {
                        IntPtr h = GetWindowHandleByPid(p.Id);
                        if (h == IntPtr.Zero) { p.Refresh(); h = p.MainWindowHandle; }
                        if (h != IntPtr.Zero) { target = p; gameHwnd = h; break; }
                    }
                    if (target == null) { target = procs[0]; gameHwnd = IntPtr.Zero; }

                    InjLog($"[AUTO] Process '{processName}' selected PID={target.Id} HWND=0x{gameHwnd:X} (of {procs.Length} matching)");

                    // ── Hook injection path ──
                    if (_injectedPids.Contains(target.Id) && _hookSharedMem != null)
                    {
                        InjLog($"[REUSE] DLL already in PID={target.Id} — updating shared memory with new profile settings");
                        var monitor = MonitorService.GetMonitor(settings.MonitorIndex);
                        _hookSharedMem.UpdateSettings(settings, (int)monitor.Width, (int)monitor.Height);
                    }
                    else if (gameHwnd != IntPtr.Zero)
                    {
                        InjectHook(target.Id, settings);
                    }
                    else
                    {
                        InjLog($"[AUTO] HWND=0 — injection deferred to retry loop");
                        _injectRetryStart = DateTime.UtcNow;
                    }
                    foreach (var p in procs) p.Dispose();
                }
                else
                {
                    InjLog($"[AUTO] Process '{processName}' not found by GetProcessesByName");
                }
            }

            // ── Borderless windowed fullscreen: apply if toggle is on ──
            if (settings.BorderlessEnabled)
                ApplyBorderlessWithRetry();

            BtnApplyBtn.Tag = "active";
            UpdateStatusBar();

            _monitoredWindowStyle = 0; // reset fullscreen tracker for new session

            break;
        }
    }

    private static bool IsProcessRunning(string processName)
    {
        try
        {
            var processes = Process.GetProcessesByName(processName);
            bool running = processes.Length > 0;
            foreach (var p in processes) p.Dispose();
            return running;
        }
        catch { return false; }
    }

    private static bool IsRomDetected(string emulatorProcessName, List<RomSystem> romSystems,
                                       bool verbose = false)
    {
        try
        {
            var processes = Process.GetProcessesByName(emulatorProcessName);
            if (processes.Length == 0 && verbose)
                InjLog($"[ROM-DET] no process found for '{emulatorProcessName}'");
            foreach (var p in processes)
            {
                int pid = p.Id; // capture before dispose
                string title = p.MainWindowTitle;
                p.Dispose();
                if (verbose)
                    InjLog($"[ROM-DET] '{emulatorProcessName}' PID={pid} title=\"{title}\"");
                foreach (var sys in romSystems)
                {
                    foreach (var rom in sys.RomNames)
                    {
                        // Match if title contains the full rom name, OR if the rom name
                        // starts with the title (handles emulators that strip trailing tags
                        // like "(Rev A)" from the window title but keep the base name).
                        bool matched = title.Length >= 4 && (
                            title.Contains(rom, StringComparison.OrdinalIgnoreCase) ||
                            rom.StartsWith(title.Trim(), StringComparison.OrdinalIgnoreCase));
                        if (matched)
                        {
                            if (verbose)
                                InjLog($"[ROM-DET] ✓ MATCH system='{sys.Name}' rom='{rom}' title='{title}'");
                            return true;
                        }
                    }
                    if (verbose)
                        InjLog($"[ROM-DET] ✗ no match in system='{sys.Name}' ({sys.RomNames.Count} roms)");
                }
            }
        }
        catch (Exception ex) { if (verbose) InjLog($"[ROM-DET] EXCEPTION: {ex.Message}"); }
        return false;
    }

    // ══════════════════════════════════════════════════════════════
    //  BORDERLESS FORCING (capture mode)
    // ══════════════════════════════════════════════════════════════

    /// <summary>
    /// Removes title bar and borders from the game window and resizes it
    /// to fill the target monitor — creating a "fake fullscreen" borderless window.
    /// </summary>
    private bool ForceBorderless(IntPtr hwnd, int monitorIndex)
    {
        if (!NativeMethods.IsWindow(hwnd)) return false;

        // Save original state only on first call (don't overwrite if retrying)
        if (_gameHwnd != hwnd)
        {
            _gameHwnd = hwnd;
            _savedGameStyle   = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_STYLE);
            _savedGameExStyle = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE);
            NativeMethods.GetWindowRect(hwnd, out _savedGameRect);
        }

        // Strip caption, thick frame, and scroll bars
        int newStyle = _savedGameStyle
                     & ~NativeMethods.WS_CAPTION
                     & ~NativeMethods.WS_THICKFRAME;
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_STYLE, newStyle);

        // Strip WS_EX_TOPMOST — prevents Z-order fights with the overlay
        int newExStyle = _savedGameExStyle & ~NativeMethods.WS_EX_TOPMOST;
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE, newExStyle);

        var monitor = MonitorService.GetMonitor(monitorIndex);
        int mx = (int)monitor.Left, my = (int)monitor.Top;
        int mw = (int)monitor.Width, mh = (int)monitor.Height;

        NativeMethods.SetWindowPos(hwnd, IntPtr.Zero,
            mx, my, mw, mh,
            NativeMethods.SWP_NOZORDER | NativeMethods.SWP_FRAMECHANGED
            | NativeMethods.SWP_SHOWWINDOW | NativeMethods.SWP_NOACTIVATE);

        // Ensure the game keeps focus after style change
        NativeMethods.SetForegroundWindow(hwnd);

        // Verify the window actually moved/resized — some games override SetWindowPos
        NativeMethods.GetWindowRect(hwnd, out var actual);
        int aw = actual.Right - actual.Left;
        int ah = actual.Bottom - actual.Top;
        bool ok = Math.Abs(actual.Left - mx) < 4
               && Math.Abs(actual.Top  - my) < 4
               && Math.Abs(aw - mw) < 4
               && Math.Abs(ah - mh) < 4;

        InjLog($"[BDL] ForceBorderless hwnd=0x{hwnd:X} monitor={mx},{my} {mw}x{mh} actual={actual.Left},{actual.Top} {aw}x{ah} → {(ok ? "OK" : "MISMATCH")}");
        return ok;
    }

    private int _savedGameExStyle;

    /// <summary>
    /// Restores the game window's original style (title bar, borders).
    /// Called when the game exits or monitoring stops.
    /// </summary>
    private void RestoreGameWindow()
    {
        if (_gameHwnd != IntPtr.Zero && _savedGameStyle != 0)
        {
            try
            {
                if (!NativeMethods.IsWindow(_gameHwnd)) { _gameHwnd = IntPtr.Zero; _savedGameStyle = 0; return; }
                NativeMethods.SetWindowLong(_gameHwnd, NativeMethods.GWL_STYLE, _savedGameStyle);
                NativeMethods.SetWindowLong(_gameHwnd, NativeMethods.GWL_EXSTYLE, _savedGameExStyle);
                int x = _savedGameRect.Left;
                int y = _savedGameRect.Top;
                int w = _savedGameRect.Right - _savedGameRect.Left;
                int h = _savedGameRect.Bottom - _savedGameRect.Top;
                NativeMethods.SetWindowPos(_gameHwnd, IntPtr.Zero,
                    x, y, w, h,
                    NativeMethods.SWP_NOZORDER | NativeMethods.SWP_FRAMECHANGED | NativeMethods.SWP_SHOWWINDOW);
            }
            catch { /* game window may already be gone */ }
        }
        _gameHwnd = IntPtr.Zero;
        _savedGameStyle = 0;
        _savedGameExStyle = 0;
    }

    private void ToggleBorderless_Changed(object sender, RoutedEventArgs e)
    {
        bool enable = ToggleBorderless.IsChecked == true;

        // Signal hook DLL immediately (SDL2 path: TrySdl2Borderless reads this flag)
        _hookSharedMem?.SetBorderless(enable);

        if (enable)
        {
            // Win32 path: works for non-SDL2 games (D3D11, D3D9, etc.)
            ApplyBorderlessWithRetry();
        }
        else
        {
            RestoreGameWindow();
        }
    }

    /// <summary>
    /// Applies borderless windowed fullscreen and retries a few times with
    /// increasing delays — needed for games that recreate their window after startup
    /// or that override SetWindowPos during initialization.
    /// On each retry the HWND is re-validated and re-discovered if stale.
    /// </summary>
    private void ApplyBorderlessWithRetry(int retriesLeft = 5, int delayMs = 600)
    {
        ApplyBorderlessToMonitoredProcess();
        if (retriesLeft > 0)
        {
            Task.Delay(delayMs).ContinueWith(_ =>
                Dispatcher.BeginInvoke(() =>
                {
                    if (_monitoredProcessName == null || ToggleBorderless.IsChecked != true) return;

                    // If previous HWND became invalid, clear it so we re-discover
                    if (_gameHwnd != IntPtr.Zero && !NativeMethods.IsWindow(_gameHwnd))
                    {
                        InjLog("[BDL] HWND invalidated between retries — will re-discover");
                        _gameHwnd = IntPtr.Zero;
                        _savedGameStyle = 0;
                        _savedGameExStyle = 0;
                    }

                    ApplyBorderlessWithRetry(retriesLeft - 1, Math.Min(delayMs * 2, 3000));
                }));
        }
    }

    private void ApplyBorderlessToMonitoredProcess()
    {
        if (_monitoredProcessName == null) return;
        try
        {
            var procs = Process.GetProcessesByName(_monitoredProcessName);
            IntPtr bestHwnd = IntPtr.Zero;
            int bestArea = 0;

            foreach (var p in procs)
            {
                // Enumerate all visible windows for this PID and pick the largest
                // — protects against multi-window games (splash screens, tool windows, etc.)
                NativeMethods.EnumWindows((hwnd, _) =>
                {
                    if (!NativeMethods.IsWindowVisible(hwnd)) return true;
                    NativeMethods.GetWindowThreadProcessId(hwnd, out uint wpid);
                    if (wpid != (uint)p.Id) return true;
                    NativeMethods.GetWindowRect(hwnd, out var r);
                    int area = (r.Right - r.Left) * (r.Bottom - r.Top);
                    if (area > bestArea) { bestArea = area; bestHwnd = hwnd; }
                    return true;
                }, IntPtr.Zero);

                // Fallback: MainWindowHandle (some engines set this but not visible initially)
                if (bestHwnd == IntPtr.Zero) { p.Refresh(); bestHwnd = p.MainWindowHandle; }
                p.Dispose();
            }

            if (bestHwnd != IntPtr.Zero)
                ForceBorderless(bestHwnd, GetSelectedMonitorIndex());
        }
        catch { }
    }

    // ══════════════════════════════════════════════════════════════
    //  D3D11 HOOK INJECTION
    // ══════════════════════════════════════════════════════════════

    /// <summary>
    /// Injects S4W_Hook.dll into the game process and writes initial scanline settings
    /// to shared memory. The hook DLL reads these settings on every Present call.
    /// </summary>
    // Simple injection log for debugging hook issues
    private static void InjLog(string msg)
    {
        try
        {
            string debugDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "debug");
            Directory.CreateDirectory(debugDir);
            string path = Path.Combine(debugDir, "S4W_Injection_Log.txt");
            File.AppendAllText(path, $"[{DateTime.Now:HH:mm:ss.fff}] {msg}\n");
        }
        catch { /* never crash for logging */ }
    }

    private void InjectHook(int processId, ScanlineSettings settings)
    {
        if (_injectedPids.Contains(processId)) return;

        InjLog($"InjectHook called for PID={processId} process={_monitoredProcessName}");

        // 1. Create shared memory (before injection so the DLL can read it immediately)
        _hookSharedMem ??= new HookSharedMemory();
        if (!_hookSharedMem.Create())
        {
            InjLog("[S4W] InjectHook: shared memory creation FAILED");
            return; // will retry on next poll
        }

        // Write initial settings (respect Apply on/off state)
        var monitor = MonitorService.GetMonitor(settings.MonitorIndex);
        _hookSharedMem.UpdateSettings(settings, (int)monitor.Width, (int)monitor.Height);
        if (!_overlayActive)
            _hookSharedMem.Deactivate();
        InjLog($"[S4W] Shared mem: screen={monitor.Width}x{monitor.Height} hEnabled={settings.HorizontalEnabled} hWidth={settings.HWidth} overlayActive={_overlayActive}");

        // 2. Check process architecture and select correct DLL
        bool isTarget32bit = false;
        try
        {
            var proc = Process.GetProcessById(processId);
            if (NativeMethods.IsWow64Process(proc.Handle, out bool isWow64))
                isTarget32bit = isWow64;
            InjLog($"[S4W] Process PID={processId} arch={( isTarget32bit ? "x86 (32-bit)" : "x64 (64-bit)" )}");
        }
        catch (Exception ex)
        {
            InjLog($"[S4W] IsWow64 check failed: {ex.Message} — assuming x64");
        }

        // Select the correct DLL for the target architecture
        string exeDir = AppDomain.CurrentDomain.BaseDirectory;
        string dllName = isTarget32bit ? "S4W_Hook_x86.dll" : "S4W_Hook.dll";
        string hookDll = Path.Combine(exeDir, dllName);
        if (!File.Exists(hookDll))
        {
            // Try Hook subdirectory (development layout)
            hookDll = Path.Combine(exeDir, "Hook", dllName);
        }

        if (!File.Exists(hookDll))
        {
            InjLog($"[S4W] InjectHook: DLL NOT FOUND — {dllName} at {hookDll}");
            if (isTarget32bit)
                InjLog($"[S4W] NOTE: {dllName} must be compiled separately (x86 target)");
            return;
        }
        InjLog($"[S4W] Using hook DLL: {dllName}");

        // 4. Inject (use 32-bit helper for WOW64 targets)
        InjLog($"[S4W] Injecting DLL: {hookDll} into PID={processId} via {(isTarget32bit ? "x86 helper" : "direct")}");
        bool ok = DllInjector.Inject(processId, hookDll, out string? error, isTarget32bit);
        if (ok)
        {
            _injectedPids.Add(processId);
            _hookInjected = true;
            _hookedProcessId = processId;
            _hookedIs32bit = isTarget32bit;
            InjLog($"[S4W] InjectHook: SUCCESS for PID={processId} arch={( isTarget32bit ? "x86" : "x64" )}");
        }
        else
        {
            InjLog($"[S4W] InjectHook: FAILED for PID={processId} — {error}");
        }
    }

    /// <summary>
    /// Updates scanline settings in shared memory so the hook DLL picks them up.
    /// Called whenever the user changes settings in the GUI.
    /// </summary>
    private void UpdateHookSettings(ScanlineSettings settings)
    {
        if (_hookSharedMem == null) return;
        var monitor = MonitorService.GetMonitor(settings.MonitorIndex);
        _hookSharedMem.UpdateSettings(settings, (int)monitor.Width, (int)monitor.Height);
    }

    /// <summary>
    /// Cleans up hook resources when the game exits or S4W closes.
    /// </summary>
    private void CleanupHook()
    {
        _hookSharedMem?.Deactivate();
        _hookSharedMem?.Dispose();
        _hookSharedMem = null;
        _hookInjected = false;
        _injectedPids.Clear();
        _hookedProcessId = 0;
    }

    /// <summary>
    /// Terminates the monitored game/emulator process on S4W exit (Alt+F4 exits everything).
    /// </summary>
    private void TerminateGameProcess()
    {
        if (_monitoredProcessName == null) return;
        try
        {
            var procs = Process.GetProcessesByName(_monitoredProcessName);
            foreach (var p in procs)
            {
                try { p.Kill(); } catch { }
                p.Dispose();
            }
        }
        catch { }
    }

    // ══════════════════════════════════════════════════════════════
    //  MONITOR AUTO-DETECTION
    // ══════════════════════════════════════════════════════════════

    private void UpdateMonitorAvailability()
    {
        int count = MonitorService.Count;
        Monitor1.IsEnabled = count >= 1;
        Monitor2.IsEnabled = count >= 2;
        Monitor3.IsEnabled = count >= 3;
        Monitor4.IsEnabled = count >= 4;

        if (GetSelectedMonitorIndex() >= count && count > 0)
            Monitor1.IsChecked = true;
    }

    // ══════════════════════════════════════════════════════════════
    //  APP HISTORY COMBO
    // ══════════════════════════════════════════════════════════════

    /// <summary>Compute the display label for an exe path, disambiguating duplicates.</summary>
    private string ComputeAppDisplayName(string fullPath)
    {
        string name = Path.GetFileName(fullPath);
        bool hasDuplicate = _appHistory.Any(p =>
            !string.Equals(p, fullPath, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(Path.GetFileName(p), name, StringComparison.OrdinalIgnoreCase));

        if (hasDuplicate)
        {
            string folder = Path.GetFileName(Path.GetDirectoryName(fullPath) ?? "") ?? "";
            if (!string.IsNullOrEmpty(folder))
                return $"{name}  ({folder})";
        }
        return name;
    }

    private void RefreshAppComboBox()
    {
        string currentPath = GetSelectedAppPath();
        CmbAppPath.Items.Clear();

        // First item: no app selected
        CmbAppPath.Items.Add(new ComboBoxItem { Content = NoneText, Tag = "" });

        // Sort alphabetically by display name (exe filename)
        foreach (string path in _appHistory.OrderBy(p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase))
            CmbAppPath.Items.Add(new ComboBoxItem
            {
                Content = ComputeAppDisplayName(path),
                Tag     = path,
                ToolTip = path    // show full path on hover
            });

        SelectAppByPath(currentPath);
    }

    private string GetSelectedAppPath()
    {
        if (CmbAppPath.SelectedItem is ComboBoxItem item)
            return item.Tag as string ?? "";
        return "";
    }

    private void SelectAppByPath(string fullPath)
    {
        if (string.IsNullOrEmpty(fullPath))
        {
            CmbAppPath.SelectedIndex = 0;
            return;
        }

        // Search for matching item
        for (int i = 1; i < CmbAppPath.Items.Count; i++)
        {
            if (CmbAppPath.Items[i] is ComboBoxItem it &&
                string.Equals(it.Tag as string, fullPath, StringComparison.OrdinalIgnoreCase))
            {
                CmbAppPath.SelectedIndex = i;
                return;
            }
        }

        // Not found in the dropdown — DO NOT auto-add it. Previously we'd silently
        // re-add the path to the history, which meant that loading any profile
        // referencing a previously-deleted app path would resurrect that path.
        // Callers that legitimately want to add a new path must call
        // AppHistoryService.TryAdd themselves before calling SelectAppByPath.
        CmbAppPath.SelectedIndex = 0;
    }

    // ══════════════════════════════════════════════════════════════
    //  BUTTON HANDLERS
// ══════════════════════════════════════════════════════════════

private void BtnApply_Click(object sender, RoutedEventArgs e)
{
    ToggleOverlayVisibility();
    BtnApplyBtn.Tag = _overlayActive ? "active" : null;

    // Sync hook: deactivate = passthrough, activate = resend current settings
    if (_hookInjected)
    {
        if (_overlayActive)
            UpdateHookSettings(BuildSettingsFromUI());
        else
            _hookSharedMem?.Deactivate();
    }
}

private async void BtnSave_Click(object sender, RoutedEventArgs e)
{
    BtnSaveBtn.Tag = "flash";

    InjLog($"[SAVE] CmbSystems.SelectedItem='{CmbSystems.SelectedItem}' Text='{CmbSystems.Text}' _currentRomSystems=[{string.Join(", ", _currentRomSystems.Select(s => $"{s.Name}|{s.Folder}"))}]");

    // Import the dropdown-selected ROM system into the profile if not already linked
    LinkSelectedRomSystem();

    InjLog($"[SAVE] After LinkSelected: _currentRomSystems=[{string.Join(", ", _currentRomSystems.Select(s => $"{s.Name}|{s.Folder}"))}]");

    string name = GetCurrentProfileName();

    var settings = BuildSettingsFromUI();
    InjLog($"[SAVE] BuildSettingsFromUI RomSystems=[{string.Join(", ", settings.RomSystems.Select(s => $"{s.Name}|{s.Folder}"))}]");

    var profile = new ProfileData { ProfileName = name, Settings = settings };
    ProfileService.Save(profile);
    RefreshProfileAppPaths();
    LoadProfileList();
    SelectProfile(name);

    await Task.Delay(600);
    BtnSaveBtn.Tag = null;
}

private void BtnBezelSearch_Click(object sender, RoutedEventArgs e)
{
    var dlg = new OpenFileDialog
    {
        Filter = "Image files|*.png;*.jpg;*.bmp;*.gif|All files|*.*",
        Title = L("Select Bezel Image", "Sélectionner l'image bezel", "Bezel-Bild auswählen", "Seleziona immagine bezel", "Seleccionar imagen de bezel", "Selecionar imagem de bezel")
    };
    if (dlg.ShowDialog() == true)
    {
        _bezelFullPath = dlg.FileName;
        TxtBezelPath.Text = Path.GetFileName(dlg.FileName);
    }
}

    private void BtnAppSearch_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = "Executable files|*.exe|All files|*.*",
            Title = L("Select Application", "Sélectionner l'application", "Anwendung auswählen", "Seleziona applicazione", "Seleccionar aplicación", "Selecionar aplicativo")
        };
        if (dlg.ShowDialog() != true) return;

        string fullPath = dlg.FileName;
        bool added = AppHistoryService.TryAdd(_appHistory, fullPath);
        if (added) RefreshAppComboBox();
        SelectAppByPath(fullPath);
    }

    private void BtnAppDelete_Click(object sender, RoutedEventArgs e)
    {
        string currentPath = GetSelectedAppPath();
        if (string.IsNullOrEmpty(currentPath)) return;
        _appHistory.RemoveAll(p => string.Equals(p, currentPath, StringComparison.OrdinalIgnoreCase));
        AppHistoryService.Save(_appHistory);
        // Clear selection BEFORE RefreshAppComboBox — otherwise RefreshAppComboBox
        // calls SelectAppByPath(currentPath) which would re-add the deleted path.
        CmbAppPath.SelectedIndex = 0;
        RefreshAppComboBox();
    }

    // ── Process Picker (Borderless Gaming style) ─────────────────────

    private void BtnPickProcess_Click(object sender, RoutedEventArgs e)
    {
        if (ProcessPickerPanel.Visibility == Visibility.Visible)
        {
            ProcessPickerPanel.Visibility = Visibility.Collapsed;
            return;
        }
        RefreshRunningProcesses();
        ProcessPickerPanel.Visibility = Visibility.Visible;
    }

    private void BtnRefreshProcesses_Click(object sender, RoutedEventArgs e)
    {
        RefreshRunningProcesses();
    }

    private void RefreshRunningProcesses()
    {
        LstRunningProcesses.Items.Clear();

        // Collect all windowed processes (like Borderless Gaming does)
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var entries = new List<(string display, string fullPath)>();

        foreach (var proc in Process.GetProcesses())
        {
            try
            {
                // Skip processes without a main window
                if (proc.MainWindowHandle == IntPtr.Zero) continue;
                if (string.IsNullOrEmpty(proc.MainWindowTitle)) continue;

                string? exePath = null;
                try { exePath = proc.MainModule?.FileName; } catch { }

                // Fallback for elevated / x86 processes where MainModule throws:
                // QueryFullProcessImageName works with PROCESS_QUERY_LIMITED_INFORMATION
                // which succeeds even when we can't open the process for full access.
                if (string.IsNullOrEmpty(exePath))
                {
                    var hProc = NativeMethods.OpenProcess(
                        NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION, false, proc.Id);
                    if (hProc != IntPtr.Zero)
                    {
                        try
                        {
                            var buf = new char[1024];
                            uint len = (uint)buf.Length;
                            if (NativeMethods.QueryFullProcessImageName(hProc, 0, buf, ref len))
                                exePath = new string(buf, 0, (int)len);
                        }
                        finally { NativeMethods.CloseHandle(hProc); }
                    }
                }

                string processName = proc.ProcessName;
                string key = exePath ?? processName;
                if (!seen.Add(key)) continue;

                // Skip our own process
                if (proc.Id == Environment.ProcessId) continue;

                // Detect architecture
                string arch = "";
                try
                {
                    if (NativeMethods.IsWow64Process(proc.Handle, out bool wow64) && wow64)
                        arch = " [x86]";
                    else
                        arch = " [x64]";
                }
                catch { }

                string title = proc.MainWindowTitle;
                if (title.Length > 50) title = title[..47] + "...";
                string display = $"{processName}{arch}  :  {title}";
                entries.Add((display, exePath ?? ""));
            }
            catch { }
            finally { proc.Dispose(); }
        }

        // Sort alphabetically
        entries.Sort((a, b) => string.Compare(a.display, b.display, StringComparison.OrdinalIgnoreCase));

        foreach (var (display, fullPath) in entries)
        {
            LstRunningProcesses.Items.Add(new ListBoxItem
            {
                Content = display,
                Tag = fullPath,
                Foreground = new SolidColorBrush(Color.FromRgb(0x9C, 0xA3, 0xAF)),
                FontSize = 11,
            });
        }
    }

    // Force immediate selection on first click regardless of ListBox focus state
    private void LstProcessItem_PreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is ListBoxItem item)
            item.IsSelected = true;
    }

    private void LstRunningProcesses_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LstRunningProcesses.SelectedItem is not ListBoxItem item)
        {
            LblPickedProcess.Text = "Select a process above";
            LblPickedProcess.Foreground = new SolidColorBrush(Color.FromRgb(0x4B, 0x55, 0x63));
            BtnAddProcess.IsEnabled = false;
            return;
        }
        string fullPath = item.Tag as string ?? "";
        string display = item.Content as string ?? "";
        LblPickedProcess.Text = display;
        LblPickedProcess.Foreground = new SolidColorBrush(Color.FromRgb(0x9C, 0xA3, 0xAF));
        BtnAddProcess.IsEnabled = !string.IsNullOrEmpty(fullPath);
    }

    private void BtnAddProcess_Click(object sender, RoutedEventArgs e)
    {
        if (LstRunningProcesses.SelectedItem is not ListBoxItem item) return;
        string fullPath = item.Tag as string ?? "";
        if (string.IsNullOrEmpty(fullPath)) return;

        bool added = AppHistoryService.TryAdd(_appHistory, fullPath);
        if (added) RefreshAppComboBox();
        SelectAppByPath(fullPath);
        ProcessPickerPanel.Visibility = Visibility.Collapsed;
    }

private void BtnDeleteProfile_Click(object sender, RoutedEventArgs e)
{
    string name = GetCurrentProfileName();
    if (name == "Desktop") return;
    ProfileService.Delete(name);
    RefreshProfileAppPaths();
    LoadProfileList();
}

private void BtnResetProfile_Click(object sender, RoutedEventArgs e)
{
    string msg = L("Reset all settings for this profile to defaults? Unsaved changes will be lost.",
        "Réinitialiser tous les paramètres de ce profil aux valeurs par défaut ? Les modifications non sauvegardées seront perdues.",
        "Alle Einstellungen dieses Profils auf Standard zurücksetzen? Nicht gespeicherte Änderungen gehen verloren.",
        "Ripristinare tutte le impostazioni di questo profilo ai valori predefiniti? Le modifiche non salvate andranno perse.",
        "¿Restablecer todos los ajustes de este perfil a los valores predeterminados? Los cambios no guardados se perderán.",
        "Redefinir todas as configurações deste perfil para os padrões? As alterações não salvas serão perdidas.");
    if (MessageBox.Show(msg, "S4W", MessageBoxButton.YesNo, MessageBoxImage.Question)
        != MessageBoxResult.Yes) return;

    // Clear all hotkey state first so RegisterXxx unregisters cleanly
    _switchBezelMods = ModifierKeys.None;       _switchBezelKey = Key.None;
    _switchBezelBackMods = ModifierKeys.None;   _switchBezelBackKey = Key.None;
    _switchProfileMods = ModifierKeys.None;     _switchProfileKey = Key.None;
    _switchProfileBackMods = ModifierKeys.None; _switchProfileBackKey = Key.None;
    _blurUpMods = ModifierKeys.None;            _blurUpKey = Key.None;
    _blurDownMods = ModifierKeys.None;          _blurDownKey = Key.None;
    _bloomUpMods = ModifierKeys.None;           _bloomUpKey = Key.None;
    _bloomDownMods = ModifierKeys.None;         _bloomDownKey = Key.None;
    _curvatureUpMods = ModifierKeys.None;       _curvatureUpKey = Key.None;
    _curvatureDownMods = ModifierKeys.None;     _curvatureDownKey = Key.None;
    _flickerUpMods = ModifierKeys.None;         _flickerUpKey = Key.None;
    _flickerDownMods = ModifierKeys.None;       _flickerDownKey = Key.None;
    _phosphorUpMods = ModifierKeys.None;        _phosphorUpKey = Key.None;
    _phosphorDownMods = ModifierKeys.None;      _phosphorDownKey = Key.None;
    _hOpacityUpMods = ModifierKeys.None;        _hOpacityUpKey = Key.None;
    _hOpacityDownMods = ModifierKeys.None;      _hOpacityDownKey = Key.None;
    _vOpacityUpMods = ModifierKeys.None;        _vOpacityUpKey = Key.None;
    _vOpacityDownMods = ModifierKeys.None;      _vOpacityDownKey = Key.None;
    _vhsUpMods = ModifierKeys.None;             _vhsUpKey = Key.None;
    _vhsDownMods = ModifierKeys.None;           _vhsDownKey = Key.None;
    _grainUpMods = ModifierKeys.None;           _grainUpKey = Key.None;
    _grainDownMods = ModifierKeys.None;         _grainDownKey = Key.None;
    _tapeNoiseUpMods = ModifierKeys.None;       _tapeNoiseUpKey = Key.None;
    _tapeNoiseDownMods = ModifierKeys.None;     _tapeNoiseDownKey = Key.None;
    RegisterSwitchBezelHotkey();
    RegisterSwitchBezelBackHotkey();
    RegisterSwitchProfileHotkey();
    RegisterSwitchProfileBackHotkey();
    RegisterBlurUpHotkey();     RegisterBlurDownHotkey();
    RegisterBloomUpHotkey();    RegisterBloomDownHotkey();
    RegisterCurvatureUpHotkey(); RegisterCurvatureDownHotkey();
    RegisterFlickerUpHotkey();  RegisterFlickerDownHotkey();
    RegisterPhosphorUpHotkey();    RegisterPhosphorDownHotkey();
    RegisterHOpacityUpHotkey();    RegisterHOpacityDownHotkey();
    RegisterVOpacityUpHotkey();    RegisterVOpacityDownHotkey();
    RegisterVhsUpHotkey();          RegisterVhsDownHotkey();
    RegisterGrainUpHotkey();        RegisterGrainDownHotkey();
    RegisterTapeNoiseUpHotkey();    RegisterTapeNoiseDownHotkey();

    ApplySettingsToUI(new ScanlineSettings());
}

private void BtnBezelReset_Click(object sender, RoutedEventArgs e)
{
    _bezelFullPath              = "";
    TxtBezelPath.Text           = NoneText;
    SliderBezelOpacity.Value    = 0;
    ToggleBezelEnabled.IsChecked = false;
    BezelContentPanel.IsEnabled  = false;
    BezelContentPanel.Opacity    = 0.35;
}

private void BtnBezelPrev_Click(object sender, RoutedEventArgs e) => CycleBezelImageBack();
private void BtnBezelNext_Click(object sender, RoutedEventArgs e) => CycleBezelImage();


    // ══════════════════════════════════════════════════════════════
    //  EXPORT / IMPORT ALL SETTINGS
    // ══════════════════════════════════════════════════════════════

    private void BtnExport_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Filter   = "S4W Backup|*.s4w",
            Title    = L("Export Settings", "Exporter les paramètres", "Einstellungen exportieren", "Esporta impostazioni", "Exportar ajustes", "Exportar configurações"),
            FileName = "S4W_Backup"
        };
        if (dlg.ShowDialog() != true) return;

        var profiles = ProfileService.ListProfiles()
            .Select(n => ProfileService.Load(n))
            .Where(p => p != null)
            .Select(p => p!)
            .ToList();

        var backup = new Models.BackupData
        {
            Version    = "1.4",
            ExportDate = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"),
            Language   = L("English", "Français", "Deutsch", "Italiano", "Español", "Português"),
            Profiles   = profiles,
            AppHistory = [.. _appHistory],
            CrtPresets = [.. _crtPresets]
        };

        var opts = new System.Text.Json.JsonSerializerOptions
            { WriteIndented = true, PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.CamelCase };
        File.WriteAllText(dlg.FileName, System.Text.Json.JsonSerializer.Serialize(backup, opts));

        MessageBox.Show(
            L("Settings exported successfully.", "Paramètres exportés avec succès.", "Einstellungen erfolgreich exportiert.", "Impostazioni esportate con successo.", "Ajustes exportados con éxito.", "Configurações exportadas com sucesso."),
            "S4W", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private void BtnImport_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "S4W Backup|*.s4w|JSON Files|*.json",
            Title  = L("Import Settings", "Importer les paramètres", "Einstellungen importieren", "Importa impostazioni", "Importar ajustes", "Importar configurações")
        };
        if (dlg.ShowDialog() != true) return;

        try
        {
            var json   = File.ReadAllText(dlg.FileName);
            var opts   = new System.Text.Json.JsonSerializerOptions
                { PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.CamelCase };
            var backup = System.Text.Json.JsonSerializer.Deserialize<Models.BackupData>(json, opts);
            if (backup == null) throw new InvalidDataException("Invalid backup file.");

            foreach (var profile in backup.Profiles)
                ProfileService.Save(profile);

            // Restore app history
            if (backup.AppHistory?.Count > 0)
            {
                foreach (var path in backup.AppHistory)
                    AppHistoryService.TryAdd(_appHistory, path);
                RefreshAppComboBox();
            }

            // Restore language setting
            var langMap = new Dictionary<string, int>
            {
                ["English"] = 0, ["Français"] = 1, ["Deutsch"] = 2,
                ["Italiano"] = 3, ["Español"] = 4, ["Português"] = 5
            };
            CmbLanguage.SelectedIndex = langMap.TryGetValue(backup.Language ?? "", out int li) ? li : 0;

            // Restore CRT presets (if present — older backups may not have them)
            if (backup.CrtPresets?.Count > 0)
            {
                S4W.Services.CrtPresetService.Save(backup.CrtPresets);
                _crtPresets = backup.CrtPresets;
                // Refresh the preset name shown in the UI
                if (_crtPresets.Count > 0)
                    TxtCrtPresetName.Text = _crtPresets[0].Name;
            }

            RefreshProfileAppPaths();
            LoadProfileList();

            MessageBox.Show(
                L("Settings imported successfully.", "Paramètres importés avec succès.", "Einstellungen erfolgreich importiert.", "Impostazioni importate con successo.", "Ajustes importados con éxito.", "Configurações importadas com sucesso."),
                "S4W", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch
        {
            MessageBox.Show(
                L("Failed to import settings. The file may be invalid or corrupted.",
                  "Échec de l'importation. Le fichier est invalide ou corrompu.",
                  "Import fehlgeschlagen. Die Datei ist ungültig oder beschädigt.",
                  "Importazione fallita. Il file potrebbe essere non valido o corrotto.",
                  "Error al importar. El archivo puede ser inválido o estar corrupto.",
                  "Falha ao importar. O arquivo pode ser inválido ou corrompido."),
                "S4W", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  PROFILE MANAGEMENT
    // ══════════════════════════════════════════════════════════════

    private void LoadProfileList()
    {
        var profiles = ProfileService.ListProfiles();

        // Strict rule: "Desktop" must always be present — insert at position 0 if missing
        if (!profiles.Any(p => string.Equals(p, "Desktop", StringComparison.OrdinalIgnoreCase)))
            profiles.Insert(0, "Desktop");

        // Preserve current selection so we don't trigger spurious profile loads
        string? currentName = CmbProfile.SelectedItem is ComboBoxItem sel
            ? sel.Content?.ToString() : null;

        CmbProfile.Items.Clear();
        foreach (var name in profiles)
            CmbProfile.Items.Add(new ComboBoxItem { Content = name });

        // Restore previous selection; fall back to index 0 only on first load
        int restore = 0;
        if (currentName != null)
        {
            for (int i = 0; i < CmbProfile.Items.Count; i++)
            {
                if (CmbProfile.Items[i] is ComboBoxItem item &&
                    item.Content?.ToString() == currentName)
                { restore = i; break; }
            }
        }
        if (CmbProfile.Items.Count > 0)
            CmbProfile.SelectedIndex = restore;
    }

    private string GetCurrentProfileName()
    {
        string? typed = CmbProfile.Text;
        if (!string.IsNullOrWhiteSpace(typed))
            return typed;
        if (CmbProfile.SelectedItem is ComboBoxItem item)
            return item.Content?.ToString() ?? "Desktop";
        return "Desktop";
    }

    private void SelectProfile(string name)
    {
        for (int i = 0; i < CmbProfile.Items.Count; i++)
        {
            if (CmbProfile.Items[i] is ComboBoxItem item &&
                item.Content?.ToString() == name)
            {
                CmbProfile.SelectedIndex = i;
                return;
            }
        }
    }

    private void BtnProfilePrev_Click(object sender, RoutedEventArgs e)
    {
        if (CmbProfile.Items.Count == 0) return;
        int current = CmbProfile.SelectedIndex < 0 ? 0 : CmbProfile.SelectedIndex;
        CmbProfile.SelectedIndex = (current - 1 + CmbProfile.Items.Count) % CmbProfile.Items.Count;
    }

    private void BtnProfileNext_Click(object sender, RoutedEventArgs e)
    {
        if (CmbProfile.Items.Count == 0) return;
        int current = CmbProfile.SelectedIndex < 0 ? -1 : CmbProfile.SelectedIndex;
        CmbProfile.SelectedIndex = (current + 1) % CmbProfile.Items.Count;
    }

    private void CmbProfile_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_isLoaded) return;
        if (CmbProfile.SelectedItem is ComboBoxItem item)
        {
            string name = item.Content?.ToString() ?? "Desktop";
            ApplyProfileWithTransition(() =>
            {
                LoadProfile(name);
                if (_overlayActive) ApplyOverlay();
            });
        }
    }

    private void LoadProfile(string name)
    {
        bool isDesktop = string.Equals(name, "Desktop", StringComparison.OrdinalIgnoreCase);
        var profile = ProfileService.Load(name);
        if (profile == null)
        {
            // Desktop profile file missing — recreate it with defaults rather than silently skip
            if (isDesktop)
            {
                profile = new ProfileData { ProfileName = "Desktop" };
                ProfileService.Save(profile);
            }
            else
            {
                UpdateCrtTabAvailability(isDesktopOverride: false);
                return;
            }
        }
        ApplySettingsToUI(profile.Settings);
        UpdateCrtTabAvailability(isDesktop);
    }

    /// <summary>Apply all settings from <paramref name="s"/> to the UI controls.</summary>
    private void ApplySettingsToUI(ScanlineSettings s)
    {
        _applyingSettings = true;
        _bezelFullPath           = s.BezelPath ?? "";
        TxtBezelPath.Text        = string.IsNullOrEmpty(s.BezelPath) ? NoneText : Path.GetFileName(s.BezelPath);
        SliderBezelOpacity.Value = s.BezelOpacity;
        ToggleBezelEnabled.IsChecked = s.BezelEnabled;
        BezelContentPanel.IsEnabled  = s.BezelEnabled;
        BezelContentPanel.Opacity    = s.BezelEnabled ? 1.0 : 0.35;

        ToggleHorizontal.IsChecked = s.HorizontalEnabled;
        TxtHGap.Text       = InputParser.FormatPixelValue(s.HGap);
        TxtHThickness.Text = InputParser.FormatPixelValue(s.HThickness);
        SliderHOpacity.Value = s.HOpacity;

        ToggleVertical.IsChecked = s.VerticalEnabled;
        TxtVGap.Text       = InputParser.FormatPixelValue(s.VGap);
        SetHookModeUI(false);
        TxtVThickness.Text = InputParser.FormatPixelValue(s.VThickness);

        SliderVOpacity.Value = s.VOpacity;

        // CRT Effects (Tab 3)
        ToggleCrtGroup.IsChecked  = s.CrtGroupEnabled;
        ToggleBlur.IsChecked      = s.BlurEnabled;
        SliderBlur.Value          = s.BlurIntensity;
        ToggleBloom.IsChecked     = s.BloomEnabled;
        SliderBloom.Value         = s.BloomIntensity;
        ToggleCurvature.IsChecked = s.CurvatureEnabled;
        ToggleVignette.IsChecked  = s.VignetteEnabled;
        SliderCurvature.Value     = s.CurvatureIntensity;
        ToggleFlicker.IsChecked   = s.FlickerEnabled;
        SliderFlicker.Value       = s.FlickerIntensity;
        SliderFlickerRate.Value   = s.FlickerRate;
        TogglePhosphor.IsChecked  = s.PhosphorEnabled;
        SliderPhosphor.Value      = s.PhosphorIntensity;
        ToggleLuma.IsChecked      = s.LumaEnabled;
        SliderBrightness.Value    = s.BrightnessValue;
        SliderContrast.Value      = s.ContrastValue;
        SliderSaturation.Value    = s.SaturationValue;
        SliderTemperature.Value   = s.TemperatureValue;
        SliderBlackLevel.Value    = BlackLevelToSlider(s.BlackLevelValue);
        SliderGamma.Value         = GammaToSlider(s.GammaValue);
        ToggleVhs.IsChecked           = s.VhsEnabled || s.GrainEnabled || s.TapeNoiseEnabled;
        ToggleVhsEffect.IsChecked     = s.VhsEnabled;
        SliderVhs.Value               = s.VhsIntensity;
        ToggleGrain.IsChecked         = s.GrainEnabled;
        SliderGrain.Value             = s.GrainIntensity;
        ToggleTapeNoise.IsChecked     = s.TapeNoiseEnabled;
        SliderTapeNoise.Value         = s.TapeNoiseIntensity;

        // Restore the active preset name; also sync _crtPresetIndex if the preset is found
        if (!string.IsNullOrEmpty(s.ActivePresetName))
        {
            TxtCrtPresetName.Text = s.ActivePresetName;
            int idx = _crtPresets.FindIndex(p =>
                string.Equals(p.Name, s.ActivePresetName, StringComparison.OrdinalIgnoreCase));
            if (idx >= 0) _crtPresetIndex = idx;
        }

        // Gray/unlock sections that are irrelevant for the Desktop profile
        UpdateDesktopProfileLock();

        ToggleStartMinimized.IsChecked      = s.StartMinimized;
        ToggleCloseToTray.IsChecked         = s.CloseToTray;
        ToggleApplyDesktopAtStart.IsChecked = s.ApplyDesktopAtStart;
        SetMonitorRadioButton(s.MonitorIndex);
        SelectAppByPath(s.ApplicationPath);
        ToggleBorderless.IsChecked  = s.BorderlessEnabled;

        // ROM Systems — honour EXACTLY the saved toggle state. Do NOT auto-enable
        // based on _currentRomSystems.Count, otherwise a profile that still has
        // orphan systems in its list (e.g. from a previous silent import) will
        // force-enable the toggle every time it is loaded.
        _currentRomSystems = s.RomSystems
            .Select(r => new RomSystem { Name = r.Name, Folder = r.Folder, RomNames = [.. r.RomNames] })
            .ToList();
        ToggleMultiSystemEnabled.IsChecked = s.MultiSystemEnabled;
        UpdateMultiSystemContent();
        RefreshSystemsComboBox(_currentRomSystems.Count > 0 ? _currentRomSystems[0].Name : null);

        // Shortcuts are global (not per-profile) — loaded once at startup by LoadShortcuts().
        // Switching profiles must NOT overwrite the current hotkey bindings.
        UpdateNoneColors();
        _applyingSettings = false;
    }

    private void RestoreHotkey(string keyStr, string modStr,
        Action<ModifierKeys, Key> apply, Button btn,
        string prefix = "", string suffix = "")
    {
        if (Enum.TryParse<Key>(keyStr, out var key) && key != Key.None)
        {
            Enum.TryParse<ModifierKeys>(modStr, out var mods);
            apply(mods, key);
            btn.Content = prefix + FormatHotkeyDisplay(mods, key) + suffix;
        }
        else
        {
            btn.Content = prefix + NoneText + suffix;
        }
    }

    // ── Global shortcuts (load once at startup, auto-save after every bind) ──

    /// <summary>Load all hotkeys from the global shortcuts file and register them.</summary>
    private void LoadShortcuts()
    {
        var s = ShortcutsService.Load();

        RestoreHotkey(s.SwitchBezelKey, s.SwitchBezelModifiers,
            (m, k) => { _switchBezelMods = m; _switchBezelKey = k; RegisterSwitchBezelHotkey(); },
            BtnSwitchBezelHotkey, suffix: " \u25BA");
        RestoreHotkey(s.SwitchBezelBackKey, s.SwitchBezelBackModifiers,
            (m, k) => { _switchBezelBackMods = m; _switchBezelBackKey = k; RegisterSwitchBezelBackHotkey(); },
            BtnSwitchBezelBackHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.SwitchProfileKey, s.SwitchProfileModifiers,
            (m, k) => { _switchProfileMods = m; _switchProfileKey = k; RegisterSwitchProfileHotkey(); },
            BtnSwitchProfileHotkey, suffix: " \u25BA");
        RestoreHotkey(s.SwitchProfileBackKey, s.SwitchProfileBackModifiers,
            (m, k) => { _switchProfileBackMods = m; _switchProfileBackKey = k; RegisterSwitchProfileBackHotkey(); },
            BtnSwitchProfileBackHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.BlurUpKey, s.BlurUpModifiers,
            (m, k) => { _blurUpMods = m; _blurUpKey = k; RegisterBlurUpHotkey(); },
            BtnBlurUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.BlurDownKey, s.BlurDownModifiers,
            (m, k) => { _blurDownMods = m; _blurDownKey = k; RegisterBlurDownHotkey(); },
            BtnBlurDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.BloomUpKey, s.BloomUpModifiers,
            (m, k) => { _bloomUpMods = m; _bloomUpKey = k; RegisterBloomUpHotkey(); },
            BtnBloomUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.BloomDownKey, s.BloomDownModifiers,
            (m, k) => { _bloomDownMods = m; _bloomDownKey = k; RegisterBloomDownHotkey(); },
            BtnBloomDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.CurvatureUpKey, s.CurvatureUpModifiers,
            (m, k) => { _curvatureUpMods = m; _curvatureUpKey = k; RegisterCurvatureUpHotkey(); },
            BtnCurvatureUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.CurvatureDownKey, s.CurvatureDownModifiers,
            (m, k) => { _curvatureDownMods = m; _curvatureDownKey = k; RegisterCurvatureDownHotkey(); },
            BtnCurvatureDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.FlickerUpKey, s.FlickerUpModifiers,
            (m, k) => { _flickerUpMods = m; _flickerUpKey = k; RegisterFlickerUpHotkey(); },
            BtnFlickerUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.FlickerDownKey, s.FlickerDownModifiers,
            (m, k) => { _flickerDownMods = m; _flickerDownKey = k; RegisterFlickerDownHotkey(); },
            BtnFlickerDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.PhosphorUpKey, s.PhosphorUpModifiers,
            (m, k) => { _phosphorUpMods = m; _phosphorUpKey = k; RegisterPhosphorUpHotkey(); },
            BtnPhosphorUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.PhosphorDownKey, s.PhosphorDownModifiers,
            (m, k) => { _phosphorDownMods = m; _phosphorDownKey = k; RegisterPhosphorDownHotkey(); },
            BtnPhosphorDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.HOpacityUpKey, s.HOpacityUpModifiers,
            (m, k) => { _hOpacityUpMods = m; _hOpacityUpKey = k; RegisterHOpacityUpHotkey(); },
            BtnHOpacityUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.HOpacityDownKey, s.HOpacityDownModifiers,
            (m, k) => { _hOpacityDownMods = m; _hOpacityDownKey = k; RegisterHOpacityDownHotkey(); },
            BtnHOpacityDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.VOpacityUpKey, s.VOpacityUpModifiers,
            (m, k) => { _vOpacityUpMods = m; _vOpacityUpKey = k; RegisterVOpacityUpHotkey(); },
            BtnVOpacityUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.VOpacityDownKey, s.VOpacityDownModifiers,
            (m, k) => { _vOpacityDownMods = m; _vOpacityDownKey = k; RegisterVOpacityDownHotkey(); },
            BtnVOpacityDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.VhsUpKey, s.VhsUpModifiers,
            (m, k) => { _vhsUpMods = m; _vhsUpKey = k; RegisterVhsUpHotkey(); },
            BtnVhsUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.VhsDownKey, s.VhsDownModifiers,
            (m, k) => { _vhsDownMods = m; _vhsDownKey = k; RegisterVhsDownHotkey(); },
            BtnVhsDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.GrainUpKey, s.GrainUpModifiers,
            (m, k) => { _grainUpMods = m; _grainUpKey = k; RegisterGrainUpHotkey(); },
            BtnGrainUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.GrainDownKey, s.GrainDownModifiers,
            (m, k) => { _grainDownMods = m; _grainDownKey = k; RegisterGrainDownHotkey(); },
            BtnGrainDownHotkey, prefix: "\u25C4 ");

        RestoreHotkey(s.TapeNoiseUpKey, s.TapeNoiseUpModifiers,
            (m, k) => { _tapeNoiseUpMods = m; _tapeNoiseUpKey = k; RegisterTapeNoiseUpHotkey(); },
            BtnTapeNoiseUpHotkey, suffix: " \u25BA");
        RestoreHotkey(s.TapeNoiseDownKey, s.TapeNoiseDownModifiers,
            (m, k) => { _tapeNoiseDownMods = m; _tapeNoiseDownKey = k; RegisterTapeNoiseDownHotkey(); },
            BtnTapeNoiseDownHotkey, prefix: "\u25C4 ");

        UpdateNoneColors();
    }

    /// <summary>Snapshot current hotkey state and persist to the global shortcuts file.</summary>
    private void SaveShortcuts()
    {
        ShortcutsService.Save(new S4W.Models.ShortcutsData
        {
            SwitchBezelModifiers     = _switchBezelMods.ToString(),
            SwitchBezelKey           = _switchBezelKey.ToString(),
            SwitchBezelBackModifiers = _switchBezelBackMods.ToString(),
            SwitchBezelBackKey       = _switchBezelBackKey.ToString(),
            SwitchProfileModifiers     = _switchProfileMods.ToString(),
            SwitchProfileKey           = _switchProfileKey.ToString(),
            SwitchProfileBackModifiers = _switchProfileBackMods.ToString(),
            SwitchProfileBackKey       = _switchProfileBackKey.ToString(),
            BlurUpModifiers   = _blurUpMods.ToString(),   BlurUpKey   = _blurUpKey.ToString(),
            BlurDownModifiers = _blurDownMods.ToString(), BlurDownKey = _blurDownKey.ToString(),
            BloomUpModifiers   = _bloomUpMods.ToString(),   BloomUpKey   = _bloomUpKey.ToString(),
            BloomDownModifiers = _bloomDownMods.ToString(), BloomDownKey = _bloomDownKey.ToString(),
            CurvatureUpModifiers   = _curvatureUpMods.ToString(),   CurvatureUpKey   = _curvatureUpKey.ToString(),
            CurvatureDownModifiers = _curvatureDownMods.ToString(), CurvatureDownKey = _curvatureDownKey.ToString(),
            FlickerUpModifiers   = _flickerUpMods.ToString(),   FlickerUpKey   = _flickerUpKey.ToString(),
            FlickerDownModifiers = _flickerDownMods.ToString(), FlickerDownKey = _flickerDownKey.ToString(),
            PhosphorUpModifiers   = _phosphorUpMods.ToString(),   PhosphorUpKey   = _phosphorUpKey.ToString(),
            PhosphorDownModifiers = _phosphorDownMods.ToString(), PhosphorDownKey = _phosphorDownKey.ToString(),
            HOpacityUpModifiers   = _hOpacityUpMods.ToString(),   HOpacityUpKey   = _hOpacityUpKey.ToString(),
            HOpacityDownModifiers = _hOpacityDownMods.ToString(), HOpacityDownKey = _hOpacityDownKey.ToString(),
            VOpacityUpModifiers   = _vOpacityUpMods.ToString(),   VOpacityUpKey   = _vOpacityUpKey.ToString(),
            VOpacityDownModifiers = _vOpacityDownMods.ToString(), VOpacityDownKey = _vOpacityDownKey.ToString(),
            VhsUpModifiers   = _vhsUpMods.ToString(),   VhsUpKey   = _vhsUpKey.ToString(),
            VhsDownModifiers = _vhsDownMods.ToString(), VhsDownKey = _vhsDownKey.ToString(),
            GrainUpModifiers   = _grainUpMods.ToString(),   GrainUpKey   = _grainUpKey.ToString(),
            GrainDownModifiers = _grainDownMods.ToString(), GrainDownKey = _grainDownKey.ToString(),
            TapeNoiseUpModifiers   = _tapeNoiseUpMods.ToString(),   TapeNoiseUpKey   = _tapeNoiseUpKey.ToString(),
            TapeNoiseDownModifiers = _tapeNoiseDownMods.ToString(), TapeNoiseDownKey = _tapeNoiseDownKey.ToString(),
        });
    }

    private void SetMonitorRadioButton(int index)
    {
        Monitor1.IsChecked = index == 0;
        Monitor2.IsChecked = index == 1;
        Monitor3.IsChecked = index == 2;
        Monitor4.IsChecked = index == 3;
    }

// ══════════════════════════════════════════════════════════════
//  GENERIC HOTKEY CAPTURE
// ══════════════════════════════════════════════════════════════

private void StartHotkeyCapture(Button btn, Action<ModifierKeys, Key> onCaptured,
                                Action? onCancel = null)
{
    // Cancel any currently active capture (e.g. user clicked a second button)
    CancelHotkeyCapture();

    _capturingButton = btn;
    _capturingButtonOriginalContent = btn.Content as string;
    btn.Content = "Press key...";
    _pendingHotkeyCallback = onCaptured;
    _pendingHotkeyCancelCallback = onCancel;
    PreviewKeyDown += GenericCaptureHotkey;
    // Window uses MA_NOACTIVATE (to not steal focus from games).
    // For hotkey capture we must activate it so PreviewKeyDown fires.
    this.Activate();
    btn.Focus();

    // 5-second timeout: restore previous binding if user does nothing
    var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
    timer.Tick += (_, _) =>
    {
        timer.Stop();
        if (_pendingHotkeyCallback != null)
            CancelHotkeyCapture();
    };
    timer.Start();
}

/// <summary>Abort an ongoing capture and restore the previous hotkey.</summary>
private void CancelHotkeyCapture()
{
    if (_pendingHotkeyCallback == null) return;
    PreviewKeyDown -= GenericCaptureHotkey;
    _pendingHotkeyCallback = null;
    if (_capturingButton != null)
        _capturingButton.Content = _capturingButtonOriginalContent;
    _capturingButton = null;
    var restore = _pendingHotkeyCancelCallback;
    _pendingHotkeyCancelCallback = null;
    restore?.Invoke(); // re-register the old hotkey
}

private void GenericCaptureHotkey(object sender, KeyEventArgs e)
{
    // Ignore spurious non-keyboard events (mouse-generated Key.None, IME, etc.)
    if (e.Key == Key.None || e.Key == Key.ImeProcessed || e.Key == Key.DeadCharProcessed) return;

    if (e.Key == Key.Delete)
    {
        PreviewKeyDown -= GenericCaptureHotkey;
        _pendingHotkeyCallback = null;

        if (_capturingButton == BtnSwitchBezelHotkey)
        {
            BtnSwitchBezelHotkey.Content = NoneText + " \u25BA";
            _switchBezelMods = ModifierKeys.None;
            _switchBezelKey = Key.None;
            _switchBezelHotkeyService?.Dispose();
            _switchBezelHotkeyService = null;
        }
        else if (_capturingButton == BtnSwitchBezelBackHotkey)
        {
            BtnSwitchBezelBackHotkey.Content = "\u25C4 " + NoneText;
            _switchBezelBackMods = ModifierKeys.None;
            _switchBezelBackKey = Key.None;
            _switchBezelBackHotkeyService?.Dispose();
            _switchBezelBackHotkeyService = null;
        }
        else if (_capturingButton == BtnSwitchProfileHotkey)
        {
            BtnSwitchProfileHotkey.Content = NoneText + " \u25BA";
            _switchProfileMods = ModifierKeys.None;
            _switchProfileKey = Key.None;
            _switchProfileHotkeyService?.Dispose();
            _switchProfileHotkeyService = null;
        }
        else if (_capturingButton == BtnSwitchProfileBackHotkey)
        {
            BtnSwitchProfileBackHotkey.Content = "\u25C4 " + NoneText;
            _switchProfileBackMods = ModifierKeys.None;
            _switchProfileBackKey = Key.None;
            _switchProfileBackHotkeyService?.Dispose();
            _switchProfileBackHotkeyService = null;
        }
        else if (_capturingButton == BtnBlurUpHotkey)
        {
            BtnBlurUpHotkey.Content = NoneText + " \u25BA";
            _blurUpMods = ModifierKeys.None; _blurUpKey = Key.None;
            _blurUpHotkeyService?.Dispose(); _blurUpHotkeyService = null;
        }
        else if (_capturingButton == BtnBlurDownHotkey)
        {
            BtnBlurDownHotkey.Content = "\u25C4 " + NoneText;
            _blurDownMods = ModifierKeys.None; _blurDownKey = Key.None;
            _blurDownHotkeyService?.Dispose(); _blurDownHotkeyService = null;
        }
        else if (_capturingButton == BtnBloomUpHotkey)
        {
            BtnBloomUpHotkey.Content = NoneText + " \u25BA";
            _bloomUpMods = ModifierKeys.None; _bloomUpKey = Key.None;
            _bloomUpHotkeyService?.Dispose(); _bloomUpHotkeyService = null;
        }
        else if (_capturingButton == BtnBloomDownHotkey)
        {
            BtnBloomDownHotkey.Content = "\u25C4 " + NoneText;
            _bloomDownMods = ModifierKeys.None; _bloomDownKey = Key.None;
            _bloomDownHotkeyService?.Dispose(); _bloomDownHotkeyService = null;
        }
        else if (_capturingButton == BtnCurvatureUpHotkey)
        {
            BtnCurvatureUpHotkey.Content = NoneText + " \u25BA";
            _curvatureUpMods = ModifierKeys.None; _curvatureUpKey = Key.None;
            _curvatureUpHotkeyService?.Dispose(); _curvatureUpHotkeyService = null;
        }
        else if (_capturingButton == BtnCurvatureDownHotkey)
        {
            BtnCurvatureDownHotkey.Content = "\u25C4 " + NoneText;
            _curvatureDownMods = ModifierKeys.None; _curvatureDownKey = Key.None;
            _curvatureDownHotkeyService?.Dispose(); _curvatureDownHotkeyService = null;
        }
        else if (_capturingButton == BtnFlickerUpHotkey)
        {
            BtnFlickerUpHotkey.Content = NoneText + " \u25BA";
            _flickerUpMods = ModifierKeys.None; _flickerUpKey = Key.None;
            _flickerUpHotkeyService?.Dispose(); _flickerUpHotkeyService = null;
        }
        else if (_capturingButton == BtnFlickerDownHotkey)
        {
            BtnFlickerDownHotkey.Content = "\u25C4 " + NoneText;
            _flickerDownMods = ModifierKeys.None; _flickerDownKey = Key.None;
            _flickerDownHotkeyService?.Dispose(); _flickerDownHotkeyService = null;
        }
        else if (_capturingButton == BtnPhosphorUpHotkey)
        {
            BtnPhosphorUpHotkey.Content = NoneText + " \u25BA";
            _phosphorUpMods = ModifierKeys.None; _phosphorUpKey = Key.None;
            _phosphorUpHotkeyService?.Dispose(); _phosphorUpHotkeyService = null;
        }
        else if (_capturingButton == BtnPhosphorDownHotkey)
        {
            BtnPhosphorDownHotkey.Content = "\u25C4 " + NoneText;
            _phosphorDownMods = ModifierKeys.None; _phosphorDownKey = Key.None;
            _phosphorDownHotkeyService?.Dispose(); _phosphorDownHotkeyService = null;
        }
        else if (_capturingButton == BtnHOpacityUpHotkey)
        {
            BtnHOpacityUpHotkey.Content = NoneText + " \u25BA";
            _hOpacityUpMods = ModifierKeys.None; _hOpacityUpKey = Key.None;
            _hOpacityUpHotkeyService?.Dispose(); _hOpacityUpHotkeyService = null;
        }
        else if (_capturingButton == BtnHOpacityDownHotkey)
        {
            BtnHOpacityDownHotkey.Content = "\u25C4 " + NoneText;
            _hOpacityDownMods = ModifierKeys.None; _hOpacityDownKey = Key.None;
            _hOpacityDownHotkeyService?.Dispose(); _hOpacityDownHotkeyService = null;
        }
        else if (_capturingButton == BtnVOpacityUpHotkey)
        {
            BtnVOpacityUpHotkey.Content = NoneText + " \u25BA";
            _vOpacityUpMods = ModifierKeys.None; _vOpacityUpKey = Key.None;
            _vOpacityUpHotkeyService?.Dispose(); _vOpacityUpHotkeyService = null;
        }
        else if (_capturingButton == BtnVOpacityDownHotkey)
        {
            BtnVOpacityDownHotkey.Content = "\u25C4 " + NoneText;
            _vOpacityDownMods = ModifierKeys.None; _vOpacityDownKey = Key.None;
            _vOpacityDownHotkeyService?.Dispose(); _vOpacityDownHotkeyService = null;
        }
        else if (_capturingButton == BtnVhsUpHotkey)
        {
            BtnVhsUpHotkey.Content = NoneText + " \u25BA";
            _vhsUpMods = ModifierKeys.None; _vhsUpKey = Key.None;
            _vhsUpHotkeyService?.Dispose(); _vhsUpHotkeyService = null;
        }
        else if (_capturingButton == BtnVhsDownHotkey)
        {
            BtnVhsDownHotkey.Content = "\u25C4 " + NoneText;
            _vhsDownMods = ModifierKeys.None; _vhsDownKey = Key.None;
            _vhsDownHotkeyService?.Dispose(); _vhsDownHotkeyService = null;
        }
        else if (_capturingButton == BtnGrainUpHotkey)
        {
            BtnGrainUpHotkey.Content = NoneText + " \u25BA";
            _grainUpMods = ModifierKeys.None; _grainUpKey = Key.None;
            _grainUpHotkeyService?.Dispose(); _grainUpHotkeyService = null;
        }
        else if (_capturingButton == BtnGrainDownHotkey)
        {
            BtnGrainDownHotkey.Content = "\u25C4 " + NoneText;
            _grainDownMods = ModifierKeys.None; _grainDownKey = Key.None;
            _grainDownHotkeyService?.Dispose(); _grainDownHotkeyService = null;
        }
        else if (_capturingButton == BtnTapeNoiseUpHotkey)
        {
            BtnTapeNoiseUpHotkey.Content = NoneText + " \u25BA";
            _tapeNoiseUpMods = ModifierKeys.None; _tapeNoiseUpKey = Key.None;
            _tapeNoiseUpHotkeyService?.Dispose(); _tapeNoiseUpHotkeyService = null;
        }
        else if (_capturingButton == BtnTapeNoiseDownHotkey)
        {
            BtnTapeNoiseDownHotkey.Content = "\u25C4 " + NoneText;
            _tapeNoiseDownMods = ModifierKeys.None; _tapeNoiseDownKey = Key.None;
            _tapeNoiseDownHotkeyService?.Dispose(); _tapeNoiseDownHotkeyService = null;
        }

        _capturingButton = null;
        _pendingHotkeyCancelCallback = null;
        UpdateNoneColors();
        SaveShortcuts(); // auto-save after clearing
        e.Handled = true;
        return;
    }

    ModifierKeys mods;
    Key key;

    if (e.Key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt
        or Key.LeftShift or Key.RightShift or Key.LWin or Key.RWin or Key.System)
    {
        if (e.Key == Key.System && e.SystemKey is not (Key.LeftAlt or Key.RightAlt))
        {
            mods = Keyboard.Modifiers;
            key = e.SystemKey;
        }
        else return;
    }
    else
    {
        mods = Keyboard.Modifiers;
        key = e.Key;
    }

    PreviewKeyDown -= GenericCaptureHotkey;
    _capturingButton = null;
    _pendingHotkeyCallback?.Invoke(mods, key);
    _pendingHotkeyCallback = null;
    e.Handled = true;
}

private void BtnSwitchBezelHotkey_Click(object sender, RoutedEventArgs e)
{
    _switchBezelHotkeyService?.Dispose(); _switchBezelHotkeyService = null; // unregister so key is capturable
    StartHotkeyCapture(BtnSwitchBezelHotkey, (m, k) =>
    {
        _switchBezelMods = m; _switchBezelKey = k;
        BtnSwitchBezelHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterSwitchBezelHotkey(); SaveShortcuts();
    }, onCancel: RegisterSwitchBezelHotkey);
}

private void RegisterSwitchBezelHotkey()
{
    _switchBezelHotkeyService?.Dispose();
    _switchBezelHotkeyService = new HotkeyService();
    _switchBezelHotkeyService.Register(this, _switchBezelMods, _switchBezelKey, CycleBezelImage);
}

private void BtnSwitchBezelBackHotkey_Click(object sender, RoutedEventArgs e)
{
    _switchBezelBackHotkeyService?.Dispose(); _switchBezelBackHotkeyService = null;
    StartHotkeyCapture(BtnSwitchBezelBackHotkey, (m, k) =>
    {
        _switchBezelBackMods = m; _switchBezelBackKey = k;
        BtnSwitchBezelBackHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterSwitchBezelBackHotkey(); SaveShortcuts();
    }, onCancel: RegisterSwitchBezelBackHotkey);
}

private void RegisterSwitchBezelBackHotkey()
{
    _switchBezelBackHotkeyService?.Dispose();
    _switchBezelBackHotkeyService = new HotkeyService();
    _switchBezelBackHotkeyService.Register(this, _switchBezelBackMods, _switchBezelBackKey, CycleBezelImageBack);
}

private void BtnSwitchProfileHotkey_Click(object sender, RoutedEventArgs e)
{
    _switchProfileHotkeyService?.Dispose(); _switchProfileHotkeyService = null;
    StartHotkeyCapture(BtnSwitchProfileHotkey, (m, k) =>
    {
        _switchProfileMods = m; _switchProfileKey = k;
        BtnSwitchProfileHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterSwitchProfileHotkey(); SaveShortcuts();
    }, onCancel: RegisterSwitchProfileHotkey);
}

private void RegisterSwitchProfileHotkey()
{
    _switchProfileHotkeyService?.Dispose();
    _switchProfileHotkeyService = new HotkeyService();
    _switchProfileHotkeyService.Register(this, _switchProfileMods, _switchProfileKey, CycleScanlineProfile);
}

private void BtnSwitchProfileBackHotkey_Click(object sender, RoutedEventArgs e)
{
    _switchProfileBackHotkeyService?.Dispose(); _switchProfileBackHotkeyService = null;
    StartHotkeyCapture(BtnSwitchProfileBackHotkey, (m, k) =>
    {
        _switchProfileBackMods = m; _switchProfileBackKey = k;
        BtnSwitchProfileBackHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterSwitchProfileBackHotkey(); SaveShortcuts();
    }, onCancel: RegisterSwitchProfileBackHotkey);
}

private void RegisterSwitchProfileBackHotkey()
{
    _switchProfileBackHotkeyService?.Dispose();
    _switchProfileBackHotkeyService = new HotkeyService();
    _switchProfileBackHotkeyService.Register(this, _switchProfileBackMods, _switchProfileBackKey, CycleScanlineProfileBack);
}

// ── CRT OSD helpers ────────────────────────────────────────────

private void ShowCrtOsd(string label, double value)
{
    string osdText = $"{label}   {value:0} %";
    _hookSharedMem?.WriteOsd(osdText);
    RestartOsdDismissTimer();
}

private void ShowGameOsd(string text)
{
    _hookSharedMem?.WriteOsd(text);
    RestartOsdDismissTimer();
}

private void RestartOsdDismissTimer()
{
    if (_crtOsdTimer == null)
    {
        _crtOsdTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _crtOsdTimer.Tick += (_, _) =>
        {
            _crtOsdTimer!.Stop();
            _hookSharedMem?.ClearOsd();
        };
    }
    _crtOsdTimer.Stop();
    _crtOsdTimer.Start();
}

// ── CRT effect hotkeys ────────────────────────────────────────��

private void AdjustCrtSlider(Slider slider, System.Windows.Controls.Primitives.ToggleButton toggle, double delta, string label)
{
    Dispatcher.Invoke(() =>
    {
        var now = DateTime.UtcNow;
        double elapsed = (now - _lastCrtAdjust).TotalMilliseconds;

        // Throttle to ~12 updates/sec max to stay readable
        if (elapsed < 80) return;

        // Accelerate when key is held (elapsed < 400 ms = still holding)
        if (elapsed < 400)
        {
            _crtRepeatCount++;
            if      (_crtRepeatCount > 30) delta = Math.Sign(delta) * 5;  // ~3 s held → ×5
            else if (_crtRepeatCount > 15) delta = Math.Sign(delta) * 3;  // ~1.5 s    → ×3
            else if (_crtRepeatCount > 8)  delta = Math.Sign(delta) * 2;  // ~0.8 s    → ×2
        }
        else
        {
            _crtRepeatCount = 0; // new key press
        }

        _lastCrtAdjust = now;
        slider.Value = Math.Clamp(slider.Value + delta, 0, 100);
        if (delta > 0 && slider.Value > 0) toggle.IsChecked = true;

        ShowCrtOsd(label, slider.Value);
        ScheduleOverlayUpdate();
    });
}

private void BtnBlurUpHotkey_Click(object s, RoutedEventArgs e)
{
    _blurUpHotkeyService?.Dispose(); _blurUpHotkeyService = null;
    StartHotkeyCapture(BtnBlurUpHotkey, (m, k) =>
    {
        _blurUpMods = m; _blurUpKey = k;
        BtnBlurUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterBlurUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterBlurUpHotkey);
}
private void BtnBlurDownHotkey_Click(object s, RoutedEventArgs e)
{
    _blurDownHotkeyService?.Dispose(); _blurDownHotkeyService = null;
    StartHotkeyCapture(BtnBlurDownHotkey, (m, k) =>
    {
        _blurDownMods = m; _blurDownKey = k;
        BtnBlurDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterBlurDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterBlurDownHotkey);
}
private void RegisterBlurUpHotkey()
{
    _blurUpHotkeyService?.Dispose();
    _blurUpHotkeyService = new HotkeyService();
    _blurUpHotkeyService.Register(this, _blurUpMods, _blurUpKey, () => AdjustCrtSlider(SliderBlur, ToggleBlur, 1, "BLUR"), allowRepeat: true);
}
private void RegisterBlurDownHotkey()
{
    _blurDownHotkeyService?.Dispose();
    _blurDownHotkeyService = new HotkeyService();
    _blurDownHotkeyService.Register(this, _blurDownMods, _blurDownKey, () => AdjustCrtSlider(SliderBlur, ToggleBlur, -1, "BLUR"), allowRepeat: true);
}

private void BtnBloomUpHotkey_Click(object s, RoutedEventArgs e)
{
    _bloomUpHotkeyService?.Dispose(); _bloomUpHotkeyService = null;
    StartHotkeyCapture(BtnBloomUpHotkey, (m, k) =>
    {
        _bloomUpMods = m; _bloomUpKey = k;
        BtnBloomUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterBloomUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterBloomUpHotkey);
}
private void BtnBloomDownHotkey_Click(object s, RoutedEventArgs e)
{
    _bloomDownHotkeyService?.Dispose(); _bloomDownHotkeyService = null;
    StartHotkeyCapture(BtnBloomDownHotkey, (m, k) =>
    {
        _bloomDownMods = m; _bloomDownKey = k;
        BtnBloomDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterBloomDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterBloomDownHotkey);
}
private void RegisterBloomUpHotkey()
{
    _bloomUpHotkeyService?.Dispose();
    _bloomUpHotkeyService = new HotkeyService();
    _bloomUpHotkeyService.Register(this, _bloomUpMods, _bloomUpKey, () => AdjustCrtSlider(SliderBloom, ToggleBloom, 1, "BLOOM"), allowRepeat: true);
}
private void RegisterBloomDownHotkey()
{
    _bloomDownHotkeyService?.Dispose();
    _bloomDownHotkeyService = new HotkeyService();
    _bloomDownHotkeyService.Register(this, _bloomDownMods, _bloomDownKey, () => AdjustCrtSlider(SliderBloom, ToggleBloom, -1, "BLOOM"), allowRepeat: true);
}

private void BtnCurvatureUpHotkey_Click(object s, RoutedEventArgs e)
{
    _curvatureUpHotkeyService?.Dispose(); _curvatureUpHotkeyService = null;
    StartHotkeyCapture(BtnCurvatureUpHotkey, (m, k) =>
    {
        _curvatureUpMods = m; _curvatureUpKey = k;
        BtnCurvatureUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterCurvatureUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterCurvatureUpHotkey);
}
private void BtnCurvatureDownHotkey_Click(object s, RoutedEventArgs e)
{
    _curvatureDownHotkeyService?.Dispose(); _curvatureDownHotkeyService = null;
    StartHotkeyCapture(BtnCurvatureDownHotkey, (m, k) =>
    {
        _curvatureDownMods = m; _curvatureDownKey = k;
        BtnCurvatureDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterCurvatureDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterCurvatureDownHotkey);
}
private void RegisterCurvatureUpHotkey()
{
    _curvatureUpHotkeyService?.Dispose();
    _curvatureUpHotkeyService = new HotkeyService();
    _curvatureUpHotkeyService.Register(this, _curvatureUpMods, _curvatureUpKey, () => AdjustCrtSlider(SliderCurvature, ToggleCurvature, 1, "CURVATURE"), allowRepeat: true);
}
private void RegisterCurvatureDownHotkey()
{
    _curvatureDownHotkeyService?.Dispose();
    _curvatureDownHotkeyService = new HotkeyService();
    _curvatureDownHotkeyService.Register(this, _curvatureDownMods, _curvatureDownKey, () => AdjustCrtSlider(SliderCurvature, ToggleCurvature, -1, "CURVATURE"), allowRepeat: true);
}

private void BtnFlickerUpHotkey_Click(object s, RoutedEventArgs e)
{
    _flickerUpHotkeyService?.Dispose(); _flickerUpHotkeyService = null;
    StartHotkeyCapture(BtnFlickerUpHotkey, (m, k) =>
    {
        _flickerUpMods = m; _flickerUpKey = k;
        BtnFlickerUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterFlickerUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterFlickerUpHotkey);
}
private void BtnFlickerDownHotkey_Click(object s, RoutedEventArgs e)
{
    _flickerDownHotkeyService?.Dispose(); _flickerDownHotkeyService = null;
    StartHotkeyCapture(BtnFlickerDownHotkey, (m, k) =>
    {
        _flickerDownMods = m; _flickerDownKey = k;
        BtnFlickerDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterFlickerDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterFlickerDownHotkey);
}
private void RegisterFlickerUpHotkey()
{
    _flickerUpHotkeyService?.Dispose();
    _flickerUpHotkeyService = new HotkeyService();
    _flickerUpHotkeyService.Register(this, _flickerUpMods, _flickerUpKey, () => AdjustCrtSlider(SliderFlicker, ToggleFlicker, 1, "FLICKER"), allowRepeat: true);
}
private void RegisterFlickerDownHotkey()
{
    _flickerDownHotkeyService?.Dispose();
    _flickerDownHotkeyService = new HotkeyService();
    _flickerDownHotkeyService.Register(this, _flickerDownMods, _flickerDownKey, () => AdjustCrtSlider(SliderFlicker, ToggleFlicker, -1, "FLICKER"), allowRepeat: true);
}

private void BtnPhosphorUpHotkey_Click(object s, RoutedEventArgs e)
{
    _phosphorUpHotkeyService?.Dispose(); _phosphorUpHotkeyService = null;
    StartHotkeyCapture(BtnPhosphorUpHotkey, (m, k) =>
    {
        _phosphorUpMods = m; _phosphorUpKey = k;
        BtnPhosphorUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterPhosphorUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterPhosphorUpHotkey);
}
private void BtnPhosphorDownHotkey_Click(object s, RoutedEventArgs e)
{
    _phosphorDownHotkeyService?.Dispose(); _phosphorDownHotkeyService = null;
    StartHotkeyCapture(BtnPhosphorDownHotkey, (m, k) =>
    {
        _phosphorDownMods = m; _phosphorDownKey = k;
        BtnPhosphorDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterPhosphorDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterPhosphorDownHotkey);
}
private void RegisterPhosphorUpHotkey()
{
    _phosphorUpHotkeyService?.Dispose();
    _phosphorUpHotkeyService = new HotkeyService();
    _phosphorUpHotkeyService.Register(this, _phosphorUpMods, _phosphorUpKey, () => AdjustCrtSlider(SliderPhosphor, TogglePhosphor, 1, "PHOSPHOR"), allowRepeat: true);
}
private void RegisterPhosphorDownHotkey()
{
    _phosphorDownHotkeyService?.Dispose();
    _phosphorDownHotkeyService = new HotkeyService();
    _phosphorDownHotkeyService.Register(this, _phosphorDownMods, _phosphorDownKey, () => AdjustCrtSlider(SliderPhosphor, TogglePhosphor, -1, "PHOSPHOR"), allowRepeat: true);
}

private void BtnHOpacityUpHotkey_Click(object s, RoutedEventArgs e)
{
    _hOpacityUpHotkeyService?.Dispose(); _hOpacityUpHotkeyService = null;
    StartHotkeyCapture(BtnHOpacityUpHotkey, (m, k) =>
    {
        _hOpacityUpMods = m; _hOpacityUpKey = k;
        BtnHOpacityUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterHOpacityUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterHOpacityUpHotkey);
}
private void BtnHOpacityDownHotkey_Click(object s, RoutedEventArgs e)
{
    _hOpacityDownHotkeyService?.Dispose(); _hOpacityDownHotkeyService = null;
    StartHotkeyCapture(BtnHOpacityDownHotkey, (m, k) =>
    {
        _hOpacityDownMods = m; _hOpacityDownKey = k;
        BtnHOpacityDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterHOpacityDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterHOpacityDownHotkey);
}
private void RegisterHOpacityUpHotkey()
{
    _hOpacityUpHotkeyService?.Dispose();
    _hOpacityUpHotkeyService = new HotkeyService();
    _hOpacityUpHotkeyService.Register(this, _hOpacityUpMods, _hOpacityUpKey, () => AdjustCrtSlider(SliderHOpacity, ToggleHorizontal, 1, "H.OPACITY"), allowRepeat: true);
}
private void RegisterHOpacityDownHotkey()
{
    _hOpacityDownHotkeyService?.Dispose();
    _hOpacityDownHotkeyService = new HotkeyService();
    _hOpacityDownHotkeyService.Register(this, _hOpacityDownMods, _hOpacityDownKey, () => AdjustCrtSlider(SliderHOpacity, ToggleHorizontal, -1, "H.OPACITY"), allowRepeat: true);
}

private void BtnVOpacityUpHotkey_Click(object s, RoutedEventArgs e)
{
    _vOpacityUpHotkeyService?.Dispose(); _vOpacityUpHotkeyService = null;
    StartHotkeyCapture(BtnVOpacityUpHotkey, (m, k) =>
    {
        _vOpacityUpMods = m; _vOpacityUpKey = k;
        BtnVOpacityUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterVOpacityUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterVOpacityUpHotkey);
}
private void BtnVOpacityDownHotkey_Click(object s, RoutedEventArgs e)
{
    _vOpacityDownHotkeyService?.Dispose(); _vOpacityDownHotkeyService = null;
    StartHotkeyCapture(BtnVOpacityDownHotkey, (m, k) =>
    {
        _vOpacityDownMods = m; _vOpacityDownKey = k;
        BtnVOpacityDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterVOpacityDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterVOpacityDownHotkey);
}
private void RegisterVOpacityUpHotkey()
{
    _vOpacityUpHotkeyService?.Dispose();
    _vOpacityUpHotkeyService = new HotkeyService();
    _vOpacityUpHotkeyService.Register(this, _vOpacityUpMods, _vOpacityUpKey, () => AdjustCrtSlider(SliderVOpacity, ToggleVertical, 1, "V.OPACITY"), allowRepeat: true);
}
private void RegisterVOpacityDownHotkey()
{
    _vOpacityDownHotkeyService?.Dispose();
    _vOpacityDownHotkeyService = new HotkeyService();
    _vOpacityDownHotkeyService.Register(this, _vOpacityDownMods, _vOpacityDownKey, () => AdjustCrtSlider(SliderVOpacity, ToggleVertical, -1, "V.OPACITY"), allowRepeat: true);
}

private void BtnVhsUpHotkey_Click(object s, RoutedEventArgs e)
{
    _vhsUpHotkeyService?.Dispose(); _vhsUpHotkeyService = null;
    StartHotkeyCapture(BtnVhsUpHotkey, (m, k) =>
    {
        _vhsUpMods = m; _vhsUpKey = k;
        BtnVhsUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterVhsUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterVhsUpHotkey);
}
private void BtnVhsDownHotkey_Click(object s, RoutedEventArgs e)
{
    _vhsDownHotkeyService?.Dispose(); _vhsDownHotkeyService = null;
    StartHotkeyCapture(BtnVhsDownHotkey, (m, k) =>
    {
        _vhsDownMods = m; _vhsDownKey = k;
        BtnVhsDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterVhsDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterVhsDownHotkey);
}
private void RegisterVhsUpHotkey()
{
    _vhsUpHotkeyService?.Dispose();
    _vhsUpHotkeyService = new HotkeyService();
    _vhsUpHotkeyService.Register(this, _vhsUpMods, _vhsUpKey, () => AdjustCrtSlider(SliderVhs, ToggleVhs, 1, "VHS"), allowRepeat: true);
}
private void RegisterVhsDownHotkey()
{
    _vhsDownHotkeyService?.Dispose();
    _vhsDownHotkeyService = new HotkeyService();
    _vhsDownHotkeyService.Register(this, _vhsDownMods, _vhsDownKey, () => AdjustCrtSlider(SliderVhs, ToggleVhs, -1, "VHS"), allowRepeat: true);
}

private void BtnGrainUpHotkey_Click(object s, RoutedEventArgs e)
{
    _grainUpHotkeyService?.Dispose(); _grainUpHotkeyService = null;
    StartHotkeyCapture(BtnGrainUpHotkey, (m, k) =>
    {
        _grainUpMods = m; _grainUpKey = k;
        BtnGrainUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterGrainUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterGrainUpHotkey);
}
private void BtnGrainDownHotkey_Click(object s, RoutedEventArgs e)
{
    _grainDownHotkeyService?.Dispose(); _grainDownHotkeyService = null;
    StartHotkeyCapture(BtnGrainDownHotkey, (m, k) =>
    {
        _grainDownMods = m; _grainDownKey = k;
        BtnGrainDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterGrainDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterGrainDownHotkey);
}
private void RegisterGrainUpHotkey()
{
    _grainUpHotkeyService?.Dispose();
    _grainUpHotkeyService = new HotkeyService();
    _grainUpHotkeyService.Register(this, _grainUpMods, _grainUpKey, () => AdjustCrtSlider(SliderGrain, ToggleGrain, 1, "GRAIN"), allowRepeat: true);
}
private void RegisterGrainDownHotkey()
{
    _grainDownHotkeyService?.Dispose();
    _grainDownHotkeyService = new HotkeyService();
    _grainDownHotkeyService.Register(this, _grainDownMods, _grainDownKey, () => AdjustCrtSlider(SliderGrain, ToggleGrain, -1, "GRAIN"), allowRepeat: true);
}

private void BtnTapeNoiseUpHotkey_Click(object s, RoutedEventArgs e)
{
    _tapeNoiseUpHotkeyService?.Dispose(); _tapeNoiseUpHotkeyService = null;
    StartHotkeyCapture(BtnTapeNoiseUpHotkey, (m, k) =>
    {
        _tapeNoiseUpMods = m; _tapeNoiseUpKey = k;
        BtnTapeNoiseUpHotkey.Content = FormatHotkeyDisplay(m, k) + " \u25BA";
        RegisterTapeNoiseUpHotkey(); SaveShortcuts();
    }, onCancel: RegisterTapeNoiseUpHotkey);
}
private void BtnTapeNoiseDownHotkey_Click(object s, RoutedEventArgs e)
{
    _tapeNoiseDownHotkeyService?.Dispose(); _tapeNoiseDownHotkeyService = null;
    StartHotkeyCapture(BtnTapeNoiseDownHotkey, (m, k) =>
    {
        _tapeNoiseDownMods = m; _tapeNoiseDownKey = k;
        BtnTapeNoiseDownHotkey.Content = "\u25C4 " + FormatHotkeyDisplay(m, k);
        RegisterTapeNoiseDownHotkey(); SaveShortcuts();
    }, onCancel: RegisterTapeNoiseDownHotkey);
}
private void RegisterTapeNoiseUpHotkey()
{
    _tapeNoiseUpHotkeyService?.Dispose();
    _tapeNoiseUpHotkeyService = new HotkeyService();
    _tapeNoiseUpHotkeyService.Register(this, _tapeNoiseUpMods, _tapeNoiseUpKey, () => AdjustCrtSlider(SliderTapeNoise, ToggleTapeNoise, 1, "TAPE NOISE"), allowRepeat: true);
}
private void RegisterTapeNoiseDownHotkey()
{
    _tapeNoiseDownHotkeyService?.Dispose();
    _tapeNoiseDownHotkeyService = new HotkeyService();
    _tapeNoiseDownHotkeyService.Register(this, _tapeNoiseDownMods, _tapeNoiseDownKey, () => AdjustCrtSlider(SliderTapeNoise, ToggleTapeNoise, -1, "TAPE NOISE"), allowRepeat: true);
}

private static string FormatHotkeyDisplay(ModifierKeys mods, Key key)
{
    var parts = new List<string>();
    if (mods.HasFlag(ModifierKeys.Control)) parts.Add("Ctrl");
    if (mods.HasFlag(ModifierKeys.Alt))     parts.Add("Alt");
    if (mods.HasFlag(ModifierKeys.Shift))   parts.Add("Shift");
    if (mods.HasFlag(ModifierKeys.Windows)) parts.Add("Win");
    parts.Add(key.ToString());
    return string.Join("+", parts);
}



    // ══════════════════════════════════════════════════════════════
    //  LANGUAGE (ComboBox in Tab 2)
    // ══════════════════════════════════════════════════════════════

    private void CmbLanguage_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_isLoaded) return;
        _langIndex = CmbLanguage.SelectedIndex;
        ApplyLanguage();
    }

    private static System.Windows.Controls.TextBlock MakeTrashIcon() =>
        new()
        {
            Text       = "\uE74D",
            FontFamily = new System.Windows.Media.FontFamily("Segoe MDL2 Assets"),
            FontSize   = 14,
            FontWeight = System.Windows.FontWeights.Normal,
            VerticalAlignment = System.Windows.VerticalAlignment.Center,
        };

    private void ApplyLanguage()
    {
        // ── Tab bar ──
        TabBtn1Text.Text = L("LAYER CONFIGURATION", "CONFIGURATION CALQUES", "EBENEN-KONFIGURATION", "CONFIGURAZIONE LIVELLI", "CONFIGURACIÓN DE CAPAS", "CONFIGURAÇÃO DE CAMADAS");
        TabBtn2Text.Text = L("CRT EFFECTS", "EFFETS CRT", "CRT-EFFEKTE", "EFFETTI CRT", "EFECTOS CRT", "EFEITOS CRT");
        TabBtn3Text.Text = L("SYSTEM SETTINGS", "PARAMÈTRES SYSTÈME", "SYSTEMEINSTELLUNGEN", "IMPOSTAZIONI DI SISTEMA", "AJUSTES DEL SISTEMA", "CONFIGURAÇÕES DO SISTEMA");

        // ── Tab 2 (CRT Effects): CRT group + sub-effects ──
        LblCrtGroupTitle.Text     = L("CRT",                       "CRT",                          "CRT",                          "CRT",                          "CRT",                          "CRT");
        LblCurvatureTitle.Text    = L("CURVATURE",                 "COURBURE",                     "KRÜMMUNG",                     "CURVATURA",                    "CURVATURA",                    "CURVATURA");
        LblCurvatureAmount.Text   = L("AMOUNT",                    "QUANTITÉ",                     "MENGE",                        "QUANTITÀ",                     "CANTIDAD",                     "QUANTIDADE");
        LblBlurTitle.Text         = L("BLUR",                      "FLOU",                         "UNSCHÄRFE",                    "SFOCATURA",                    "DESENFOQUE",                   "DESFOQUE");
        LblBlurIntensity.Text     = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblBloomTitle.Text        = L("BLOOM",                     "BLOOM",                        "BLOOM",                        "BLOOM",                        "BLOOM",                        "BLOOM");
        LblBloomIntensity.Text    = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblFlickerTitle.Text      = L("FLICKER",                   "SCINTILLEMENT",                "FLIMMERN",                     "SFARFALLIO",                   "PARPADEO",                     "CINTILAÇÃO");
        LblFlickerIntensity.Text  = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblFlickerRate.Text       = L("RATE",                      "FRÉQUENCE",                    "RATE",                         "FREQUENZA",                    "FRECUENCIA",                   "FREQUÊNCIA");
        LblPhosphorTitle.Text     = L("PHOSPHOR GLOW",             "LUEUR PHOSPHORE",              "PHOSPHORLEUCHTEN",             "FOSFORESCENZA",                "BRILLO FÓSFORO",               "BRILHO FÓSFORO");
        LblPhosphorIntensity.Text = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblLumaTitle.Text         = L("LUMINOSITY COMPENSATION",   "COMPENSATION LUMINOSITÉ",      "HELLIGKEITSKOMPENSATION",      "COMPENSAZIONE LUMINOSITÀ",     "COMPENSACIÓN LUMINOSIDAD",     "COMPENSAÇÃO LUMINOSIDADE");
        LblBrightnessLbl.Text     = L("BRIGHTNESS",                "LUMINOSITÉ",                   "HELLIGKEIT",                   "LUMINOSITÀ",                   "BRILLO",                       "BRILHO");
        LblContrastLbl.Text       = L("CONTRAST",                  "CONTRASTE",                    "KONTRAST",                     "CONTRASTO",                    "CONTRASTE",                    "CONTRASTE");
        LblSaturationLbl.Text     = L("SATURATION",                "SATURATION",                   "SÄTTIGUNG",                    "SATURAZIONE",                  "SATURACIÓN",                   "SATURAÇÃO");
        LblTemperatureLbl.Text    = L("TEMPERATURE",               "TEMPÉRATURE",                  "TEMPERATUR",                   "TEMPERATURA",                  "TEMPERATURA",                  "TEMPERATURA");
        LblBlackLevelLbl.Text     = L("BLACK LEVEL",               "NIVEAU DE NOIR",               "SCHWARZPEGEL",                 "LIVELLO NERO",                 "NIVEL DE NEGRO",               "NÍVEL DE PRETO");
        LblGammaLbl.Text          = L("GAMMA",                     "GAMMA",                        "GAMMA",                        "GAMMA",                        "GAMMA",                        "GAMMA");

        // ── Tab 2 (CRT Effects): VHS section ──
        LblVhsTitle.Text          = L("VHS",                       "VHS",                          "VHS",                          "VHS",                          "VHS",                          "VHS");
        LblVhsEffectTitle.Text    = L("VHS EFFECT",                "EFFET VHS",                    "VHS-EFFEKT",                   "EFFETTO VHS",                  "EFECTO VHS",                   "EFEITO VHS");
        LblVhsIntensity.Text      = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblGrainTitle.Text        = L("FILM GRAIN",                "GRAIN CINÉMATIQUE",            "FILMKORN",                     "GRANA CINEMATOGRAFICA",        "GRANO DE PELÍCULA",            "GRÃO DE FILME");
        LblGrainIntensity.Text    = L("INTENSITY",                 "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");
        LblTapeNoiseTitle.Text    = L("TAPE NOISE",                "BRUIT DE BANDE",               "BANDRAUSCHEN",                 "RUMORE DI NASTRO",             "RUIDO DE CINTA",               "RUÍDO DE FITA");
        LblTapeNoiseIntensity.Text= L("INTENSITY",                  "INTENSITÉ",                    "INTENSITÄT",                   "INTENSITÀ",                    "INTENSIDAD",                   "INTENSIDADE");

        // ── Tab 1: Bezel ──
        LblBezelSectionHeader.Text = "BEZEL";
        LblFilePath.Text           = L("FILE PATH", "CHEMIN DU FICHIER", "DATEIPFAD", "PERCORSO FILE", "RUTA DEL ARCHIVO", "CAMINHO DO ARQUIVO");
        BtnBezelSearchBtn.Content  = L("Set", "Définir", "Setzen", "Imposta", "Definir", "Definir");
        BtnBezelResetBtn.Content   = L("Reset", "Réinitialiser", "Zurücksetzen", "Ripristina", "Restablecer", "Redefinir");
        LblBezelOpacity.Text       = L("OPACITY", "OPACITÉ", "DECKKRAFT", "OPACITÀ", "OPACIDAD", "OPACIDADE");

        // ── Tab 1: button tooltips ──
        BtnBezelSearchBtn.ToolTip  = L("Choose bezel image file", "Choisir l'image de bezel", "Bezel-Bilddatei wählen", "Scegli file immagine bezel", "Elegir archivo de imagen bezel", "Escolher arquivo de imagem bezel");
        BtnBezelResetBtn.ToolTip   = L("Remove bezel", "Supprimer le bezel", "Bezel entfernen", "Rimuovi bezel", "Eliminar bezel", "Remover bezel");
        BtnAppSearchBtn.ToolTip    = L("Browse for executable (.exe)", "Parcourir l'exécutable (.exe)", "Ausführbare Datei suchen (.exe)", "Sfoglia eseguibile (.exe)", "Buscar ejecutable (.exe)", "Procurar executável (.exe)");
        BtnPickProcessBtn.ToolTip  = L("Pick a running process", "Choisir un processus actif", "Laufenden Prozess wählen", "Scegli processo in esecuzione", "Elegir proceso activo", "Escolher processo ativo");
        BtnAppDeleteBtn.ToolTip     = L("Delete", "Supprimer", "Löschen", "Elimina", "Eliminar", "Excluir");
        BtnDeleteProfileBtn.ToolTip = L("Delete", "Supprimer", "Löschen", "Elimina", "Eliminar", "Excluir");
        BtnResetProfileBtn.ToolTip  = L("Reset all profile settings to default values (scanlines, CRT effects and bezel included)",
                                        "Réinitialiser tous les réglages du profil aux valeurs par défaut (scanlines, effets CRT et bezel inclus)",
                                        "Alle Profileinstellungen auf Standardwerte zurücksetzen (Scanlines, CRT-Effekte und Bezel eingeschlossen)",
                                        "Ripristina tutte le impostazioni del profilo ai valori predefiniti (scanlines, effetti CRT e bezel inclusi)",
                                        "Restablecer todos los ajustes del perfil a los valores predeterminados (scanlines, efectos CRT y bezel incluidos)",
                                        "Redefinir todas as configurações do perfil para os valores padrão (scanlines, efeitos CRT e bezel incluídos)");
        BtnRomSet.ToolTip          = L("Assign a ROM folder to this system", "Attribuer un dossier ROM à ce système", "ROM-Ordner diesem System zuweisen", "Assegna cartella ROM a questo sistema", "Asignar carpeta ROM a este sistema", "Atribuir pasta ROM a este sistema");
        BtnScanSystem.ToolTip      = L("Scan folder and index all ROMs", "Scanner le dossier et indexer les ROMs", "Ordner scannen und ROMs indizieren", "Scansiona cartella e indicizza ROM", "Escanear carpeta e indexar ROMs", "Escanear pasta e indexar ROMs");

        // ── Tab 2 (CRT Effects): preset button ──
        BtnPresetSave.Content      = L("Save", "Sauvegarder", "Speichern", "Salva", "Guardar", "Salvar");
        BtnPresetSave.ToolTip      = L("Save current CRT settings as preset", "Sauvegarder les réglages CRT comme préréglage", "CRT-Einstellungen als Preset speichern", "Salva impostazioni CRT come preset", "Guardar ajustes CRT como preset", "Salvar configurações CRT como preset");
        BtnPresetDelete.ToolTip    = L("Delete", "Supprimer", "Löschen", "Elimina", "Eliminar", "Excluir");

        // ── Tab 1: Scanlines ──
        LblScanlinesSectionHeader.Text = L("SCANLINES GENERATOR", "GÉNÉRATEUR DE SCANLINES", "SCANLINES-GENERATOR", "GENERATORE DI SCANLINES", "GENERADOR DE SCANLINES", "GERADOR DE SCANLINES");
        LblHorizontal.Text   = "HORIZONTAL";
        LblHGap.Text         = L("GAP", "ÉCART", "ABSTAND", "DISTANZA", "ESPACIO", "ESPAÇO");
        LblHThickness.Text   = L("THICKNESS", "ÉPAISSEUR", "DICKE", "SPESSORE", "GROSOR", "ESPESSURA");
        LblHOpacity.Text     = L("OPACITY", "OPACITÉ", "DECKKRAFT", "OPACITÀ", "OPACIDAD", "OPACIDADE");
        LblVertical.Text     = "VERTICAL";
        LblVGap.Text         = L("GAP", "ÉCART", "ABSTAND", "DISTANZA", "ESPACIO", "ESPAÇO");
        LblVThickness.Text   = L("THICKNESS", "ÉPAISSEUR", "DICKE", "SPESSORE", "GROSOR", "ESPESSURA");
        LblVOpacity.Text     = L("OPACITY", "OPACITÉ", "DECKKRAFT", "OPACITÀ", "OPACIDAD", "OPACIDADE");

        // ── Tab 1: Automation ──
        LblAutomationSectionHeader.Text = L("ASSOCIATION PROFILE", "ASSOCIATION PROFIL", "PROFIL-ZUORDNUNG", "ASSOCIAZIONE PROFILO", "ASOCIACIÓN DE PERFIL", "ASSOCIAÇÃO DE PERFIL");
        LblAssociatedProfile.Text       = L("ASSOCIATED PROFILE", "PROFIL ASSOCIÉ", "ZUGEORDNETES PROFIL", "PROFILO ASSOCIATO", "PERFIL ASOCIADO", "PERFIL ASSOCIADO");
        BtnResetProfileBtn.Content      = L("Reset", "Réinitialiser", "Zurücksetzen", "Ripristina", "Restablecer", "Redefinir");
        BtnDeleteProfileBtn.Content     = MakeTrashIcon();
        LblAppPath.Text                 = L("APPLICATION / EMULATOR PATH", "CHEMIN APPLICATION / ÉMULATEUR", "ANWENDUNGS- / EMULATORPFAD", "PERCORSO APPLICAZIONE / EMULATORE", "RUTA DE APLICACIÓN / EMULADOR", "CAMINHO DO APLICATIVO / EMULADOR");
        BtnAppSearchBtn.Content         = L("Set Path", "Définir chemin", "Pfad festlegen", "Imposta percorso", "Definir ruta", "Definir caminho");
        BtnPickProcessBtn.Content       = L("Set Process", "Définir processus", "Prozess festlegen", "Imposta processo", "Definir proceso", "Definir processo");
        BtnAppDeleteBtn.Content         = MakeTrashIcon();
        LblBorderlessToggle.Text        = L("BORDERLESS WINDOWED", "PLEIN ECRAN SANS BORDURE", "RANDLOSES FENSTER", "FINESTRA SENZA BORDI", "VENTANA SIN BORDES", "JANELA SEM BORDAS");

        // ── Tab 1: ROM Systems ──
        LblRomSystemsHeader.Text = L("MULTI-SYSTEM EMULATOR (only)", "ÉMULATEUR MULTI-SYSTÈME (uniquement)", "MULTI-SYSTEM-EMULATOR (nur)", "EMULATORE MULTI-SISTEMA (solo)", "EMULADOR MULTI-SISTEMA (solo)", "EMULADOR MULTI-SISTEMA (apenas)");
        LblRomSystemName.Text    = L("SYSTEM NAME", "NOM DU SYSTÈME", "SYSTEMNAME", "NOME DEL SISTEMA", "NOMBRE DEL SISTEMA", "NOME DO SISTEMA");
        BtnRomSet.Content        = L("Set", "Définir", "Setzen", "Imposta", "Definir", "Definir");
        BtnScanSystem.Content    = L("Scan", "Scanner", "Scannen", "Scansiona", "Escanear", "Escanear");
        BtnResetRomSystem.Content  = L("Reset", "Réinit.", "Reset", "Reset", "Reset", "Reset");
        BtnResetRomSystem.ToolTip  = L("Dissociate this system from the profile (folder kept)", "Dissocier ce système du profil (dossier conservé)", "System vom Profil trennen (Ordner bleibt)", "Dissocia sistema dal profilo (cartella mantenuta)", "Disociar sistema del perfil (carpeta conservada)", "Dissociar sistema do perfil (pasta mantida)");
        BtnDeleteRomSystem.Content = MakeTrashIcon();
        BtnDeleteRomSystem.ToolTip = L("Remove this system entry from the list (folder on disk is kept)", "Supprimer cette entrée de la liste (le dossier sur le disque est conservé)", "Eintrag aus der Liste entfernen (Ordner bleibt erhalten)", "Rimuovi voce dalla lista (cartella conservata)", "Eliminar entrada de la lista (carpeta conservada)", "Remover entrada da lista (pasta mantida)");
        MenuCreateFolder.Header  = L("Create new folder", "Créer un nouveau dossier", "Neuen Ordner erstellen", "Crea nuova cartella", "Crear nueva carpeta", "Criar nova pasta");
        MenuUseFolder.Header     = L("Use existing folder", "Utiliser un dossier existant", "Vorhandenen Ordner verwenden", "Usa cartella esistente", "Usar carpeta existente", "Usar pasta existente");
        SyncRomFolderUI();

        // ── Tab 2: Window Size ──
        LblWindowSizeHeader.Text = L("WINDOW SIZE", "TAILLE DE LA FENÊTRE", "FENSTERGRÖSSE", "DIMENSIONE FINESTRA", "TAMAÑO DE VENTANA", "TAMANHO DA JANELA");

        // ── Tab 2: Behavior ──
        LblBehaviorSectionHeader.Text = L("SOFTWARE BEHAVIOR", "COMPORTEMENT LOGICIEL", "SOFTWAREVERHALTEN", "COMPORTAMENTO SOFTWARE", "COMPORTAMIENTO DEL SOFTWARE", "COMPORTAMENTO DO SOFTWARE");
        LblStartMinimized.Text        = L("Start Minimized", "Démarrer réduit", "Minimiert starten", "Avvia ridotto a icona", "Iniciar minimizado", "Iniciar minimizado");
        LblCloseToTray.Text           = L("Close to Tray", "Fermer dans le tray", "In den Tray schließen", "Chiudi nella barra", "Cerrar en la bandeja", "Fechar na bandeja");
        LblApplyDesktopAtStart.Text   = L("Apply Desktop profile at start", "Appliquer le profil Desktop au démarrage", "Desktop-Profil beim Start anwenden", "Applica profilo Desktop all'avvio", "Aplicar perfil Desktop al inicio", "Aplicar perfil Desktop ao iniciar");
        LblStartWithWindows.Text      = L("Start with Windows", "Démarrer avec Windows", "Mit Windows starten", "Avvia con Windows", "Iniciar con Windows", "Iniciar com Windows");

        // ── Tab 2: Monitor ──
        LblMonitorSectionHeader.Text = L("SCANLINES DISPLAY MONITOR (Desktop)", "MONITEUR SCANLINES (Desktop)", "SCANLINES-MONITOR (Desktop)", "MONITOR SCANLINES (Desktop)", "MONITOR DE SCANLINES (Desktop)", "MONITOR DE SCANLINES (Desktop)");

        // ── Tab 2: Shortcuts ──
        LblShortcutsSectionHeader.Text = L("GLOBAL SHORTCUTS", "RACCOURCIS GLOBAUX", "GLOBALE TASTENKÜRZEL", "SCORCIATOIE GLOBALI", "ATAJOS GLOBALES", "ATALHOS GLOBAIS");
        LblSwitchProfileHotkey.Text    = L("Switch Profile", "Changer de profil", "Profil wechseln", "Cambia profilo", "Cambiar perfil", "Alternar perfil");
        LblSwitchBezelHotkey.Text      = L("Switch Bezel", "Changer de bezel", "Bezel wechseln", "Cambia bezel", "Cambiar bezel", "Alternar bezel");
        LblCrtBlurHotkey.Text          = "Blur";
        LblCrtBloomHotkey.Text         = "Bloom";
        LblCrtCurvatureHotkey.Text     = "Curvature";
        LblCrtFlickerHotkey.Text       = "Flicker";
        LblCrtPhosphorHotkey.Text      = "Phosphor Glow";
        LblCrtHOpacityHotkey.Text      = L("Scanlines Opacity H", "Opacité Scanlines H", "Scanlines Deckkraft H", "Opacità Scanlines H", "Opacidad Scanlines H", "Opacidade Scanlines H");
        LblCrtVOpacityHotkey.Text      = L("Scanlines Opacity V", "Opacité Scanlines V", "Scanlines Deckkraft V", "Opacità Scanlines V", "Opacidad Scanlines V", "Opacidade Scanlines V");
        LblCrtVhsHotkey.Text           = "VHS Intensity";
        LblCrtGrainHotkey.Text         = L("Grain Intensity", "Intensité Grain", "Körnung Intensität", "Intensità Grano", "Intensidad Grano", "Intensidade Granulado");
        LblCrtTapeNoiseHotkey.Text     = L("Tape Noise Intensity", "Intensité Bruit de Bande", "Bandrauschen Intensität", "Intensità Rumore Nastro", "Intensidad Ruido de Cinta", "Intensidade Ruído de Fita");

        // ── Tab 2: Language ──
        LblLanguageSectionHeader.Text = L("LANGUAGE & REGION", "LANGUE & RÉGION", "SPRACHE & REGION", "LINGUA & REGIONE", "IDIOMA Y REGIÓN", "IDIOMA E REGIÃO");

        // ── Tab 2: Backup ──
        LblBackupSectionHeader.Text = L("BACKUP & RESTORE", "SAUVEGARDE & RESTAURATION", "SICHERUNG & WIEDERHERSTELLUNG", "BACKUP & RIPRISTINO", "COPIA DE SEGURIDAD & RESTAURACIÓN", "BACKUP & RESTAURAÇÃO");
        LblBackupDesc.Text = L(
            "Export all your settings, profiles, paths and shortcuts to a single file. Import it to restore everything.",
            "Exportez réglages, profils, chemins et raccourcis dans un fichier. Importez-le pour tout restaurer.",
            "Einstellungen, Profile, Pfade und Kürzel in eine Datei exportieren. Importieren zum Wiederherstellen.",
            "Esporta impostazioni, profili, percorsi e scorciatoie in un file. Importa per ripristinare tutto.",
            "Exporta ajustes, perfiles, rutas y atajos a un archivo. Importa para restaurar todo.",
            "Exporte configurações, perfis, caminhos e atalhos para um arquivo. Importe para restaurar tudo.");
        TxtBtnExport.Text = L("EXPORT", "EXPORTER", "EXPORTIEREN", "ESPORTA", "EXPORTAR", "EXPORTAR");
        TxtBtnImport.Text = L("IMPORT", "IMPORTER", "IMPORTIEREN", "IMPORTA", "IMPORTAR", "IMPORTAR");

        // ── Footer ──
        TxtBtnSave.Text  = L("Save", "Sauvegarder", "Speichern", "Salva", "Guardar", "Salvar");
        TxtBtnApply.Text = L("Apply", "Appliquer", "Anwenden", "Applica", "Aplicar", "Aplicar");

        // ── Status bar ── (delegate to UpdateStatusBar so [CAPTURE]/[LEGACY] suffix is preserved)
        UpdateStatusBar();

        // ── Tray menu ──
        if (_trayMenuShow != null) _trayMenuShow.Text = L("Show S4W", "Afficher S4W", "S4W anzeigen", "Mostra S4W", "Mostrar S4W", "Mostrar S4W");
        if (_trayMenuExit != null) _trayMenuExit.Text = L("Exit", "Quitter", "Beenden", "Esci", "Salir", "Sair");

        // ── Refresh ComboBox "None" item and hotkey button labels ──
        RefreshAppComboBox();
        UpdateHotkeyNoneLabels();

        // ── Info tooltips (built programmatically for all languages) ──
        BuildInfoTooltips();
    }

    /// <summary>Re-applies the translated "None" label to any hotkey button / field currently showing no value.</summary>
    private void UpdateHotkeyNoneLabels()
    {
        if (string.IsNullOrEmpty(_bezelFullPath)) TxtBezelPath.Text = NoneText;
        if (_switchBezelKey       == Key.None) BtnSwitchBezelHotkey.Content        = NoneText + " \u25BA";
        if (_switchBezelBackKey   == Key.None) BtnSwitchBezelBackHotkey.Content    = "\u25C4 " + NoneText;
        if (_switchProfileKey     == Key.None) BtnSwitchProfileHotkey.Content      = NoneText + " \u25BA";
        if (_switchProfileBackKey == Key.None) BtnSwitchProfileBackHotkey.Content  = "\u25C4 " + NoneText;
        if (_blurUpKey      == Key.None) BtnBlurUpHotkey.Content      = NoneText + " \u25BA";
        if (_blurDownKey    == Key.None) BtnBlurDownHotkey.Content    = "\u25C4 " + NoneText;
        if (_bloomUpKey     == Key.None) BtnBloomUpHotkey.Content     = NoneText + " \u25BA";
        if (_bloomDownKey   == Key.None) BtnBloomDownHotkey.Content   = "\u25C4 " + NoneText;
        if (_curvatureUpKey == Key.None) BtnCurvatureUpHotkey.Content = NoneText + " \u25BA";
        if (_curvatureDownKey == Key.None) BtnCurvatureDownHotkey.Content = "\u25C4 " + NoneText;
        if (_flickerUpKey   == Key.None) BtnFlickerUpHotkey.Content   = NoneText + " \u25BA";
        if (_flickerDownKey == Key.None) BtnFlickerDownHotkey.Content = "\u25C4 " + NoneText;
        if (_phosphorUpKey  == Key.None) BtnPhosphorUpHotkey.Content  = NoneText + " \u25BA";
        if (_phosphorDownKey == Key.None) BtnPhosphorDownHotkey.Content = "\u25C4 " + NoneText;
    }

    private void BuildInfoTooltips()
    {
        // ── Association Profile tooltip ──
        {
            var assocTip = BuildTooltip(
                L("Use this for PC Games & Emulators (.exe files)",
                  "Utilisez ceci pour les jeux PC et émulateurs (fichiers .exe)",
                  "Verwenden Sie dies für PC-Spiele und Emulatoren (.exe-Dateien)",
                  "Usalo per giochi PC ed emulatori (file .exe)",
                  "Úselo para juegos de PC y emuladores (archivos .exe)",
                  "Use isto para jogos de PC e emuladores (arquivos .exe)"),
                new[] {
                    L("1. Enter a profile name", "1. Entrez un nom de profil", "1. Geben Sie einen Profilnamen ein", "1. Inserisci un nome profilo", "1. Ingrese un nombre de perfil", "1. Digite um nome de perfil"),
                    L("3. Save profile", "3. Sauvegarder le profil", "3. Profil speichern", "3. Salva profilo", "3. Guardar perfil", "3. Salvar perfil")
                },
                L("S4W will automatically load this profile when the game / emulator launches.\nUse MULTI-SYSTEM EMULATOR section below only if your emulator supports multiple systems.",
                  "S4W chargera automatiquement ce profil au lancement du jeu / émulateur.\nUtilisez la section ÉMULATEUR MULTI-SYSTÈME ci-dessous uniquement si votre émulateur prend en charge plusieurs systèmes.",
                  "S4W lädt dieses Profil automatisch, wenn das Spiel / der Emulator gestartet wird.\nVerwenden Sie den Abschnitt MULTI-SYSTEM-EMULATOR unten nur, wenn Ihr Emulator mehrere Systeme unterstützt.",
                  "S4W caricherà automaticamente questo profilo all'avvio del gioco / emulatore.\nUsa la sezione EMULATORE MULTI-SISTEMA qui sotto solo se il tuo emulatore supporta più sistemi.",
                  "S4W cargará automáticamente este perfil cuando se inicie el juego / emulador.\nUse la sección EMULADOR MULTI-SISTEMA a continuación solo si su emulador admite múltiples sistemas.",
                  "S4W carregará automaticamente este perfil quando o jogo / emulador for iniciado.\nUse a seção EMULADOR MULTI-SISTEMA abaixo apenas se o seu emulador suporta múltiplos sistemas."));
            var assocSp = (StackPanel)assocTip.Content;
            // Insert sub-note right after step 1 (index 2 = after title + step1)
            assocSp.Children.Insert(2, new TextBlock
            {
                Text = L("A profile name is required to unlock the MULTI-SYSTEM SIMULATOR section.",
                         "Un nom de profil est requis pour déverrouiller la section MULTI-SYSTEM SIMULATOR.",
                         "Ein Profilname ist erforderlich, um den Bereich MULTI-SYSTEM-SIMULATOR freizuschalten.",
                         "Un nome profilo è richiesto per sbloccare la sezione MULTI-SYSTEM SIMULATOR.",
                         "Se requiere un nombre de perfil para desbloquear la sección MULTI-SYSTEM SIMULATOR.",
                         "Um nome de perfil é necessário para desbloquear a seção MULTI-SYSTEM SIMULATOR."),
                Foreground = (Brush)new BrushConverter().ConvertFrom("#6B7280")!,
                FontSize = 10, FontStyle = FontStyles.Italic,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(10, 0, 0, 3)
            });
            // Step 2 custom block with bold labels and line breaks
            {
                var gray  = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!;
                var white = System.Windows.Media.Brushes.White;
                var step2 = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3), Foreground = gray };
                step2.Inlines.Add(new System.Windows.Documents.Run(L("2. Assign the app by using :", "2. Assigner l'application via :", "2. App zuweisen über :", "2. Assegna l'app tramite :", "2. Asignar la app usando :", "2. Atribuir o app usando :")));
                step2.Inlines.Add(new System.Windows.Documents.LineBreak());
                step2.Inlines.Add(new System.Windows.Documents.Run(L("Set Path", "Définir chemin", "Pfad festlegen", "Imposta percorso", "Definir ruta", "Definir caminho")) { Foreground = white });
                step2.Inlines.Add(new System.Windows.Documents.Run(L(" : browse for the .exe via the file browser.", " : parcourir et sélectionner le .exe via l'explorateur de fichiers.", " : .exe über den Datei-Browser suchen.", " : sfoglia e seleziona il .exe tramite il browser file.", " : busca el .exe mediante el explorador de archivos.", " : procure o .exe pelo explorador de arquivos.")));
                step2.Inlines.Add(new System.Windows.Documents.LineBreak());
                step2.Inlines.Add(new System.Windows.Documents.Run(L("or", "ou", "oder", "oppure", "o", "ou")) { Foreground = (Brush)new BrushConverter().ConvertFrom("#6B7280")!, FontStyle = FontStyles.Italic });
                step2.Inlines.Add(new System.Windows.Documents.LineBreak());
                step2.Inlines.Add(new System.Windows.Documents.Run(L("Set Process", "Définir processus", "Prozess festlegen", "Imposta processo", "Definir proceso", "Definir processo")) { Foreground = white });
                step2.Inlines.Add(new System.Windows.Documents.Run(L(" : pick from the currently running processes.", " : choisir parmi les processus en cours d'exécution.", " : aus den laufenden Prozessen auswählen.", " : scegli tra i processi in esecuzione.", " : elige entre los procesos en ejecución.", " : escolha entre os processos em execução.")));
                assocSp.Children.Insert(3, step2);
            }
            var desktopNote = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0) };
            desktopNote.Inlines.Add(new System.Windows.Documents.Run(
                L("Desktop profile overlays the entire Windows screen. No application required.",
                  "Le profil Desktop recouvre tout l'écran Windows. Aucune application à associer.",
                  "Das Desktop-Profil überlagert den gesamten Windows-Bildschirm. Keine Anwendung erforderlich.",
                  "Il profilo Desktop sovrappone l'intero schermo Windows. Nessuna applicazione richiesta.",
                  "El perfil Desktop cubre toda la pantalla de Windows. No se requiere aplicación.",
                  "O perfil Desktop cobre toda a tela do Windows. Nenhum aplicativo necessário."))
            { FontWeight = FontWeights.Bold, FontStyle = FontStyles.Italic,
              Foreground = (Brush)new BrushConverter().ConvertFrom("#9CA3AF")! });
            assocSp.Children.Add(desktopNote);
            InfoAssocProfile.ToolTip = assocTip;
        }

        // ── Multi-System Emulator tooltip ──
        var msSteps = new[] {
            L("1. Enter a system name", "1. Entrez un nom de système", "1. Geben Sie einen Systemnamen ein", "1. Inserisci un nome di sistema", "1. Ingrese un nombre de sistema", "1. Digite um nome de sistema"),
            L("2. Click Set, then choose between:", "2. Cliquez sur Définir, puis choisissez entre :", "2. Klicken Sie auf Setzen, dann wählen Sie zwischen:", "2. Clicca Imposta, poi scegli tra:", "2. Haga clic en Definir, luego elija entre:", "2. Clique em Definir, depois escolha entre:")
        };
        var msSubSteps = new[] {
            L("• Create new folder : creates a subfolder named after the system.",
              "• Créer un nouveau dossier / crée un sous-dossier portant le nom du système.",
              "• Neuen Ordner erstellen / erstellt einen Unterordner mit dem Systemnamen.",
              "• Crea nuova cartella / crea una sottocartella con il nome del sistema.",
              "• Crear nueva carpeta / crea una subcarpeta con el nombre del sistema.",
              "• Criar nova pasta / cria uma subpasta com o nome do sistema."),
            L("• Use existing folder / links an existing ROM directory.",
              "• Utiliser un dossier existant / lie un répertoire ROM existant.",
              "• Vorhandenen Ordner verwenden / verknüpft ein vorhandenes ROM-Verzeichnis.",
              "• Usa cartella esistente / collega una directory ROM esistente.",
              "• Usar carpeta existente / enlaza un directorio ROM existente.",
              "• Usar pasta existente / vincula um diretório ROM existente.")
        };
        var msStep3 = L("3. Click Scan to index all ROMs in the folder.",
                         "3. Cliquez sur Scanner pour indexer toutes les ROMs du dossier.",
                         "3. Klicken Sie auf Scannen, um alle ROMs im Ordner zu indizieren.",
                         "3. Clicca Scansiona per indicizzare tutte le ROM nella cartella.",
                         "3. Haga clic en Escanear para indexar todas las ROMs de la carpeta.",
                         "3. Clique em Escanear para indexar todas as ROMs da pasta.");
        var msFooter = L("S4W will automatically load this profile when the emulator launches with a matching ROM title.",
                          "S4W chargera automatiquement ce profil lorsque l'émulateur sera lancé avec un titre de ROM correspondant.",
                          "S4W lädt dieses Profil automatisch, wenn der Emulator mit einem passenden ROM-Titel gestartet wird.",
                          "S4W caricherà automaticamente questo profilo quando l'emulatore viene avviato con un titolo ROM corrispondente.",
                          "S4W cargará automáticamente este perfil cuando el emulador se inicie con un título de ROM coincidente.",
                          "S4W carregará automaticamente este perfil quando o emulador for iniciado com um título de ROM correspondente.");

        // ── CRT Effects section tooltips ──
        InfoVhs.ToolTip = BuildCrtTooltip(
            L("VHS + Film Grain", "VHS + Grain Cinématique", "VHS + Filmkorn", "VHS + Grana Cinematografica", "VHS + Grano de Película", "VHS + Grão de Filme"),
            L("VHS simulates analog videotape degradation: scanline jitter, scrolling luma noise, NTSC dot crawl on chroma edges, head-switching artifact at the bottom, and subtle desaturation with warm shadow toning. Film Grain adds cinematic luminance noise, stronger in shadows for a realistic look.",
              "VHS simule la dégradation d'une cassette analogique : instabilité des scanlines, bandes de bruit luma, dot crawl NTSC sur les bords colorés, artefact de tête en bas d'écran, et légère désaturation avec teinte chaude dans les ombres. Grain de film ajoute un bruit de luminance cinématique, plus intense dans les ombres pour un rendu réaliste.",
              "VHS simuliert analoge Videokassettenverzerrung: Scanline-Jitter, scrollendes Luma-Rauschen, NTSC-Dot-Crawl an Chromakanten, Head-Switching-Artefakt am unteren Rand und subtile Entsättigung mit warmem Schattenton. Filmkorn fügt kinematisches Helligkeitsrauschen hinzu, stärker in den Schatten für einen realistischen Look.",
              "VHS simula il degrado di un nastro analogico: jitter delle scanline, rumore luma scorrevole, dot crawl NTSC sui bordi cromatici, artefatto di testina in basso, e leggera desaturazione con tono caldo nelle ombre. Grana cinematografica aggiunge rumore di luminanza, più intenso nelle ombre per un aspetto realistico.",
              "VHS simula la degradación de cinta analógica: jitter de scanlines, bandas de ruido luma, dot crawl NTSC en bordes de chroma, artefacto de cabezal en la parte inferior, y sutil desaturación con tono cálido en sombras. Grano de película añade ruido de luminancia cinematográfico, más intenso en sombras para un aspecto realista.",
              "VHS simula a degradação de fita analógica: jitter de scanlines, bandas de ruído luma, dot crawl NTSC em bordas de chroma, artefato de cabeça na parte inferior, e sutil dessaturação com tom quente nas sombras. Grão de filme adiciona ruído de luminância cinematográfico, mais forte nas sombras para um visual realista."));

        // ── Bezel section tooltip ──
        InfoBezel.ToolTip = BuildCrtTooltip(
            L("BEZEL",                      "BEZEL",                          "BEZEL",                      "BEZEL",                        "BEZEL",                      "BEZEL"),
            L("Bezels are PNG images placed over the screen to frame the game image, like a real arcade or TV cabinet border.\n\n• The image must match your screen resolution (e.g. 1920×1080 for a 1080p monitor).\n• Use a PNG with transparent areas where the game should show through.\n• Set the folder containing your bezel PNGs using the Set button, then use ◀ ▶ to cycle through available bezels.\n• You can also assign a hotkey in the Shortcuts tab to switch bezels without opening S4W.",
              "Les bezels sont des images PNG placées sur l'écran pour encadrer l'image du jeu, comme un vrai cabinet d'arcade ou un bord de télévision.\n\n• L'image doit correspondre à la résolution de votre écran (ex : 1920×1080 pour un moniteur Full HD).\n• Utilisez un PNG avec des zones transparentes là où le jeu doit apparaître.\n• Définissez le dossier contenant vos PNG de bezel via le bouton Set, puis utilisez ◀ ▶ pour parcourir les bezels disponibles.\n• Vous pouvez aussi assigner un raccourci clavier dans l'onglet Raccourcis pour changer de bezel sans ouvrir S4W.",
              "Bezels sind PNG-Bilder, die über den Bildschirm gelegt werden, um das Spielbild zu rahmen, wie ein echtes Arcade-Kabinett oder ein TV-Rahmen.\n\n• Das Bild muss Ihrer Bildschirmauflösung entsprechen (z. B. 1920×1080 für einen Full-HD-Monitor).\n• Verwenden Sie ein PNG mit transparenten Bereichen, wo das Spiel zu sehen sein soll.\n• Legen Sie den Ordner mit Ihren Bezel-PNGs über die Schaltfläche Set fest, dann mit ◀ ▶ durch die verfügbaren Bezels blättern.\n• Sie können auch in den Tastenkürzel-Einstellungen einen Hotkey zuweisen, um Bezels zu wechseln, ohne S4W zu öffnen.",
              "I bezel sono immagini PNG sovrapposte allo schermo per incorniciare l'immagine del gioco, come un vero cabinet arcade o un bordo TV.\n\n• L'immagine deve corrispondere alla risoluzione dello schermo (es. 1920×1080 per un monitor Full HD).\n• Usa un PNG con aree trasparenti dove il gioco deve essere visibile.\n• Imposta la cartella con i tuoi PNG bezel tramite il pulsante Set, poi usa ◀ ▶ per scorrere i bezel disponibili.\n• Puoi anche assegnare un tasto di scelta rapida nella scheda Scorciatoie per cambiare bezel senza aprire S4W.",
              "Los bezels son imágenes PNG colocadas sobre la pantalla para enmarcar la imagen del juego, como un gabinete arcade real o un borde de TV.\n\n• La imagen debe coincidir con la resolución de tu pantalla (p. ej. 1920×1080 para un monitor Full HD).\n• Usa un PNG con áreas transparentes donde debe verse el juego.\n• Establece la carpeta con tus PNG de bezel usando el botón Set, luego usa ◀ ▶ para recorrer los bezels disponibles.\n• También puedes asignar un atajo de teclado en la pestaña Atajos para cambiar bezels sin abrir S4W.",
              "Bezels são imagens PNG sobrepostas à tela para emoldurar a imagem do jogo, como um gabinete arcade real ou borda de TV.\n\n• A imagem deve corresponder à resolução da tela (ex: 1920×1080 para monitor Full HD).\n• Use um PNG com áreas transparentes onde o jogo deve aparecer.\n• Defina a pasta com seus PNGs de bezel pelo botão Set, depois use ◀ ▶ para percorrer os bezels disponíveis.\n• Você também pode atribuir um atalho de teclado na aba Atalhos para trocar bezels sem abrir o S4W."));

        // ── CRT Effects group tooltip ──
        {
            var tip = new System.Windows.Controls.ToolTip
            {
                MaxWidth = 270, Padding = new Thickness(8),
                Background = (Brush)new BrushConverter().ConvertFrom("#1F2937")!,
                BorderBrush = (Brush)new BrushConverter().ConvertFrom("#374151")!,
                BorderThickness = new Thickness(1),
                Foreground = System.Windows.Media.Brushes.White, HasDropShadow = true
            };
            var sp = new StackPanel();
            sp.Children.Add(new TextBlock
            {
                Text = L("CRT EFFECTS","CRT EFFECTS","CRT EFFEKTE","EFFETTI CRT","EFECTOS CRT","EFEITOS CRT"),
                FontWeight = FontWeights.SemiBold, Foreground = System.Windows.Media.Brushes.White,
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8)
            });
            var crtLines = new (string name, string desc)[]
            {
                (L("Curvature","Courbure","Krümmung","Curvatura","Curvatura","Curvatura"),
                 L("Bends the screen like a CRT tube.", "Bombe l'écran comme un tube cathodique.", "Wölbt den Bildschirm wie eine CRT-Röhre.", "Incurva lo schermo come un tubo CRT.", "Curva la pantalla como un tubo CRT.", "Curva a tela como um tubo CRT.")),
                (L("Blur","Flou","Unschärfe","Sfocatura","Desenfoque","Desfoque"),
                 L("Softens pixels to mimic natural CRT blur.", "Adoucit les pixels pour imiter le flou naturel du CRT.", "Weicht Pixel für natürlichen CRT-Weichzeichner auf.", "Ammorbidisce i pixel per il tipico effetto CRT.", "Suaviza píxeles imitando el desenfoque natural del CRT.", "Suaviza pixels imitando o desfoque natural do CRT.")),
                (L("Bloom","Bloom","Bloom","Bloom","Bloom","Bloom"),
                 L("Light halo around bright areas.", "Halo lumineux autour des zones lumineuses.", "Lichthof um helle Bereiche.", "Alone luminoso attorno alle zone luminose.", "Halo de luz alrededor de zonas brillantes.", "Halo de luz ao redor de áreas brilhantes.")),
                (L("Flicker","Scintillement","Flimmern","Sfarfallio","Parpadeo","Cintilação"),
                 L("Simulates CRT refresh rate flickering.", "Scintillement simulant le refresh rate CRT.", "Simuliert CRT-Bildwiederholrate-Flimmern.", "Simula lo sfarfallio della frequenza CRT.", "Simula el parpadeo de la frecuencia de actualización CRT.", "Simula a cintilação da taxa de atualização CRT.")),
                (L("Phosphor Glow","Lueur Phosphore","Phosphorleuchten","Fosforescenza","Brillo Fósforo","Brilho Fósforo"),
                 L("Phosphor persistence, pixel light trail.", "Persistance phosphorescente, traînée lumineuse des pixels.", "Phosphorpersistenz, Lichtschweif der Pixel.", "Persistenza del fosforo, scia luminosa dei pixel.", "Persistencia del fósforo, rastro de luz de píxeles.", "Persistência do fósforo, rastro de luz dos pixels.")),
                (L("Luma","Compensation Luminosité","Helligkeitskompensation","Compensazione Luminosità","Compensación Luminosidad","Compensação Luminosidade"),
                 L("Adjusts overall brightness after all effects.", "Compense la luminosité globale après les effets.", "Passt die Gesamthelligkeit nach allen Effekten an.", "Regola la luminosità complessiva dopo gli effetti.", "Ajusta el brillo general tras todos los efectos.", "Ajusta o brilho geral após todos os efeitos.")),
            };
            var grayBrush  = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!;
            foreach (var (crtName, crtDesc) in crtLines)
            {
                var tb = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3), Foreground = grayBrush };
                tb.Inlines.Add(new System.Windows.Documents.Run(crtName) { FontWeight = FontWeights.Bold, Foreground = System.Windows.Media.Brushes.White });
                tb.Inlines.Add(new System.Windows.Documents.Run(" : " + crtDesc));
                sp.Children.Add(tb);
            }
            tip.Content = sp;
            InfoCrtGroup.ToolTip = tip;
        }

        // ── CRT Effects tab header tooltip ──
        InfoCrtEffectsTab.ToolTip = BuildCrtTooltip(
            L("CRT Effects", "Effets CRT", "CRT-Effekte", "Effetti CRT", "Efectos CRT", "Efeitos CRT"),
            L("CRT effects are not available for the Desktop profile. Switch to a game or emulator profile to use them.",
              "Les effets CRT ne sont pas disponibles pour le profil Desktop. Utilisez un profil jeu ou émulateur pour les activer.",
              "CRT-Effekte sind für das Desktop-Profil nicht verfügbar. Wechseln Sie zu einem Spiel- oder Emulatorprofil, um sie zu verwenden.",
              "Gli effetti CRT non sono disponibili per il profilo Desktop. Passa a un profilo gioco o emulatore per usarli.",
              "Los efectos CRT no están disponibles para el perfil Desktop. Cambia a un perfil de juego o emulador para usarlos.",
              "Os efeitos CRT não estão disponíveis para o perfil Desktop. Mude para um perfil de jogo ou emulador para usá-los."));

        // ── Display Monitor tooltip ──
        InfoMonitor.ToolTip = BuildCrtTooltip(
            L("Scanlines Display Monitor (Desktop)",
              "Moniteur d'affichage des scanlines (Desktop)",
              "Scanlines-Anzeigebildschirm (Desktop)",
              "Monitor visualizzazione scanlines (Desktop)",
              "Monitor de visualización de scanlines (Desktop)",
              "Monitor de exibição de scanlines (Desktop)"),
            L("Selects the monitor where scanlines are displayed. Active only for the Desktop profile. In injection mode, scanlines are applied directly inside the game or emulator process and no monitor selection is needed.",
              "Sélectionne le moniteur sur lequel les scanlines s'affichent. Actif uniquement pour le profil Desktop. En mode injection, les scanlines sont appliquées directement dans le processus du jeu ou de l'émulateur, aucune sélection de moniteur n'est nécessaire.",
              "Wählt den Monitor aus, auf dem Scanlines angezeigt werden. Nur für das Desktop-Profil aktiv. Im Injektionsmodus werden Scanlines direkt im Spiel- oder Emulatorprozess angewendet, keine Monitorauswahl notwendig.",
              "Seleziona il monitor su cui vengono visualizzate le scanlines. Attivo solo per il profilo Desktop. In modalità iniezione, le scanlines vengono applicate direttamente nel processo del gioco o dell'emulatore, nessuna selezione monitor necessaria.",
              "Selecciona el monitor donde se muestran las scanlines. Activo solo para el perfil Desktop. En modo inyección, las scanlines se aplican directamente en el proceso del juego o emulador, sin necesidad de seleccionar monitor.",
              "Seleciona o monitor onde as scanlines são exibidas. Ativo apenas para o perfil Desktop. No modo de injeção, as scanlines são aplicadas diretamente no processo do jogo ou emulador, sem necessidade de selecionar monitor."));

        InfoMultiSystem.ToolTip = BuildMultiSystemTooltip(
            L("Use this only with Multi-system Emulators (e.g., higan, Ares)",
              "Utilisez ceci uniquement avec les émulateurs multi-systèmes (ex : higan, Ares)",
              "Verwenden Sie dies nur mit Multi-System-Emulatoren (z. B. higan, Ares)",
              "Usalo solo con emulatori multi-sistema (es. higan, Ares)",
              "Úselo solo con emuladores multi-sistema (p. ej., higan, Ares)",
              "Use isto apenas com emuladores multi-sistema (ex: higan, Ares)"),
            msSteps, msSubSteps, msStep3, msFooter);

        // ── Borderless Windowed tooltip ──
        InfoBorderless.ToolTip = BuildCrtTooltip(
            L("Borderless Windowed Fullscreen",
              "Plein Ecran Sans Bordure",
              "Randloses Fenster-Vollbild",
              "Schermo Intero Senza Bordi",
              "Pantalla Completa Sin Bordes",
              "Tela Cheia Sem Bordas"),
            L("Switches the game to borderless windowed fullscreen.\nSome games run in exclusive fullscreen, which hides the S4W app window. This option forces a windowed mode so you can access S4W and adjust scanlines and effects in real time while playing.",
              "Bascule le jeu en mode plein ecran fenetre sans bordure.\nCertains jeux tournent en plein ecran exclusif, ce qui masque la fenetre du logiciel S4W. Cette option force un mode fenetre pour acceder a S4W et ajuster les scanlines et effets en temps reel.",
              "Schaltet das Spiel in den randlosen Fenstermodus.\nManche Spiele laufen im exklusiven Vollbild, das das S4W-Fenster verdeckt. Diese Option erzwingt einen Fenstermodus, damit S4W sichtbar bleibt und Effekte in Echtzeit angepasst werden koennen.",
              "Passa il gioco a schermo intero senza bordi.\nAlcuni giochi usano il fullscreen esclusivo che nasconde la finestra di S4W. Questa opzione forza la modalita finestra per accedere a S4W e regolare scanline ed effetti in tempo reale.",
              "Cambia el juego a pantalla completa sin bordes.\nAlgunos juegos usan pantalla completa exclusiva que oculta la ventana de S4W. Esta opcion fuerza el modo ventana para acceder a S4W y ajustar scanlines y efectos en tiempo real.",
              "Muda o jogo para tela cheia sem bordas.\nAlguns jogos usam tela cheia exclusiva que esconde a janela do S4W. Esta opcao forca o modo janela para acessar o S4W e ajustar scanlines e efeitos em tempo real."));
    }

    private System.Windows.Controls.ToolTip BuildTooltip(string title, string[] steps, string footer)
    {
        var tip = new System.Windows.Controls.ToolTip
        {
            MaxWidth = 270, Padding = new Thickness(8),
            Background = (Brush)new BrushConverter().ConvertFrom("#1F2937")!,
            BorderBrush = (Brush)new BrushConverter().ConvertFrom("#374151")!,
            BorderThickness = new Thickness(1),
            Foreground = System.Windows.Media.Brushes.White, HasDropShadow = true
        };
        var sp = new StackPanel();
        sp.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, Foreground = System.Windows.Media.Brushes.White, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) });
        foreach (var step in steps)
            sp.Children.Add(new TextBlock { Text = step, Foreground = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3) });
        sp.Children.Add(new TextBlock { Text = footer, Foreground = (Brush)new BrushConverter().ConvertFrom("#6B7280")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0), FontSize = 11 });
        tip.Content = sp;
        return tip;
    }

    private System.Windows.Controls.ToolTip BuildCrtTooltip(string title, string description)
    {
        var tip = new System.Windows.Controls.ToolTip
        {
            MaxWidth = 260, Padding = new Thickness(8),
            Background = (Brush)new BrushConverter().ConvertFrom("#1F2937")!,
            BorderBrush = (Brush)new BrushConverter().ConvertFrom("#374151")!,
            BorderThickness = new Thickness(1),
            Foreground = System.Windows.Media.Brushes.White, HasDropShadow = true
        };
        var sp = new StackPanel();
        sp.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, Foreground = System.Windows.Media.Brushes.White, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 6) });
        sp.Children.Add(new TextBlock { Text = description, Foreground = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!, TextWrapping = TextWrapping.Wrap });
        tip.Content = sp;
        return tip;
    }

    private System.Windows.Controls.ToolTip BuildMultiSystemTooltip(string title, string[] steps, string[] subSteps, string step3, string footer)
    {
        var tip = new System.Windows.Controls.ToolTip
        {
            MaxWidth = 270, Padding = new Thickness(8),
            Background = (Brush)new BrushConverter().ConvertFrom("#1F2937")!,
            BorderBrush = (Brush)new BrushConverter().ConvertFrom("#374151")!,
            BorderThickness = new Thickness(1),
            Foreground = System.Windows.Media.Brushes.White, HasDropShadow = true
        };
        var sp = new StackPanel();
        sp.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, Foreground = System.Windows.Media.Brushes.White, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) });
        sp.Children.Add(new TextBlock { Text = steps[0], Foreground = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3) });
        sp.Children.Add(new TextBlock { Text = steps[1], Foreground = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3) });
        foreach (var sub in subSteps)
            sp.Children.Add(new TextBlock { Text = sub, Foreground = (Brush)new BrushConverter().ConvertFrom("#9CA3AF")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(12, 0, 0, 2) });
        sp.Children.Add(new TextBlock { Text = step3, Foreground = (Brush)new BrushConverter().ConvertFrom("#D1D5DB")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 3) });
        sp.Children.Add(new TextBlock { Text = footer, Foreground = (Brush)new BrushConverter().ConvertFrom("#6B7280")!, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 0), FontSize = 11 });
        tip.Content = sp;
        return tip;
    }
}
