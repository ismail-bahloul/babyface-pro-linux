param([string]$Pcap = "cap_eq3.pcap")
$out = .\ParsePcap.exe $Pcap --ep 0xA 2>&1
$n = $out.Count
$seen = @{}
$order = New-Object System.Collections.ArrayList
$blocks = 0
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0xA fn=0x0009.*data=64') {
        $rec = [int]$matches[1]; $t = $matches[2]
        $hex = ''
        for ($k = 1; $k -le 4; $k++) {
            $l = $out[$i+$k].ToString()
            if ($l -match '^\s*[0-9A-F]{5,6}\s+(.+)') { $hex += $matches[1] -replace '\s','' }
        }
        if ($hex.Length -ge 64) {
            $blocks++
            if (-not $seen.ContainsKey($hex)) {
                $seen[$hex] = @{ rec = $rec; t = $t; count = 0 }
                [void]$order.Add($hex)
            }
            $seen[$hex].count++
        }
        $i += 4
    }
}
Write-Host "total 64-byte blocks: $blocks"
Write-Host "distinct: $($seen.Count)"
$c = 0
foreach ($k in $order) {
    $c++
    if ($c -le 30) {
        $m = $seen[$k]
        Write-Host "--- #$c rec=$($m.rec) t=$($m.t) x$($m.count) ---"
        for ($o = 0; $o -lt 64; $o += 16) {
            Write-Host ("  {0:X4}  {1}" -f $o, ($k.Substring($o*2, 32) -replace '(.{2})','$1 '))
        }
    }
}
