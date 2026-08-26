# Analyze cap_wheelpress.pcap: extract 0x17 readbacks + any host->device writes
$ErrorActionPreference = 'Stop'
$pcap = 'cap_wheelpress.pcap'
$lines = & .\ParsePcap.exe $pcap 2>$null

$readbacks = @()   # 0x17 readback values (4 bytes) in order
$writes = @()      # any SETUP stage with dir=OUT
$want17 = $false
$i = 0
foreach ($l in $lines) {
  if ($l -match 'bReq=0x17') {
    $want17 = $true
  } elseif ($want17 -and $l -match '^\s+[0-9A-F]{6}\s+([0-9A-F]{2} [0-9A-F]{2} [0-9A-F]{2} [0-9A-F]{2})\s') {
    $i++
    $readbacks += ('#{0,-5} {1}' -f $i, $Matches[1])
    $want17 = $false
  } elseif ($l -match 'bReq=0x1[1C-E]|bReq=0x11') {
    # other reads (0x11/0x1C/0x1E/0x1F) - ignore, but don't consume pending flag
  } elseif ($l -match 'setup: dir=OUT') {
    $writes += $l.Trim()
    $want17 = $false
  } elseif ($l -match '^\s+[0-9A-F]{6}\s') {
    $want17 = $false
  }
}
Write-Host "=== 0x17 readback UNIQUE values (in order) ==="
$prev = $null
foreach ($r in $readbacks) {
  $v = ($r -split '\s+', 2)[1]
  if ($v -ne $prev) { Write-Host $v; $prev = $v }
}
Write-Host ""
Write-Host "=== host->device writes ($($writes.Count)) ==="
$writes | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "=== last 12 samples ==="
$readbacks | Select-Object -Last 12 | ForEach-Object { Write-Host $_ }
