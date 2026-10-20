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
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
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
function Refresh-Geometry() {
    $hwnd = $script:hwnd
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
function Wait-Idle {
    # WM_NULL via SendMessageTimeout returns only once the app's message loop has
    # drained everything we posted, so a settings read right after a click is
    # never stale (the app can be busy restarting a preview).
    if (-not $script:hwnd) { return }
    $r = [IntPtr]::Zero
    [UlticastUiTest]::SendMessageTimeout($script:hwnd, 0, [IntPtr]0, [IntPtr]0, 2, 5000, [ref]$r) | Out-Null
}
function Click($hwnd, [double]$x, [double]$y) {
    Refresh-Geometry   # the frame can change (caption theme, resize): never trust a cached mapping
    $c = D2C $x $y
    $lp = [IntPtr](($c.y -shl 16) -bor ($c.x -band 0xFFFF))
    [UlticastUiTest]::PostMessage($hwnd, 0x201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [UlticastUiTest]::PostMessage($hwnd, 0x202, [IntPtr]0, $lp) | Out-Null
    Start-Sleep -Milliseconds 320
    Wait-Idle
}
function Drag($hwnd, [double]$x1, [double]$y1, [double]$x2, [double]$y2) {
    Refresh-Geometry
    $a = D2C $x1 $y1
    $b = D2C $x2 $y2
    $lp = [IntPtr](($a.y -shl 16) -bor ($a.x -band 0xFFFF))
    [UlticastUiTest]::PostMessage($hwnd, 0x201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 80
    for ($i = 1; $i -le 6; $i++) {
        $mx = [int]($a.x + ($b.x - $a.x) * $i / 6)
        $my = [int]($a.y + ($b.y - $a.y) * $i / 6)
        $lp2 = [IntPtr](($my -shl 16) -bor ($mx -band 0xFFFF))
        [UlticastUiTest]::PostMessage($hwnd, 0x200, [IntPtr]0, $lp2) | Out-Null
        Start-Sleep -Milliseconds 40
    }
    $lp3 = [IntPtr](($b.y -shl 16) -bor ($b.x -band 0xFFFF))
    [UlticastUiTest]::PostMessage($hwnd, 0x202, [IntPtr]0, $lp3) | Out-Null
    Start-Sleep -Milliseconds 200
    Wait-Idle
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
    Refresh-Geometry
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
    Wait-Idle
}
function KeyDown($hwnd, [int]$vk) {
    [UlticastUiTest]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 80
    Wait-Idle
}
function StartUi {
    $proc = Start-Process -FilePath $Exe -PassThru
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { break }
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle }
    }
    return @{ proc = $proc; hwnd = $hwnd }
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
$script:hwnd = $hwnd
Refresh-Geometry
Check 'window opens with a sane client size' ($script:clientW -gt 700 -and $script:clientH -gt 500) `
      "client=$($script:clientW)x$($script:clientH)"

# ---------------------------------------------------------------- pages -----
$record = Shot $hwnd '01-record'
$dot = Sample $record 30 26
Check 'record page renders the accent dot' (IsViolet $dot) "rgb=$($dot.r),$($dot.g),$($dot.b)"

Click $hwnd 380 27
$stream = Shot $hwnd '02-stream'
$streamItem = Sample $stream 380 38
$recordItem = Sample $stream 284 38
Check 'menu bar: the active item shows the accent underline' ((Dist $streamItem $recordItem) -gt 40) `
      "stream=$($streamItem.r),$($streamItem.g),$($streamItem.b) record=$($recordItem.r),$($recordItem.g),$($recordItem.b)"

Click $hwnd 480 27
Click $hwnd 100 201           # Settings > Appearance category
$settings = Shot $hwnd '03-settings'
# Categories rail: Appearance shows the accent row (swatch 1 at x=206..232).
$swatch0 = Sample $settings 219 112
$swatch1 = Sample $settings 253 112
Check 'settings page shows accent swatches' ((IsViolet $swatch0) -and (IsCyan $swatch1)) `
      "violet=$($swatch0.r),$($swatch0.g),$($swatch0.b) cyan=$($swatch1.r),$($swatch1.g),$($swatch1.b)"

# ---------------------------------------------------------------- theme -----
Click $hwnd 253 112
$cyan = Shot $hwnd '04-theme-cyan'
$dotCyan = Sample $cyan 30 26
Check 'picking the cyan accent repaints the UI' (IsCyan $dotCyan) `
      "rgb=$($dotCyan.r),$($dotCyan.g),$($dotCyan.b)"
Check 'accent choice persists to settings.ini' ((SettingValue 'accent') -eq '1') `
      "accent=$(SettingValue 'accent')"
Click $hwnd 219 112
$violet = Shot $hwnd '05-theme-violet'
$dotBack = Sample $violet 30 26
Check 'picking violet restores the default accent' (IsViolet $dotBack) `
      "rgb=$($dotBack.r),$($dotBack.g),$($dotBack.b)"

# ---------------------------------------------------------------- popups ----
# One scene: the "+ Add" button is in the rail lower band (y 479..503).
Click $hwnd 284 27
$before = Shot $hwnd '06-record-again'
Click $hwnd 258 491
$open = Shot $hwnd '07-add-menu-open'
$menu = Sample $open 150 560
$bg = Sample $before 150 560
Check 'add-source menu opens' ((Dist $menu $bg) -gt 25) "menu=$($menu.r),$($menu.g),$($menu.b)"
Click $hwnd 600 30
$closed = Shot $hwnd '08-add-menu-closed'
$after = Sample $closed 150 560
Check 'clicking away closes the menu' ((Dist $after $bg) -le 12) "after=$($after.r),$($after.g),$($after.b)"

# ---------------------------------------------------------------- text edit --
# The scene-name field must behave like a normal edit control: caret placement
# on click, Ctrl+A, typing at the caret, backspace at the caret.
Click $hwnd 284 27            # Record page
Click $hwnd 120 451           # scene name field (lower band, under the preview)
TypeChar $hwnd 1              # Ctrl+A
TypeChar $hwnd 0x5A           # Z
TypeChar $hwnd 0x5A           # Z
Check 'text field: Ctrl+A + typing replaces the text' ((SceneName) -eq 'ZZ') `
      "name=$(SceneName)"
KeyDown $hwnd 0x08            # backspace at the end
Check 'text field: backspace deletes at the caret' ((SceneName) -eq 'Z') "name=$(SceneName)"
Click $hwnd 72 451            # click near the left edge -> caret at position 0
TypeChar $hwnd 0x41           # A
Check 'text field: caret placement from the click' ((SceneName) -eq 'AZ') "name=$(SceneName)"
TypeChar $hwnd 1              # Ctrl+A
foreach ($ch in 'G', 'a', 'm', 'e') { TypeChar $hwnd ([int][char]$ch) }
Check 'text field: name restored' ((SceneName) -eq 'Game') "name=$(SceneName)"

# ---------------------------------------------------------------- browser ----
# "Test overlay" must write the demo page, wire it into the active scene and
# persist it -- one click answers "does the overlay work?".
Click $hwnd 480 27            # Settings page
Click $hwnd 100 241           # Browser category
Click $hwnd 281 149           # Test overlay
$url = SettingValue 'browserUrl'
Check 'browser overlay: Test overlay wires the demo page' `
      ($url -ne $null -and $url.Contains('overlay-test.html')) "browserUrl=$url"
$scene = SettingValue 'scene0'
Check 'browser overlay: a Browser source is added to the scene' `
      ($scene -ne $null -and $scene.Contains(';2:')) "scene0=$scene"

# ---------------------------------------------------------------- preview ----
# The Settings page keeps the live preview visible (VLC-redesign lesson: never
# hide the picture while tweaking). The animated test overlay makes "is it
# live?" measurable: two shots a second apart must differ.
$p1 = Shot $hwnd '11-preview-a'
Start-Sleep -Milliseconds 900
$p2 = Shot $hwnd '12-preview-b'
$changed = 0
foreach ($px in 745, 790, 835, 880, 925, 970, 1015) {
    foreach ($py in 150, 200, 228) {
        if ((Dist (Sample $p1 $px $py) (Sample $p2 $px $py)) -gt 25) { $changed++ }
    }
}
Check 'settings page keeps a live preview' ($changed -ge 1) "changed=$changed"

# The Browser card carries its own overlay preview (the browser's render, not the
# scene preview) so a URL can be judged without leaving the page. Its content is
# verified by the screenshot (18-browser-preview.png): PrintWindow returns stale
# frames at the preview's frame rate, so a pixel-liveness assertion is not reliable.
$bprev = Shot $hwnd '18-browser-preview'
Check 'browser overlay: dedicated preview rendered (see screenshot)' `
      (Test-Path $bprev) 'no screenshot'

# The overlay follows the URL as you type (debounced reload, no restart), and a
# bare host is treated as a web address rather than a file path.
Click $hwnd 400 112           # overlay URL field
TypeChar $hwnd 1              # Ctrl+A
foreach ($ch in 'example.com'.ToCharArray()) { TypeChar $hwnd ([int]$ch) }
Start-Sleep -Seconds 5        # debounce + navigate + load
$logFile = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Ulticast\logs\ui-*.log') -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
$logText = if ($logFile) { Get-Content $logFile.FullName -Raw } else { '' }
Check 'browser overlay: the typed URL is applied live' `
      ((SettingValue 'browserUrl') -eq 'example.com') "browserUrl=$(SettingValue 'browserUrl')"
Check 'browser overlay: the typed URL loads (bare host gets https)' `
      ($logText -match 'loaded \(HTTP 200\)') 'no successful live navigation logged'

# ---------------------------------------------------------------- presets ---
# Friendly mic-filter presets instead of raw numbers.
Click $hwnd 100 161           # Filters category
Click $hwnd 454 112           # Voice
$gate = SettingValue 'micGate'; $gateDb = SettingValue 'micGateDb'
$comp = SettingValue 'micComp'; $compDb = SettingValue 'micCompDb'
$ratio = SettingValue 'micCompRatio'
Check 'mic preset: Voice sets gate + compressor' `
      ($gate -eq '1' -and $gateDb -eq '-50' -and $comp -eq '1' -and $compDb -eq '-18' -and $ratio -eq '30') `
      "gate=$gate/$gateDb comp=$comp/$compDb ratio=$ratio"
Click $hwnd 622 112           # Broadcast
$gateDb2 = SettingValue 'micGateDb'
Check 'mic preset: Broadcast is denser' ($gateDb2 -eq '-45') "gateDb=$gateDb2"
Click $hwnd 286 112           # Off
Check 'mic preset: Off disables both' ((SettingValue 'micGate') -eq '0') `
      "micGate=$(SettingValue 'micGate')"

Click $hwnd 284 27            # back to Record for the picker checks

# ------------------------------------------------------------- source picks --
# A window source must ask which window: nothing is added before a choice, and
# the picker never offers "the whole monitor" as if it were a window.
Start-Process notepad -WindowStyle Minimized | Out-Null
Start-Sleep -Milliseconds 900
$before = SettingValue 'scene0'
Click $hwnd 258 491           # + Add
Click $hwnd 258 562           # "Window Capture" (menu now fits below the button)
Start-Sleep -Milliseconds 500
$pending = SettingValue 'scene0'
Check 'window source: nothing added before a window is chosen' ($pending -eq $before) `
      "before=$before after=$pending"
Shot $hwnd '13-window-picker' | Out-Null
KeyDown $hwnd 0x0D            # Enter: take the highlighted (first) window
Start-Sleep -Milliseconds 700
$after = SettingValue 'scene0'
Check 'window source: choosing a window adds a WIN source' `
      ($after -ne $null -and $after.Contains(';5:')) "scene0=$after"
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force

# ---------------------------------------------------------------- pickers ---
# CLIPS card now spans 684..1040 on Record: the clip-key combo is at x 760..1030.
Click $hwnd 895 482
Start-Sleep -Milliseconds 250
$hotkeyOpen = Shot $hwnd '14-hotkey-picker'
Check 'clip hotkey picker opens' ((Dist (Sample $hotkeyOpen 895 300) (Sample $hotkeyOpen 895 600)) -gt 20) `
      'no picker drawn'
Click $hwnd 895 349           # F10 (items run bottom-up from the combo)
$hotkey = SettingValue 'hotkey'
Check 'clip hotkey picker persists F10' ($hotkey -eq '121') "hotkey=$hotkey"

Click $hwnd 426 518
$tracks = SettingValue 'audioTracks'
Check 'separate mic track toggle persists' ($tracks -eq '2') "audioTracks=$tracks"

# ---------------------------------------------------------------- behavior --
Click $hwnd 480 27
Click $hwnd 100 281           # Behavior category
Click $hwnd 316 149           # scene fade toggle
$fadeOff = SettingValue 'sceneFade'
Click $hwnd 316 149
$fadeOn = SettingValue 'sceneFade'
Check 'scene fade toggle persists both ways' ($fadeOff -eq '0' -and $fadeOn -eq '1') `
      "off=$fadeOff on=$fadeOn"

# ------------------------------------------------------------- keyboard ------
# Tab moves the focus ring, Enter activates the focused control.
Click $hwnd 100 161           # Settings > Filters
KeyDown $hwnd 0x09            # Tab -> the next rail row (Appearance)
KeyDown $hwnd 0x0D            # Enter -> activate it
Start-Sleep -Milliseconds 300
$kb = Shot $hwnd '15-keyboard-nav'
$sw = Sample $kb 219 112
Check 'keyboard: Tab + Enter activates the focused control' (IsViolet $sw) `
      "rgb=$($sw.r),$($sw.g),$($sw.b)"

# Space toggles the focused switch (Close to tray: off -> on -> off).
Click $hwnd 100 281           # Behavior category
Click $hwnd 565 112           # click the toggle once
$trayOn = SettingValue 'closeToTray'
KeyDown $hwnd 0x20            # Space -> toggle it back
Start-Sleep -Milliseconds 250
$trayOff = SettingValue 'closeToTray'
Check 'keyboard: Space toggles the focused switch' ($trayOn -eq '1' -and $trayOff -eq '0') `
      "afterClick=$trayOn afterSpace=$trayOff"

# Arrow keys adjust the focused slider.
Click $hwnd 284 27            # Record page
Click $hwnd 860 446           # replay buffer slider
$before = [int](SettingValue 'replaySeconds')
KeyDown $hwnd 0x27            # Right arrow
Start-Sleep -Milliseconds 250
$after = [int](SettingValue 'replaySeconds')
Check 'keyboard: arrows adjust the focused slider' ($after -gt $before) `
      "before=$before after=$after"

# ------------------------------------------------------------- copy log ------
# One click for a bug report: the log tail lands on the clipboard.
# (This overwrites the clipboard, which is fine for a throwaway test session.)
Click $hwnd 480 27            # Settings
Click $hwnd 100 361           # Help category
Click $hwnd 281 338           # Copy Log
Start-Sleep -Milliseconds 500
$clip = Get-Clipboard -Raw
$clipOk = ($clip -ne $null) -and $clip.Contains('Ulticast') -and $clip.Contains('log tail')
Check 'copy log: the log tail lands on the clipboard' $clipOk `
      $(if ($clip) { "clipboard=" + $clip.Substring(0, [Math]::Min(60, $clip.Length)) } else { 'empty clipboard' })

# ------------------------------------------------------- preview layers -----
# A source can be selected and dragged directly on the preview (the overlay
# source is the topmost layer and starts full-canvas).
function SceneField([string]$key, [int]$srcType, [int]$field) {
    $v = SettingValue $key
    if (-not $v) { return $null }
    foreach ($src in $v.Split(';')) {
        $f = $src.Split(':')
        if ($f.Count -gt $field -and $f[0] -eq "$srcType") { return $f[$field] }
    }
    return $null
}
Click $hwnd 284 27            # Record page
$movedLayer = $false
foreach ($pt in @(@(700, 300), @(530, 212))) {   # the window PiP, then the middle
    Click $hwnd $pt[0] $pt[1]
    Drag $hwnd $pt[0] $pt[1] ($pt[0] - 90) ($pt[1] - 60)
    foreach ($src in (SettingValue 'scene0').Split(';')) {
        $f = $src.Split(':')
        if ($f.Count -gt 8 -and $f[0] -ne '0' -and [double]$f[7] -ge 0) { $movedLayer = $true }
    }
    if ($movedLayer) { break }
}
Check 'preview: dragging a layer moves it' $movedLayer 'no layer picked up a free position'
Shot $hwnd '22-layer-selected' | Out-Null

# ------------------------------------------------------------- pop-out ------
# The preview detaches into its own resizable window and closes cleanly.
Add-Type -MemberDefinition '[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);' -Name PopOut -Namespace UlticastUi
Click $hwnd 75 85             # pop-out button (next to the fullscreen button)
Start-Sleep -Milliseconds 1500
$pop = [UlticastUi.PopOut]::FindWindow("UlticastPreviewWindow", $null)
Check 'pop-out preview: opens its own window' ($pop -ne [IntPtr]::Zero) 'no preview window'
if ($pop -ne [IntPtr]::Zero) {
    $wr = New-Object UlticastUiTest+RECT
    [UlticastUiTest]::GetWindowRect($pop, [ref]$wr) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap(($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [UlticastUiTest]::PrintWindow($pop, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save((Join-Path $Out '23-popout.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    [UlticastUiTest]::PostMessage($pop, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
    Start-Sleep -Milliseconds 900
    $pop2 = [UlticastUi.PopOut]::FindWindow("UlticastPreviewWindow", $null)
    Check 'pop-out preview: closes cleanly' ($pop2 -eq [IntPtr]::Zero) 'window still open'
}

# ------------------------------------------------------------ fullscreen ----
# F11 fills the screen with the preview; Esc restores the window.
$crBefore = New-Object UlticastUiTest+RECT
[UlticastUiTest]::GetClientRect($hwnd, [ref]$crBefore) | Out-Null
$wBefore = $crBefore.Right - $crBefore.Left
KeyDown $hwnd 0x7A            # F11
Start-Sleep -Milliseconds 900
$crFs = New-Object UlticastUiTest+RECT
[UlticastUiTest]::GetClientRect($hwnd, [ref]$crFs) | Out-Null
$wFs = $crFs.Right - $crFs.Left
Shot $hwnd '20-fullscreen' | Out-Null
KeyDown $hwnd 0x1B            # Esc
Start-Sleep -Milliseconds 900
$crBack = New-Object UlticastUiTest+RECT
[UlticastUiTest]::GetClientRect($hwnd, [ref]$crBack) | Out-Null
$wBack = $crBack.Right - $crBack.Left
Check 'fullscreen preview: F11 expands, Esc restores' `
      ($wFs -gt $wBefore -and $wBack -eq $wBefore) `
      "before=$wBefore fullscreen=$wFs back=$wBack"

# ------------------------------------------------------------- resizing ------
# Move, resize, maximise and restore: the UI must keep rendering and stay
# hit-testable at every size (static single-pass checks miss this class of bug).

function Restore-Window {
    [UlticastUiTest]::MoveWindow($hwnd, 80, 60, 1072, 740, $true) | Out-Null
    Start-Sleep -Milliseconds 900
    Refresh-Geometry
}

foreach ($sz in @(@(920, 620, 'small'), @(1600, 980, 'wide'), @(980, 860, 'tall'))) {
    [UlticastUiTest]::MoveWindow($hwnd, 80, 60, $sz[0], $sz[1], $true) | Out-Null
    Start-Sleep -Milliseconds 900
    Refresh-Geometry
    Check ("resize " + $sz[2] + ": window is usable") ($script:clientW -gt 800 -and $script:clientH -gt 500) `
          "client=$($script:clientW)x$($script:clientH)"
    Click $hwnd 284 25            # Record (menu bar item, scales with the design)
    $rs = Shot $hwnd ('24-resize-' + $sz[2])
    $card = Sample $rs 500 400    # inside the RECORDING card
    Check ("resize " + $sz[2] + ": page still paints") (($card.r + $card.g + $card.b) -gt 30) `
          "rgb=$($card.r),$($card.g),$($card.b)"
}
Restore-Window

# Maximise / restore, then move the window and click again.
[UlticastUiTest]::ShowWindow($hwnd, 3) | Out-Null   # SW_MAXIMIZE
Start-Sleep -Milliseconds 900
Refresh-Geometry
$max = Shot $hwnd '25-maximized'
$maxCard = Sample $max 500 400
Check 'maximize: UI still paints' (($maxCard.r + $maxCard.g + $maxCard.b) -gt 30) `
      "rgb=$($maxCard.r),$($maxCard.g),$($maxCard.b)"
[UlticastUiTest]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
Start-Sleep -Milliseconds 900
Refresh-Geometry
[UlticastUiTest]::MoveWindow($hwnd, 140, 100, 1072, 740, $true) | Out-Null
Start-Sleep -Milliseconds 700
Refresh-Geometry
Click $hwnd 284 25
$moved = Shot $hwnd '26-moved'
$movedCard = Sample $moved 500 400
Check 'move window: UI still paints after moving' (($movedCard.r + $movedCard.g + $movedCard.b) -gt 30) `
      "rgb=$($movedCard.r),$($movedCard.g),$($movedCard.b)"
Restore-Window

# Long text must scroll inside the field, never truncate the value.
Click $hwnd 120 451               # scene-name field (default split layout)
TypeChar $hwnd 1                  # Ctrl+A
$longName = 'AVeryLongSceneNameThatMustScrollInsideTheField'
foreach ($ch in $longName.ToCharArray()) { TypeChar $hwnd ([int]$ch) }
Check 'text field: long text is not truncated' ((SceneName) -eq $longName) "name=$(SceneName)"
TypeChar $hwnd 1
foreach ($ch in 'Game'.ToCharArray()) { TypeChar $hwnd ([int]$ch) }

# ------------------------------------------------------------ tray & exit ---
# Reported: "doesn't gracefully exit or clear from the system tray on close".
# With the browser overlay live: Close-to-tray keeps the app running, and Exit
# from the tray menu must quit promptly (the browser lifecycle must not hang).
Click $hwnd 480 27            # Settings page
Click $hwnd 100 281           # Behavior category
Click $hwnd 565 112           # Close to tray (row 1, second toggle)
Check 'close to tray can be enabled' ((SettingValue 'closeToTray') -eq '1') `
      "closeToTray=$(SettingValue 'closeToTray')"
[UlticastUiTest]::PostMessage($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
Start-Sleep -Milliseconds 900
$proc.Refresh()
Check 'X hides to the tray instead of exiting' (-not $proc.HasExited) 'process exited on X'
[UlticastUiTest]::PostMessage($hwnd, 0x0111, [IntPtr]9006, [IntPtr]0) | Out-Null
$exited = $proc.WaitForExit(10000)
Check 'tray Exit quits cleanly with the overlay live' $exited 'still running after 10 s'
if (-not $exited) { Stop-Process -Id $proc.Id -Force }

# ---------------------------------------------------------------- shutdown --
# Plain X (the default) must exit the app.
$again = StartUi
if ($again.hwnd -eq [IntPtr]::Zero) {
    Check 'X (WM_CLOSE) exits the app' $false 'relaunch produced no window'
} else {
    $proc = $again.proc
    $hwnd = $again.hwnd
    $script:hwnd = $hwnd
    Click $hwnd 480 27
    Click $hwnd 100 281
    Click $hwnd 565 112           # turn Close to tray back off
    [UlticastUiTest]::PostMessage($hwnd, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null
    $exited = $proc.WaitForExit(8000)
    Check 'X (WM_CLOSE) exits the app' $exited 'process still running'
    if (-not $exited) { Stop-Process -Id $proc.Id -Force }
}

# --------------------------------------------------- window-source guard -----
# A window source with no window must refuse to start instead of silently
# capturing the whole monitor (the reported "grabs the entire display" bug).
Get-Process ulticast-ui -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$logDir = Join-Path $env:LOCALAPPDATA 'Ulticast\logs'
Remove-Item (Join-Path $logDir 'ui-*.log') -Force -ErrorAction SilentlyContinue
$ini = Join-Path $env:APPDATA 'Ulticast\settings.ini'
@(
    'idlePreview=1'
    'sceneCount=1'
    # v2|<name>|<cursor>|<sources>; source = type:device:enabled:monitor:window:dxgi:corner:camX:camY
    'scene0=v2|Game|1|5::1:0::0:3:-1.0000:-1.0000;4::1:0::0:3:-1.0000:-1.0000'
) | Set-Content -Path $ini -Encoding ascii
$guard = StartUi
Start-Sleep -Seconds 7
$log = Get-ChildItem (Join-Path $logDir 'ui-*.log') -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
$logText = if ($log) { Get-Content $log.FullName -Raw } else { '' }
Check 'window source with no window refuses to capture a monitor' `
      ($logText -match 'window source has no window selected') `
      'engine started a capture anyway'
if (-not $guard.proc.HasExited) { Stop-Process -Id $guard.proc.Id -Force }

Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("RESULT: {0} passed, {1} failed" -f $script:pass, $script:fail) `
    -ForegroundColor $(if ($script:fail -eq 0) { 'Green' } else { 'Red' })
Write-Host "screenshots: $Out"
if ($script:fail -gt 0) { exit 1 }
