[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$CMakePath,

    [switch]$Clean,

    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Normalize-Path {
    param([Parameter(Mandatory)][string]$Path)
    return [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
}

$projectRoot = Normalize-Path $PSScriptRoot
$outRoot = Join-Path $projectRoot "out"
$buildDirectory = Join-Path $outRoot "build"
$runtimeDirectory = Join-Path $outRoot "bin"

if ($Clean) {
    # out/bin 中包含运行后生成的便携数据库与凭据，绝不由构建脚本清理。
    $resolved = Normalize-Path $buildDirectory
    if (-not [IO.Path]::GetDirectoryName($resolved).Equals(
            $outRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        $item = Get-Item -LiteralPath $resolved -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean reparse point: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

New-Item -ItemType Directory -Path $outRoot -Force | Out-Null

$cmakeCandidates = [Collections.Generic.List[string]]::new()
if ($CMakePath) {
    $cmakeCandidates.Add($CMakePath)
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = $null
if (Test-Path -LiteralPath $vswhere) {
    $visualStudio = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -eq 0 -and $visualStudio) {
        $cmakeCandidates.Add((Join-Path ($visualStudio | Select-Object -First 1) `
            "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"))
    }
}

$pathCMake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($pathCMake) {
    $cmakeCandidates.Add($pathCMake.Source)
}

$cmake = $null
foreach ($candidate in $cmakeCandidates) {
    if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $cmake = Normalize-Path $candidate
        break
    }
}
if (-not $cmake) {
    throw "CMake was not found. Install Visual Studio 2022 C++ tools or pass -CMakePath."
}

$ctest = Join-Path ([IO.Path]::GetDirectoryName($cmake)) "ctest.exe"
if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
    throw "CTest was not found next to CMake: $ctest"
}

$testsDirectory = Join-Path $projectRoot "tests"
$buildTests = if ($SkipTests -or
    -not (Test-Path -LiteralPath $testsDirectory -PathType Container)) {
    "OFF"
} else {
    "ON"
}

$generatorInstance = $null
if (-not $visualStudio) {
    # 手动迁移的 VS 未必登记在 Installer 中；CMake 可使用附带的实例版本识别它。
    $candidateRoot = [IO.Path]::GetDirectoryName($cmake)
    for ($level = 0; $level -lt 7; ++$level) {
        $candidateRoot = [IO.Path]::GetDirectoryName($candidateRoot)
    }
    $devenv = Join-Path $candidateRoot "Common7\IDE\devenv.exe"
    if (Test-Path -LiteralPath $devenv -PathType Leaf) {
        $vsVersion = (Get-Item -LiteralPath $devenv).VersionInfo.ProductVersion.Split(' ')[0]
        $generatorInstance = "$($candidateRoot.Replace('\', '/')),version=$vsVersion"
    }
}

Write-Host "CMake: $cmake" -ForegroundColor Cyan
$configureArgs = @(
    "-S", $projectRoot,
    "-B", $buildDirectory,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DSLINK_BUILD_TESTS=$buildTests")
if ($generatorInstance) {
    $configureArgs += "-DCMAKE_GENERATOR_INSTANCE=$generatorInstance"
}
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

& $cmake --build $buildDirectory `
    --config $Configuration `
    --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

# 旧版本曾将调试符号写到 out/bin；仅迁移这两个已知构建产物，不处理用户数据。
$legacySymbols = @(@("SLink.pdb", "SLink.map") | Where-Object {
    Test-Path -LiteralPath (Join-Path $runtimeDirectory $_) -PathType Leaf
})
if ($legacySymbols.Count -gt 0) {
    $archiveDirectory = Join-Path $buildDirectory `
        ("symbols\previous-out-bin-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $archiveDirectory -Force | Out-Null
    foreach ($file in $legacySymbols) {
        Move-Item -LiteralPath (Join-Path $runtimeDirectory $file) `
            -Destination $archiveDirectory
    }
}

if (-not $SkipTests) {
    & $ctest --test-dir $buildDirectory `
        --build-config $Configuration `
        --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE."
    }
}

$requiredOutputs = @("SLink.exe", "Scintilla.dll", "Lexilla.dll")
foreach ($file in $requiredOutputs) {
    $path = Join-Path $runtimeDirectory $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Expected build output was not produced: $path"
    }
}
$unexpectedOutputs = @(Get-ChildItem -LiteralPath $runtimeDirectory -File |
    Where-Object { $_.Name -notin $requiredOutputs } |
    Select-Object -ExpandProperty Name)
if ($unexpectedOutputs.Count -gt 0) {
    throw "Unexpected files in $runtimeDirectory`: $($unexpectedOutputs -join ', ')"
}

Write-Host "`nBuild completed: $runtimeDirectory" -ForegroundColor Green
Get-ChildItem -LiteralPath $runtimeDirectory -File |
    Select-Object Name, Length
