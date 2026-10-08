@if not defined _echo echo off

rem Reuse a Developer Command Prompt, or locate a C++ installation via its installer.
if defined VSCMD_VER (
  where msbuild >nul 2>nul
  if not errorlevel 1 exit /b 0
)

set "gc_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%gc_vswhere%" (
  echo Visual Studio Installer's vswhere.exe is missing. Install VS 2022 or Build Tools with the components in .vsconfig.
  exit /b 2
)

for /f "usebackq delims=" %%i in (`"%gc_vswhere%" -products * -latest -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  if exist "%%i\Common7\Tools\VsDevCmd.bat" (
    call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 %*
    exit /b
  )
)

echo Unable to locate Visual Studio with C++ tools. Install the components in .vsconfig.
exit /b 2
