# 0x17 READBACK (4-byte) around a time window, plus the OUT writes timeline
param([string]$Pcap = "cap_reflevel2.pcap")
$lines = & .\ParsePcap.exe $Pcap 2>$null
$writes = @()
$reads = @()
$want17 = $false
foreach ($l in $lines) {
  if ($l -match 't=(\d+\.\d+) OUT dev=\d+ bus=\d+ ep=0x0 fn=0x0017') {
    $t = $Matches[1]
    $l2 = $null
  } elseif ($l -match 'bReq=0x17 wVal=0x([0-9A-F]{4}) wIdx=0x([0-9A-F]{4}) wLen=0') {
    $writes += "OUT 0x$($Matches[1]) 0x$($Matches[2])"
  } elseif ($l -match 'bReq=0x17') {
    $want17 = $true
  } elseif ($want17 -and $l -match '^\s+[0-9A-F]{6}\s+([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2})\s') {
    $reads += '{0} {1} {2} {3}' -f $Matches[1], $Matches[2], $Matches[3], $Matches[4]
    $want17 = $false
  } elseif ($l -match '^\s+[0-9A-F]{6}\s') {
    $want17 = $false
  }
}
Write-Host "=== OUT 0x17 writes ==="
$writes | ForEach-Object { Write-Host $_ }
Write-Host "=== 0x17 READBACK unique values ==="
$reads | Select-Object -Unique | ForEach-Object { Write-Host $_ }
