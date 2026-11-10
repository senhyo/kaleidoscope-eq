# Kaleidoscope EQ engine install/uninstall (run elevated). CONTRACT section 3.
# ASCII only on purpose: Windows PowerShell 5.1 reads BOM-less scripts in the ANSI code page.
#   register.ps1 -Dll <path> [-Endpoints "{guid},{guid}"]   default: every active render endpoint
#   register.ps1 -Uninstall
# Attach = put our APO first in the endpoint-effect (EFX) list PKEY_CompositeFX_EndpointEffectClsid.
# Vendor APOs stay after it; the SFX/MFX slots are never touched. The first attach saves the original
# value under HKLM\SOFTWARE\KaleidoscopeEQ\Endpoints\<GUID>; uninstall puts it back.
# Exit code: 0 ok, 1 failed (reason on stderr), 3 some endpoints failed (listed on stderr).
param([string]$Dll, [string]$Endpoints, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$Clsid = '{3CDCC045-A1A2-4D8A-9369-FEDF2BD6C4C6}'           # must equal engine/src/guids.h
$EfxList = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15'      # PKEY_CompositeFX_EndpointEffectClsid (MULTI_SZ)
$EfxSingle = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7'     # PKEY_FX_EndpointEffectClsid (older single-CLSID form)
$InstallDir = Join-Path $env:ProgramFiles 'KaleidoscopeEQ'
$DllPath = Join-Path $InstallDir 'KaleidoscopeEQ.dll'
$DataDir = Join-Path $env:ProgramData 'KaleidoscopeEQ'
$Root = [Microsoft.Win32.Registry]::LocalMachine            # test/register.tests.ps1 swaps in HKCU
$RenderKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$StateKey = 'SOFTWARE\KaleidoscopeEQ'
$Multi = [Microsoft.Win32.RegistryValueKind]::MultiString

# Endpoints without an FxProperties key (often USB/Bluetooth) need one, but even Administrators cannot create
# subkeys under an endpoint key. Opening with SeRestorePrivilege (backup/restore semantics) bypasses the ACL.
Add-Type -TypeDefinition @'
using System; using System.ComponentModel; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class KeqReg {
  [StructLayout(LayoutKind.Sequential, Pack = 4)] struct TP { public int Count; public long Luid; public int Attr; }
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, int a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string s, string n, out long l);
  [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr t, bool d, ref TP n, int l, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] static extern int RegCreateKeyEx(SafeRegistryHandle k, string s, int r, string c, int o, int sam, IntPtr sa, out IntPtr h, out int d);
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode)] static extern int RegCreateKeyEx(IntPtr k, string s, int r, string c, int o, int sam, IntPtr sa, out IntPtr h, out int d);
  [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr h);
  [DllImport("ntdll.dll")] static extern int NtDeleteKey(IntPtr h);
  static void Privilege(string name) {
    IntPtr t; TP p = new TP { Count = 1, Attr = 2 };
    if (OpenProcessToken(GetCurrentProcess(), 0x28, out t) && LookupPrivilegeValue(null, name, out p.Luid)) AdjustTokenPrivileges(t, false, ref p, 0, IntPtr.Zero, IntPtr.Zero);
  }
  static IntPtr Open(SafeRegistryHandle root, string path) {
    Privilege("SeRestorePrivilege"); Privilege("SeBackupPrivilege");
    IntPtr h; int d; int e = RegCreateKeyEx(root, path, 0, null, 4 /*BACKUP_RESTORE*/, 0, IntPtr.Zero, out h, out d);
    if (e != 0) e = RegCreateKeyEx(root, path, 0, null, 0, 0x3001F /*KEY_READ|KEY_WRITE|DELETE*/, IntPtr.Zero, out h, out d);
    if (e != 0) throw new Win32Exception(e, path);
    return h;
  }
  public static void CreateChild(SafeRegistryHandle root, string parent, string child) {
    IntPtr p = Open(root, parent), h; int d;
    try { int e = RegCreateKeyEx(p, child, 0, null, 0, 3, IntPtr.Zero, out h, out d); if (e != 0) throw new Win32Exception(e, child); RegCloseKey(h); }
    finally { RegCloseKey(p); }
  }
  public static void Delete(SafeRegistryHandle root, string path) {
    IntPtr h = Open(root, path);
    try { int s = NtDeleteKey(h); if (s != 0) throw new Win32Exception("NtDeleteKey 0x" + s.ToString("x")); } finally { RegCloseKey(h); }
  }
}
'@

