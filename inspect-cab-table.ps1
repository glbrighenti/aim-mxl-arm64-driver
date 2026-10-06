param(
    [Parameter(Mandatory = $true)][string]$HeaderPath,
    [string]$CabPath,
    [string]$ExtractName,
    [string]$OutputPath,
    [string]$NamePattern = 'AIM_USB|11CC'
)

$bytes = [IO.File]::ReadAllBytes($HeaderPath)
if ([Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'ISc(') {
    throw 'Not an InstallShield cabinet header'
}

$descriptorOffset = [BitConverter]::ToUInt32($bytes, 12)
$tableOffset = $descriptorOffset + [BitConverter]::ToUInt32($bytes, $descriptorOffset + 12)
$directoryCount = [BitConverter]::ToUInt32($bytes, $descriptorOffset + 28)
$fileCount = [BitConverter]::ToUInt32($bytes, $descriptorOffset + 40)
$descriptorStart = $tableOffset + [BitConverter]::ToUInt32($bytes, $descriptorOffset + 44)
if ($descriptorStart + 0x57 * $fileCount -gt $bytes.Length) {
    throw "File descriptors exceed header: offset=$descriptorStart files=$fileCount"
}

"Table offset: $tableOffset; directories: $directoryCount; files: $fileCount"
for ($index = 0; $index -lt $fileCount; $index++) {
    $entry = $descriptorStart + 0x57 * $index
    $nameOffset = [BitConverter]::ToUInt32($bytes, $entry + 0x3a)
    $nameStart = $tableOffset + $nameOffset
    if ($nameOffset -eq 0 -or $nameStart -ge $bytes.Length) { continue }
    $nameEnd = $nameStart
    while ($nameEnd -lt $bytes.Length -and $bytes[$nameEnd] -ne 0) { $nameEnd++ }
    $name = [Text.Encoding]::ASCII.GetString($bytes, $nameStart, $nameEnd - $nameStart)
    if ($name -match $NamePattern) {
        $flags = [BitConverter]::ToUInt16($bytes, $entry)
        $expanded = [BitConverter]::ToUInt64($bytes, $entry + 2)
        $compressed = [BitConverter]::ToUInt64($bytes, $entry + 10)
        $dataOffset = [BitConverter]::ToUInt64($bytes, $entry + 18)
        $volume = [BitConverter]::ToUInt16($bytes, $entry + 0x55)
        "[$index] $name volume=$volume offset=$dataOffset compressed=$compressed expanded=$expanded flags=$flags"
        if ($name -eq $ExtractName) {
            if ($volume -ne 2 -or $flags -ne 4 -or !$CabPath -or !$OutputPath) {
                throw 'Expected a compressed volume-2 entry, a CAB path, and an output path'
            }
            $cab = [IO.File]::ReadAllBytes($CabPath)
            $position = [int]$dataOffset
            $end = $position + [int]$compressed
            $output = New-Object IO.MemoryStream
            try {
                while ($position -lt $end) {
                    $blockLength = [BitConverter]::ToUInt16($cab, $position)
                    $position += 2
                    if ($blockLength -eq 0 -or $position + $blockLength -gt $end) {
                        throw 'Invalid compressed block length'
                    }
                    $block = New-Object IO.MemoryStream($cab, $position, $blockLength, $false)
                    $inflater = New-Object IO.Compression.DeflateStream($block, [IO.Compression.CompressionMode]::Decompress)
                    try { $inflater.CopyTo($output) } finally { $inflater.Dispose(); $block.Dispose() }
                    $position += $blockLength
                }
                $result = $output.ToArray()
            } finally { $output.Dispose() }
            if ($result.Length -ne $expanded) { throw 'Expanded size mismatch' }
            $expectedHash = [BitConverter]::ToString($bytes, $entry + 26, 16)
            $actualHash = [BitConverter]::ToString(([Security.Cryptography.MD5]::Create()).ComputeHash($result))
            if ($actualHash -ne $expectedHash) { throw 'MD5 mismatch' }
            [IO.File]::WriteAllBytes($OutputPath, $result)
            "Extracted $name to $OutputPath"
        }
    }
}