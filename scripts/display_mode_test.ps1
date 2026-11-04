# Real test of display handling, with the virtual display driver installed and no phone.
# The screen will rearrange for a moment. It checks that:
#   - the phone gets its own extended display, and is not a mirror of the main one
#   - switching to mirroring and back works during a session
#   - the main display's resolution never changes, and the layout comes back afterwards
param(
    [string]$Bin = "$PSScriptRoot\..\build\host\Release",
    [int]$Port = 27191
)
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Disp {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct DM { [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Name; public short a,b,size,extra; public int fields;
    public int x,y; public int orient, fixedOut; public short c,d,e,f,g;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string form; public short lp; public int bpp, w, h, flags, hz, icm,im,mt,dt,r1,r2,pw,ph; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DM m);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  public static string Primary() { var m=new DM(); m.size=(short)Marshal.SizeOf(m); EnumDisplaySettingsW(null,-1,ref m); return m.w+"x"+m.h+"@"+m.hz; }
  public static int Virtual() { return GetSystemMetrics(78); }
  [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [StructLayout(LayoutKind.Sequential)] struct P { public int x,y; }
  [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(P p, int flags);
  [DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr m, int type, out int x, out int y);
  // Effective scale of the display at a physical position, as Windows itself reports it.
  public static int ScaleAt(int px, int py) {
    var old = SetThreadDpiAwarenessContext(new IntPtr(-4));
    var p = new P(); p.x = px; p.y = py;
    int x, y; GetDpiForMonitor(MonitorFromPoint(p, 0), 0, out x, out y);
    SetThreadDpiAwarenessContext(old);
    return x * 100 / 96;
  }
}
'@
$driverPresent = @(Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'Virtual Display Driver' }).Count -gt 0
if (-not $driverPresent) {
    'SKIPPED: the virtual display driver is not installed. Install it first, then run this again.'
    exit 2
}

$failed = 0
function Check($name, $ok) { if ($ok) { "PASS $name" } else { "FAIL $name"; $script:failed++ } }

# The test changes settings as a phone would, so put the real ones back afterwards.
$settingsFile = "$env:LOCALAPPDATA\SideDroid\settings.ini"
$settingsBackup = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw } else { $null }
# Whatever the user last chose, the test starts from the defaults.
Set-Content $settingsFile "width=0`nheight=0`nhz=0`nmaxKbps=0`nallowWireless=1`nmirror=0`nscale=0`n" -NoNewline -Encoding ascii
$mainScale = [Disp]::ScaleAt(10, 10)
$before = [Disp]::Primary()
$widthBefore = [Disp]::Virtual()
"before: primary $before, desktop width $widthBefore"

$h = Start-Process "$Bin\sidedroid-host.exe" -ArgumentList '--headless','--seconds','42','--port',$Port,'--test-virtual-display','--no-adb','--no-ipc','--no-touch','--no-discovery' -PassThru
Start-Sleep 2
$afterStart = [Disp]::Primary()
Check 'main display unchanged after host start' ($afterStart -eq $before)

$job = Start-Job -ScriptBlock {
    param($Bin, $Port)
    & "$Bin\sd-loopback.exe" --port $Port --seconds 30 --size 3168 1440 --density 520 --mirror-at 10 --extend-at 17 --scale-at 22 150
} -ArgumentList (Resolve-Path $Bin).Path, $Port

Start-Sleep 7
$midPrimary = [Disp]::Primary()
$midWidth = [Disp]::Virtual()
"extended: primary $midPrimary, desktop width $midWidth"
& "$PSScriptRoot\display_diag.ps1"
Check 'main display resolution unchanged while extended' ($midPrimary -eq $before)
Check 'desktop grew: the phone is a separate display' ($midWidth -gt $widthBefore)
$midScale = [Disp]::ScaleAt(2570, 10)
"virtual display scale: $midScale%"
Check 'virtual display at the recommended scale' ($midScale -eq 225)
Check 'main display scale unchanged' ([Disp]::ScaleAt(10, 10) -eq $mainScale)

Start-Sleep 7
$mirrorPrimary = [Disp]::Primary()
$mirrorWidth = [Disp]::Virtual()
"mirror: primary $mirrorPrimary, desktop width $mirrorWidth"
Check 'main display unchanged while mirroring' ($mirrorPrimary -eq $before)
Check 'desktop back to one display while mirroring' ($mirrorWidth -eq $widthBefore)

Start-Sleep 6
$backWidth = [Disp]::Virtual()
Check 'extended again after switching back' ($backWidth -gt $widthBefore)

Start-Sleep 5
$setScale = [Disp]::ScaleAt(2570, 10)
"virtual display scale after choosing 150%: $setScale%"
Check 'chosen scale applied' ($setScale -eq 150)
Check 'main display scale still unchanged' ([Disp]::ScaleAt(10, 10) -eq $mainScale)

$out = Receive-Job $job -Wait -AutoRemoveJob
$out | Select-String 'config:|options:|RESULT|disconnected: (?!stopped)' | ForEach-Object { $_.Line }
$configs = @($out | Select-String 'config:').Line
Check 'phone received its own resolution' (@($configs | Where-Object { $_ -match 'display 3168x1440.*mirror=0' }).Count -ge 1)
Check 'phone received the main display when mirroring' (@($configs | Where-Object { $_ -match 'mirror=1' }).Count -ge 1)
Check 'phone told the recommended scale' (@($out | Select-String 'options:.*recommended=225').Count -ge 1)
Check 'frames kept flowing' (($out | Select-String 'RESULT frames=(\d+)').Matches[0].Groups[1].Value -as [int]) -gt 100

# After the phone leaves, a short grace period, then everything goes back.
Start-Sleep 8
$endPrimary = [Disp]::Primary()
$endWidth = [Disp]::Virtual()
"after disconnect: primary $endPrimary, desktop width $endWidth"
Check 'main display back to its original mode' ($endPrimary -eq $before)
Check 'desktop back to its original size' ($endWidth -eq $widthBefore)
$h.WaitForExit(12000) | Out-Null
if ($null -ne $settingsBackup) { Set-Content $settingsFile $settingsBackup -NoNewline -Encoding ascii } else { Remove-Item $settingsFile -ErrorAction SilentlyContinue }
"--- log"; Get-Content "$env:LOCALAPPDATA\SideDroid\host-test.log" | Select-String 'pipeline|layout|refus|failed|capture opened|desktop|mode|scale' | ForEach-Object { $_.Line }
& "$PSScriptRoot\display_diag.ps1"
if ($failed) { "$failed check(s) failed"; exit 1 }
'display mode tests passed'
