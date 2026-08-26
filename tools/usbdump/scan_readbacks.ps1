# Deduped 0x17 readbacks (match setup line, payload at +3)
param([string]$Pcap = "cap_set2.pcap")
$out = .\ParsePcap.exe $Pcap 2>&1
$n = $out.Count
$last = ''
$tlast = ''
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match 'bReq=0x17 ') {
        if ($i + 3 -lt $n -and $out[$i+3].ToString() -match '^\s*00001C\s+([0-9A-F]{2})\s+([0-9A-F]{2})\s+([0-9A-F]{2})\s+([0-9A-F]{2})') {
            $val = "$($matches[1]) $($matches[2]) $($matches[3]) $($matches[4])"
            if ($val -ne $last) {
                $hdr = $out[$i-1].ToString()
                $t = if ($hdr -match 't=(\d+\.\d+)') { $matches[1] } else { '?' }
                Write-Host "t=$t  $val"
                $last = $val
            }
        }
        $i += 3
    }
}
