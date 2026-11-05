# Builds every release file into out\:
#   SideDroid-<version>-x64.msi       full Windows package
#   SideDroid-<version>-android-*.apk one per CPU type, and a universal one
#   SHA256SUMS.txt
# Windows parts build here; the APKs build inside WSL, where the Android SDK lives.
param(
    [string]$Version = '0.0.1',
    [string]$JavaHome = 'C:\Program Files\Microsoft\jdk-17.0.18.8-hotspot',
    [switch]$SkipApk
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$out = Join-Path $root 'out'
$dist = Join-Path $out 'dist'
$tools = Join-Path $out 'tools'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist, $tools | Out-Null

# Files of an earlier build must not end up in this release's checksums.
Get-ChildItem "$out\SideDroid-*" -File | Where-Object { -not ($SkipApk -and $_.Extension -eq '.apk') } | Remove-Item
Remove-Item "$out\SHA256SUMS.txt" -ErrorAction SilentlyContinue

function Require-Success($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" } }

Write-Host '== Host'
& cmake -S $root -B "$root\build" -G 'Visual Studio 18 2026' -A x64 | Out-Null
Require-Success 'cmake configure'
& cmake --build "$root\build" --config Release --parallel | Out-Null
Require-Success 'cmake build'
& ctest --test-dir "$root\build" -C Release --output-on-failure
Require-Success 'host and core tests'
Copy-Item "$root\build\host\Release\sidedroid-host.exe", "$root\build\host\Release\sidedroid-driver-tool.exe" $dist

Write-Host '== Virtual Display Driver (unmodified)'
$zip = "$root\third_party\vdd\VirtualDisplayDriver-25.7.23-x64.zip"
$expected = 'E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A'
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $expected) { throw 'VDD package hash mismatch' }
$vddTemp = Join-Path $out 'vdd-extract'
if (Test-Path $vddTemp) { Remove-Item $vddTemp -Recurse -Force }
Expand-Archive $zip $vddTemp
New-Item -ItemType Directory -Force "$dist\driver" | Out-Null
Copy-Item "$vddTemp\VirtualDisplayDriver\*" "$dist\driver"
foreach ($f in 'mttvdd.cat', 'MttVDD.dll') {
    $sig = Get-AuthenticodeSignature "$dist\driver\$f"
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'SignPath Foundation') {
        throw "unexpected signature on ${f}: $($sig.Status) $($sig.SignerCertificate.Subject)"
    }
}
Copy-Item "$root\third_party\vdd\LICENSE" "$dist\driver\LICENSE.txt"

Write-Host '== adb'
$adbZip = Join-Path $tools 'platform-tools.zip'
if (-not (Test-Path $adbZip)) {
    Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $adbZip -UseBasicParsing
}
$adbTemp = Join-Path $out 'adb-extract'
if (Test-Path $adbTemp) { Remove-Item $adbTemp -Recurse -Force }
Expand-Archive $adbZip $adbTemp
New-Item -ItemType Directory -Force "$dist\adb" | Out-Null
Copy-Item "$adbTemp\platform-tools\adb.exe", "$adbTemp\platform-tools\AdbWinApi.dll", "$adbTemp\platform-tools\AdbWinUsbApi.dll", "$adbTemp\platform-tools\NOTICE.txt" "$dist\adb"

Write-Host '== Console'
$env:JAVA_HOME = $JavaHome
Push-Location "$root\ui\console"
& .\gradlew.bat test createDistributable --console=plain -q
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "console build failed ($code)" }
Copy-Item "$root\ui\console\build\compose\binaries\main\app\SideDroid" "$dist\console" -Recurse

Copy-Item "$root\docs\THIRD_PARTY.md" "$dist\THIRD_PARTY.md"
Copy-Item "$root\LICENSE" "$dist\LICENSE.txt"

if (-not $SkipApk) {
    Write-Host '== Android (in WSL)'
    & wsl.exe --cd "$root" -e bash -l scripts/build_apk.sh
    Require-Success 'android build'
    foreach ($abi in 'arm64-v8a', 'armeabi-v7a', 'x86_64', 'universal') {
        Copy-Item "$root\android\app\build\outputs\apk\release\app-$abi-release.apk" (Join-Path $out "SideDroid-$Version-android-$abi.apk") -Force
    }
}

Write-Host '== MSI'
$wix = Join-Path $tools 'wix\wix.exe'
if (-not (Test-Path $wix)) {
    # WiX 5 is pinned: later major versions come with different licence terms.
    & dotnet tool install wix --version 5.0.2 --tool-path (Join-Path $tools 'wix') | Out-Null
    Require-Success 'install wix'
    & $wix extension add -g WixToolset.Util.wixext/5.0.2 | Out-Null
    Require-Success 'install wix util extension'
}
$msi = Join-Path $out "SideDroid-$Version-x64.msi"
& $wix build "$root\installer\Product.wxs" -arch x64 -ext WixToolset.Util.wixext -d "Version=$Version" -d "Dist=$dist" -d "IconFile=$root\host\resources\sidedroid.ico" -o $msi
Require-Success 'wix build'
Remove-Item "$out\SideDroid-*.wixpdb" -ErrorAction SilentlyContinue

Write-Host '== Checksums'
$files = Get-ChildItem $out -File | Where-Object { $_.Name -like 'SideDroid-*' } | Sort-Object Name
$lines = $files | ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" }
# Plain LF and no BOM, so that sha256sum -c accepts the file.
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), (($lines -join "`n") + "`n"))

Get-ChildItem $out -File | Select-Object Name, Length | Format-Table -AutoSize
