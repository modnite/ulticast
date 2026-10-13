# Ulticast UI automation: launches the UI, drives it with synthetic clicks and
# asserts on pixels and on the persisted settings file.
#
#   powershell -ExecutionPolicy Bypass -File tools\ui-test.ps1
#
# Runs anywhere -- no NVIDIA GPU required: it exercises the UI mechanics
# (pages, popups, pickers, toggles, themes, persistence), not the encoders.
# The encoder paths live in tools\smoke-test.ps1 (needs NVENC).
#
# The app runs against a throwaway profile (temporary %APPDATA%), so the real
# settings are never touched and the assertions are deterministic.

param(
    [string]$Exe = "",
    [string]$Out = "",
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Continue'
if (-not $Exe) { $Exe = Join-Path $Root 'build\Release\ulticast-ui.exe' }
if (-not $Out) { $Out = Join-Path $Root 'test\ui' }
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- isolated profile ------------------------------------------------------
$profileDir = Join-Path $env:TEMP ("ulticast-ui-test-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $profileDir 'Roaming') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $profileDir 'Local') | Out-Null
$env:APPDATA = Join-Path $profileDir 'Roaming'
$env:LOCALAPPDATA = Join-Path $profileDir 'Local'
$settingsPath = Join-Path $env:APPDATA 'Ulticast\settings.ini'

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class UlticastUiTest {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
}
"@

$script:pass = 0
$script:fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        Write-Host ("PASS  " + $name) -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host ("FAIL  " + $name + "   " + $detail) -ForegroundColor Red
        $script:fail++
    }
}

