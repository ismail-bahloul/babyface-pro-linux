# Extract SETTLED EQ states: group blocks by time gaps >1s, take last block of each group
param([string]$Pcap = "cap_eq3.pcap")
$out = .\ParsePcap.exe $Pcap --ep 0xA 2>&1
$n = $out.Count
$blocks = @()
for ($i = 0; $i -lt $n; $i++) {
    $s = $out[$i].ToString()
    if ($s -match '#(\d+) t=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0xA fn=0x0009.*data=64') {
        $t = [double]$matches[2]
        $hex = ''
        for ($k = 1; $k -le 4; $k++) {
            $l = $out[$i+$k].ToString()
            if ($l -match '^\s*[0-9A-F]{5,6}\s+(.+)') { $hex += $matches[1] -replace '\s','' }
        }
        if ($hex.Length -ge 64) { $blocks += @{ t = $t; h = $hex } }
        $i += 4
    }
}
Write-Host "blocks: $($blocks.Count)"
# group: a new group starts when gap > 1s OR header changes
$groups = @()
$cur = @()
$prevT = -1
foreach ($b in $blocks) {
    if ($prevT -ge 0 -and ($b.t - $prevT) -gt 1.0) {
        $groups += ,@($cur)
        $cur = @()
    }
    $cur += $b
    $prevT = $b.t
}
if ($cur.Count) { $groups += ,@($cur) }
Write-Host "groups: $($groups.Count)"
$g = 0
foreach ($grp in $groups) {
    $g++
    $last = $grp[-1]
    Write-Host "--- group $g : $($grp.Count) blocks, last t=$($last.t) ---"
    for ($o = 0; $o -lt 64; $o += 16) {
        Write-Host ("  {0:X4}  {1}" -f $o, ($last.h.Substring($o*2, 32) -replace '(.{2})','$1 '))
    }
}