function Say($m) { [Console]::Out.WriteLine($m) }
function Normalize([string]$g) { $g = $g.Trim().Trim('{', '}').ToLowerInvariant(); if ($g) { "{$g}" } }
function Ours([string[]]$list) { , @(@($list) | Where-Object { $_ -and $_ -ne $Clsid }) }
function Same($a, $b) { (@($a) -join '|') -eq (@($b) -join '|') }
# Ask for value read/write only: that is all Administrators are granted on FxProperties.
function FxKey($guid) { $Root.OpenSubKey("$RenderKey\$guid\FxProperties", [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Security.AccessControl.RegistryRights]'QueryValues, SetValue') }
function Attached { $k = $Root.OpenSubKey("$StateKey\Endpoints"); if ($k) { $k.GetSubKeyNames(); $k.Close() } }

function ActiveEndpoints {
    $k = $Root.OpenSubKey($RenderKey)
    foreach ($g in $k.GetSubKeyNames()) {
        $d = $Root.OpenSubKey("$RenderKey\$g")
        if ($d -and $d.GetValue('DeviceState') -eq 1) { Normalize $g }
    }
}

function Attach($guid) {
    if (-not $Root.OpenSubKey("$RenderKey\$guid")) { throw "no such render endpoint: $guid" }
    $fx = FxKey $guid
    $created = 0
    if (-not $fx) { [KeqReg]::CreateChild($Root.Handle, "$RenderKey\$guid", 'FxProperties'); $created = 1; $fx = FxKey $guid }
    $list = $fx.GetValue($EfxList)
    $single = $fx.GetValue($EfxSingle)
    $state = $Root.OpenSubKey("$StateKey\Endpoints\$guid", $true)
    if (-not $state) {  # back up the original only on first attach (an update must not overwrite it)
        $state = $Root.CreateSubKey("$StateKey\Endpoints\$guid")
        if ($null -ne $list) { $state.SetValue('OriginalEfx', [string[]]@($list), $Multi) }
        elseif ($single) { $state.SetValue('SeedEfx', [string[]]@($single), $Multi) }
        $state.SetValue('CreatedFxKey', $created, [Microsoft.Win32.RegistryValueKind]::DWord)
    }
    # Ours first, so the vendor's endpoint processing (speaker protection, tuning) still comes last.
    $base = if ($null -ne $list) { @($list) } elseif ($single) { @($single) } else { @() }
    $fx.SetValue($EfxList, [string[]](@($Clsid) + (Ours $base)), $Multi)
    $fx.Close(); $state.Close()
}

function Detach($guid) {
    $state = $Root.OpenSubKey("$StateKey\Endpoints\$guid")
    $fx = FxKey $guid
    if ($fx -and $state) {
        $orig = $state.GetValue('OriginalEfx')
        $rest = Ours $fx.GetValue($EfxList)
        if ($null -ne $orig) { $fx.SetValue($EfxList, [string[]]$(if ($rest.Count) { $rest } else { @($orig) }), $Multi) }
        elseif (-not $rest.Count -or (Same $rest $state.GetValue('SeedEfx'))) { $fx.DeleteValue($EfxList, $false) }
        else { $fx.SetValue($EfxList, [string[]]$rest, $Multi) }  # keep anything added by others since
        $empty = $fx.ValueCount -eq 0
        $fx.Close()
        if ($state.GetValue('CreatedFxKey') -eq 1 -and $empty) { [KeqReg]::Delete($Root.Handle, "$RenderKey\$guid\FxProperties") }
    }
    if ($state) { $state.Close() }
    $Root.DeleteSubKeyTree("$StateKey\Endpoints\$guid", $false)
}

