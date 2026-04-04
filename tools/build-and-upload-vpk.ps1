[CmdletBinding()]
param(
    [string]$FtpHost,
    [int]$FtpPort,
    [string]$FtpRemoteDir,
    [string]$Configuration = "Release",
    [string]$ConfigFile
)

$ErrorActionPreference = "Stop"

$defaults = @{
    FtpHost = "192.168.1.20"
    FtpPort = 1337
    FtpRemoteDir = "ux0:/homebrews"
}
$ftpConnectTimeoutSeconds = 10
$ftpUploadStallTimeoutSeconds = 10
$ftpUploadStallSpeedBytes = 1

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = Join-Path $repoRoot "build"

if ([string]::IsNullOrWhiteSpace($ConfigFile)) {
    $ConfigFile = Join-Path $PSScriptRoot "build-and-upload-vpk.local.env"
}

$config = @{}
if (Test-Path -LiteralPath $ConfigFile) {
    foreach ($rawLine in Get-Content -LiteralPath $ConfigFile) {
        $line = $rawLine.Trim()
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line.StartsWith("#")) {
            continue
        }

        $separatorIndex = $line.IndexOf("=")
        if ($separatorIndex -le 0) {
            continue
        }

        $key = $line.Substring(0, $separatorIndex).Trim()
        $value = $line.Substring($separatorIndex + 1).Trim()
        if (($value.Length -ge 2) -and (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'")))) {
            $value = $value.Substring(1, $value.Length - 2)
        }

        switch ($key) {
            "FTP_HOST" {
                if (-not [string]::IsNullOrWhiteSpace($value)) {
                    $config["FTP_HOST"] = $value
                }
            }
            "FTP_PORT" {
                if (-not [string]::IsNullOrWhiteSpace($value)) {
                    $config["FTP_PORT"] = $value
                }
            }
            "FTP_REMOTE_DIR" {
                if (-not [string]::IsNullOrWhiteSpace($value)) {
                    $config["FTP_REMOTE_DIR"] = $value
                }
            }
            default {}
        }
    }
    Write-Host "==> Config loaded: $ConfigFile"
}
else {
    Write-Host "==> Config not found: $ConfigFile (defaults will be used when needed)"
}

$resolvedFtpHost = $defaults.FtpHost
if ($config.ContainsKey("FTP_HOST") -and -not [string]::IsNullOrWhiteSpace([string]$config["FTP_HOST"])) {
    $resolvedFtpHost = [string]$config["FTP_HOST"]
}
if ($PSBoundParameters.ContainsKey("FtpHost") -and -not [string]::IsNullOrWhiteSpace($FtpHost)) {
    $resolvedFtpHost = $FtpHost
}

$resolvedFtpPort = $defaults.FtpPort
if ($config.ContainsKey("FTP_PORT")) {
    $portFromConfig = 0
    if ([int]::TryParse([string]$config["FTP_PORT"], [ref]$portFromConfig)) {
        $resolvedFtpPort = $portFromConfig
    }
}
if ($PSBoundParameters.ContainsKey("FtpPort")) {
    $resolvedFtpPort = $FtpPort
}

$resolvedFtpRemoteDir = $defaults.FtpRemoteDir
if ($config.ContainsKey("FTP_REMOTE_DIR") -and -not [string]::IsNullOrWhiteSpace([string]$config["FTP_REMOTE_DIR"])) {
    $resolvedFtpRemoteDir = [string]$config["FTP_REMOTE_DIR"]
}
if ($PSBoundParameters.ContainsKey("FtpRemoteDir") -and -not [string]::IsNullOrWhiteSpace($FtpRemoteDir)) {
    $resolvedFtpRemoteDir = $FtpRemoteDir
}

$resolvedFtpRemoteDir = $resolvedFtpRemoteDir.Trim('/')

Push-Location $repoRoot
try {
    Write-Host "==> Configure CMake"
    cmake -S . -B $buildDir

    Write-Host "==> Build VPK ($Configuration)"
    cmake --build $buildDir --config $Configuration

    $vpkFile = Get-ChildItem (Join-Path $buildDir "*.vpk") -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $vpkFile) {
        throw "No .vpk file found in $buildDir"
    }

    $remoteUrl = "ftp://$resolvedFtpHost`:$resolvedFtpPort/$resolvedFtpRemoteDir/$($vpkFile.Name)"
    Write-Host "==> Upload: $($vpkFile.FullName)"
    Write-Host "==> Destination: $remoteUrl"
    Write-Host "==> FTP timeout policy"
    Write-Host "    - Connection timeout: $($ftpConnectTimeoutSeconds)s"
    Write-Host "    - Upload stall timeout: $($ftpUploadStallTimeoutSeconds)s (speed < $ftpUploadStallSpeedBytes B/s)"

    # Use curl.exe to avoid the PowerShell curl alias.
    & curl.exe `
        --ftp-method nocwd `
        --connect-timeout $ftpConnectTimeoutSeconds `
        --speed-time $ftpUploadStallTimeoutSeconds `
        --speed-limit $ftpUploadStallSpeedBytes `
        -T "$($vpkFile.FullName)" `
        "$remoteUrl"

    if ($LASTEXITCODE -ne 0) {
        if ($LASTEXITCODE -eq 28) {
            throw "FTP timeout reached (connection or upload stalled for 10 seconds)."
        }
        throw "FTP upload failed with curl exit code $LASTEXITCODE."
    }

    Write-Host "==> Completed"
}
finally {
    Pop-Location
}