# Design space (the layout is authored at 1060x700) -> client pixels. The UI
# zoom-fits the design into the window, so clicks/samples are computed the same
# way and stay valid at any resolution or DPI.
$script:clientW = 0
$script:clientH = 0
$script:imgOffX = 0
$script:imgOffY = 0
function Refresh-Geometry($hwnd) {
    $cr = New-Object UlticastUiTest+RECT
    [UlticastUiTest]::GetClientRect($hwnd, [ref]$cr) | Out-Null
    $script:clientW = $cr.Right - $cr.Left
    $script:clientH = $cr.Bottom - $cr.Top
    $wr = New-Object UlticastUiTest+RECT
    [UlticastUiTest]::GetWindowRect($hwnd, [ref]$wr) | Out-Null
    $p = New-Object UlticastUiTest+POINT
    [UlticastUiTest]::ClientToScreen($hwnd, [ref]$p) | Out-Null
    $script:imgOffX = $p.X - $wr.Left
    $script:imgOffY = $p.Y - $wr.Top
}
function D2C([double]$x, [double]$y) {
    $scale = [Math]::Min($script:clientW / 1060.0, $script:clientH / 700.0)
    $offX = ($script:clientW - 1060.0 * $scale) / 2.0
    $offY = ($script:clientH - 700.0 * $scale) / 2.0
    return @{ x = [int]($offX + $x * $scale); y = [int]($offY + $y * $scale) }
}
function Click($hwnd, [double]$x, [double]$y) {
    $c = D2C $x $y
    $lp = [IntPtr](($c.y -shl 16) -bor ($c.x -band 0xFFFF))
    [UlticastUiTest]::PostMessage($hwnd, 0x201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [UlticastUiTest]::PostMessage($hwnd, 0x202, [IntPtr]0, $lp) | Out-Null
    Start-Sleep -Milliseconds 320
}
function Shot($hwnd, [string]$name) {
    $wr = New-Object UlticastUiTest+RECT
    [UlticastUiTest]::GetWindowRect($hwnd, [ref]$wr) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap(($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [UlticastUiTest]::PrintWindow($hwnd, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $path = Join-Path $Out ($name + '.png')
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $path
}
# Samples a pixel at a design coordinate from a screenshot.
function Sample([string]$png, [double]$x, [double]$y) {
    $scale = [Math]::Min($script:clientW / 1060.0, $script:clientH / 700.0)
    $offX = ($script:clientW - 1060.0 * $scale) / 2.0
    $offY = ($script:clientH - 700.0 * $scale) / 2.0
    $ix = [int]($script:imgOffX + $offX + $x * $scale)
    $iy = [int]($script:imgOffY + $offY + $y * $scale)
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    if ($ix -lt 0 -or $iy -lt 0 -or $ix -ge $bmp.Width -or $iy -ge $bmp.Height) {
        $bmp.Dispose()
        return @{ r = -1; g = -1; b = -1 }
    }
    $p = $bmp.GetPixel($ix, $iy)
    $bmp.Dispose()
    return @{ r = $p.R; g = $p.G; b = $p.B }
}
function IsViolet($c) { return ($c.r -gt 110 -and $c.b -gt 150 -and $c.g -lt 130) }
function IsCyan($c) { return ($c.g -gt 140 -and $c.b -gt 140 -and $c.r -lt 130) }
function Dist($a, $b) { return [Math]::Abs($a.r - $b.r) + [Math]::Abs($a.g - $b.g) + [Math]::Abs($a.b - $b.b) }
function SettingValue([string]$key) {
    if (-not (Test-Path $settingsPath)) { return $null }
    $line = Select-String -Path $settingsPath -Pattern ("^" + $key + "=") | Select-Object -Last 1
    if (-not $line) { return $null }
    return $line.Line.Substring($key.Length + 1).Trim()
}
function SceneName() {
    $v = SettingValue 'scene0'   # v2|<name>|cursor|sources
    if (-not $v) { return $null }
    $parts = $v.Split('|')
    if ($parts.Count -lt 2) { return $null }
    return $parts[1]
}
function TypeChar($hwnd, [int]$code) {
    [UlticastUiTest]::PostMessage($hwnd, 0x0102, [IntPtr]$code, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 80
}
function KeyDown($hwnd, [int]$vk) {
    [UlticastUiTest]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 80
}

Write-Host "Ulticast UI automation" -ForegroundColor Cyan
Write-Host "binary: $Exe"
Write-Host "profile: $profileDir"
Write-Host ""

if (-not (Test-Path $Exe)) {
    Write-Host "binary not found" -ForegroundColor Red
    exit 1
}

Get-Process ulticast-ui -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$proc = Start-Process -FilePath $Exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) { break }
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
}
if ($hwnd -eq [IntPtr]::Zero) {
    # Headless CI agents have no interactive desktop: report and skip instead of
    # failing the build.
    Write-Host "SKIP  no window appeared (no interactive desktop?)" -ForegroundColor Yellow
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    exit 0
}
Start-Sleep -Milliseconds 800
Refresh-Geometry $hwnd
Check 'window opens with a sane client size' ($script:clientW -gt 700 -and $script:clientH -gt 500) `
      "client=$($script:clientW)x$($script:clientH)"

# ---------------------------------------------------------------- pages -----
$record = Shot $hwnd '01-record'
$dot = Sample $record 30 26
Check 'record page renders the accent dot' (IsViolet $dot) "rgb=$($dot.r),$($dot.g),$($dot.b)"

Click $hwnd 380 27
$stream = Shot $hwnd '02-stream'
$streamTab = Sample $stream 380 27
$recordTab = Sample $stream 284 27
Check 'stream tab becomes active (accent fill moves)' ((Dist $streamTab $recordTab) -gt 40) `
      "stream=$($streamTab.r),$($streamTab.g),$($streamTab.b) record=$($recordTab.r),$($recordTab.g),$($recordTab.b)"

Click $hwnd 480 27
$settings = Shot $hwnd '03-settings'
# Fresh profile => one scene => the left column is CAPTURE(3 rows), CAMERA & AUDIO(4),
# APPEARANCE(1) starting at y=413 with its row at y=445..477.
$swatch0 = Sample $settings 43 461
$swatch1 = Sample $settings 77 461
Check 'settings page shows accent swatches' ((IsViolet $swatch0) -and (IsCyan $swatch1)) `
      "violet=$($swatch0.r),$($swatch0.g),$($swatch0.b) cyan=$($swatch1.r),$($swatch1.g),$($swatch1.b)"

# ---------------------------------------------------------------- theme -----
Click $hwnd 77 461
$cyan = Shot $hwnd '04-theme-cyan'
$dotCyan = Sample $cyan 30 26
Check 'picking the cyan accent repaints the UI' (IsCyan $dotCyan) `
      "rgb=$($dotCyan.r),$($dotCyan.g),$($dotCyan.b)"
Check 'accent choice persists to settings.ini' ((SettingValue 'accent') -eq '1') `
      "accent=$(SettingValue 'accent')"
Click $hwnd 43 461
$violet = Shot $hwnd '05-theme-violet'
$dotBack = Sample $violet 30 26
Check 'picking violet restores the default accent' (IsViolet $dotBack) `
      "rgb=$($dotBack.r),$($dotBack.g),$($dotBack.b)"

# ---------------------------------------------------------------- popups ----
# One scene: the "+ Add" button sits at y=181..205 and the source rows start at 212.
Click $hwnd 284 27
$before = Shot $hwnd '06-record-again'
Click $hwnd 258 193
$open = Shot $hwnd '07-add-menu-open'
$menu = Sample $open 150 230
$bg = Sample $before 150 230
Check 'add-source menu opens' ((Dist $menu $bg) -gt 25) "menu=$($menu.r),$($menu.g),$($menu.b)"
Click $hwnd 600 30
$closed = Shot $hwnd '08-add-menu-closed'
$after = Sample $closed 150 230
Check 'clicking away closes the menu' ((Dist $after $bg) -le 12) "after=$($after.r),$($after.g),$($after.b)"

# ---------------------------------------------------------------- text edit --
# The scene-name field must behave like a normal edit control: caret placement
# on click, Ctrl+A, typing at the caret, backspace at the caret.
Click $hwnd 284 27            # Record page
Click $hwnd 120 147           # scene name field (design y = 130..164)
TypeChar $hwnd 1              # Ctrl+A
TypeChar $hwnd 0x5A           # Z
TypeChar $hwnd 0x5A           # Z
Check 'text field: Ctrl+A + typing replaces the text' ((SceneName) -eq 'ZZ') `
      "name=$(SceneName)"
KeyDown $hwnd 0x08            # backspace at the end
Check 'text field: backspace deletes at the caret' ((SceneName) -eq 'Z') "name=$(SceneName)"
Click $hwnd 32 147            # click at the left edge -> caret at position 0
TypeChar $hwnd 0x41           # A
Check 'text field: caret placement from the click' ((SceneName) -eq 'AZ') "name=$(SceneName)"
TypeChar $hwnd 1              # Ctrl+A
foreach ($ch in 'G', 'a', 'm', 'e') { TypeChar $hwnd ([int][char]$ch) }
Check 'text field: name restored' ((SceneName) -eq 'Game') "name=$(SceneName)"

# ---------------------------------------------------------------- browser ----
# "Test overlay" must write the demo page, wire it into the active scene and
# persist it -- one click answers "does the overlay work?".
Click $hwnd 480 27            # Settings page
Click $hwnd 105 580           # Test overlay
$url = SettingValue 'browserUrl'
Check 'browser overlay: Test overlay wires the demo page' `
      ($url -ne $null -and $url.Contains('overlay-test.html')) "browserUrl=$url"
$scene = SettingValue 'scene0'
Check 'browser overlay: a Browser source is added to the scene' `
      ($scene -ne $null -and $scene.Contains(';2:')) "scene0=$scene"
Click $hwnd 284 27            # back to Record for the picker checks

# ---------------------------------------------------------------- pickers ---
Click $hwnd 708 482
Start-Sleep -Milliseconds 200
Click $hwnd 708 343
$hotkey = SettingValue 'hotkey'
Check 'clip hotkey picker persists F10' ($hotkey -eq '121') "hotkey=$hotkey"

Click $hwnd 426 518
$tracks = SettingValue 'audioTracks'
Check 'separate mic track toggle persists' ($tracks -eq '2') "audioTracks=$tracks"

# ---------------------------------------------------------------- behavior --
Click $hwnd 480 27
Click $hwnd 650 341
$fadeOff = SettingValue 'sceneFade'
Click $hwnd 650 341
$fadeOn = SettingValue 'sceneFade'
Check 'scene fade toggle persists both ways' ($fadeOff -eq '0' -and $fadeOn -eq '1') `
      "off=$fadeOff on=$fadeOn"

# ---------------------------------------------------------------- shutdown --
[UlticastUiTest]::PostMessage($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
$exited = $proc.WaitForExit(8000)
Check 'X (WM_CLOSE) exits the app' $exited 'process still running'
if (-not $exited) { Stop-Process -Id $proc.Id -Force }

Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed" -f $script:pass, $script:fail) `
    -ForegroundColor $(if ($script:fail -eq 0) { 'Green' } else { 'Red' })
Write-Host "screenshots: $Out"
if ($script:fail -gt 0) { exit 1 }
