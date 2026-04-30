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
    Write-Error "All-users uninstall requires Administrator rights. Open PowerShell as Administrator, cd to this folder, then run: .\uninstall-all-users.ps1"
}

$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
if (-not [string]::IsNullOrWhiteSpace($machinePath)) {
    $normalizedInstallDir = Normalize-PathEntry $InstallDir
    $entries = $machinePath -split ';' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Where-Object { (Normalize-PathEntry $_) -ine $normalizedInstallDir }
    [Environment]::SetEnvironmentVariable("Path", ($entries -join ';'), "Machine")
}

if (Test-Path $InstallDir) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
}

Write-Host "Removed all-users wdu install from:"
Write-Host "  $InstallDir"
