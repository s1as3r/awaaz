$ErrorActionPreference = "Stop"

$ExeName = "awaaz.exe"
$CC = "cl"
$CXX = "cl"

$BuildPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) "build"))
$Src = (Resolve-Path "./src/")

$BacePath = (Resolve-Path "./external/bace/")
$BaceObj = "bace.obj"

$UuidObj = "uuid.obj"

$CimguiPath = (Resolve-Path "./external/cimgui/")
$SDLPath = (Resolve-Path "./external/SDL/")

function Build-SDL {
    Write-Host "== building sdl3 =="
    cmake -S "$SDLPath" -B "$BuildPath/sdl" `
        -DSDL_STATIC=on `
        -DSDL_SHARED=off `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded

    cmake --build "$BuildPath/sdl" --config Debug
    if ($LASTEXITCODE -ne 0) { throw "cmake failed" }
}

function Build-Cimgui {
    Write-Host "== building cimgui =="
    cmake -S "$CimguiPath" -B "$BuildPath/cimgui" `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    cmake --build "$BuildPath/cimgui" --config Debug
    if ($LASTEXITCODE -ne 0) { throw "cmake failed" }
}

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
    if (-not (Build-Cimgui)) { return $false }
    if (-not (Build-SDL)) { return $false }

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
    "/LIBPATH:$BuildPath\cimgui\cimgui\Debug"
    "/LIBPATH:$BuildPath\sdl\Debug"
    "$BaceObj"
    "$UuidObj"
    "SDL3-static.lib"
    "cimgui.lib"
    "ole32.lib"
    "ksuser.lib"
    "winmm.lib"
    "advapi32.lib"
    "gdi32.lib"
    "version.lib"
    "oleaut32.lib"
)

# compiler flags
$CompFlags = @(
    "/I$BacePath\include"
    "/I$CimguiPath\cimgui"
    "/I$SDLPath\include"
    "/I"
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
    "/incremental:no"
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
