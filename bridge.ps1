# KaleidoscopeEQ 다리. 화면(kaleidoscope 앱)과 우리 APO 엔진 사이를 잇는다. 계약은 CONTRACT.md §1·§2·§4.
# 쉘이 powershell.exe(Windows PowerShell 5.1)로 이 스크립트를 띄우고, 표준 입력으로 JSON 한 줄씩 요청을 받아
# 표준 출력으로 JSON 한 줄씩 답한다. 요청을 기다리는 동안 1초마다 기본 출력 장치를 보고, 바뀌면 먼저 알린다.
#   {"id":n,"op":"status"}                                   → engine, device, devices(모든 활성 출력 장치)
#   {"id":n,"op":"apply","device_id":"{guid}","config":{…}}   → <데이터>\endpoints\<guid>.cfg를 원자적으로 바꿔 쓴다
#   {"id":n,"op":"install","dll_b64":"…","endpoints":[…]?}   → register.ps1 -Dll …를 관리자 권한(UAC)으로
#   {"id":n,"op":"uninstall"}                                → register.ps1 -Uninstall을 관리자 권한(UAC)으로
#   알림 {"event":"device","device":{…}|null}: 기본 출력 장치 또는 그 상태(registered, active, 이름)가 마지막으로 알린 것과 달라지면.
# ok 답에는 늘 engine과 device를 싣는다. 실패는 {"id":n,"status":"error","message":"…"}이고 다리는 계속 돈다.
#
# 빌드 때 build.py가 할 일 (등록 스크립트 넣기):
#   아래 `$RegisterGz = '…'` 줄의 따옴표 안 자리표시자(밑줄 둘 + REGISTER_PS1 + 밑줄 둘, 이 파일에 정확히 한 번 나온다)를
#   base64(gzip(engine/register.ps1의 UTF-8 바이트)) 문자열로 바꾼다. Python:
#       base64.b64encode(gzip.compress((HERE / "engine" / "register.ps1").read_bytes())).decode("ascii")
#   base64에는 따옴표·줄바꿈·# 이 없으므로 어떤 스크립트 본문도 그대로 살아남고, 주석 줄을 빼는 strip()과도 부딪히지 않는다.
#   화면은 빌드된 이 스크립트를 창 저장소에 파일로 두고 `powershell -File <경로>`로 띄운다(명령줄 길이 제한 없음).
#
# 시험용 환경 변수: KEQ_DATA_DIR(기본 %ProgramData%\KaleidoscopeEQ), KEQ_REG_HKCU(있으면 HKLM 대신 HKCU\SOFTWARE\KaleidoscopeEQ를 본다).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$inv = [Globalization.CultureInfo]::InvariantCulture
$data = if ($env:KEQ_DATA_DIR) { $env:KEQ_DATA_DIR } else { Join-Path $env:ProgramData 'KaleidoscopeEQ' }
$endpoints = Join-Path $data 'endpoints'
# 레지스트리는 64비트 보기로 읽는다(32비트 쉘이 띄운 32비트 PowerShell이라도 WOW6432Node로 새지 않게).
$hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey($(if ($env:KEQ_REG_HKCU) { 'CurrentUser' } else { 'LocalMachine' }), 'Registry64')
$regSub = 'SOFTWARE\KaleidoscopeEQ'
$guidRe = '^\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}$'
$RegisterGz = '__REGISTER_PS1__'

function Send($o) { [Console]::Out.WriteLine((ConvertTo-Json -InputObject $o -Compress -Depth 5)); [Console]::Out.Flush() }

