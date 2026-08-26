# Time-ordered OUT writes, deduped per register (show value changes only)
param([string]$Pcap = "cap_ctrl2.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = $out.Count
$last = @{}
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $rec = [int]$matches[1]; $t = $matches[2]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x([0-9A-F]{2}) wVal=0x([0-9A-F]{4}) wIdx=0x([0-9A-F]{4}) wLen=0') {
            $req = $matches[1]; $val = $matches[2]; $idx = $matches[3]
            $key = "0x$req 0x$idx"
            if (-not $last.ContainsKey($key) -or $last[$key] -ne $val) {
                Write-Host ("{0,6} t={1}  {2} = {3}" -f $rec, $t, $key, $val)
                $last[$key] = $val
            }
        }
    }
}
