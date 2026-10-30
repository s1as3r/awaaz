[CmdletBinding()]
param(
    [Alias("D")][switch]$NoDeps,
    [switch]$Release
)

$ErrorActionPreference = "Stop"

$Config  = if ($Release) { "Release" } else { "Debug" }
$ExeName = "awaaz.exe"
$CC      = "cl"

$BuildPath   = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) "build"))
$Src         = (Resolve-Path "./src/").Path
$BacePath    = (Resolve-Path "./external/bace/").Path
$CimguiPath  = (Resolve-Path "./external/cimgui/").Path
$SDLPath     = (Resolve-Path "./external/SDL/").Path
$BaceObj     = "bace.obj"
$UuidObj     = "uuid.obj"

$OptFlags = if ($Release) { @("/O2", "/DNDEBUG") } else { @("/Od") }

$Libs = @(
    "/LIBPATH:$BuildPath"
    "/LIBPATH:$BuildPath\cimgui\cimgui\$Config"
    "/LIBPATH:$BuildPath\sdl\$Config"
    $BaceObj, $UuidObj
    "SDL3-static.lib", "cimgui.lib"
    "ole32.lib", "ksuser.lib", "winmm.lib", "advapi32.lib"
    "gdi32.lib", "version.lib", "oleaut32.lib", "avrt.lib"
)

$CompFlags = @(
    "/I$BacePath\include", "/I$CimguiPath\cimgui", "/I$SDLPath\include"
    "/W4", "/WX", "/wd4324"
    "/std:c11", "/experimental:c11atomics"
    "/nologo", "/Wv:18", "/Zi"
) + $OptFlags

$ExeArgs = $CompFlags + @(
    "/Fe$BuildPath\$ExeName"
    "$Src\main.c"
    "/link", "/incremental:no", "/subsystem:windows", "/entry:mainCRTStartup"
) + $Libs


function Invoke-Native {
    param([Parameter(Mandatory)][scriptblock]$Command)
    $global:LASTEXITCODE = 0
    & $Command | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "command failed (exit $LASTEXITCODE): $Command"
    }
}

function Build-SDL {
    Write-Host "== building sdl3 =="
    Invoke-Native {
        cmake -S "$SDLPath" -B "$BuildPath/sdl" `
            -DSDL_STATIC=on -DSDL_SHARED=off `
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    }
    Invoke-Native { cmake --build "$BuildPath/sdl" --config $Config }
}

function Build-Cimgui {
    Write-Host "== building cimgui =="
    Invoke-Native {
        cmake -S "$CimguiPath" -B "$BuildPath/cimgui" `
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
    }
    Invoke-Native { cmake --build "$BuildPath/cimgui" --config $Config }
}

function Build-Bace {
    Write-Host "== building bace =="
    Push-Location $BacePath
    $oldBPath = $env:BUILD_PATH
    $oldBType = $env:BUILD_TYPE
    $env:BUILD_PATH = $BuildPath
    $env:BUILD_TYPE = $Config
    try {
        Invoke-Native { & "./build.ps1" }
    } finally {
        $env:BUILD_PATH = $oldBPath
        $env:BUILD_TYPE = $oldBType
        Pop-Location
    }
}

function Build-Uuid {
    Write-Host "== building uuid =="
    Invoke-Native { cl /nologo /c "$Src\win32\uuid.cpp" /Fo"$BuildPath\$UuidObj" }
}


$ok = $false
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path $BuildPath)) {
        Write-Host "created build directory"
        New-Item -ItemType Directory -Path $BuildPath -Force | Out-Null
    }

    if (-not $NoDeps) {
        Write-Host "=== building deps ($Config) ==="
        Build-Bace
        Build-Cimgui
        Build-SDL
        Build-Uuid
    }

    Push-Location $BuildPath
    try {
        Write-Host "===== $ExeName ($Config) ====="
        Write-Host $CC @ExeArgs
        Invoke-Native { & $CC @ExeArgs }
    } finally {
        Pop-Location
    }
    $ok = $true
} catch {
    Write-Host "build failed: $($_.Exception.Message)" -ForegroundColor Red
} finally {
    Pop-Location
}
exit ([int](-not $ok))
