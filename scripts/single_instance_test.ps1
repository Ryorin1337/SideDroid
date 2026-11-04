# Guards against processes starting each other. Uses a test port, a test pipe and a
# test lock, so an installed SideDroid is not affected. Everything here ends by itself.
param(
    [string]$Dist = "$PSScriptRoot\..\out\dist",
    [int]$Port = 27192
)
$failed = 0
function Check($name, $ok) { if ($ok) { "PASS $name" } else { "FAIL $name"; $script:failed++ } }
function Count($n) { @(Get-Process -Name $n -ErrorAction SilentlyContinue).Count }

$hostsBefore = Count 'sidedroid-host'
$consolesBefore = Count 'SideDroid'
$out = Join-Path $env:TEMP "sd-single-$PID.txt"
$hostArgs = '--headless','--seconds','28','--port',$Port,'--source','\\.\DISPLAY1','--no-touch','--no-discovery','--no-adb'
$consoleArgs = '--instance',$Port,'--self-check',$out,'--self-check-ms','16000'

$h = Start-Process "$Dist\sidedroid-host.exe" -ArgumentList $hostArgs -PassThru
Start-Sleep 1
$first = Start-Process "$Dist\console\SideDroid.exe" -ArgumentList $consoleArgs -PassThru
Start-Sleep 6
# The packaged launcher runs as two processes, so compare with this one running alone.
$oneConsole = (Count 'SideDroid') - $consolesBefore
"one console is $oneConsole process(es)"

# What the tray's menu and a second click on the shortcut do.
$second = Start-Process "$Dist\console\SideDroid.exe" -ArgumentList $consoleArgs -PassThru
$third = Start-Process "$Dist\console\SideDroid.exe" -ArgumentList $consoleArgs -PassThru
$dupHost = Start-Process "$Dist\sidedroid-host.exe" -ArgumentList '--headless','--seconds','28','--port',$Port -PassThru

Check 'second console exits by itself' ($second.WaitForExit(8000))
Check 'third console exits by itself' ($third.WaitForExit(8000))
Check 'duplicate host exits by itself' ($dupHost.WaitForExit(4000))

# The original failure grew over time, so give it time to show up.
Start-Sleep 4
$consolesNow = (Count 'SideDroid') - $consolesBefore
$hostsNow = (Count 'sidedroid-host') - $hostsBefore
"consoles started by this test and still running: $consolesNow, hosts: $hostsNow"
Check 'no console was added' ($oneConsole -ge 1 -and $consolesNow -eq $oneConsole)
Check 'exactly one host remains' ($hostsNow -eq 1)

# Two clients at once: a connected console must not make the pipe look unavailable.
$a = New-Object System.IO.Pipes.NamedPipeClientStream('.', "SideDroidHost-$Port", [System.IO.Pipes.PipeDirection]::InOut)
$b = New-Object System.IO.Pipes.NamedPipeClientStream('.', "SideDroidHost-$Port", [System.IO.Pipes.PipeDirection]::InOut)
$a.Connect(3000); $b.Connect(3000)
$enc = New-Object System.Text.UTF8Encoding($false)
foreach ($p in $a, $b) {
    $w = New-Object System.IO.StreamWriter($p, $enc); $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($p)
    $w.WriteLine('{"cmd":"status"}')
    $reply = $r.ReadLine() | ConvertFrom-Json
    Check 'pipe answers with several clients connected' ($reply.ok -eq $true -and $reply.options.pcW -gt 0)
}
$a.Dispose(); $b.Dispose()

# The first console's self check reports whether it stayed connected to the host.
$deadline = (Get-Date).AddSeconds(15)
while (-not (Test-Path $out) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
if (Test-Path $out) {
    Start-Sleep -Milliseconds 300
    Check 'first console reached the host' ((Get-Content $out -Raw) -match 'hostReachable=true')
    Remove-Item $out -Force
} else {
    Check 'first console reported back' $false
}

# Exit must not hang, even with a console connected earlier.
Check 'host exits on its own' ($h.WaitForExit(16000))
$first.WaitForExit(5000) | Out-Null
Check 'no test processes left' (((Count 'SideDroid') -le $consolesBefore) -and ((Count 'sidedroid-host') -le $hostsBefore))
if ($failed) { "$failed check(s) failed"; exit 1 }
'single instance tests passed'
