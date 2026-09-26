# Measures what the overlay costs against a generated library.
#
# See docs/PERF.md for what the scenarios are and how to read the numbers.
#
# Everything runs against an isolated APPDATA under the system temp
# directory, so a measurement can never touch - or be perturbed by - the real
# library and config.
#
#   .\measure.ps1 -Exe <path> [-Scenario name] [-Mode tessellated|polyline|rasterized]
#                 [-Seconds 8] [-Screenshot <path>]
#
# The CPU figure is a percentage of ONE core. The overlay presents on vsync,
# so the frame budget is one refresh interval - read the actual rate off the
# HUD with -ShowFpsHud rather than assuming 60 Hz; this machine runs at 120,
# where the budget is 8.3 ms, not 16.7.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$LibrarySource,
    [ValidateSet('tessellated', 'polyline', 'rasterized')][string]$Mode = 'tessellated',
    [int]$Seconds = 8,
    [int]$Repeat = 1,
    [int]$SettleSeconds = 6,
    # Turns on the input-options HUD, whose first line is the frame rate and
    # frame time (see ScreenChrome::DrawInputOptionsHud). Note this is NOT
    # `showDebugOverlay`, which draws the cyan border and the canvas/mouse
    # readout and carries no timing at all. The HUD also claims the number
    # keys while it is up, so don't send digits during a measurement.
    [switch]$ShowFpsHud,
    [string]$Screenshot,
    [string]$Label = '',
    # Only used to turn the CPU percentage into milliseconds per frame.
    # Defaults to whatever the primary display is actually set to rather than
    # to 60, which this machine is not.
    [int]$RefreshHz = (Get-CimInstance Win32_VideoController |
                        Where-Object { $_.CurrentRefreshRate } |
                        Select-Object -First 1 -ExpandProperty CurrentRefreshRate)
)

$ErrorActionPreference = 'Stop'

