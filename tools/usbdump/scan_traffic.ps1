# Quick scan: any OUT traffic at all in a pcap + the device it belongs to
param([string]$Pcap = "cap_reflevel2.pcap")
$lines = & .\ParsePcap.exe $Pcap 2>$null
$out = 0; $in = 0; $vend = 0; $first = $null
foreach ($l in $lines) {
  if ($l -match 't=(\d+\.\d+) (OUT|IN)  dev=(\d+)') {
    if ($null -eq $first) { $first = $l.Trim() }
    if ($Matches[2] -eq 'OUT') { $out++ } else { $in++ }
    if ($l -match 'ep=0x0 ') { $vend++ }
  }
}
Write-Host "first: $first"
Write-Host "OUT: $out  IN: $in  vendor-ep0(OUT+IN): $vend"
