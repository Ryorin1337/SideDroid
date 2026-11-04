# Starts the packaged console next to a short-lived host. Both end on their own.
param(
    [string]$Dist = "$PSScriptRoot\..\out\dist",
    [string]$Source = '\\.\DISPLAY1'
)
$out = Join-Path $env:TEMP "sd-self-check-$PID.txt"
$h = Start-Process "$Dist\sidedroid-host.exe" -ArgumentList '--headless','--seconds','14','--no-touch','--no-discovery','--no-adb','--source',$Source -PassThru
Start-Sleep 1
$c = Start-Process "$Dist\console\SideDroid.exe" -ArgumentList '--self-check',$out -PassThru
$deadline = (Get-Date).AddSeconds(30)
while (-not (Test-Path $out) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
$ok = $false
if (Test-Path $out) {
    Start-Sleep -Milliseconds 300
    $text = Get-Content $out -Raw
    $text
    $ok = $text -match 'window=shown' -and $text -match 'hostReachable=true'
    Remove-Item $out -Force
} else {
    'console did not report back in time'
}
$h.WaitForExit(20000) | Out-Null
"host exit code: $($h.ExitCode)"
if ($ok) { 'console self check passed' } else { 'console self check FAILED'; exit 1 }