# Guarded, not just error-suppressed: a matrix runs this script many times in
# one session, and re-adding a type that already exists is a hard error.
if (-not ([System.Management.Automation.PSTypeName]'SzMeasure').Type) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SzMeasure {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public static void Chord(byte k) {
        keybd_event(0x11, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(k, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        keybd_event(k, 0, 2, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero); keybd_event(0x11, 0, 2, IntPtr.Zero);
    }
    public static bool Visible() {
        IntPtr h = FindWindowW("SpickzettelOverlayWindowClass", null);
        return h != IntPtr.Zero && IsWindowVisible(h);
    }
}
"@
}

# A config with the input options all off: this drives the app with synthetic
# input, and the grab/software-pointer paths would fight it. Everything else
# is the shipped default, so a measurement reflects the app as delivered.
function New-MeasurementConfig {
    param([string]$Path, [string]$Mode, [bool]$Hud)
    $json = @"
{
  "version": 1,
  "hotkeys": { "editMode": "Ctrl+Alt+O", "viewMode": "Ctrl+Alt+V", "quickCapture": "Ctrl+Alt+C", "silentCapture": "Ctrl+Alt+S" },
  "drawing": { "strokeColor": "#FF0000", "strokeWidth": 3.0, "renderMode": "$Mode" },
  "appearance": { "showItemBorders": true },
  "overview": { "showStrokes": true, "showBitmaps": false },
  "behavior": { "dontStealFocus": false, "softwarePointer": false, "rawMouseInput": false,
                "dontForwardKeystrokes": false, "counterRawMouseInput": false, "freezeScreen": false },
  "diagnostics": { "showDebugOverlay": false, "showInputOptionsHud": $($Hud.ToString().ToLower()) }
}
"@
    Set-Content -Path $Path -Value $json -Encoding UTF8
}

Get-Process -Name Spickzettel* -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ("sz_measure_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path (Join-Path $sandbox 'Spickzettel') | Out-Null
if ($LibrarySource -and (Test-Path $LibrarySource)) {
    Copy-Item $LibrarySource (Join-Path $sandbox 'Spickzettel\library.db') -Force
}
New-MeasurementConfig -Path (Join-Path $sandbox 'Spickzettel\config.json') -Mode $Mode -Hud ([bool]$ShowFpsHud)

try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["APPDATA"] = $sandbox
    $proc = [System.Diagnostics.Process]::Start($psi)

    # Getting the overlay up is deliberately careful rather than "sleep, then
    # press the hotkey". The app shows itself on some starts and not others
    # (a first run puts the welcome note up), so a blind press is a coin flip
    # between showing it and hiding it again - which is exactly what produced
    # samples of 0.0%, the app sitting in the tray with the loop parked in
    # GetMessage and nothing being drawn at all.
    $deadline = (Get-Date).AddSeconds(15)
    while (-not [SzMeasure]::Visible() -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    if (-not [SzMeasure]::Visible()) {
        [SzMeasure]::Chord(0x4F)
        $deadline = (Get-Date).AddSeconds(10)
        while (-not [SzMeasure]::Visible() -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    }
    if (-not [SzMeasure]::Visible()) { throw "the overlay never became visible" }
    Start-Sleep -Seconds $SettleSeconds

    # Sampled several times and reported as the median: one sample is easily
    # a couple of percent out, and occasionally much more if something else
    # on the machine happens to want the GPU during it.
    $samples = @()
    for ($i = 0; $i -lt $Repeat; $i++) {
        # Checked either side of the window, not just before: a sample taken
        # across the overlay being hidden measures a process that stopped
        # drawing halfway through, and reads as a number rather than as an
        # error.
        if (-not [SzMeasure]::Visible()) { throw "the overlay was hidden before sample $($i + 1)" }
        $proc.Refresh()
        $cpu0 = $proc.TotalProcessorTime
        $wall0 = Get-Date
        Start-Sleep -Seconds $Seconds
        $proc.Refresh()
        $cpu1 = $proc.TotalProcessorTime
        $wall1 = Get-Date
        if (-not [SzMeasure]::Visible()) { throw "the overlay was hidden during sample $($i + 1)" }
        $samples += 100.0 * ($cpu1 - $cpu0).TotalMilliseconds / ($wall1 - $wall0).TotalMilliseconds
    }
    $sorted = $samples | Sort-Object
    $pct = $sorted[[int][math]::Floor($sorted.Count / 2)]

    if ($Screenshot) {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        # With -ShowFpsHud the fps/frame-time readout is the top line of the
        # HUD panel at (14,14), so a small top-left crop captures it.
        $w = if ($ShowFpsHud) { 680 } else { $bounds.Width }
        $h = if ($ShowFpsHud) { 200 } else { $bounds.Height }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
        $bmp.Save($Screenshot, [System.Drawing.Imaging.ImageFormat]::Png)
        $gfx.Dispose(); $bmp.Dispose()
    }

    [PSCustomObject]@{
        Label       = $Label
        Mode        = $Mode
        CpuPctOfOne = [math]::Round($pct, 1)
        # What one frame costs in CPU, at whatever rate the display actually
        # refreshes - so this needs the real rate, read off the HUD. Once
        # CpuPctOfOne approaches 100 the render thread is saturated and frames
        # are being missed, and the fps line is the thing to read instead.
        CpuMsPerFrame = [math]::Round($pct / 100.0 * (1000.0 / $RefreshHz), 2)
        RefreshHz   = $RefreshHz
        Spread      = [math]::Round(($sorted[-1] - $sorted[0]), 1)
        Threads     = $proc.Threads.Count
        PrivateMB   = [math]::Round($proc.PrivateMemorySize64 / 1MB, 0)
    }
} finally {
    Get-Process -Name Spickzettel* -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Remove-Item -Recurse -Force $sandbox -ErrorAction SilentlyContinue
}
