# Measures what the overlay costs against a generated library.
#
# See docs/PERF.md for what the scenarios are and how to read the numbers.
#
# Everything runs in a data folder of its own under the system temp
# directory (--data-dir), so a measurement can neither touch nor be
# perturbed by the real library and config. A copy of Spickzettel already
# running is refused rather than stopped: it is someone's, with a note
# perhaps half typed; quit it first. Only the copy started here is looked
# at, by its process id, and stopped; no key is pressed (--edit-mode brings
# the overlay up). A build that does not take --data-dir is refused.
#
#   .\measure.ps1 -Exe <path> [-Scenario name]
#                 [-Seconds 8] [-Screenshot <path>]
#
# The CPU figure is a percentage of ONE core. The overlay presents on vsync,
# so the frame budget is one refresh interval - read the actual rate off the
# debug overlay with -ShowFps rather than assuming 60 Hz; this machine runs at 120,
# where the budget is 8.3 ms, not 16.7.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$LibrarySource,
    [int]$Seconds = 8,
    [int]$Repeat = 1,
    [int]$SettleSeconds = 6,
    # Turns on the debug overlay, whose third line in edit mode is the frame
    # rate and frame time (see ScreenChrome::DrawInputReadout). -ShowFpsHud
    # is its name from when that line was the input options HUD's.
    [Alias('ShowFpsHud')][switch]$ShowFps,
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
# one session, and re-adding a type that already exists is a hard error. Named
# anew whenever its members change, for the same reason.
#
# Every question is asked of the copy this script started, by its process id:
# another copy's overlay - one under another name, started meanwhile - is not
# the one being measured, and nothing here sends a key, which would reach
# whichever copy holds the hotkey.
if (-not ([System.Management.Automation.PSTypeName]'SzMeasureWindows').Type) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SzMeasureWindows {
    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    // Whether process `pid` has a visible top-level window of class `cls`.
    public static bool HasVisible(int pid, string cls) {
        bool found = false;
        EnumWindows((h, p) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner != (uint)pid || !IsWindowVisible(h)) { return true; }
            var name = new StringBuilder(64);
            GetClassNameW(h, name, name.Capacity);
            if (name.ToString() == cls) { found = true; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // Its overlay, up.
    public static bool OverlayUp(int pid) { return HasVisible(pid, "SpickzettelOverlayWindowClass"); }
    // A message box of its own: the app said why it would not start - a copy
    // already running, an unreadable library, an option it does not know.
    public static bool SaysSomething(int pid) { return HasVisible(pid, "#32770"); }
}
"@
}

# A config with the input options all off: what is measured is the overlay
# drawing, not the input grab's hooks. Everything else - the hotkeys
# included, which nothing here presses - is the shipped default, so a
# measurement reflects the app as delivered.
function New-MeasurementConfig {
    param([string]$Path, [bool]$Fps)
    $json = @"
{
  "version": 1,
  "drawing": { "strokeColor": "#FF0000", "strokeWidth": 3.0 },
  "appearance": { "showItemBorders": true },
  "overview": { "showStrokes": true, "showBitmaps": false },
  "behavior": { "dontStealFocus": false, "softwarePointer": false, "rawMouseInput": false,
                "dontForwardKeystrokes": false, "counterRawMouseInput": false, "freezeScreen": false },
  "diagnostics": { "showDebugOverlay": $($Fps.ToString().ToLower()) }
}
"@
    Set-Content -Path $Path -Value $json -Encoding UTF8
}

# A build from before --data-dir ignores it and opens the real config and
# library. The option's name is in every build that takes it - the table of
# options is text in the executable - so one without it is refused before it
# starts. To compare against an older build, run it under another Windows
# account instead.
$exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes((Resolve-Path $Exe).Path))
foreach ($option in '--data-dir', '--edit-mode') {
    if (-not $exeText.Contains($option)) {
        throw "$Exe does not take $option, so it would run on the real config and library: a build from before it"
    }
}
$exeText = $null

# The single-instance lock is per user, not per folder: with a copy up, the
# one started here refuses to start (see main_win32.cpp). Said here first;
# a copy under another name, or one started meanwhile, is caught below by
# the refusal it causes.
$running = Get-Process -Name Spickzettel* -ErrorAction SilentlyContinue
if ($running) {
    throw "Spickzettel is running ($(($running | ForEach-Object { $_.ProcessName }) -join ', ')): quit it first"
}

