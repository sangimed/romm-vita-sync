param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InputPath,

    [Parameter(Position = 1)]
    [string]$OutputPath,

    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

function Get-DefaultOutputPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath
    )

    $extension = [System.IO.Path]::GetExtension($SourcePath)
    if ($extension -and $extension.Equals(".vmp", [System.StringComparison]::OrdinalIgnoreCase)) {
        return [System.IO.Path]::ChangeExtension($SourcePath, ".srm")
    }

    return "$SourcePath.srm"
}

function Find-ConverterBinary {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$inputFullPath = [System.IO.Path]::GetFullPath($InputPath)

if (-not (Test-Path -LiteralPath $inputFullPath)) {
    throw "Input file not found: $inputFullPath"
}

if (-not $OutputPath) {
    $OutputPath = Get-DefaultOutputPath -SourcePath $inputFullPath
}

$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$buildDir = Join-Path $repoRoot "build-tools"
$candidateBinaries = @(
    (Join-Path $buildDir "Release\vmp2srm.exe"),
    (Join-Path $buildDir "vmp2srm.exe"),
    (Join-Path $buildDir "RelWithDebInfo\vmp2srm.exe"),
    (Join-Path $buildDir "Debug\vmp2srm.exe")
)

$converterExe = if ($Rebuild) { $null } else { Find-ConverterBinary -Candidates $candidateBinaries }

if (-not $converterExe) {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "vmp2srm.exe not found and cmake is unavailable. Build the tools target first."
    }

    & $cmake.Source -S (Join-Path $repoRoot "tools") -B $buildDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed."
    }

    & $cmake.Source --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed."
    }

    $converterExe = Find-ConverterBinary -Candidates $candidateBinaries
    if (-not $converterExe) {
        throw "Build finished but vmp2srm.exe was not found."
    }
}

& $converterExe $inputFullPath $outputFullPath
if ($LASTEXITCODE -ne 0) {
    throw "Conversion failed."
}

Write-Host "SRM generated: $outputFullPath"
