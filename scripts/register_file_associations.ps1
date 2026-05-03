param(
    [string]$ExePath = "",
    [string]$IconPath = ""
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingPath([string[]]$Candidates) {
    foreach ($c in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($c)) { continue }
        if (Test-Path -LiteralPath $c) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    return ""
}

function Set-DefaultValue([string]$Path, [string]$Value) {
    New-Item -Path $Path -Force | Out-Null
    New-ItemProperty -Path $Path -Name "" -PropertyType String -Value $Value -Force | Out-Null
}

$root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Resolve-ExistingPath @(
        (Join-Path $root "cmake-build-release\SyncPlay.exe"),
        (Join-Path $root "cmake-build-debug\SyncPlay.exe"),
        (Join-Path $root "cmake-build-publish\SyncPlay.exe"),
        (Join-Path (Get-Location) "SyncPlay.exe")
    )
} else {
    $ExePath = Resolve-ExistingPath @($ExePath)
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    throw "SyncPlay.exe not found. Pass -ExePath."
}

if ([string]::IsNullOrWhiteSpace($IconPath)) {
    $IconPath = Resolve-ExistingPath @(
        (Join-Path $root "assets\SyncPlay.ico"),
        (Join-Path (Split-Path $ExePath -Parent) "assets\SyncPlay.ico")
    )
}

if ([string]::IsNullOrWhiteSpace($IconPath)) {
    $IconPath = $ExePath
}

$progId = "SyncPlay.Video"
$exts = @(".mp4", ".mkv", ".mov", ".avi", ".webm")
$base = "HKCU:\Software\Classes"

Set-DefaultValue "$base\$progId" "SyncPlay Video"
Set-DefaultValue "$base\$progId\DefaultIcon" "`"$IconPath`""
Set-DefaultValue "$base\$progId\shell\open\command" "`"$ExePath`" `"%1`""

Set-DefaultValue "$base\Applications\SyncPlay.exe\shell\open\command" "`"$ExePath`" `"%1`""

foreach ($ext in $exts) {
    New-Item -Path "$base\$ext\OpenWithProgids" -Force | Out-Null
    New-ItemProperty -Path "$base\$ext\OpenWithProgids" -Name $progId -Value "" -PropertyType String -Force | Out-Null
}

Write-Host "Registered SyncPlay as an 'Open with' option for: $($exts -join ', ')"
Write-Host "Defaults are unchanged."
