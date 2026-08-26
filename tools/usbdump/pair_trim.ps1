# Pair the low-map (0x0000) and standard-map (0x0034) writes by time
param([string]$Pcap = "cap_trim.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = $out.Count
$pairs = @{}
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$matches[1]; $t = [double]$matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x12 wVal=0x([0-9A-F]{4}) wIdx=0x(0034|0000) wLen=0') {
            $val = [Convert]::ToInt32($matches[1], 16)
            $idx = $matches[2]
            $key = '{0:N3}' -f ([math]::Round($t, 3))
            if (-not $pairs.ContainsKey($key)) { $pairs[$key] = @{} }
            $pairs[$key][$idx] = $val
        }
    }
}
$ordered = $pairs.GetEnumerator() | Sort-Object { [double]$_.Name }
$lastlow = -1; $laststd = -1
foreach ($p in $ordered) {
    $low = if ($p.Value.ContainsKey('0000')) { $p.Value['0000'] } else { $lastlow }
    $std = if ($p.Value.ContainsKey('0034')) { $p.Value['0034'] } else { $laststd }
    if ($p.Value.ContainsKey('0000')) { $lastlow = $low }
    if ($p.Value.ContainsKey('0034')) { $laststd = $std }
    if ($p.Value.ContainsKey('0000') -and $p.Value.ContainsKey('0034')) {
        '{0}  low=0x{1:X4}  std=0x{2:X4}' -f $p.Name, $low, $std
    }
}
