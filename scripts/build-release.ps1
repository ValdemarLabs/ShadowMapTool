param(
    [ValidateSet("Release")]
    [string]$Configuration = "Release",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Find-BuildTool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $relativePath = "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$Name.exe"
        $found = & $vswhere -latest -products '*' -find $relativePath | Select-Object -First 1
        if ($found -and (Test-Path -LiteralPath $found)) {
            return $found
        }
    }

    throw "$Name was not found. Install CMake or a Visual Studio C++ workload with CMake tools."
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$cmakeFile = Join-Path $projectRoot "CMakeLists.txt"
$cmakeText = Get-Content -Raw -LiteralPath $cmakeFile
$versionMatch = [regex]::Match($cmakeText, 'project\(ShadowMapTool VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) {
    throw "Could not read the project version from CMakeLists.txt."
}

$version = $versionMatch.Groups[1].Value
$buildDir = Join-Path $projectRoot "build"
$distDir = Join-Path $projectRoot "dist"
$packageName = "ShadowMapTool-$version-win64"
$stageDir = Join-Path $distDir $packageName
$zipPath = Join-Path $distDir "$packageName.zip"
$checksumPath = Join-Path $distDir "SHA256SUMS.txt"

if (-not $SkipBuild) {
    $cmake = Find-BuildTool "cmake"
    $ctest = Find-BuildTool "ctest"
    & $cmake -S $projectRoot -B $buildDir -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    & $cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "Release build failed." }
    & $ctest --test-dir $buildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Release tests failed." }
}

$binaryDir = Join-Path $buildDir $Configuration
$files = @(
    (Join-Path $binaryDir "w3shadow-gui.exe"),
    (Join-Path $binaryDir "w3shadow.exe"),
    (Join-Path $binaryDir "CascLib.dll"),
    (Join-Path $projectRoot "README.md"),
    (Join-Path $projectRoot "CHANGELOG.md"),
    (Join-Path $projectRoot "THIRD_PARTY_NOTICES.md")
)
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required release file is missing: $file"
    }
}

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
New-Item -ItemType Directory -Path $stageDir | Out-Null
Copy-Item -LiteralPath $files -Destination $stageDir
Compress-Archive -LiteralPath $stageDir -DestinationPath $zipPath -CompressionLevel Optimal

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $checksumPath -Encoding ascii -Value "$hash  $($packageName).zip"
Write-Host "Created $zipPath"
Write-Host "SHA256 $hash"
