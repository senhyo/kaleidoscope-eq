# Offline test of register.ps1's attach/detach logic against fake endpoints under HKCU (never HKLM, no services, no files).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\register.ps1')
$scratch = 'Software\KaleidoscopeEQ-RegisterTest'
$Root = [Microsoft.Win32.Registry]::CurrentUser
$RenderKey = "$scratch\Render"
$StateKey = "$scratch\State"
$Root.DeleteSubKeyTree($scratch, $false)

$vendorEfx = '{90705486-CFCC-4D2F-9671-B62846279A2C}'
$vendorSfx = '{905069CC-CF0D-4EAD-B7D7-FBC5A9E38BD5}'
$cases = [ordered]@{
    '{00000000-0000-0000-0000-00000000000a}' = @{ $EfxList = [string[]]@($vendorEfx); '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13' = [string[]]@($vendorSfx) }  # composite lists (Realtek)
    '{00000000-0000-0000-0000-00000000000b}' = @{ $EfxSingle = $vendorEfx }   # single-CLSID EFX only
    '{00000000-0000-0000-0000-00000000000c}' = @{}                            # FxProperties with nothing
    '{00000000-0000-0000-0000-00000000000d}' = $null                          # no FxProperties key at all
}
function Snapshot($g) {
    $k = $Root.OpenSubKey("$RenderKey\$g\FxProperties")
    if (-not $k) { return '<no FxProperties>' }
    ($k.GetValueNames() | Sort-Object | ForEach-Object { "$_=$(@($k.GetValue($_)) -join ';')" }) -join ' | '
}
$before = @{}
foreach ($g in $cases.Keys) {
    $d = $Root.CreateSubKey("$RenderKey\$g")
    $d.SetValue('DeviceState', 1, 'DWord')
    if ($null -ne $cases[$g]) {
        $fx = $Root.CreateSubKey("$RenderKey\$g\FxProperties")
        foreach ($n in $cases[$g].Keys) { $v = $cases[$g][$n]; $fx.SetValue($n, $v, $(if ($v -is [array]) { 'MultiString' } else { 'String' })) }
    }
    $before[$g] = Snapshot $g
}

$fail = 0
function Check($ok, $what) { Write-Host "  [$(if ($ok) { 'PASS' } else { 'FAIL' })] $what"; if (-not $ok) { $script:fail++ } }
function Efx($g) { @((FxKey $g).GetValue($EfxList)) -join ';' }

Write-Host "register.ps1 attach/detach (HKCU scratch)"
Check ((@(ActiveEndpoints) -join ',') -eq (@($cases.Keys) -join ',')) 'ActiveEndpoints lists DeviceState=1 render endpoints'
foreach ($g in $cases.Keys) { Attach $g }
Check ((Efx '{00000000-0000-0000-0000-00000000000a}') -eq "$Clsid;$vendorEfx") 'composite EFX: ours first, vendor EFX kept'
Check ((Snapshot '{00000000-0000-0000-0000-00000000000a}') -match [regex]::Escape($vendorSfx)) 'composite EFX: SFX slot untouched'
Check ((Efx '{00000000-0000-0000-0000-00000000000b}') -eq "$Clsid;$vendorEfx") 'single EFX: becomes composite [ours, vendor]'
Check ((Efx '{00000000-0000-0000-0000-00000000000c}') -eq $Clsid) 'empty FxProperties: [ours]'
Check ((Efx '{00000000-0000-0000-0000-00000000000d}') -eq $Clsid) 'missing FxProperties: key created, [ours]'
foreach ($g in $cases.Keys) { Attach $g }  # update / reinstall
Check ((Efx '{00000000-0000-0000-0000-00000000000a}') -eq "$Clsid;$vendorEfx") 'second attach is idempotent'
Check ((@(Attached) -join ',') -eq (@($cases.Keys) -join ',')) 'state keys record every attached endpoint'
foreach ($g in @(Attached)) { Detach $g }
foreach ($g in $cases.Keys) { Check ((Snapshot $g) -eq $before[$g]) "detach restores $g exactly ($($before[$g]))" }
Check (-not @(Attached).Count) 'state keys removed'

Attach '{00000000-0000-0000-0000-00000000000c}'  # someone else adds an effect after us: detach keeps theirs
$fx = FxKey '{00000000-0000-0000-0000-00000000000c}'
$fx.SetValue($EfxList, [string[]]@($Clsid, '{11111111-1111-1111-1111-111111111111}'), 'MultiString')
Detach '{00000000-0000-0000-0000-00000000000c}'
Check ((Efx '{00000000-0000-0000-0000-00000000000c}') -eq '{11111111-1111-1111-1111-111111111111}') 'detach removes only ours when others were added later'

$Root.DeleteSubKeyTree($scratch, $false)
Write-Host "$(if ($fail) { "$fail failed" } else { 'all passed' })"
exit $fail
