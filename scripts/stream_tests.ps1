# End-to-end stream tests without a phone: host and sd-loopback on one machine.
# Captures the given display (nothing is changed on it) with a noise pattern on top,
# so bitrate behaviour is repeatable. Output is checked with ffprobe, then deleted.
param(
    [string]$Source = '\\.\DISPLAY1',
    [string]$Bin = "$PSScriptRoot\..\build\host\Release",
    [string]$Temp = $env:TEMP
)
$ErrorActionPreference = 'Continue'
$failed = 0

function Run-Case($name, $clientArgs, $secs, $ext, [scriptblock]$check) {
    Write-Output "=== $name"
    $h = Start-Process "$Bin\sidedroid-host.exe" -ArgumentList '--headless','--seconds',($secs + 4),'--port','27190','--source',$Source,'--test-pattern','--no-adb','--no-ipc','--no-touch','--no-discovery' -PassThru
    Start-Sleep 1
    $f = Join-Path $Temp "sd-test$ext"
    $o = & "$Bin\sd-loopback.exe" --port 27190 --seconds $secs --out $f @clientArgs
    $h.WaitForExit(($secs + 8) * 1000) | Out-Null
    $o | Select-String 'config:|RESULT' | ForEach-Object { $_.Line }
    $series = ($o | Select-String 't=').Line | ForEach-Object { ($_ -replace '^t=\s*(\d+)s\s+(\d+) kbps.*', '$2') }
    Write-Output ("kbps/s: " + ($series -join ' '))
    $probe = & ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,nb_read_frames -of csv=p=0 $f 2>&1 | Select-Object -Last 1
    Write-Output "ffprobe: $probe"
    Remove-Item $f -Force -ErrorAction SilentlyContinue
    $result = ($o | Select-String 'RESULT').Line
    $configs = @($o | Select-String 'config:').Count
    $kbps = @($series | ForEach-Object { [int]$_ })
    $ok = & $check $result $configs $kbps $probe
    if ($ok) { Write-Output "PASS" } else { Write-Output "FAIL"; $script:failed++ }
}

$framesOk = { param($r) ($r -match 'frames=(\d+)') -and ([int]$Matches[1] -gt 100) }

Run-Case 'tcp hevc' @() 8 '.h265' { param($r, $c, $k, $p) (& $framesOk $r) -and $p -match '^hevc' }
Run-Case 'tcp h264' @('--h264') 6 '.h264' { param($r, $c, $k, $p) (& $framesOk $r) -and $p -match '^h264' }

# Clean UDP must not degrade: one configuration only, and real throughput.
Run-Case 'udp clean' @('--udp') 10 '.h265' {
    param($r, $c, $k, $p) (& $framesOk $r) -and $c -eq 1 -and $p -match '^hevc' -and ($k | Measure-Object -Maximum).Maximum -gt 5000
}

# 5% random loss: FEC and NACK repair it, stream stays decodable and is not downgraded.
Run-Case 'udp 5% loss' @('--udp','--drop','5') 12 '.h265' {
    param($r, $c, $k, $p) (& $framesOk $r) -and $c -eq 1 -and $p -match '^hevc'
}

# A 6 Mbps path: bitrate must settle under the limit and frames must keep flowing.
Run-Case 'udp 6 Mbps limit' @('--udp','--limit-kbps','6000') 30 '.h265' {
    param($r, $c, $k, $p)
    $tail = $k | Select-Object -Last 8
    (& $framesOk $r) -and (($tail | Measure-Object -Average).Average -lt 6600) -and (($tail | Measure-Object -Average).Average -gt 800)
}

if ($failed) { Write-Output "$failed case(s) failed"; exit 1 }
Write-Output 'all stream tests passed'
