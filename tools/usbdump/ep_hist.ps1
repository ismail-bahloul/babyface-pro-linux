# Endpoint histogram of a pcap
param([string]$Pcap = "cap_reflevel2.pcap")
$lines = & .\ParsePcap.exe $Pcap 2>$null
$hist = @{}
$devs = @{}
foreach ($l in $lines) {
  if ($l -match 't=(\d+\.\d+) (OUT|IN)  dev=(\d+) bus=(\d+) ep=0x([0-9A-Fa-f]+) fn=0x([0-9A-Fa-f]+)\(([A-Z_]+)\) type=([A-Z_]+)') {
    $k = "dev=$($Matches[3]) ep=0x$($Matches[5]) type=$($Matches[8])"
    if ($hist.ContainsKey($k)) { $hist[$k]++ } else { $hist[$k] = 1 }
    $dk = "dev=$($Matches[3]) bus=$($Matches[4])"
    if ($devs.ContainsKey($dk)) { $devs[$dk]++ } else { $devs[$dk] = 1 }
  }
}
Write-Host "=== devices ==="
$devs.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Host $_.Name, "x$($_.Value)" }
Write-Host "=== endpoints ==="
$hist.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Host $_.Name, "x$($_.Value)" }
