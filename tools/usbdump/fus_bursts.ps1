# Extract 0x1B writes (the banked DSP-register uploads) as a time series,
# grouped into bursts by >1s gaps. Each burst = one settings change.
param([string]$Pcap = "cap_fus3.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$writes = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $out.Count; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=([\d.]+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$Matches[1]; $t = [double]$Matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x1B wVal=0x([0-9A-F]{4}) wIdx=0x([0-9A-F]{4}) wLen=0') {
            [void]$writes.Add(@($t, [Convert]::ToInt32($Matches[1],16), [Convert]::ToInt32($Matches[2],16)))
        }
    }
}
Write-Host "total 0x1B writes: $($writes.Count)"
if ($writes.Count -eq 0) { exit }
# group by >1.0 s gaps
$groups = New-Object System.Collections.ArrayList
$cur = New-Object System.Collections.ArrayList
$prevT = -1e9
foreach ($w in $writes) {
    if ($w[0] - $prevT -gt 1.0 -and $cur.Count -gt 0) {
        [void]$groups.Add($cur); $cur = New-Object System.Collections.ArrayList
    }
    [void]$cur.Add($w); $prevT = $w[0]
}
if ($cur.Count -gt 0) { [void]$groups.Add($cur) }
Write-Host "bursts (settled settings changes): $($groups.Count)"
$gi = 0
foreach ($g in $groups) {
    $gi++
    $t0 = $g[0][0]; $t1 = $g[-1][0]
    $n = $g.Count
    # count distinct (wIdx high byte = bank)
    $banks = @{}
    foreach ($w in $g) { $b = '{0:X2}' -f (($w[2] -shr 8) -band 0xFF); if ($banks.ContainsKey($b)) { $banks[$b]++ } else { $banks[$b]=1 } }
    $bs = ($banks.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)x$($_.Value)" }) -join ' '
    Write-Host ("--- burst {0}: {1,4} writes, t={2:N2}..{3:N2}s  banks[{4}]" -f $gi, $n, $t0, $t1, $bs)
    if ($gi -le 3 -or $n -lt 8) {
        $g | Select-Object -First 10 | ForEach-Object { '    wVal=0x{0:X4} wIdx=0x{1:X4}' -f $_[1], $_[2] }
        if ($n -gt 10) { Write-Host "    ... ($($n-10) more)" }
    }
}
