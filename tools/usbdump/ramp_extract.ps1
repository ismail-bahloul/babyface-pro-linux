# Extract the (time, value) ramp of one write register from a pcap
param([string]$Pcap = "cap_fx3.pcap", [string]$Req = "0x12", [string]$Idx = "0x0138")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = $out.Count
$prev = -1
$seq = @()
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$matches[1]; $t = $matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match "bReq=$Req wVal=0x([0-9A-F]{4}) wIdx=$Idx wLen=0") {
            $val = [Convert]::ToInt32($matches[1], 16)
            if ($val -ne $prev) {
                $seq += ("{0,-6} 0x{1:X4} {2}" -f $rec, $val, $t)
                $prev = $val
            }
        }
    }
}
Write-Host "distinct values: $($seq.Count)"
$seq | ForEach-Object { Write-Host $_ }
