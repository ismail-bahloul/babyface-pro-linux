# pdftext2.ps1 - proper PDF text extraction (ToUnicode CMaps + position sort)
param(
    [Parameter(Mandatory=$true)][string]$Pdf,
    [string]$Out = ""
)
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Pdf))
# Latin1 maps bytes 0-255 to chars 1:1 — ASCII would destroy binary stream data
$ascii = [System.Text.Encoding]::GetEncoding('ISO-8859-1').GetString($bytes)

function Get-Decompressed([byte[]]$data) {
    try {
        $ms = New-Object System.IO.MemoryStream
        $off = 0
        if ($data.Length -gt 2 -and $data[0] -eq 0x78) { $off = 2 }
        $ms.Write($data, $off, $data.Length - $off)
        $ms.Position = 0
        $ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
        $out = New-Object System.IO.MemoryStream
        $ds.CopyTo($out)
        return [System.Text.Encoding]::GetEncoding('ISO-8859-1').GetString($out.ToArray())
    } catch { return $null }
}

# compact-dict helpers (this PDF has no spaces after keys: /Type/Catalog/Pages 2 0 R)
function Get-Type($dict) {
    $m = [regex]::Match($dict, '/Type\s*/?([A-Za-z]+)')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
function Get-Ref($dict, $key) {
    $m = [regex]::Match($dict, $key + '\s*(\d+)\s+0\s+R')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return $null
}
function Get-Arr($dict, $key) {
    $m = [regex]::Match($dict, $key + '\s*\[([^\]]*)\]')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
function Get-Str($dict, $key) {
    $m = [regex]::Match($dict, $key + '\s*\(([^)]*)\)')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# ---- 1. objects ----
$objects = @{}
$rx = [regex]::Matches($ascii, '(?s)(\d+) 0 obj(.*?)endobj')
foreach ($mt in $rx) {
    $num = [int]$mt.Groups[1].Value
    $body = $mt.Groups[2].Value
    $si = $body.IndexOf('stream')
    if ($si -ge 0) {
        $dict = $body.Substring(0, $si)
        $start = $si + 6
        if ($body[$start] -eq "`r") { $start += 2 } elseif ($body[$start] -eq "`n") { $start += 1 }
        $ei = $body.IndexOf('endstream', $start)
        if ($ei -lt 0) { $ei = $body.Length }
        $objects[$num] = @{ dict = $dict; stream = $body.Substring($start, $ei - $start) }
    } else {
        $objects[$num] = @{ dict = $body; stream = $null }
    }
}

# ---- 2. ToUnicode CMaps + font encodings (Identity-H = 2-byte codes) ----
$script:cMaps = @{}
$script:fontEnc = @{}
foreach ($num in $objects.Keys) {
    $o = $objects[$num]
    if ((Get-Type $o.dict) -eq 'Font') {
        $tu = Get-Ref $o.dict '/ToUnicode'
        if ($tu) { "DEBUG font $num -> tu $tu, streamLen=$($objects[$tu].stream.Length)" | Write-Host }
        if ($o.dict -match 'Identity-H') { $script:fontEnc[$num] = 'idh' }
        if ($tu -and $objects.ContainsKey($tu) -and $objects[$tu].stream) {
            $cmap = Get-Decompressed ([byte[]][char[]]$objects[$tu].stream)
            if ($cmap) {
                $map = @{}
                foreach ($b in [regex]::Matches($cmap, '(?s)beginbfchar(.*?)endbfchar')) {
                    foreach ($p in [regex]::Matches($b.Groups[1].Value, '<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>')) {
                        $code = [Convert]::ToInt32($p.Groups[1].Value, 16)
                        $u = $p.Groups[2].Value
                        $ch = ''
                        for ($k = 0; $k -lt $u.Length; $k += 4) { $ch += [char][Convert]::ToInt32($u.Substring($k, 4), 16) }
                        $map[$code] = $ch
                    }
                }
                foreach ($b in [regex]::Matches($cmap, '(?s)beginbfrange(.*?)endbfrange')) {
                    foreach ($r in [regex]::Matches($b.Groups[1].Value, '<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>')) {
                        $lo = [Convert]::ToInt32($r.Groups[1].Value, 16)
                        $hi = [Convert]::ToInt32($r.Groups[2].Value, 16)
                        $sv = [Convert]::ToInt32($r.Groups[3].Value, 16)
                        for ($c = $lo; $c -le $hi; $c++) {
                            $u = $sv + ($c - $lo)
                            $ch = ''
                            for ($k = 0; $k -lt 4; $k++) { $ch += [char](($u -shr (8 * (3 - $k))) -band 0xFF) }
                            $map[$c] = $ch
                        }
                    }
                }
                $script:cMaps[$num] = $map
            }
        }
    }
}
"CMaps parsed: $($script:cMaps.Count)" | Write-Host

# ---- 3. page tree walk ----
$script:pageFonts = @{}
$script:pageContents = @{}
$script:pageOrder = New-Object System.Collections.ArrayList
function Get-FontMap($resDict, $parentFonts) {
    $fonts = @{}
    if ($parentFonts) { foreach ($k in $parentFonts.Keys) { $fonts[$k] = $parentFonts[$k] } }
    $m = [regex]::Match($resDict, '/Font\s*<<(.*?)>>', 'Singleline')
    if ($m.Success) {
        foreach ($f in [regex]::Matches($m.Groups[1].Value, '/(F\d+)\s+(\d+)\s+0\s+R')) {
            $fonts[$f.Groups[1].Value] = [int]$f.Groups[2].Value
        }
    }
    return $fonts
}
function Walk-Pages($nodeNum, $inheritedFonts, $inheritedContents) {
    if (-not $script:objects.ContainsKey($nodeNum)) { return }
    $node = $script:objects[$nodeNum]
    $type = Get-Type $node.dict
    if ($type -eq 'Pages') {
        $kids = Get-Arr $node.dict '/Kids'
        if ($kids) {
            foreach ($k in [regex]::Matches($kids, '(\d+)\s+0\s+R')) { Walk-Pages ([int]$k.Groups[1].Value) $inheritedFonts $inheritedContents }
        }
    } elseif ($type -eq 'Page') {
        [void]$script:pageOrder.Add($nodeNum)
        $fonts = Get-FontMap $node.dict $inheritedFonts
        $script:pageFonts[$nodeNum] = $fonts
        $contents = @()
        if ($inheritedContents) { $contents = $inheritedContents.Clone() }
        $c = [regex]::Match($node.dict, '/Contents\s*(?:\d+\s+0\s+R|\[([^\]]*)\])', 'Singleline')
        if ($c.Success) {
            if ($c.Groups[1].Success) {
                foreach ($a in [regex]::Matches($c.Groups[1].Value, '(\d+)\s+0\s+R')) { $contents += [int]$a.Groups[1].Value }
            } else {
                $contents += [int][regex]::Match($c.Value, '\d+').Value
            }
        }
        $script:pageContents[$nodeNum] = $contents
    }
}
$rootM = [regex]::Match($ascii, '/Root\s+(\d+)\s+0\s+R')
if ($rootM.Success) {
    $pagesRef = Get-Ref $objects[[int]$rootM.Groups[1].Value].dict '/Pages'
    if ($pagesRef) { Walk-Pages $pagesRef $null $null }
}
"Pages walked: $($script:pageFonts.Count)" | Write-Host

# ---- 4. decode text ----
function Decode-Text([byte[]]$codes, $fontNum) {
    $sb = New-Object System.Text.StringBuilder
    if ($null -ne $fontNum -and $script:fontEnc.ContainsKey($fontNum) -and $script:fontEnc[$fontNum] -eq 'idh') {
        # Identity-H: 2-byte codes
        $i = 0
        while ($i + 1 -lt $codes.Length) {
            $code = ($codes[$i] -shl 8) -bor $codes[$i+1]
            if ($script:cMaps.ContainsKey($fontNum) -and $script:cMaps[$fontNum].ContainsKey($code)) {
                [void]$sb.Append($script:cMaps[$fontNum][$code])
            } else { [void]$sb.Append('?') }
            $i += 2
        }
    } else {
        if ($null -ne $fontNum -and $script:cMaps.ContainsKey($fontNum)) {
            $map = $script:cMaps[$fontNum]
            foreach ($c in $codes) {
                if ($map.ContainsKey([int]$c)) { [void]$sb.Append($map[[int]$c]) }
                else { [void]$sb.Append('?') }
            }
        } else {
            foreach ($c in $codes) { [void]$sb.Append([char]$c) }
        }
    }
    return $sb.ToString()
}
function Bytes-From-String($s) {
    # literal string -> bytes (Latin1) ; hex string <..> -> bytes
    if ($s.StartsWith('<')) {
        $hex = $s.Substring(1, $s.Length - 2) -replace '\s', ''
        $out = New-Object System.Collections.ArrayList
        for ($k = 0; $k + 1 -lt $hex.Length; $k += 2) {
            [void]$out.Add([Convert]::ToByte($hex.Substring($k, 2), 16))
        }
        return [byte[]]$out.ToArray()
    } else {
        $inner = $s.Substring(1, $s.Length - 2) -replace '\\\(', '(' -replace '\\\)', ')' -replace '\\\\', '\\'
        return [byte[]][char[]]$inner
    }
}

$script:allRuns = @()
foreach ($pnum in $script:pageOrder) {
    $fonts = $script:pageFonts[$pnum]
    foreach ($cnum in $script:pageContents[$pnum]) {
        if (-not $objects.ContainsKey($cnum)) { continue }
        $stream = $objects[$cnum].stream
        if (-not $stream) { continue }
        $cs = Get-Decompressed ([byte[]][char[]]$stream)
        if (-not $cs) { continue }
        $x = 0.0; $y = 0.0; $curFont = $null
        $toks = [regex]::Matches($cs, '\((?:\.|[^\\()])*\)|\[(?:[^\[\]]*)\]|<[0-9A-Fa-f\s]*>|/[A-Za-z0-9]+|BT|ET|Tf|Td|TD|Tm|Tj|TJ|-?\d+(?:\.\d+)?')
        $i = 0
        while ($i -lt $toks.Count) {
            $t = $toks[$i].Value
            if ($t -eq 'BT' -or $t -eq 'ET') { }
            elseif ($t -match '^/F\d+$') { $curFont = $t.Substring(1) }
            elseif ($t -eq 'Tm') {
                if ($i -ge 6) { $x = [double]$toks[$i-1].Value; $y = [double]$toks[$i-2].Value }
            }
            elseif ($t -eq 'Td' -or $t -eq 'TD') {
                if ($i -ge 2) { $x += [double]$toks[$i-2].Value; $y += [double]$toks[$i-1].Value }
            }
            elseif ($t -ceq 'Tj') {
                if ($i -ge 1 -and ($toks[$i-1].Value.StartsWith('(') -or $toks[$i-1].Value.StartsWith('<'))) {
                    $fnum = 0; if ($fonts.ContainsKey($curFont)) { $fnum = $fonts[$curFont] }
                    $txt = Decode-Text (Bytes-From-String $toks[$i-1].Value) $fnum
                    if ($txt -match '\S') { $script:allRuns += @{ page = $pnum; y = $y; x = $x; text = $txt } }
                }
            }
            elseif ($t -ceq 'TJ') {
                if ($i -ge 1 -and $toks[$i-1].Value.StartsWith('[')) {
                    $arr = $toks[$i-1].Value.Substring(1, $toks[$i-1].Value.Length - 2)
                    $line = New-Object System.Text.StringBuilder
                    foreach ($sg in [regex]::Matches($arr, '\((?:\.|[^\\()])*\)|<[0-9A-Fa-f]+>')) {
                        $fnum = 0; if ($fonts.ContainsKey($curFont)) { $fnum = $fonts[$curFont] }
                        [void]$line.Append((Decode-Text (Bytes-From-String $sg.Value) $fnum))
                    }
                    if ($line.ToString() -match '\S') { $script:allRuns += @{ page = $pnum; y = $y; x = $x; text = $line.ToString() } }
                }
            }
            $i++
        }
    }
}
"Runs: $($script:allRuns.Count)" | Write-Host

# ---- 5. assemble ----
$sorted = $script:allRuns | Sort-Object @{ Expression = { $_.page } }, @{ Expression = { -$_.y } }, @{ Expression = { $_.x } }
$sb = New-Object System.Text.StringBuilder
$lastPage = -1; $lastY = 1e9
foreach ($r in $sorted) {
    if ($r.page -ne $lastPage) {
        if ($lastPage -ge 0) { [void]$sb.AppendLine("`n===== PAGE $($r.page) =====") }
        else { [void]$sb.AppendLine("===== PAGE $($r.page) =====") }
        $lastPage = $r.page; $lastY = $r.y
    } elseif ([Math]::Abs($r.y - $lastY) -gt 6) {
        [void]$sb.AppendLine(''); $lastY = $r.y
    } else { [void]$sb.Append(' ') }
    [void]$sb.Append($r.text)
}
$result = $sb.ToString()
if ($Out) { $result | Out-File $Out -Encoding utf8 }
"chars: $($result.Length)" | Write-Host
if (-not $Out) { $result }
