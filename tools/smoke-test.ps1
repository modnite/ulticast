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

# ------------------------------------------------------------ 7. noise gate
# Same source, same duration: the gated mix must never be louder than the
# ungated one (the gate only ever attenuates). With room noise present the
# delta is several dB; with a silent mic it is 0.
& $exe --backend wgc --monitor 0 --seconds 4 --out "$t\gate.mkv" --mic --gate -20 2>&1 | Out-Null
& $exe --backend wgc --monitor 0 --seconds 4 --out "$t\nogate.mkv" --mic 2>&1 | Out-Null
function MeanVolume([string]$file) {
    $txt = ffmpeg -i $file -af volumedetect -f null - 2>&1 | Out-String
    if ($txt -match 'mean_volume:\s*(-?[0-9.]+)') { return [double]$Matches[1] }
    return -99.0
}
$gv = MeanVolume "$t\gate.mkv"
$nv = MeanVolume "$t\nogate.mkv"
Check 'noise gate: gated mix is never louder than ungated' ($gv -le ($nv + 0.5)) `
      ("gate=" + $gv + " dB ungated=" + $nv + " dB delta=" + [math]::Round($nv - $gv, 1) + " dB")

# Compressor: with a low threshold the -1 dB safety clip must cap the peaks.
& $exe --backend wgc --monitor 0 --seconds 4 --out "$t\comp.mkv" --mic --comp -35 2>&1 | Out-Null
function PeakVolume([string]$file) {
    $txt = ffmpeg -i $file -af volumedetect -f null - 2>&1 | Out-String
    if ($txt -match 'max_volume:\s*(-?[0-9.]+)') { return [double]$Matches[1] }
    return -99.0
}
$pv = PeakVolume "$t\comp.mkv"
Check 'compressor: peaks are limited (safety clip at -1 dB)' ($pv -le -0.9) `
      ("max=" + $pv + " dB")

# Mic monitoring: the render endpoint must initialize (audio is played to the
# default output while the recording keeps its own audio).
$monOut = & $exe --backend wgc --monitor 0 --seconds 3 --out "$t\mon.mkv" --mic --monitor-mic 2>&1 | Out-String
Check 'monitor mic: output endpoint initializes' ($monOut -match '\[audio\] monitor:') `
      'no monitor line in output'

# ------------------------------------------------------ 9. DirectShow camera
# The DirectShow fallback (for virtual cameras MF cannot see) must capture real
# frames through the SampleGrabber graph.
& $exe --backend wgc --monitor 0 --seconds 3 --out "$t\ds.mkv" --webcam --force-dshow 2>&1 | Out-Null
$frames = [int](ProbeInfo "$t\ds.mkv" 'stream=nb_read_frames' | Select-Object -First 1)
Check 'dshow camera: DirectShow fallback captures frames' ($frames -ge 60) "frames=$frames"

# ------------------------------------------------------- 10. window capture
# Window capture must work for odd-sized windows too (the canvas rounds to even
# and the frame is drawn scaled). Notepad is a plain GDI window with an odd
# client size, so it exercises exactly that path.
$np = Start-Process notepad -PassThru
Start-Sleep -Seconds 3
& $exe --backend wgc --window "Notepad" --seconds 3 --out "$t\wincap.mkv" 2>&1 | Out-Null
if (ExtractFrame "$t\wincap.mkv" 2 "$t\wincap.png") {
    $bmp = New-Object System.Drawing.Bitmap "$t\wincap.png"
    $sum = 0.0
    $n = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 12) {
        for ($x = 0; $x -lt $bmp.Width; $x += 12) {
            $p = $bmp.GetPixel($x, $y)
            $sum += ($p.R + $p.G + $p.B) / 3.0
            $n++
        }
    }
    $luma = if ($n -gt 0) { $sum / $n } else { 0 }
    $bmp.Dispose()
    Check 'window capture: real window content (odd size, scaled canvas)' ($luma -gt 60) `
          ("luma=" + [math]::Round($luma, 1))
} else {
    Check 'window capture: real window content (odd size, scaled canvas)' $false 'frame extraction failed'
}
if ($np -and -not $np.HasExited) { Stop-Process -Id $np.Id -Force }

# --------------------------------------------------------------- 11. cursor
# The scene "Show Cursor" toggle must reach *both* capture backends: DXGI
# composites the cursor ourselves, WGC bakes it into the frame. Capture the same
# desktop twice concurrently (one with, one without) so the cursor is the only
# difference. (Both captures run at the same wall-clock time, so desktop content
# changes cannot mask the result -- and it does not matter where the pointer is.)
[Smoke]::SetCursorPos(600, 400) | Out-Null
Start-Sleep -Milliseconds 300
foreach ($backend in @('dxgi', 'wgc')) {
    $a = Start-Process -FilePath $exe -ArgumentList "--backend $backend --monitor 0 --seconds 5 --out `"$t\cursor-$backend.mkv`"" -PassThru -NoNewWindow
    Start-Sleep -Milliseconds 400
    $b = Start-Process -FilePath $exe -ArgumentList "--backend $backend --monitor 0 --seconds 5 --no-cursor --out `"$t\nocursor-$backend.mkv`"" -PassThru -NoNewWindow
    $a.WaitForExit()
    $b.WaitForExit()
    if ((ExtractFrame "$t\cursor-$backend.mkv" 2 "$t\cursor-$backend.png") -and
        (ExtractFrame "$t\nocursor-$backend.mkv" 2 "$t\nocursor-$backend.png")) {
        $ia = New-Object System.Drawing.Bitmap "$t\cursor-$backend.png"
        $ib = New-Object System.Drawing.Bitmap "$t\nocursor-$backend.png"
        $found = $false
        $diff = 0
        for ($y = 0; $y -lt $ia.Height -and -not $found; $y += 2) {
            for ($x = 0; $x -lt $ia.Width -and -not $found; $x += 2) {
                $pa = $ia.GetPixel($x, $y)
                $pb = $ib.GetPixel($x, $y)
                $d = [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)
                if ($d -gt 90) {
                    $diff++
                    if ($diff -ge 6) { $found = $true }   # the arrow is ~100+ pixels
                }
            }
        }
        $ia.Dispose()
        $ib.Dispose()
        Check "$backend cursor: toggle changes the captured frame" $found "diffpixels=$diff"
    } else {
        Check "$backend cursor: toggle changes the captured frame" $false 'frame extraction failed'
    }
}

# ------------------------------------------------------------------ summary
Write-Host ''
Write-Host ("RESULT: " + $script:pass + " passed, " + $script:fail + " failed") -ForegroundColor ($(if ($script:fail -eq 0) { 'Green' } else { 'Red' }))
exit $script:fail
