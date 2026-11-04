# Read-only checks of what package.ps1 produced. Installs nothing.
param([string]$Version = '0.0.1')
$root = Resolve-Path "$PSScriptRoot\.."
$failed = 0
function Check($name, $ok) { if ($ok) { "PASS $name" } else { "FAIL $name"; $script:failed++ } }

# The version shown in the about dialog must be the one being packaged.
Check 'about dialog shows this version' ((Get-Content "$root\ui\design\src\com\sidedroid\design\About.kt" -Raw -Encoding utf8) -match ('VERSION = "' + [regex]::Escape($Version) + '"'))
Check 'licence is packaged' (Test-Path "$root\out\dist\LICENSE.txt")

# APK
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apk = "$root\out\SideDroid-$Version.apk"
$zip = [IO.Compression.ZipFile]::OpenRead($apk)
$names = $zip.Entries | ForEach-Object { $_.FullName }
foreach ($abi in 'arm64-v8a', 'armeabi-v7a', 'x86_64') {
    Check "apk has native library for $abi" ($names -contains "lib/$abi/libsidedroid.so")
}
Check 'apk has code' ($names -contains 'classes.dex')
$zip.Dispose()

# MSI, through the Windows Installer API.
$msi = "$root\out\SideDroid-$Version.msi"
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($msi, 0))
function Query($sql) {
    $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @($sql))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
    $rows = @()
    while ($true) {
        $rec = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $rec) { break }
        $rows += $rec.GetType().InvokeMember('StringData', 'GetProperty', $null, $rec, @(1))
    }
    $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
    return $rows
}
$files = Query 'SELECT `FileName` FROM `File`'
$has = { param($n) @($files | Where-Object { $_ -match [regex]::Escape($n) }).Count -gt 0 }
foreach ($n in 'sidedroid-host.exe', 'sidedroid-driver-tool.exe', 'MttVDD.dll', 'MttVDD.inf', 'mttvdd.cat', 'vdd_settings.xml', 'adb.exe', 'SideDroid.exe') {
    Check "msi contains $n" (& $has $n)
}
"msi files: $($files.Count)"
$ca = Query 'SELECT `Action` FROM `CustomAction`'
foreach ($a in 'FirewallAdd', 'FirewallRemove', 'StopHost', 'DriverRemove', 'LaunchConsole') { Check "msi action $a" ($ca -contains $a) }

# Driver files in the package must be byte-identical to the pinned upstream zip.
$dist = "$root\out\dist\driver"
$tmp = Join-Path $env:TEMP "sd-verify-$PID"
Expand-Archive "$root\third_party\vdd\VirtualDisplayDriver-25.7.23-x64.zip" $tmp -Force
foreach ($f in 'mttvdd.cat', 'MttVDD.dll', 'MttVDD.inf', 'vdd_settings.xml') {
    Check "driver file unmodified: $f" ((Get-FileHash "$dist\$f").Hash -eq (Get-FileHash "$tmp\VirtualDisplayDriver\$f").Hash)
}
Remove-Item $tmp -Recurse -Force

if ($failed) { "$failed check(s) failed"; exit 1 }
'artifact checks passed'
