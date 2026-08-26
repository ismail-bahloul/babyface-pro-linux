# Pair low-map (0x0000) and standard-map (0x0034) writes by nearest time
param([string]$Pcap = "cap_trim2.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$lows = New-Object System.Collections.ArrayList
$stds = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $out.Count; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=([\d.]+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$Matches[1]; $t = [double]$Matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x12 wVal=0x([0-9A-F]{4}) wIdx=0x(0034|0000) wLen=0') {
            $val = [Convert]::ToInt32($Matches[1], 16)
            if ($Matches[2] -eq '0000') { [void]$lows.Add(@($t, $val, $rec)) }
            else { [void]$stds.Add(@($t, $val, $rec)) }
        }
    }
}
$pairs = @()
$prev = $null
foreach ($st in $stds) {
    # nearest low at or before this std
    $best = $null; $bd = 1e12
    foreach ($lo in $lows) {
        $d = [math]::Abs($st[0] - $lo[0])
        if ($d -lt $bd) { $bd = $d; $best = $lo }
    }
    if ($null -ne $best -and $bd -lt 0.05) {
        $pairs += ,@($best[1], $st[1])
    }
}
# distinct pairs (ratio buckets)
$buckets = @{}
foreach ($p in $pairs) {
    $ratio = $p[1] / $p[0]
    $key = '{0:N4}' -f ([math]::Round($ratio, 4))
    if (-not $buckets.ContainsKey($key)) { $buckets[$key] = 0 }
    $buckets[$key]++
}
Write-Host "=== ratio std/low buckets (low, std, ratio) ==="
foreach ($p in $pairs) {
    '{0,5}  low=0x{1:X4}  std=0x{2:X4}  ratio={3:N4}' -f $p[0], $p[0], $p[1], ($p[1]/$p[0])
}
