# Ulticast smoke test -- exercises every major path end to end and verifies
# the output files (frame counts, durations, and actual pixels where it matters).
#
#   powershell -ExecutionPolicy Bypass -File tools\smoke-test.ps1
#
# Requires ffmpeg/ffprobe on PATH. Takes about two minutes.

param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Continue'
$exe = Join-Path $Root 'build\Release\ulticast.exe'
$t = Join-Path $Root 'test\smoke'
New-Item -ItemType Directory -Force -Path $t | Out-Null
Remove-Item "$t\*" -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Smoke {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
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
function ProbeInfo([string]$file, [string]$entries, [string]$streams = '') {
    $args = @('-v', 'error', '-count_frames', '-show_entries', $entries, '-of', 'csv=p=0')
    if ($streams -ne '') { $args = @('-v', 'error', '-count_frames', '-select_streams', $streams, '-show_entries', $entries, '-of', 'csv=p=0') }
    return (& ffprobe @args $file)
}
function ExtractFrame([string]$file, [double]$at, [string]$png) {
    ffmpeg -v error -y -ss $at -i $file -frames:v 1 $png 2>$null | Out-Null
    return (Test-Path $png)
}

Write-Host "Ulticast smoke test" -ForegroundColor Cyan
Write-Host "binary: $exe"
Write-Host ""

# ---------------------------------------------------------------- 1. record
& $exe --backend wgc --monitor 0 --seconds 3 --out "$t\rec.mkv" 2>&1 | Out-Null
$frames = [int](ProbeInfo "$t\rec.mkv" 'stream=nb_read_frames' | Select-Object -First 1)
Check 'record: >= 150 video frames in 3 s' ($frames -ge 150) "frames=$frames"

# ----------------------------------------------------------------- 2. pause
& $exe --backend wgc --monitor 0 --seconds 8 --pause-at 2 --resume-at 4 --out "$t\pause.mkv" 2>&1 | Out-Null
$dur = [double](ProbeInfo "$t\pause.mkv" 'format=duration')
Check 'pause: 8 s run with 2 s paused -> ~6 s media' ([math]::Abs($dur - 6.0) -lt 0.4) "duration=$dur"

# ------------------------------------------------------------------ 3. RTMP
$ff = Start-Process ffmpeg -ArgumentList @('-hide_banner','-listen','1','-i','rtmp://127.0.0.1:1940/live/smoke','-c','copy','-y',"$t\stream.flv") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
& $exe --backend wgc --monitor 0 --rtmp "rtmp://127.0.0.1:1940/live/smoke" --seconds 4 2>&1 | Out-Null
Start-Sleep -Seconds 2
if (-not $ff.HasExited) { $ff.Kill() }
$codecs = (ProbeInfo "$t\stream.flv" 'stream=codec_name' | Sort-Object) -join '+'
Check 'rtmp: received H.264 + AAC' ($codecs -eq 'aac+h264') "codecs=$codecs"

# ----------------------------------------------------------------- 4. replay
& $exe --backend wgc --monitor 0 --out none --replay --replay-seconds 5 --replay-prefix "$t\Clip" --seconds 8 --clip-at 5 2>&1 | Out-Null
$clip = Get-ChildItem "$t\Clip*.mkv" -ErrorAction SilentlyContinue | Select-Object -First 1
Check 'replay: clip file saved' ($null -ne $clip) 'no clip file'
if ($clip) {
    $cdur = [double](ProbeInfo $clip.FullName 'format=duration')
    $cstreams = (ProbeInfo $clip.FullName 'stream=codec_type' | Sort-Object) -join '+'
    Check 'replay: clip ~4-5 s with video+audio' (($cdur -ge 3.5) -and ($cdur -le 5.5) -and ($cstreams -eq 'audio+video')) "duration=$cdur streams=$cstreams"
}

# ------------------------------------------------------------- 5. dual output
$ff = Start-Process ffmpeg -ArgumentList @('-hide_banner','-listen','1','-i','rtmp://127.0.0.1:1941/live/dual','-c','copy','-y',"$t\dual.flv") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
& $exe --backend wgc --monitor 0 --rtmp "rtmp://127.0.0.1:1941/live/dual" --also-record "$t\dual.mkv" --seconds 4 2>&1 | Out-Null
Start-Sleep -Seconds 2
if (-not $ff.HasExited) { $ff.Kill() }
$recCodec = ProbeInfo "$t\dual.mkv" 'stream=codec_name' 'v:0' | Select-Object -First 1
$strCodec = ProbeInfo "$t\dual.flv" 'stream=codec_name' 'v:0' | Select-Object -First 1
Check 'dual output: HEVC file + H.264 stream at once' (($recCodec -eq 'hevc') -and ($strCodec -eq 'h264')) "file=$recCodec stream=$strCodec"

# ------------------------------------------------------- 6. browser overlay
$overlay = Join-Path $Root 'test\overlay.html'
if (Test-Path $overlay) {
    & $exe --backend wgc --monitor 0 --seconds 4 --browser $overlay --out "$t\browser.mkv" 2>&1 | Out-Null
    if (ExtractFrame "$t\browser.mkv" 3 "$t\browser.png") {
        $bmp = New-Object System.Drawing.Bitmap "$t\browser.png"
        $px = $bmp.GetPixel(950, 300)   # red square in the test page
        $bmp.Dispose()
        Check 'browser overlay: red square composited' (($px.R -gt 170) -and ($px.G -lt 70) -and ($px.B -lt 70)) "pixel=$px"
    } else {
        Check 'browser overlay: red square composited' $false 'frame extraction failed'
    }
} else {
    Write-Host 'SKIP  browser overlay (test\overlay.html missing)' -ForegroundColor Yellow
}

# --------------------------------------------------------------- 7. cursor
# Park the cursor, then capture the same desktop twice *concurrently* (one run
# with cursor compositing, one without). Because both captures happen at the
# same wall-clock time, the desktop content is identical and the only pixel
# difference is the cursor -- no fragile white-pixel hunting, and it does not
# matter where the pointer actually ends up.
[Smoke]::SetCursorPos(600, 400) | Out-Null
Start-Sleep -Milliseconds 300
$p1 = Start-Process -FilePath $exe -ArgumentList "--backend dxgi --monitor 0 --seconds 5 --out `"$t\cursor.mkv`"" -PassThru -NoNewWindow
Start-Sleep -Milliseconds 400
$p2 = Start-Process -FilePath $exe -ArgumentList "--backend dxgi --monitor 0 --seconds 5 --no-cursor --out `"$t\nocursor.mkv`"" -PassThru -NoNewWindow
$p1.WaitForExit()
$p2.WaitForExit()
if ((ExtractFrame "$t\cursor.mkv" 2 "$t\cursor.png") -and
    (ExtractFrame "$t\nocursor.mkv" 2 "$t\nocursor.png")) {
    $a = New-Object System.Drawing.Bitmap "$t\cursor.png"
    $b = New-Object System.Drawing.Bitmap "$t\nocursor.png"
    $found = $false
    $diff = 0
    for ($y = 0; $y -lt $a.Height -and -not $found; $y += 2) {
        for ($x = 0; $x -lt $a.Width -and -not $found; $x += 2) {
            $pa = $a.GetPixel($x, $y)
            $pb = $b.GetPixel($x, $y)
            $d = [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)
            if ($d -gt 90) {
                $diff++
                if ($diff -ge 30) { $found = $true }   # an arrow is hundreds of pixels
            }
        }
    }
    $a.Dispose()
    $b.Dispose()
    Check 'dxgi cursor: arrow composited (concurrent captures, only the cursor differs)' $found "diffpixels=$diff"
} else {
    Check 'dxgi cursor: arrow composited (concurrent captures, only the cursor differs)' $false 'frame extraction failed'
}

# ------------------------------------------------------------------ summary
Write-Host ''
Write-Host ("RESULT: " + $script:pass + " passed, " + $script:fail + " failed") -ForegroundColor ($(if ($script:fail -eq 0) { 'Green' } else { 'Red' }))
exit $script:fail
