# Analyze a pcap's OUT writes: unique bReq/wIdx combos + count
param([string]$Pcap = "cap_ctrl.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = $out.Count
$map = @{}
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$matches[1]; $t = $matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x([0-9A-F]{2}) wVal=0x([0-9A-F]{4}) wIdx=0x([0-9A-F]{4}) wLen=0') {
            $req = $matches[1]; $val = $matches[2]; $idx = $matches[3]
            $base = ([Convert]::ToInt32($idx, 16)) -band 0x3FFF  # strip 2-bit counter
            $key = "0x$req 0x$('{0:X4}' -f $base)"
            if (-not $map.ContainsKey($key)) { $map[$key] = @{ count = 0; first = $rec; last = $rec; vals = New-Object System.Collections.Generic.HashSet[string] } }
            $map[$key].count++
            $map[$key].last = $rec
            [void]$map[$key].vals.Add("$val@$t")
        }
    }
}
Write-Host "unique (req, baseWIdx) pairs: $($map.Count)"
$map.GetEnumerator() | Sort-Object { $_.Value.first } | ForEach-Object {
    $m = $_.Value
    $v = ($m.vals | Select-Object -First 3) -join ' '
    Write-Host ("{0,-14} x{1,-5} rec {2}..{3}  sample: {4}" -f $_.Key, $m.count, $m.first, $m.last, $v)
}
