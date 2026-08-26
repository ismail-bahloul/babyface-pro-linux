# Builds usbdump.exe and ParsePcap.exe with the .NET Framework compiler (no SDK needed).
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { $csc = 'C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe' }
& $csc /nologo /out:usbdump.exe usbdump.cs
& $csc /nologo /out:ParsePcap.exe ParsePcap.cs
if ($LASTEXITCODE -eq 0) { Write-Host 'Built usbdump.exe + ParsePcap.exe' }
