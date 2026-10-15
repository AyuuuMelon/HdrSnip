$ErrorActionPreference = 'Stop'
cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') -G 'Visual Studio 17 2022' -A x64 -DHDRSNIP_BUILD_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
cmake --build (Join-Path $PSScriptRoot 'build') --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
Write-Host ('Built: ' + (Join-Path $PSScriptRoot 'build\Release\HdrSnip.exe'))
