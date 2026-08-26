# analyze_sweep.ps1 — segment sweep_writes.txt (--ctlout output) into
# per-action clusters (time gap > 2 s) and summarize each one.
$clusters = @()
$cur = $null
Get-Content sweep_writes.txt | ForEach-Object {
    if ($_ -match '^#(\d+) t=(\d+)\.(\d+) bReq=0x(\w\w) wVal=0x(\w\w\w\w) wIdx=0x(\w\w\w\w)') {
        $t = [double]("$($matches[2]).$($matches[3])")
        $idx = [int]("0x" + $matches[6])
        $req = [int]("0x" + $matches[4])
        $val = [int]("0x" + $matches[5])
        if ($cur -and ($t - $cur.start) -gt 2) { $clusters += , $cur; $cur = $null }
        if (-not $cur) {
            $cur = @{ start = $t; end = $t; count = 0; idxs = @{}; reqs = @{}; vals = @() }
        }
        $cur.end = $t
        $cur.count++
        $key = $idx -band 0x0FFF
        if (-not $cur.idxs.ContainsKey($key)) { $cur.idxs[$key] = 0 }
        $cur.idxs[$key]++
        $cur.reqs[$req] = $true
        $cur.vals += $val
    }
}
if ($cur) { $clusters += , $cur }

$n = 0
$prevEnd = 0.0
foreach ($c in $clusters) {
    $n++
    $min = [int]($c.vals | Measure-Object -Minimum).Minimum
    $max = [int]($c.vals | Measure-Object -Maximum).Maximum
    $idxList = ($c.idxs.GetEnumerator() | Sort-Object Name | ForEach-Object { ("0x{0:X4}" -f [int]$_.Name) + "x" + $_.Value }) -join " "
    $reqList = ($c.reqs.Keys | Sort-Object | ForEach-Object { "0x{0:X2}" -f [int]$_ }) -join ","
    $gap = [math]::Round($c.start - $prevEnd, 1)
    Write-Output ("C{0,2} gap={1,5}s t={2,14:F3} n={3,-4} bReq={4} val={5:X4}-{6:X4} idx=[{7}]" -f $n, $gap, $c.start, $c.count, $reqList, $min, $max, $idxList)
    $prevEnd = $c.end
}
