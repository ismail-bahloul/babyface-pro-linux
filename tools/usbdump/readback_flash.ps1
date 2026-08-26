# 0x17 readback transitions + byte3 flashes (buttons) during a capture
param([string]$Pcap = "cap_select.pcap")
$lines = & .\ParsePcap.exe $Pcap 2>$null
$want17 = $false
$prev = ''
$n = 0
foreach ($l in $lines) {
  if ($l -match 'bReq=0x17') {
    $want17 = $true
  } elseif ($want17 -and $l -match '^\s+[0-9A-F]{6}\s+([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2})\s') {
    $cur = '{0} {1} {2} {3}' -f $Matches[1], $Matches[2], $Matches[3], $Matches[4]
    $b3 = [Convert]::ToByte($Matches[4], 16)
    $n++
    $marker = ''
    if ($b3 -ne 0x40) { $marker = '  <<< button flash 0x{0:X2}' -f $b3 }
    if ($cur -ne $prev -or $marker) {
      '{0,5}  {1}{2}' -f $n, $cur, $marker
      $prev = $cur
    }
    $want17 = $false
  } elseif ($l -match '^\s+[0-9A-F]{6}\s') {
    $want17 = $false
  }
}
