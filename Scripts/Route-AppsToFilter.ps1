<#
  Sends Spotify and your browsers to the Explicitly Desktop filter (VB-CABLE),
  or back to a normal output, using Windows' per-app output setting (the same
  one as Settings > System > Sound > Volume mixer). Windows remembers the choice
  per app, so this only needs to run once while the apps are open.

    .\Route-AppsToFilter.ps1                 # route to "CABLE Input"
    .\Route-AppsToFilter.ps1 -Restore        # back to the Windows default output
    .\Route-AppsToFilter.ps1 -List           # show output devices
#>
param(
    [switch]$Restore,
    [switch]$List,
    [string]$Device = "CABLE Input",
    [string[]]$Apps = @("Spotify", "firefox", "chrome", "brave")
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class AppAudioRouting
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}

    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    }

    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")]
    interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out int count);
        [PreserveSig] int Item(int index, out IMMDevice device);
    }

    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    interface IMMDevice
    {
        int Activate();
        [PreserveSig] int OpenPropertyStore(int access, out IPropertyStore store);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    }

    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99")]
    interface IPropertyStore
    {
        int GetCount(out int c);
        int GetAt(int i, out PROPERTYKEY k);
        [PreserveSig] int GetValue(ref PROPERTYKEY k, out PROPVARIANT v);
    }

    [StructLayout(LayoutKind.Sequential)] struct PROPERTYKEY { public Guid fmtid; public int pid; }
    [StructLayout(LayoutKind.Explicit)] struct PROPVARIANT { [FieldOffset(0)] public short vt; [FieldOffset(8)] public IntPtr p; }

    // Undocumented Windows 11 per-app endpoint API (same one EarTrumpet uses)
    [Guid("ab3d4648-e242-459f-b02f-541c70306324"), InterfaceType(ComInterfaceType.InterfaceIsIInspectable)]
    interface IAudioPolicyConfigFactory
    {
        int f1(); int f2(); int f3(); int f4(); int f5(); int f6(); int f7(); int f8(); int f9(); int f10();
        int f11(); int f12(); int f13(); int f14(); int f15(); int f16(); int f17(); int f18(); int f19();
        [PreserveSig] int SetPersistedDefaultAudioEndpoint(uint processId, int flow, int role, IntPtr deviceId);
        [PreserveSig] int GetPersistedDefaultAudioEndpoint(uint processId, int flow, int role, [MarshalAs(UnmanagedType.HString)] out string deviceId);
        [PreserveSig] int ClearAllPersistedApplicationDefaultEndpoints();
    }

    [DllImport("combase.dll")] static extern int WindowsCreateString([MarshalAs(UnmanagedType.LPWStr)] string s, int len, out IntPtr hstring);
    [DllImport("combase.dll")] static extern int WindowsDeleteString(IntPtr hstring);
    [DllImport("combase.dll")] static extern int RoGetActivationFactory(IntPtr classId, ref Guid iid, [MarshalAs(UnmanagedType.IInspectable)] out object factory);

    const string RenderSuffix = "#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    const string Prefix = @"\\?\SWD#MMDEVAPI#";

    // Active render devices: name -> endpoint id
    public static Dictionary<string, string> RenderDevices()
    {
        var result = new Dictionary<string, string>();
        var e = (IMMDeviceEnumerator)(new MMDeviceEnumerator());
        IMMDeviceCollection col;
        Marshal.ThrowExceptionForHR(e.EnumAudioEndpoints(0, 1, out col));
        int n; col.GetCount(out n);
        for (int i = 0; i < n; i++)
        {
            IMMDevice d; col.Item(i, out d);
            string id; d.GetId(out id);
            IPropertyStore s; d.OpenPropertyStore(0, out s);
            var key = new PROPERTYKEY { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
            PROPVARIANT v; s.GetValue(ref key, out v);
            result[Marshal.PtrToStringUni(v.p)] = id;
        }
        return result;
    }

    static IAudioPolicyConfigFactory Factory()
    {
        IntPtr cls;
        string name = "Windows.Media.Internal.AudioPolicyConfig";
        WindowsCreateString(name, name.Length, out cls);
        try
        {
            var iid = typeof(IAudioPolicyConfigFactory).GUID;
            object f;
            Marshal.ThrowExceptionForHR(RoGetActivationFactory(cls, ref iid, out f));
            return (IAudioPolicyConfigFactory)f;
        }
        finally { WindowsDeleteString(cls); }
    }

    // endpointId null/empty = follow the Windows default output again
    public static void SetAppOutput(uint processId, string endpointId)
    {
        var factory = Factory();
        IntPtr h = IntPtr.Zero;
        if (!string.IsNullOrEmpty(endpointId))
        {
            string full = Prefix + endpointId + RenderSuffix;
            WindowsCreateString(full, full.Length, out h);
        }
        try
        {
            // eConsole and eMultimedia roles
            Marshal.ThrowExceptionForHR(factory.SetPersistedDefaultAudioEndpoint(processId, 0, 0, h));
            Marshal.ThrowExceptionForHR(factory.SetPersistedDefaultAudioEndpoint(processId, 0, 1, h));
        }
        finally { if (h != IntPtr.Zero) WindowsDeleteString(h); }
    }

    public static string GetAppOutput(uint processId)
    {
        string id;
        return Factory().GetPersistedDefaultAudioEndpoint(processId, 0, 1, out id) == 0 ? id : null;
    }
}
'@

$devices = [AppAudioRouting]::RenderDevices()

if ($List) {
    $devices.Keys | Sort-Object
    return
}

$endpoint = $null
if (-not $Restore) {
    $match = $devices.Keys | Where-Object { $_ -like "$Device*" } | Select-Object -First 1
    if (-not $match) { throw "No active output device named '$Device'. Is VB-CABLE installed? Use -List to see devices." }
    $endpoint = $devices[$match]
    Write-Host "Routing to: $match"
} else {
    Write-Host "Restoring apps to the Windows default output"
}

foreach ($app in $Apps) {
    $procs = Get-Process -Name $app -ErrorAction SilentlyContinue
    if (-not $procs) { Write-Host ("  {0,-8} not running, open it and run this again" -f $app); continue }
    # Windows only accepts this on the app's audio process (helpers return
    # E_INVALIDARG), and remembers it for the whole app from then on
    $ok = 0
    foreach ($p in $procs) {
        try { [AppAudioRouting]::SetAppOutput([uint32]$p.Id, $endpoint); $ok++ } catch { }
    }
    if ($ok -gt 0) { Write-Host ("  {0,-8} done" -f $app) }
    else { Write-Host ("  {0,-8} not set yet: play something in it, then run this again" -f $app) }
}
