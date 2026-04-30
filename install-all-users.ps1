param(
    [string]$InstallDir = "$env:ProgramFiles\wdu"
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Normalize-PathEntry([string]$PathEntry) {
    return $PathEntry.Trim().TrimEnd('\')
}

if (-not (Test-IsAdmin)) {
    Write-Error "All-users install requires Administrator rights. Open PowerShell as Administrator, cd to this folder, then run: .\install-all-users.ps1"
}

$sourceExe = Join-Path $PSScriptRoot "wdu.exe"
if (-not (Test-Path $sourceExe)) {
    $buildScript = Join-Path $PSScriptRoot "build.bat"
    if (Test-Path $buildScript) {
        & $buildScript
    }
}

if (-not (Test-Path $sourceExe)) {
    Write-Error "wdu.exe was not found. Build it first with build.bat, then run this installer again."
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Force -Path $sourceExe -Destination (Join-Path $InstallDir "wdu.exe")

$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
$entries = @()
if (-not [string]::IsNullOrWhiteSpace($machinePath)) {
    $entries = $machinePath -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}

$normalizedInstallDir = Normalize-PathEntry $InstallDir
$alreadyPresent = $false
foreach ($entry in $entries) {
    if ((Normalize-PathEntry $entry) -ieq $normalizedInstallDir) {
        $alreadyPresent = $true
        break
    }
}

if (-not $alreadyPresent) {
    $newPath = ($entries + $InstallDir) -join ';'
    [Environment]::SetEnvironmentVariable("Path", $newPath, "Machine")
}

$env:Path = $env:Path + ";" + $InstallDir
$installed = Get-Command wdu.exe -ErrorAction SilentlyContinue

Write-Host "Installed wdu for all users:"
Write-Host "  $(Join-Path $InstallDir 'wdu.exe')"
if ($installed) {
    Write-Host "Verified command:"
    Write-Host "  $($installed.Source)"
}
Write-Host "Open a new terminal before using wdu from other sessions."
