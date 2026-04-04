[CmdletBinding()]
param(
    [string]$DockerImage = $env:VITASDK_DOCKER_IMAGE,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildScriptArgs
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($DockerImage)) {
    $DockerImage = "gnuton/vitasdk-docker"
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker CLI is required but was not found in PATH."
}

try {
    docker info | Out-Null
}
catch {
    throw "Docker daemon is not reachable. Start Docker Desktop/Engine and retry."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

Write-Host "==> Docker build+upload wrapper"
Write-Host "==> Image: $DockerImage"
Write-Host "==> Container lifecycle: --rm (auto removed after script completion)"

$dockerArgs = @(
    "run",
    "--rm",
    "-v",
    "${repoRoot}:/build/git",
    $DockerImage,
    "sh",
    "-lc",
    'set -eu; cd /build; ./git/tools/build-and-upload-vpk.sh "$@"',
    "--"
)

if ($BuildScriptArgs) {
    $dockerArgs += $BuildScriptArgs
}

& docker @dockerArgs
$dockerExitCode = $LASTEXITCODE
if ($dockerExitCode -ne 0) {
    exit $dockerExitCode
}

Write-Host "==> Done"
