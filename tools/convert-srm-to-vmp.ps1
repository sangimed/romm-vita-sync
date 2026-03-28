param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$InputPath,

    [Parameter(Position = 1)]
    [string]$OutputPath,

    [Parameter(Position = 2)]
    [string]$TemplateVmpPath,

    [ValidateSet('0','1')]
    [string]$Slot,

    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$outputWasProvided = $PSBoundParameters.ContainsKey('OutputPath')

function Get-DefaultOutputPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath
    )

    $extension = [System.IO.Path]::GetExtension($SourcePath)
    if ($extension -and $extension.Equals(".srm", [System.StringComparison]::OrdinalIgnoreCase)) {
        return [System.IO.Path]::ChangeExtension($SourcePath, ".vmp")
    }

    return "$SourcePath.vmp"
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

if ($Slot -and -not $outputWasProvided) {
    $inputDir = Split-Path -Parent $inputFullPath
    $OutputPath = Join-Path $inputDir ("SCEVMC{0}.VMP" -f $Slot)
}

if (-not $TemplateVmpPath) {
    if ($env:ROMM_VMP_TEMPLATE_PATH) {
        $TemplateVmpPath = $env:ROMM_VMP_TEMPLATE_PATH
    } else {
        $defaultTemplatePath = Join-Path $scriptDir "SCEVMC0.VMP"
        if (Test-Path -LiteralPath $defaultTemplatePath) {
            $TemplateVmpPath = $defaultTemplatePath
        } else {
            $repoTemplate = Join-Path $repoRoot "samples/vmp-templates/SCEVMC0.VMP"
            if (Test-Path -LiteralPath $repoTemplate) {
                $TemplateVmpPath = $repoTemplate
            }
        }
    }
}

if (-not $TemplateVmpPath) {
    throw "Template VMP required. Pass an existing .VMP file as the third argument or set ROMM_VMP_TEMPLATE_PATH. A reference template is provided in samples/vmp-templates/ when checked out."
}

$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$templateFullPath = [System.IO.Path]::GetFullPath($TemplateVmpPath)

if (-not (Test-Path -LiteralPath $templateFullPath)) {
    throw "Template VMP not found: $templateFullPath"
}

$buildDir = Join-Path $repoRoot "build-tools"
$candidateBinaries = @(
    (Join-Path $buildDir "Release\srm2vmp.exe"),
    (Join-Path $buildDir "Release\srm2vmp"),
    (Join-Path $buildDir "srm2vmp.exe"),
    (Join-Path $buildDir "srm2vmp"),
    (Join-Path $buildDir "RelWithDebInfo\srm2vmp.exe"),
    (Join-Path $buildDir "RelWithDebInfo\srm2vmp"),
    (Join-Path $buildDir "Debug\srm2vmp.exe"),
    (Join-Path $buildDir "Debug\srm2vmp")
)

$signerCandidates = @(
    (Join-Path $buildDir "Release\vita-mcr2vmp.exe"),
    (Join-Path $buildDir "Release\vita-mcr2vmp"),
    (Join-Path $buildDir "vita-mcr2vmp.exe"),
    (Join-Path $buildDir "vita-mcr2vmp"),
    (Join-Path $buildDir "RelWithDebInfo\vita-mcr2vmp.exe"),
    (Join-Path $buildDir "RelWithDebInfo\vita-mcr2vmp"),
    (Join-Path $buildDir "Debug\vita-mcr2vmp.exe"),
    (Join-Path $buildDir "Debug\vita-mcr2vmp")
)

$converterExe = if ($Rebuild) { $null } else { Find-ConverterBinary -Candidates $candidateBinaries }

if (-not $converterExe) {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "srm2vmp.exe not found and cmake is unavailable. Build the tools target first."
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
        throw "Build finished but srm2vmp.exe was not found."
    }
}

$signerExe = if ($Rebuild) { $null } else { Find-ConverterBinary -Candidates $signerCandidates }

if (-not $signerExe) {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "vita-mcr2vmp.exe not found and cmake is unavailable. Build the tools target first."
    }

    & $cmake.Source -S (Join-Path $repoRoot "tools") -B $buildDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed."
    }

    & $cmake.Source --build $buildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed."
    }

    $signerExe = Find-ConverterBinary -Candidates $signerCandidates
    if (-not $signerExe) {
        throw "Build finished but vita-mcr2vmp.exe was not found. Ensure submodule tools/vita-mcr2vmp is present."
    }
}

& $converterExe $inputFullPath $templateFullPath $outputFullPath
if ($LASTEXITCODE -ne 0) {
    throw "Conversion failed."
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($tempDir) | Out-Null

try {
    $unsignedVmpPath = Join-Path $tempDir "unsigned.VMP"
    Copy-Item -LiteralPath $outputFullPath -Destination $unsignedVmpPath -Force

    & $signerExe $unsignedVmpPath
    if ($LASTEXITCODE -ne 0) {
        throw "Signing step (VMP -> MCR) failed."
    }

    $mcrPath = "$unsignedVmpPath.mcr"
    if (-not (Test-Path -LiteralPath $mcrPath)) {
        throw "Signing failed: expected intermediate MCR not found at $mcrPath"
    }

    & $signerExe $mcrPath
    if ($LASTEXITCODE -ne 0) {
        throw "Signing step (MCR -> VMP) failed."
    }

    $signedVmpPath = "$mcrPath.VMP"
    if (-not (Test-Path -LiteralPath $signedVmpPath)) {
        throw "Signing failed: expected signed VMP not found at $signedVmpPath"
    }

    Copy-Item -LiteralPath $signedVmpPath -Destination $outputFullPath -Force
}
finally {
    if (Test-Path -LiteralPath $tempDir) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
}

Write-Host "VMP generated and signed: $outputFullPath"
