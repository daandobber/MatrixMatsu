param(
    [string]$TargetParent = (Get-Location).Path,
    [string]$FolderName = "MatrixMatsu",
    [string]$ProjectRepo = "https://github.com/daandobber/MatrixMatsu.git",
    [string]$TemplateRepo = "https://github.com/Nicolai-Electronics/tanmatsu-template.git",
    [switch]$CloneTemplateToo,
    [switch]$SetupSdk,
    [switch]$SetupBadgeLink
)

$ErrorActionPreference = "Stop"

function Invoke-Step {
    param([string]$Title, [scriptblock]$Body)
    Write-Host ""
    Write-Host "==> $Title"
    & $Body
}

function Ensure-Remote {
    param(
        [string]$Name,
        [string]$Url
    )

    $existing = git remote get-url $Name 2>$null
    if ($LASTEXITCODE -eq 0) {
        if ($existing -ne $Url) {
            git remote set-url $Name $Url
        }
    } else {
        git remote add $Name $Url
    }
}

$target = Join-Path $TargetParent $FolderName

Invoke-Step "Clone MatrixMatsu project repo" {
    if (Test-Path (Join-Path $target ".git")) {
        Write-Host "Repository already exists: $target"
    } elseif (Test-Path $target) {
        throw "Target folder exists but is not a git repository: $target"
    } else {
        git clone $ProjectRepo $target
    }
}

Set-Location $target

Invoke-Step "Configure remotes" {
    Ensure-Remote -Name "matrixmatsu" -Url $ProjectRepo
    Ensure-Remote -Name "template" -Url $TemplateRepo
    git fetch matrixmatsu
    git fetch template
    git remote -v
}

if ($CloneTemplateToo) {
    Invoke-Step "Clone original Tanmatsu template next to MatrixMatsu" {
        $templateTarget = Join-Path $TargetParent "tanmatsu-template-upstream"
        if (Test-Path (Join-Path $templateTarget ".git")) {
            Write-Host "Template repository already exists: $templateTarget"
        } elseif (Test-Path $templateTarget) {
            throw "Template target folder exists but is not a git repository: $templateTarget"
        } else {
            git clone $TemplateRepo $templateTarget
        }
    }
}

if ($SetupSdk) {
    Invoke-Step "Clone and install ESP-IDF" {
        $env:IDF_TOOLS_PATH = Join-Path (Get-Location).Path "esp-idf-tools"
        if (Test-Path ".\esp-idf") {
            Write-Host "esp-idf already exists."
        } else {
            git clone --recursive --branch v5.5.1 --depth=1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf
        }
        if (Test-Path ".\esp-idf\install.ps1") {
            powershell -ExecutionPolicy Bypass -File ".\esp-idf\install.ps1" all
        } else {
            Write-Host "ESP-IDF cloned. Run the platform-specific install script in .\esp-idf manually."
        }
    }
}

if ($SetupBadgeLink) {
    Invoke-Step "Clone BadgeLink tooling" {
        if (Test-Path ".\badgelink_v020\tools\badgelink.py") {
            Write-Host "BadgeLink tooling already exists."
        } else {
            git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink_v020
        }
    }
}

Write-Host ""
Write-Host "Ready: $target"
Write-Host ""
Write-Host "Next commands:"
Write-Host "  Get-Content AGENTS.md"
Write-Host "  Get-Content starter-kit\COMMANDS.md"
Write-Host "  `$env:IDF_TOOLS_PATH = `"`$(Get-Location)\esp-idf-tools`""
Write-Host "  . .\esp-idf\export.ps1"
Write-Host "  idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0"
Write-Host "  .\install-badgelink.ps1 -NoBuild"
