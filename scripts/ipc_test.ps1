# Talks to a running host over its real pipe. Run with the host started by the caller.
param([int]$TimeoutMs = 5000)
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'SideDroidHost', [System.IO.Pipes.PipeDirection]::InOut)
$pipe.Connect($TimeoutMs)
$w = New-Object System.IO.StreamWriter($pipe, (New-Object System.Text.UTF8Encoding($false)))
$w.AutoFlush = $true
$r = New-Object System.IO.StreamReader($pipe)
function Call($json) { $w.WriteLine($json); return $r.ReadLine() }
$failed = 0
function Check($name, $ok) { if ($ok) { "PASS $name" } else { "FAIL $name"; $script:failed++ } }

$before = Call '{"cmd":"status"}' | ConvertFrom-Json
Check 'status parses' ($before.ok -eq $true -and $null -ne $before.settings)

$set = Call '{"cmd":"set","width":1920,"height":1080,"hz":90,"maxKbps":25000,"allowWireless":false}' | ConvertFrom-Json
$now = Call '{"cmd":"status"}' | ConvertFrom-Json
Check 'set persists' ($set.ok -and $now.settings.width -eq 1920 -and $now.settings.hz -eq 90 -and $now.settings.maxKbps -eq 25000 -and -not $now.settings.allowWireless)

$p = Call '{"cmd":"pair_start"}' | ConvertFrom-Json
$now = Call '{"cmd":"status"}' | ConvertFrom-Json
Check 'pairing code shown' ($p.ok -and $p.code -match '^[0-9A-Z]{4}-[0-9A-Z]{4}$' -and $now.pairingCode -eq $p.code -and $now.pairingSeconds -gt 100)
$c = Call '{"cmd":"pair_cancel"}' | ConvertFrom-Json
$now = Call '{"cmd":"status"}' | ConvertFrom-Json
Check 'pairing cancelled' ($c.ok -and $now.pairingCode -eq '')

Check 'unknown command refused' (-not (Call '{"cmd":"nope"}' | ConvertFrom-Json).ok)
Check 'unpair unknown id refused' (-not (Call '{"cmd":"unpair","id":"00"}' | ConvertFrom-Json).ok)

# Put the settings back as they were.
$s = $before.settings
$restore = '{"cmd":"set","width":' + $s.width + ',"height":' + $s.height + ',"hz":' + $s.hz + ',"maxKbps":' + $s.maxKbps + ',"allowWireless":' + $s.allowWireless.ToString().ToLower() + '}'
$null = Call $restore
$pipe.Dispose()
if ($failed) { exit 1 }
'ipc tests passed'
