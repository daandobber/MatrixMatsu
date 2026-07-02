param(
    [string]$AssetDir = ".\emoji_assets\argb8888",
    [string]$OutFile = ".\emoji_assets\emoji.pak"
)

$ErrorActionPreference = "Stop"

$files = Get-ChildItem $AssetDir -Filter *.argb8888 | Sort-Object Name
if ($files.Count -eq 0) {
    throw "No .argb8888 emoji assets found in $AssetDir"
}

$outDir = Split-Path -Parent $OutFile
if ($outDir) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$fs = [System.IO.File]::Open($OutFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
try {
    $magic = [byte[]](0x45, 0x50, 0x4B, 0x31) # EPK1 little-endian magic used by the app.
    $fs.Write($magic, 0, 4)

    $countBytes = [System.BitConverter]::GetBytes([uint32]$files.Count)
    $fs.Write($countBytes, 0, 4)

    foreach ($file in $files) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
        $nameBytes = New-Object byte[] 32
        $rawName = [System.Text.Encoding]::ASCII.GetBytes($name)
        [Array]::Copy($rawName, $nameBytes, [Math]::Min($rawName.Length, 31))
        $fs.Write($nameBytes, 0, 32)

        $data = [System.IO.File]::ReadAllBytes($file.FullName)
        if ($data.Length -ne 4096) {
            throw "Unexpected emoji size for $($file.Name): $($data.Length), expected 4096"
        }
        $fs.Write($data, 0, $data.Length)
    }
} finally {
    $fs.Dispose()
}

Write-Host "Wrote $($files.Count) emoji assets to $OutFile"
