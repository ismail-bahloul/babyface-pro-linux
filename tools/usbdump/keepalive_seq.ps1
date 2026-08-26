# Full keepalive (0x10 0x05CF) value sequence with timestamps
param([string]$Pcap = "cap_fus3.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = 0
for ($i = 0; $i -lt $out.Count; $i++) {
    $s = $out[$i].ToString()
    if ($s -match 't=([\d.]+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
        $t = [double]$Matches[1]
        $setup = $out[$i+1].ToString()
        if ($setup -match 'bReq=0x10 wVal=0x([0-9A-F]{4}) wIdx=0x05CF wLen=0') {
            $n++
            '{0,2}: t={1:N3} wVal=0x{2}' -f $n, $t, $Matches[1]
        }
    }
}
Write-Host "total keepalives: $n"
