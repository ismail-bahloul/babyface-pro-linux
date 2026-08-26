# Scan a pcap for 0x17 readback byte2 values (mode + counter histogram)
$ErrorActionPreference = 'Stop'
foreach ($pcap in $args) {
  if (-not (Test-Path $pcap)) { continue }
  Write-Host "=== $pcap ==="
  $lines = & .\ParsePcap.exe $pcap 2>$null
  $hist = @{}
  $want17 = $false
  foreach ($l in $lines) {
    if ($l -match 'bReq=0x17') {
      $want17 = $true
    } elseif ($want17 -and $l -match '^\s+[0-9A-F]{6}\s+([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2})\s') {
      $key = '{0} {1} {2} {3}' -f $Matches[1], $Matches[2], $Matches[3], $Matches[4]
      $b2 = [Convert]::ToByte($Matches[3], 16)
      if ($hist.ContainsKey($key)) { $hist[$key]++ } else { $hist[$key] = 1 }
      $want17 = $false
    } elseif ($l -match '^\s+[0-9A-F]{6}\s') {
      $want17 = $false
    }
  }
  $hist.GetEnumerator() | Sort-Object Name | ForEach-Object {
    $b2 = [Convert]::ToByte(($_.Name -split ' ')[2], 16)
    $tag = switch ($b2 -shr 4) { 0 { 'MIX?' } 8 { 'OUT' } 9 { 'OUT+carry?' } 4 { 'IN1/2' } 5 { 'IN3/4' } 6 { 'IN-Opt' } default { '??' } }
    '{0}  x{1}  [{2}]' -f $_.Name, $_.Value, $tag
  }
}