# audiodg holds the DLL: swap files while audio is stopped, then start it (endpoint effects are re-read then).
function Stop-Audio {
    $deps = @(Get-Service audiosrv -DependentServices | Where-Object Status -eq 'Running')
    Stop-Service audiosrv -Force
    $until = (Get-Date).AddSeconds(15)
    while ((Get-Process audiodg -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }
    $deps
}
function Start-Audio($deps) {
    Start-Service audiosrv
    foreach ($d in $deps) { Start-Service $d.Name -ErrorAction SilentlyContinue }
}

function RegSvr($arguments) {
    $p = Start-Process regsvr32.exe -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    if ($p.ExitCode -ne 0) { throw "regsvr32 $arguments failed (code $($p.ExitCode))" }
}

function Install {
    if (-not $Dll -or -not (Test-Path -LiteralPath $Dll)) { throw "DLL not found: $Dll" }
    $want = if ($Endpoints) { @($Endpoints -split ',' | ForEach-Object { Normalize $_ } | Where-Object { $_ }) } else { @(ActiveEndpoints) }
    $targets = @(@($want) + @(Attached) | Select-Object -Unique)  # endpoints attached earlier stay attached (refreshed)
    if (-not $targets.Count) { throw 'no render endpoint to attach to' }
    $failed = @()
    $deps = Stop-Audio
    try {
        New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
        Get-ChildItem -LiteralPath $InstallDir -Filter '*.old-*' | Remove-Item -Force -ErrorAction SilentlyContinue
        try { Copy-Item -LiteralPath $Dll -Destination $DllPath -Force }
        catch { Move-Item -LiteralPath $DllPath -Destination "$DllPath.old-$([DateTime]::UtcNow.Ticks)" -Force; Copy-Item -LiteralPath $Dll -Destination $DllPath -Force }
        RegSvr @('/s', "`"$DllPath`"")

        $ep = Join-Path $DataDir 'endpoints'
        New-Item -ItemType Directory -Force -Path $ep | Out-Null
        # Users: the bridge writes .cfg. LOCAL SERVICE (audiodg): writes .live. SIDs, not names (localized Windows).
        $null = & icacls.exe $ep /grant '*S-1-5-32-545:(OI)(CI)M' '*S-1-5-19:(OI)(CI)M'
        if ($LASTEXITCODE -ne 0) { throw "icacls failed (code $LASTEXITCODE)" }

        $s = $Root.CreateSubKey($StateKey)
        $s.SetValue('InstallPath', $InstallDir)
        $s.SetValue('Version', [string](Get-Item -LiteralPath $DllPath).VersionInfo.ProductVersion)
        $s.Close()
        foreach ($g in $targets) {
            try { Attach $g; Say "attached $g" }
            catch { $failed += "$g ($($_.Exception.Message))" }
        }
    } finally { Start-Audio $deps }
    if ($failed.Count) {
        [Console]::Error.WriteLine("could not attach to: $($failed -join '; ')")
        exit $(if ($failed.Count -eq $targets.Count) { 1 } else { 3 })
    }
    Say "installed $DllPath"
}

function Remove-All {
    $deps = Stop-Audio
    try {
        foreach ($g in @(Attached)) { Detach $g; Say "detached $g" }
        if (Test-Path -LiteralPath $DllPath) { RegSvr @('/u', '/s', "`"$DllPath`"") }
        foreach ($k in "SOFTWARE\Classes\CLSID\$Clsid", "SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$Clsid") { $Root.DeleteSubKeyTree($k, $false) }
        $Root.DeleteSubKeyTree($StateKey, $false)
        foreach ($d in $InstallDir, $DataDir) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
    } finally { Start-Audio $deps }
    Say 'uninstalled'
}

if ($MyInvocation.InvocationName -ne '.') {  # dot-sourced (tests): define functions only
    try {
        $me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'must run as Administrator' }
        if ($Uninstall) { Remove-All } else { Install }
        exit 0
    } catch {
        [Console]::Error.WriteLine("register.ps1: $($_.Exception.Message)")
        exit 1
    }
}
