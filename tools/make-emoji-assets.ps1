param(
    [string]$OutDir = ".\emoji_assets\argb8888"
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$items = @(
    @{ Name = "smile";      Code = "1f642" },
    @{ Name = "grin";       Code = "1f600" },
    @{ Name = "joy";        Code = "1f602" },
    @{ Name = "heart_eyes"; Code = "1f60d" },
    @{ Name = "cool";       Code = "1f60e" },
    @{ Name = "cry";        Code = "1f622" },
    @{ Name = "thinking";   Code = "1f914" },
    @{ Name = "thumbs_up";  Code = "1f44d" },
    @{ Name = "thumbs_down";Code = "1f44e" },
    @{ Name = "wave";       Code = "1f44b" },
    @{ Name = "pray";       Code = "1f64f" },
    @{ Name = "heart";      Code = "2764" },
    @{ Name = "fire";       Code = "1f525" },
    @{ Name = "party";      Code = "1f389" },
    @{ Name = "rocket";     Code = "1f680" },
    @{ Name = "star";       Code = "2b50" },
    @{ Name = "check";      Code = "2705" },
    @{ Name = "cross";      Code = "274c" },
    @{ Name = "hundred";    Code = "1f4af" },
    @{ Name = "eyes";       Code = "1f440" },
    @{ Name = "angry";      Code = "1f621" },
    @{ Name = "skull";      Code = "1f480" },
    @{ Name = "sun";        Code = "2600" },
    @{ Name = "moon";       Code = "1f319" },
    @{ Name = "bolt";       Code = "26a1" },
    @{ Name = "gift";       Code = "1f381" },
    @{ Name = "music";      Code = "1f3b5" },
    @{ Name = "pizza";      Code = "1f355" },
    @{ Name = "coffee";     Code = "2615" },
    @{ Name = "wink";       Code = "1f609" },
    @{ Name = "blush";      Code = "1f60a" },
    @{ Name = "kiss";       Code = "1f618" },
    @{ Name = "heart_hands";Code = "1faf6" },
    @{ Name = "strong";     Code = "1f4aa" },
    @{ Name = "clap";       Code = "1f44f" },
    @{ Name = "sob";        Code = "1f62d" },
    @{ Name = "rofl";       Code = "1f923" },
    @{ Name = "melting";    Code = "1fae0" },
    @{ Name = "pleading";   Code = "1f97a" },
    @{ Name = "neutral";    Code = "1f610" },
    @{ Name = "unamused";   Code = "1f612" },
    @{ Name = "sweat_smile";Code = "1f605" },
    @{ Name = "relieved";   Code = "1f60c" },
    @{ Name = "sleeping";   Code = "1f634" },
    @{ Name = "flushed";    Code = "1f633" },
    @{ Name = "scream";     Code = "1f631" },
    @{ Name = "mind_blown"; Code = "1f92f" },
    @{ Name = "smirk";      Code = "1f60f" },
    @{ Name = "zany";       Code = "1f92a" },
    @{ Name = "party_face"; Code = "1f973" },
    @{ Name = "hugging";    Code = "1f917" },
    @{ Name = "shushing";   Code = "1f92b" },
    @{ Name = "facepalm";   Code = "1f926" },
    @{ Name = "shrug";      Code = "1f937" },
    @{ Name = "ok_hand";    Code = "1f44c" },
    @{ Name = "raised_hands";Code = "1f64c" },
    @{ Name = "point_right";Code = "1f449" },
    @{ Name = "point_left"; Code = "1f448" },
    @{ Name = "point_up";   Code = "261d" },
    @{ Name = "point_down"; Code = "1f447" },
    @{ Name = "handshake";  Code = "1f91d" },
    @{ Name = "orange_heart";Code = "1f9e1" },
    @{ Name = "yellow_heart";Code = "1f49b" },
    @{ Name = "green_heart";Code = "1f49a" },
    @{ Name = "blue_heart"; Code = "1f499" },
    @{ Name = "purple_heart";Code = "1f49c" },
    @{ Name = "black_heart";Code = "1f5a4" },
    @{ Name = "broken_heart";Code = "1f494" },
    @{ Name = "sparkles";   Code = "2728" },
    @{ Name = "poop";       Code = "1f4a9" },
    @{ Name = "boom";       Code = "1f4a5" },
    @{ Name = "drops";      Code = "1f4a6" },
    @{ Name = "zzz";        Code = "1f4a4" },
    @{ Name = "dash";       Code = "1f4a8" },
    @{ Name = "monkey_see"; Code = "1f648" },
    @{ Name = "cat_smile";  Code = "1f63a" },
    @{ Name = "dog";        Code = "1f436" },
    @{ Name = "cat";        Code = "1f431" },
    @{ Name = "beer";       Code = "1f37a" },
    @{ Name = "wine";       Code = "1f377" },
    @{ Name = "burger";     Code = "1f354" },
    @{ Name = "fries";      Code = "1f35f" },
    @{ Name = "cake";       Code = "1f382" },
    @{ Name = "soccer";     Code = "26bd" },
    @{ Name = "game";       Code = "1f3ae" },
    @{ Name = "phone";      Code = "1f4f1" },
    @{ Name = "laptop";     Code = "1f4bb" },
    @{ Name = "bulb";       Code = "1f4a1" },
    @{ Name = "money";      Code = "1f4b0" },
    @{ Name = "gem";        Code = "1f48e" },
    @{ Name = "warning";    Code = "26a0" },
    @{ Name = "question";   Code = "2753" },
    @{ Name = "exclamation";Code = "2757" },
    @{ Name = "calendar";   Code = "1f4c5" },
    @{ Name = "clock";      Code = "1f552" },
    @{ Name = "home";       Code = "1f3e0" },
    @{ Name = "car";        Code = "1f697" },
    @{ Name = "train";      Code = "1f686" },
    @{ Name = "airplane";   Code = "2708" },
    @{ Name = "globe";      Code = "1f30d" },
    @{ Name = "flag_eu";    Code = "1f1ea-1f1fa" },
    @{ Name = "flag_lu";    Code = "1f1f1-1f1fa" },
    @{ Name = "flag_lv";    Code = "1f1f1-1f1fb" },
    @{ Name = "flag_nl";    Code = "1f1f3-1f1f1" },
    @{ Name = "rainbow";    Code = "1f308" },
    @{ Name = "snowflake";  Code = "2744" },
    @{ Name = "umbrella";   Code = "2614" },
    @{ Name = "cloud";      Code = "2601" },
    @{ Name = "lock";       Code = "1f512" },
    @{ Name = "key";        Code = "1f511" },
    @{ Name = "gear";       Code = "2699" },
    @{ Name = "magnify";    Code = "1f50d" },
    @{ Name = "bell";       Code = "1f514" },
    @{ Name = "pin";        Code = "1f4cc" }
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$pngDir = Join-Path (Split-Path -Parent $OutDir) "png"
New-Item -ItemType Directory -Force -Path $pngDir | Out-Null

foreach ($item in $items) {
    $pngPath = Join-Path $pngDir "$($item.Name).png"
    $url = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72/$($item.Code).png"
    if (!(Test-Path $pngPath)) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $pngPath
        } catch {
            Write-Warning "Skipping $($item.Name): $url"
            continue
        }
    }

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

            $outPath = Join-Path $OutDir "$($item.Name).argb8888"
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
}

Write-Host "Wrote $($items.Count) emoji assets to $OutDir"