# Core Audio(출력 장치 목록·기본 장치·속성), 원자적 파일 교체, pid가 살아 있는 audiodg인지 확인은 PowerShell에 없어 작은 C#으로 한다.
# (Get-Process는 모든 프로세스를 훑어 1초 감시에 쓰기엔 느리다. 그 pid 하나만 열어 본다. 일반 사용자 권한으로 된다.)
# COM 인터페이스는 vtable 순서대로 앞에서부터 쓰는 데까지만 선언한다(Activate는 자리만 차지).
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public struct KsPK { public Guid g; public int p; }
public struct KsPV { public ushort vt, r1, r2, r3; public IntPtr a, b; }
[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IEn { void EnumAudioEndpoints(int flow, int mask, out ICo c); void GetDefaultAudioEndpoint(int flow, int role, out IDe d); }
[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ICo { void GetCount(out int n); void Item(int i, out IDe d); }
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IDe { void Activate(); void OpenPropertyStore(int access, out IPS s); void GetId([MarshalAs(UnmanagedType.LPWStr)] out string id); }
[ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPS { void GetCount(out int n); void GetAt(int i, out KsPK k); void GetValue(ref KsPK k, out KsPV v); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class KsMMDE {}
public class KsEp { public string Id, Name, Path; public int Form = 10; }
public static class KsAudio {
  [DllImport("ole32.dll")] static extern int PropVariantClear(ref KsPV v);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool MoveFileEx(string from, string to, int flags);
  // 원자적으로 바꿔 끼운다(MOVEFILE_REPLACE_EXISTING | WRITE_THROUGH). 백신·색인기가 막 바뀐 파일을 지우기 공유 없이
  // 잠깐 열고 있으면 접근 거부(5)나 공유 위반(32)이 난다. 그건 곧 풀리므로 잠깐씩 다시 하고(약 0.5초까지), 다른 오류는 바로 알린다.
  public static void Replace(string from, string to) {
    for (int i = 0; ; i++) {
      if (MoveFileEx(from, to, 9)) return;
      int e = Marshal.GetLastWin32Error();
      if ((e != 5 && e != 32) || i >= 10) throw new System.ComponentModel.Win32Exception(e);
      System.Threading.Thread.Sleep(10 + 10 * i);
    }
  }
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out int code);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr h, int flags, StringBuilder s, ref int n);
  public static bool IsAudiodg(int pid) {
    IntPtr h = OpenProcess(0x1000, false, pid); if (h == IntPtr.Zero) return false;
    try { var s = new StringBuilder(1024); int n = s.Capacity, code;
      return GetExitCodeProcess(h, out code) && code == 259 && QueryFullProcessImageName(h, 0, s, ref n)
        && System.IO.Path.GetFileName(s.ToString()).Equals("audiodg.exe", StringComparison.OrdinalIgnoreCase);
    } finally { CloseHandle(h); }
  }
  static object Prop(IPS s, string g, int p) {
    KsPK k; k.g = new Guid(g); k.p = p; KsPV v;
    try { s.GetValue(ref k, out v); } catch (COMException) { return null; }
    try {
      if (v.vt == 31) return Marshal.PtrToStringUni(v.a);
      if (v.vt == 19) return unchecked((int)(v.a.ToInt64() & 0xFFFFFFFF));
      return null;
    } finally { PropVariantClear(ref v); }
  }
  static KsEp Ep(IDe d) {
    var e = new KsEp(); IPS s;
    try {
      d.GetId(out e.Id); d.OpenPropertyStore(0, out s);
      e.Name = Prop(s, "A45C254E-DF1C-4EFD-8020-67D146A850E0", 14) as string;
      e.Path = Prop(s, "B3F8FA53-0004-438E-9003-51A46E139BFC", 2) as string;
      object f = Prop(s, "1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E", 0); if (f is int) e.Form = (int)f;
      Marshal.ReleaseComObject(s);
    } finally { Marshal.ReleaseComObject(d); }
    return e;
  }
  public static KsEp Default() {
    var en = (IEn)new KsMMDE(); IDe d;
    try { en.GetDefaultAudioEndpoint(0, 0, out d); return Ep(d); } catch (COMException) { return null; } finally { Marshal.ReleaseComObject(en); }
  }
  public static KsEp[] Active() {
    var en = (IEn)new KsMMDE(); var r = new List<KsEp>(); ICo c; int n;
    try {
      en.EnumAudioEndpoints(0, 1, out c); c.GetCount(out n);
      for (int i = 0; i < n; i++) { IDe d; c.Item(i, out d); r.Add(Ep(d)); }
      Marshal.ReleaseComObject(c);
    } finally { Marshal.ReleaseComObject(en); }
    return r.ToArray();
  }
}
"@

# 장치 종류. 블루투스 통화용(Hands-Free) 끝점은 BTHHFENUM 아래에 있고, 폼 팩터가 Headset/Handset인 것도 통화용이다.
# 폼 팩터: 1 Speakers, 3 Headphones, 5 Headset, 6 Handset, 그 밖(라인, HDMI, S/PDIF, 알 수 없음)은 other.
function Kind($ep) {
    if ($ep.Path -match 'BTHHFENUM' -or $ep.Form -eq 5 -or $ep.Form -eq 6) { return 'headset' }
    if ($ep.Form -eq 3) { return 'headphones' }
    if ($ep.Form -eq 1) { return 'speakers' }
    'other'
}

# 장치 하나의 상태(§2). registered: 레지스트리에 그 장치 키가 있다. active: APO가 남긴 .live의 pid가 지금 살아 있는 audiodg다.
function Device($ep) {
    if (-not $ep) { return $null }
    $guid = $ep.Id.Split('.')[-1].ToLowerInvariant()
    $active = $false
    try {
        $live = [IO.File]::ReadAllText((Join-Path $endpoints "$guid.live"))
        if ($live -match '^\s*pid\s+(\d+)') { $active = [KsAudio]::IsAudiodg([int]$Matches[1]) }
    } catch {}
    $k = $hive.OpenSubKey("$regSub\Endpoints\$guid")
    if ($k) { $k.Close() }
    [ordered]@{ id = $guid; name = $ep.Name; kind = (Kind $ep); registered = ($null -ne $k); active = $active }
}

function Engine {
    $k = $hive.OpenSubKey($regSub)
    if (-not $k) { return [ordered]@{ installed = $false; version = $null } }
    $v = $k.GetValue('Version'); $k.Close()
    [ordered]@{ installed = $true; version = $(if ($null -ne $v) { [string]$v } else { $null }) }
}

# 마지막으로 화면에 알린 기본 장치(JSON). 답을 보낼 때와 알림을 보낼 때 갱신해, 달라졌을 때만 알린다.
$script:seen = $null
function DefaultDevice { $d = Device ([KsAudio]::Default()); $script:seen = ConvertTo-Json -InputObject $d -Compress; $d }

function Watch {
    $before = $script:seen
    try { $d = DefaultDevice } catch { return }
    if ($null -ne $before -and $script:seen -ne $before) { Send ([ordered]@{ event = 'device'; device = $d }) }
}

function Num($v, $what) {
    if (-not ($v -is [int] -or $v -is [long] -or $v -is [decimal] -or $v -is [double])) { throw "${what}: 숫자가 아님" }
    $d = [double]$v
    if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { throw "${what}: 유한한 수가 아님" }
    $d.ToString('R', $inv)
}

# 화면의 config를 §1의 .cfg 본문으로 옮긴다.
function Render($c) {
    if ($null -eq $c) { throw 'config가 없음' }
    $out = @('# kaleidoscope-eq 1', "enabled $(if ($c.enabled) { 1 } else { 0 })", "preamp $(Num $c.preamp 'preamp')")
    # if/else를 식으로 쓰면 결과가 파이프라인을 지나며 바깥 배열이 한 겹 풀린다. peaks가 하나뿐이면 [[f,q,g]]가
    # [f,q,g]로 바뀌어 거부됐다. 그래서 파이프라인 없이 바로 대입한다.
    $peaks = @()
    if ($null -ne $c.peaks) { $peaks = $c.peaks }
    if ($peaks.Count -gt 20) { throw "peaks: 20개까지 ($($peaks.Count)개)" }
    foreach ($p in $peaks) {
        $p = @($p)
        if ($p.Count -ne 3) { throw 'peaks: 항목마다 [주파수, Q, 이득]' }
        $out += "peak $(Num $p[0] 'peak 주파수') $(Num $p[1] 'peak Q') $(Num $p[2] 'peak 이득')"
    }
    $r = $c.reverb
    if ($null -ne $r) { $out += "reverb $(Num $r.decay 'reverb decay') $(Num $r.predelay 'reverb predelay') $(Num $r.wet 'reverb wet')" }
    $out += "spatial $(Num $c.spatial 'spatial')"
    ($out -join "`n") + "`n"
}

function Apply($m) {
    $guid = ([string]$m.device_id).ToLowerInvariant()
    if ($guid -notmatch $guidRe) { throw "잘못된 device_id: $($m.device_id)" }
    $text = Render $m.config
    [void][IO.Directory]::CreateDirectory($endpoints)
    $dst = Join-Path $endpoints "$guid.cfg"
    $tmp = "$dst.$PID.tmp"
    try { [IO.File]::WriteAllText($tmp, $text, $utf8); [KsAudio]::Replace($tmp, $dst) }
    finally { if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force } }
}

# register.ps1을 관리자 권한으로 실행한다. Windows가 승인(UAC)을 묻고, 거절하면 cancelled다.
# 승격된 프로세스의 출력은 직접 받을 수 없어, 승격된 PowerShell이 register.ps1을 자식으로 돌려 출력을 log.txt에 남기고 종료 코드를 넘긴다.
function Admin([byte[]]$dll, [string[]]$targets) {
    if ($RegisterGz.StartsWith('__')) { throw '등록 스크립트가 들어 있지 않음(build.py로 빌드한 판에서만 설치할 수 있음)' }
    $dir = Join-Path ([IO.Path]::GetTempPath()) ('KaleidoscopeEQ-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($dir)
    try {
        $gz = New-Object IO.Compression.GZipStream((New-Object IO.MemoryStream(, [Convert]::FromBase64String($RegisterGz))), ([IO.Compression.CompressionMode]::Decompress))
        $reg = Join-Path $dir 'register.ps1'
        [IO.File]::WriteAllText($reg, (New-Object IO.StreamReader($gz, $utf8)).ReadToEnd(), (New-Object Text.UTF8Encoding($true)))
        $log = Join-Path $dir 'log.txt'
        $q = { param($s) "'" + $s.Replace("'", "''") + "'" }
        $argv = '-Uninstall'
        if ($dll) {
            $path = Join-Path $dir 'KaleidoscopeEQ.dll'
            [IO.File]::WriteAllBytes($path, $dll)
            $argv = "-Dll $(& $q $path)"
            if ($targets) { $argv += " -Endpoints $(& $q ($targets -join ','))" }
        }
        $cmd = "[Console]::OutputEncoding = New-Object Text.UTF8Encoding(`$false); " +
            "`$o = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $(& $q $reg) $argv 2>&1; " +
            "[IO.File]::WriteAllLines($(& $q $log), [string[]]@(`$o | ForEach-Object { `"`$_`" })); exit `$LASTEXITCODE"
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = 'powershell.exe'
        $psi.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
        $psi.Verb = 'runas'; $psi.UseShellExecute = $true; $psi.WindowStyle = 'Hidden'
        try { $p = [Diagnostics.Process]::Start($psi) } catch {
            for ($e = $_.Exception; $e; $e = $e.InnerException) {
                if ($e -is [ComponentModel.Win32Exception] -and $e.NativeErrorCode -eq 1223) { return @{ elevated = 'cancelled' } }
            }
            throw
        }
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) {
            $detail = if (Test-Path -LiteralPath $log) { ([IO.File]::ReadAllText($log)).Trim() } else { '' }
            throw "등록 스크립트 실패(종료 코드 $($p.ExitCode)): $detail"
        }
        @{ elevated = 'done' }
    } finally { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }
}

function Handle($m) {
    $more = @{}
    switch ([string]$m.op) {
        'status' { $more.devices = @([KsAudio]::Active() | ForEach-Object { Device $_ }) }
        'apply' { Apply $m }
        'install' {
            $targets = @($m.endpoints | Where-Object { $null -ne $_ } | ForEach-Object { ([string]$_).ToLowerInvariant() })
            foreach ($t in $targets) { if ($t -notmatch $guidRe) { throw "잘못된 endpoints 항목: $t" } }
            $dll = [Convert]::FromBase64String([string]$m.dll_b64)
            if ($dll.Length -eq 0) { throw 'dll_b64가 비었음' }
            $more = Admin $dll $targets
        }
        'uninstall' { $more = Admin $null $null }
        default { throw "알 수 없는 op: $($m.op)" }
    }
    $r = [ordered]@{ id = $m.id; status = 'ok'; engine = (Engine); device = (DefaultDevice) }
    foreach ($k in $more.Keys) { $r[$k] = $more[$k] }
    Send $r
}

# 표준 입력은 비동기로 읽는다(Console.In의 ReadLineAsync는 .NET Framework에서 동기라 쓰지 않는다).
# 줄이 오지 않은 1초마다 기본 장치를 살핀다. 입력이 닫히면 끝난다.
$stdin = New-Object IO.StreamReader([Console]::OpenStandardInput(), $utf8)
$pending = $stdin.ReadLineAsync()
while ($true) {
    if (-not $pending.Wait(1000)) { Watch; continue }
    $line = $pending.Result
    if ($null -eq $line) { break }
    $pending = $stdin.ReadLineAsync()
    if (-not $line.Trim()) { continue }
    $m = $null
    try { $m = ConvertFrom-Json $line; Handle $m }
    catch { Send ([ordered]@{ id = $(if ($m) { $m.id } else { $null }); status = 'error'; message = $_.Exception.Message }) }
}
