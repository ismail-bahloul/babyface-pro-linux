# capture.ps1 - capture USB traffic from the Babyface's root hub (USBPcap).
#
# Run AFTER a reboot (USBPcap must be attached to the xHCI controller).
#
# 1) List capture interfaces:
#       .\capture.ps1 -List
# 2) Capture for N seconds on interface N (all devices on that root hub):
#       .\capture.ps1 -Device 2 -Seconds 45 -OutFile capture.pcap
#    While it runs, move faders / toggle things in TotalMix.
#
# USBPcapCMD writes the pcap incrementally; this script stops it after
# the requested number of seconds.

param(
    [switch]$List,
    [int]$Device = 2,
    [int]$Seconds = 45,
    [int]$Delay = 15,
    [string]$OutFile = "capture.pcap"
)

$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
if (-not (Test-Path $usbpcap)) { Write-Error 'USBPcapCMD.exe not found'; exit 1 }

if ($List) {
    Write-Host '--- USB devices matching USBPcap ---'
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'USBPcap' -or $_.DeviceID -match 'USBPcap' } |
        Select-Object Status, Name, DeviceID | Format-Table -AutoSize
    exit 0
}

# Re-init NonStandardHWIDs (needed for USB 3.0 capture on this hub)
& $usbpcap -I 2>&1 | Out-Null

$full = Join-Path (Get-Location) $OutFile
$devPath = '\\.\USBPcap' + $Device

# Countdown so the user has time to get ready in TotalMix
Write-Host "GET READY in TotalMix! Capture starts in ${Delay}s:"
for ($d = $Delay; $d -gt 0; $d -= 5) {
    Write-Host "  $d s..."
    Start-Sleep -Seconds ([Math]::Min(5, $d))
}

Write-Host "Capturing on $devPath -> $full for ${Seconds}s (total traffic of the root hub)"
Write-Host '>>>>> NOW: move faders / toggle 48V / play audio in TotalMix <<<<<'

$p = Start-Process -FilePath $usbpcap -ArgumentList @('-d', $devPath, '-o', $full, '-A') -NoNewWindow -PassThru -RedirectStandardError "$full.stderr.txt"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 400
    $left = [int]($Seconds - $sw.Elapsed.TotalSeconds)
    if ($left -gt 0 -and ($left % 15) -eq 0) {
        Write-Host "  ... $left s remaining - keep going!"
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500

if (Test-Path $full) {
    $f = Get-Item $full
    Write-Host "Capture saved: $($f.FullName) ($($f.Length) bytes)"
} else {
    Write-Host "Capture file missing. stderr: $(Get-Content "$full.stderr.txt" -ErrorAction SilentlyContinue)"
}
