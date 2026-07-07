param(
    [string]$DataFile = ".\tools\emoji-data.json",
    [string]$OutDir = ".\emoji_assets\argb8888"
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$items = Get-Content $DataFile -Raw | ConvertFrom-Json

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$pngDir = Join-Path (Split-Path -Parent $OutDir) "png"
New-Item -ItemType Directory -Force -Path $pngDir | Out-Null

$total = $items.Count
$done = 0
$skippedExisting = 0
$failed = @()

foreach ($item in $items) {
    $done++
    $outPath = Join-Path $OutDir "$($item.key).argb8888"
    if (Test-Path $outPath) {
        $skippedExisting++
        continue
    }

    $pngPath = Join-Path $pngDir $item.twemoji_file
    $url = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72/$($item.twemoji_file)"
    if (!(Test-Path $pngPath)) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $pngPath -TimeoutSec 30
        } catch {
            Write-Warning "[$done/$total] Skipping $($item.key) ($($item.name)): download failed: $url"
            $failed += $item.key
            continue
        }
    }

    try {
        $src = [System.Drawing.Image]::FromFile((Resolve-Path $pngPath))
        try {
            $dst = New-Object System.Drawing.Bitmap 32, 32, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $gfx = [System.Drawing.Graphics]::FromImage($dst)
                try {
                    $gfx.Clear([System.Drawing.Color]::Transparent)
                    $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                    $gfx.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $gfx.DrawImage($src, 0, 0, 32, 32)
                } finally {
                    $gfx.Dispose()
                }

                $fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
                try {
                    for ($y = 0; $y -lt 32; $y++) {
                        for ($x = 0; $x -lt 32; $x++) {
                            $c = $dst.GetPixel($x, $y)
                            $argb = (($c.A -shl 24) -bor ($c.R -shl 16) -bor ($c.G -shl 8) -bor $c.B)
                            $bytes = [System.BitConverter]::GetBytes([uint32]$argb)
                            $fs.Write($bytes, 0, 4)
                        }
                    }
                } finally {
                    $fs.Dispose()
                }
            } finally {
                $dst.Dispose()
            }
        } finally {
            $src.Dispose()
        }
    } catch {
        Write-Warning "[$done/$total] Skipping $($item.key) ($($item.name)): convert failed: $_"
        $failed += $item.key
        continue
    }

    if ($done % 100 -eq 0) {
        Write-Host "[$done/$total] processed ($skippedExisting already existed, $($failed.Count) failed)"
    }
}

Write-Host "Wrote assets for $($total - $skippedExisting - $failed.Count) new emoji ($skippedExisting already existed, $($failed.Count) failed) to $OutDir"
if ($failed.Count -gt 0) {
    Write-Host "Failed keys: $($failed -join ', ')"
}
