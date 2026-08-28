# Anthropolis engine build (fork stringandstickytape/godot, branch 4.7.2-anthropolis).
# Produces bin\godot.windows.editor.x86_64.mono.exe + bin\GodotSharp\ — the payload that
# Anthropolis\_tools\godot\ carries (LFS). Run from the repo root in PowerShell:
#   powershell -ExecutionPolicy Bypass -File misc\scripts\build_windows_mono_anthropolis.ps1
# Prerequisites: VS 2022 MSVC x64, Windows SDK, Python 3 with SCons (pip install scons),
# .NET SDK 8+; run `python misc\scripts\install_d3d12_sdk_windows.py` once for the D3D12 deps.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..\..')
python -m SCons platform=windows arch=x86_64 target=editor module_mono_enabled=yes d3d12=yes vsproj=no -j([Environment]::ProcessorCount - 4)
if ($LASTEXITCODE -ne 0) { throw "scons failed ($LASTEXITCODE)" }
& .\bin\godot.windows.editor.x86_64.mono.exe --headless --generate-mono-glue modules/mono/glue
if ($LASTEXITCODE -ne 0) { throw "glue generation failed ($LASTEXITCODE)" }
python modules/mono/build_scripts/build_assemblies.py --godot-output-dir=./bin --godot-platform=windows
if ($LASTEXITCODE -ne 0) { throw "assembly build failed ($LASTEXITCODE)" }
& .\bin\godot.windows.editor.x86_64.mono.exe --version
