# pdftext.ps1 - crude PDF text extractor v2 (string-based positions)
param(
    [Parameter(Mandatory=$true)][string]$Pdf,
    [string]$Out = ""
)
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Pdf))
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
$sb = New-Object System.Text.StringBuilder
$pos = 0
$found = 0
while ($true) {
    $si = $ascii.IndexOf("stream", $pos)
    if ($si -lt 0) { break }
    # data starts after "stream" + EOL
    $start = $si + 6
    if ($ascii[$start] -eq "`r" -and $ascii[$start+1] -eq "`n") { $start += 2 }
    elseif ($ascii[$start] -eq "`n") { $start += 1 }
    $ei = $ascii.IndexOf("endstream", $start)
    if ($ei -lt 0) { break }
    # strip trailing EOL before endstream
    $end = $ei
    while ($end -gt $start -and ($ascii[$end-1] -eq "`n" -or $ascii[$end-1] -eq "`r")) { $end-- }
    $len = $end - $start
    if ($len -gt 0) {
        $data = New-Object byte[] $len
        [Array]::Copy($bytes, $start, $data, 0, $len)
        try {
            $ms = New-Object System.IO.MemoryStream
            $off = 0
            if ($data.Length -gt 2 -and $data[0] -eq 0x78) { $off = 2 }  # zlib header
            $ms.Write($data, $off, $data.Length - $off)
            $ms.Position = 0
            $ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
            $out = New-Object System.IO.MemoryStream
            $ds.CopyTo($out)
            $text = [System.Text.Encoding]::ASCII.GetString($out.ToArray())
            if ($text -match 'BT|Tj|TJ') {
                $rx = [regex]::Matches($text, '\((?:[^()\\]|\\.)*\)')
                foreach ($mt in $rx) {
                    $s = $mt.Value.Substring(1, $mt.Value.Length - 2)
                    $s = $s -replace '\\\(', '(' -replace '\\\)', ')' -replace '\\\\', '\'
                    if ($s -match '[A-Za-z0-9]{3,}') { [void]$sb.Append($s).Append(' ') }
                }
                [void]$sb.AppendLine('')
                $found++
            }
        } catch { }
    }
    $pos = $ei + 9
}
$result = $sb.ToString()
if ($Out) { $result | Out-File $Out -Encoding utf8 }
"--- $found text streams, chars: $($result.Length) ---" | Write-Host
if (-not $Out) { $result }