# The real files, to check afterwards that nothing here wrote to them.
$realFiles = @(
    (Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'Spickzettel\config.json'),
    (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Spickzettel\library.db'),
    (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Spickzettel\library.db-wal')
)
$realBefore = $realFiles | ForEach-Object { if (Test-Path $_) { (Get-Item $_).LastWriteTimeUtc } else { $null } }

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ("sz_measure_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null
if ($LibrarySource -and (Test-Path $LibrarySource)) {
    Copy-Item $LibrarySource (Join-Path $sandbox 'library.db') -Force
}
New-MeasurementConfig -Path (Join-Path $sandbox 'config.json') -Fps ([bool]$ShowFps)

$proc = $null
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    # Arguments, not ArgumentList, which Windows PowerShell's .NET lacks.
    # The sandbox's path holds no quotes to escape. --edit-mode brings the
    # overlay up by itself, whatever the start would do otherwise (a first
    # run shows it, a start on a library does not): pressing the edit
    # hotkey instead was a coin flip between showing and hiding it, and a
    # global key goes to whichever copy holds it.
    $psi.Arguments = "--data-dir `"$sandbox`" --edit-mode"
    $proc = [System.Diagnostics.Process]::Start($psi)

    # Its overlay, and nothing else: up, or the start failed - the process
    # gone, or a message box of its own saying why.
    $deadline = (Get-Date).AddSeconds(15)
    while (-not [SzMeasureWindows]::OverlayUp($proc.Id) -and (Get-Date) -lt $deadline) {
        if ($proc.HasExited) { throw "Spickzettel exited at its start, code $($proc.ExitCode)" }
        if ([SzMeasureWindows]::SaysSomething($proc.Id)) {
            throw "Spickzettel would not start - a copy already running? See the message it shows"
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not [SzMeasureWindows]::OverlayUp($proc.Id)) { throw "the overlay never became visible" }
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
        if (-not [SzMeasureWindows]::OverlayUp($proc.Id)) { throw "the overlay was hidden before sample $($i + 1)" }
        $proc.Refresh()
        $cpu0 = $proc.TotalProcessorTime
        $wall0 = Get-Date
        Start-Sleep -Seconds $Seconds
        $proc.Refresh()
        $cpu1 = $proc.TotalProcessorTime
        $wall1 = Get-Date
        if (-not [SzMeasureWindows]::OverlayUp($proc.Id)) { throw "the overlay was hidden during sample $($i + 1)" }
        $samples += 100.0 * ($cpu1 - $cpu0).TotalMilliseconds / ($wall1 - $wall0).TotalMilliseconds
    }
    $sorted = $samples | Sort-Object
    $pct = $sorted[[int][math]::Floor($sorted.Count / 2)]

    if ($Screenshot) {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        # With -ShowFps the fps/frame-time readout is the debug overlay's
        # third line, top left, so a small top-left crop captures it.
        $w = if ($ShowFps) { 680 } else { $bounds.Width }
        $h = if ($ShowFps) { 200 } else { $bounds.Height }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
        $bmp.Save($Screenshot, [System.Drawing.Imaging.ImageFormat]::Png)
        $gfx.Dispose(); $bmp.Dispose()
    }

    [PSCustomObject]@{
        Label       = $Label
        CpuPctOfOne = [math]::Round($pct, 1)
        # What one frame costs in CPU, at whatever rate the display actually
        # refreshes - so this needs the real rate, read off the debug overlay. Once
        # CpuPctOfOne approaches 100 the render thread is saturated and frames
        # are being missed, and the fps line is the thing to read instead.
        CpuMsPerFrame = [math]::Round($pct / 100.0 * (1000.0 / $RefreshHz), 2)
        RefreshHz   = $RefreshHz
        Spread      = [math]::Round(($sorted[-1] - $sorted[0]), 1)
        Threads     = $proc.Threads.Count
        PrivateMB   = [math]::Round($proc.PrivateMemorySize64 / 1MB, 0)
    }
} finally {
    # This copy alone, and forced: its data is the sandbox's, about to go.
    if ($proc -and -not $proc.HasExited) {
        $proc.Kill()
        $proc.WaitForExit(5000) | Out-Null
    }
    Remove-Item -Recurse -Force $sandbox -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt $realFiles.Count; $i++) {
        $after = if (Test-Path $realFiles[$i]) { (Get-Item $realFiles[$i]).LastWriteTimeUtc } else { $null }
        if ($after -ne $realBefore[$i]) { Write-Warning "$($realFiles[$i]) changed during the measurement" }
    }
}
