[CmdletBinding(PositionalBinding=$false)]
param(
    [string]$BuildDir = '',
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')][string]$BuildType = 'Release',
    [ValidateSet('ON','OFF')][string]$Wx = 'ON',
    [ValidateSet('ON','OFF')][string]$RequireWx = 'OFF',
    [ValidateSet('ON','OFF')][string]$Cli = 'ON',
    [int]$Parallel = 0,
    [string]$Target = '',
    [string]$Generator = '',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$VcpkgTriplet = 'x64-windows-static',
    [switch]$NoVcpkg,
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ExtraCMakeArgs
)

$ErrorActionPreference = 'Stop'
$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ProjectName = Split-Path -Leaf $RootDir
$ProjectPrefix = (($ProjectName -replace '[^A-Za-z0-9]', '').ToUpperInvariant())
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RootDir 'build'
}
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeArgs = @()
if (-not [string]::IsNullOrWhiteSpace($Generator)) {
    $cmakeArgs += @('-G', $Generator)
}
$cmakeArgs += @(
    '-S', $RootDir,
    '-B', $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-D$($ProjectPrefix)_BUILD_WX_GUI=$Wx",
    "-D$($ProjectPrefix)_REQUIRE_WX_GUI=$RequireWx",
    "-D$($ProjectPrefix)_BUILD_CLI=$Cli"
)

if (-not $NoVcpkg -and -not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $ToolchainFile = Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
    if (Test-Path $ToolchainFile) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile"
        if (-not [string]::IsNullOrWhiteSpace($VcpkgTriplet)) {
            $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
        }
    }
}
if ($ExtraCMakeArgs -and $ExtraCMakeArgs.Count -gt 0) {
    $cmakeArgs += $ExtraCMakeArgs
}

Write-Host "Configuring $ProjectName in $BuildDir"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @('--build', $BuildDir, '--config', $BuildType)
if ($Parallel -gt 0) {
    $buildArgs += @('--parallel', [string]$Parallel)
}
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $buildArgs += @('--target', $Target)
}

Write-Host "Building $ProjectName"
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
