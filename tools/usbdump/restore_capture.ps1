# restore_capture.ps1 - capture a TotalMix session restore.
# The USER closes TotalMix gracefully during the countdown; this script
# relaunches it 20 s into the capture and records the re-apply traffic.
param(
    [int]$Seconds = 90,
    [int]$Delay = 20,
    [string]$OutFile = "cap_restore.pcap"
)
$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$tm = 'C:\Program Files\RME\Fireface\TotalMixFX_x64.exe'

Write-Host "GET READY: during the countdown, CLOSE TotalMix gracefully (File->Exit)!"
for ($d = $Delay; $d -gt 0; $d -= 5) { Write-Host "  $d s..."; Start-Sleep -Seconds ([Math]::Min(5, $d)) }

$full = Join-Path (Get-Location) $OutFile
$devPath = '\\.\USBPcap' + 2
$p = Start-Process -FilePath $usbpcap -ArgumentList @('-d', $devPath, '-o', $full, '-A') -NoNewWindow -PassThru -RedirectStandardError "$full.stderr.txt"
Write-Host "Capturing on $devPath -> $full"
Start-Sleep -Seconds 20
Write-Host ">>> relaunching TotalMix <<<"
Start-Process $tm
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Milliseconds 400 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
Write-Host "Capture saved: $full ($((Get-Item $full).Length) bytes)"
