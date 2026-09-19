$ErrorActionPreference = "Stop"

$ExeName = "awaaz.exe"
$CC = "cl"
$CXX = "cl"

$BuildPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) "build"))
$Src = (Resolve-Path "./src/")

$BacePath = (Resolve-Path "./external/bace/")
$BaceObj = "bace.obj"

$UuidObj = "uuid.obj"

function Build-Bace {
    Write-Host "== building bace =="
    Push-Location "$BacePath"
    $old_bpath = $env:BUILD_PATH
    $env:BUILD_PATH = $BuildPath
    try {
        & "./build.ps1"
        $baceExitCode = $LASTEXITCODE
    } finally {
        $env:BUILD_PATH = $old_bpath
    }

    if ($baceExitCode -ne 0) {
        Write-Host "building bace failed"
        exit 1
    }
    Pop-Location
}

function Build-External {
    # & git submodule update --init --recursive
    # if ($LASTEXITCODE -ne 0) { return $false }

    if (-not (Build-Bace)) { return $false }

    return $true
}


# debug flags
$Debug = @(
    "/Zi"
)

# compile time defines
$Defines = @()

# windows platform libraries
$Libs = @(
    "/LIBPATH:$BuildPath"
    "$BaceObj"
    "$UuidObj"
    "ole32.lib"
    "ksuser.lib"
)

# compiler flags
$CompFlags = @(
    "/I$BacePath\include"
    "/W4"
    "/WX"
    "/Od"
    "/std:c11"
    "/experimental:c11atomics"
    "/nologo"
)

# build commands
$ExeArgs = $Defines + $Debug + $CompFlags + @(
    "/Fe$BuildPath\$ExeName"
    "$Src\main.c"
    "/link"
) + $Libs

$NoBuildDeps = $false
foreach ($arg in $args) {
    switch ($arg) {
        { $_ -cin "--no-deps", "-D" } { $NoBuildDeps = $true; break }
    }
}

function Build-Uuid {
    Write-Host "== building uuid =="
    & $CC /nologo /c "$Src\uuid.cpp" /Fo"$BuildPath\$UuidObj"
    return $LASTEXITCODE -eq 0
}

# create build directory if it doesn't exist
if (-not (Test-Path $BuildPath)) {
    Write-Host "created build directory"
    New-Item -ItemType Directory -Path $BuildPath -Force | Out-Null
}

if (-not $NoBuildDeps) {
    Write-Host "=== building deps ==="
    if (-not (Build-External)) { exit 1; }
    if (-not (Build-Uuid)) { exit 1; }
}

Push-Location $BuildPath
Write-Host "===== $ExeName ====="
Write-Host $CC @ExeArgs
& $CC @ExeArgs
$exitCode = $LASTEXITCODE
Pop-Location
exit $exitCode
