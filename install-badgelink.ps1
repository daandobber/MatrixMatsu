param(
    [string]$Device = "tanmatsu",
    [string]$Slug = "matrixmatsu",
    [string]$Title = "MatrixMatsu",
    [int]$Version = 0,
    [switch]$NoBuild,
    [switch]$NoStart
)

# Must match APP_REPOSITORY_SLUG in main/main.c: that's the path the firmware
# actually looks for emoji.pak under on the SD card (/sd/apps/<slug>/), which is
# a different namespace than the BadgeLink AppFS dev slug above.
$AppRepositorySlug = "nl.daandobber.matrixmatsu"

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$IdfPath = Join-Path $Root "esp-idf"
$IdfToolsPath = Join-Path $Root "esp-idf-tools"
$BuildDir = Join-Path $Root "build\$Device"
$BadgelinkTools = Join-Path $Root "badgelink_v020\tools"
$BadgelinkPython = Join-Path $BadgelinkTools ".venv\Scripts\python.exe"
$BadgelinkScript = Join-Path $BadgelinkTools "badgelink.py"
$AppBin = Join-Path $BuildDir "application.bin"

function Invoke-BadgeLink {
    param(
        [string[]]$Arguments,
        [string[]]$FailurePatterns = @()
    )

    $output = & $BadgelinkPython $BadgelinkScript @Arguments 2>&1
    $output | ForEach-Object { Write-Host $_ }

    if ($LASTEXITCODE -ne 0) {
        throw "BadgeLink failed: $($Arguments -join ' ')"
    }

    $text = ($output | Out-String)
    foreach ($pattern in $FailurePatterns) {
        if ($text -match $pattern) {
            throw "BadgeLink reported failure: $pattern"
        }
    }
}

if (!(Test-Path $IdfPath)) {
    throw "ESP-IDF folder not found: $IdfPath"
}

if (!(Test-Path $BadgelinkScript)) {
    throw "BadgeLink tool not found: $BadgelinkScript"
}

if (!(Test-Path $BadgelinkPython)) {
    Write-Host "Creating BadgeLink Python environment..."
    python -m venv (Join-Path $BadgelinkTools ".venv")
    & $BadgelinkPython -m pip install -r (Join-Path $BadgelinkTools "requirements.txt")
}

if (!$NoBuild) {
    $rootSafe = $Root -replace "\\", "/"
    $idfSafe = $IdfPath -replace "\\", "/"
    $openthreadSafe = (Join-Path $IdfPath "components\openthread\openthread") -replace "\\", "/"

    $env:IDF_TOOLS_PATH = $IdfToolsPath
    $env:PYTHONIOENCODING = "utf-8"
    $env:GIT_CONFIG_COUNT = "3"
    $env:GIT_CONFIG_KEY_0 = "safe.directory"
    $env:GIT_CONFIG_VALUE_0 = $rootSafe
    $env:GIT_CONFIG_KEY_1 = "safe.directory"
    $env:GIT_CONFIG_VALUE_1 = $idfSafe
    $env:GIT_CONFIG_KEY_2 = "safe.directory"
    $env:GIT_CONFIG_VALUE_2 = $openthreadSafe

    . (Join-Path $IdfPath "export.ps1")

    Write-Host "Building $Device application..."
    idf.py `
        --no-ccache `
        -B "build/$Device" `
        build `
        -DDEVICE=$Device `
        "-DSDKCONFIG_DEFAULTS=sdkconfigs/general;sdkconfigs/$Device" `
        -DSDKCONFIG="sdkconfig_$Device" `
        -DIDF_TARGET=esp32p4 `
        -DFAT=0
}

if (!(Test-Path $AppBin)) {
    throw "Build output not found: $AppBin"
}

Write-Host "Checking BadgeLink connection..."
Invoke-BadgeLink -Arguments @("appfs", "list")

Write-Host "Uploading $AppBin as '$Slug'..."
Invoke-BadgeLink -Arguments @("appfs", "upload", $Slug, $Title, "$Version", $AppBin) -FailurePatterns @("Out of FLASH space", "not found", "Disconnected", "Malformed")

$EmojiPack = Join-Path $Root "emoji_assets\emoji.pak"
$EmojiPackSdPath = "/sd/apps/$AppRepositorySlug/emoji.pak"
if (Test-Path $EmojiPack) {
    Write-Host "Uploading emoji.pak to SD card at ${EmojiPackSdPath}..."
    try {
        Invoke-BadgeLink -Arguments @("fs", "mkdir", "/sd/apps")
    } catch {
        Write-Host "  (mkdir /sd/apps failed, probably already exists)"
    }
    try {
        Invoke-BadgeLink -Arguments @("fs", "mkdir", "/sd/apps/$AppRepositorySlug")
    } catch {
        Write-Host "  (mkdir /sd/apps/$AppRepositorySlug failed, probably already exists)"
    }
    try {
        Invoke-BadgeLink -Arguments @("fs", "upload", $EmojiPackSdPath, $EmojiPack)
    } catch {
        Write-Warning "Failed to upload emoji.pak to the SD card: $_"
    }
} else {
    Write-Host "No emoji_assets\emoji.pak found locally; skipping emoji asset upload. Run tools\make-emoji-assets.ps1 + tools\make-emoji-pack.ps1 first if you want emoji icons."
}

if (!$NoStart) {
    Write-Host "Starting '$Slug'..."
    Invoke-BadgeLink -Arguments @("start", $Slug) -FailurePatterns @("not found", "Disconnected", "Malformed")
}
