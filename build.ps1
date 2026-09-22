$ErrorActionPreference = "Stop"

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Could not locate a Visual Studio install with the C++ toolset." }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path ".\build" | Out-Null

$raylibInclude = Resolve-Path ".\vendor\raylib-6.0_win64_msvc16\include"
$raylibLib = Resolve-Path ".\vendor\raylib-6.0_win64_msvc16\lib"

$cmd = "call `"$vcvars`" >nul && cl /nologo /EHsc /std:c++17 /MD /I `"$raylibInclude`" src\main.cpp /Fo:build\ /Fe:build\game.exe /link /LIBPATH:`"$raylibLib`" raylib.lib winmm.lib gdi32.lib user32.lib shell32.lib"

cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Copy-Item ".\vendor\raylib-6.0_win64_msvc16\lib\raylib.dll" ".\build\raylib.dll" -Force
if (Test-Path ".\sound_effects") {
    New-Item -ItemType Directory -Force -Path ".\build\sound_effects" | Out-Null
    Copy-Item ".\sound_effects\*" ".\build\sound_effects\" -Recurse -Force
}
if (Test-Path ".\soundtrack") {
    New-Item -ItemType Directory -Force -Path ".\build\soundtrack" | Out-Null
    Copy-Item ".\soundtrack\*" ".\build\soundtrack\" -Recurse -Force
}
if (Test-Path ".\monologues.txt") { Copy-Item ".\monologues.txt" ".\build\monologues.txt" -Force }
if (Test-Path ".\poster") {
    New-Item -ItemType Directory -Force -Path ".\build\poster" | Out-Null
    Copy-Item ".\poster\*" ".\build\poster\" -Recurse -Force
}

Write-Host "Build succeeded: build\game.exe" -ForegroundColor Green
