# Read-only snapshot of the Windows display topology, for diagnosing display problems.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Diag {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct DD { public int cb; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Name;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Str; public int Flags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Id; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Key; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool EnumDisplayDevicesW(string dev, int i, ref DD d, int flags);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct DM { [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Name; public short a,b,size,extra; public int fields;
    public int x,y; public int orient, fixedOut; public short c,d,e,f,g;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string form; public short lp; public int bpp, w, h, flags, hz, icm,im,mt,dt,r1,r2,pw,ph; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DM m);
  [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
  [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint np, [Out] byte[] paths, ref uint nm, [Out] byte[] modes, IntPtr topo);
  [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint np, [Out] byte[] paths, ref uint nm, [Out] byte[] modes, out int topo);
  const int PATH=72, MODE=64;
  public static void Dump() {
    var d = new DD(); d.cb = Marshal.SizeOf(d);
    for (int i=0; EnumDisplayDevicesW(null,i,ref d,0); i++) {
      var dm = new DM(); dm.size=(short)Marshal.SizeOf(dm);
      bool cur = EnumDisplaySettingsW(d.Name,-1,ref dm);
      bool att=(d.Flags&1)!=0, pri=(d.Flags&4)!=0;
      var mons=new List<string>();
      var m=new DD(); m.cb=Marshal.SizeOf(m);
      for (int j=0; EnumDisplayDevicesW(d.Name,j,ref m,1); j++) mons.Add(m.Id);
      if (mons.Count==0 && !att) continue;
      Console.WriteLine("{0} [{1}] attached={2} primary={3} {4}", d.Name, d.Str, att, pri, cur? dm.w+"x"+dm.h+"@"+dm.hz+" at "+dm.x+","+dm.y : "(no mode)");
      foreach (var s in mons) Console.WriteLine("    monitor: {0}", s);
    }
    uint np, nm; GetDisplayConfigBufferSizes(4,out np,out nm); // QDC_DATABASE_CURRENT
    var pb=new byte[np*PATH]; var mb=new byte[nm*MODE]; int topo;
    int rc=QueryDisplayConfig(4,ref np,pb,ref nm,mb,out topo);
    Console.WriteLine("database topology rc={0} value={1} (1=internal 2=clone 4=extend 8=external)", rc, topo);
    GetDisplayConfigBufferSizes(2,out np,out nm); // ONLY_ACTIVE
    pb=new byte[np*PATH]; mb=new byte[nm*MODE];
    rc=QueryDisplayConfig(2,ref np,pb,ref nm,mb,IntPtr.Zero);
    Console.WriteLine("active paths={0} modes={1}", np, nm);
    for (int i=0;i<np;i++) {
      int o=i*PATH;
      Console.WriteLine("  path {0}: source id={1} target id={2}", i, BitConverter.ToUInt32(pb,o+8), BitConverter.ToUInt32(pb,o+28));
    }
  }
}
'@
[Diag]::Dump()
