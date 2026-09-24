@echo off
rem Builds every Windows preset from nothing, for a release: deletes the
rem presets' build trees and dist\, then configures, builds and tests each
rem one in turn, and stops at the first failure. What is handed out lands
rem in dist\: Spickzettel.exe, Spickzettel Prerelease.exe and Spickzettel
rem Demo.exe, their PDBs in dist\symbols\. Debug is built for its UI tests
rem and copied nowhere.
rem
rem Needs Visual Studio with the C++ workload; finds it itself, so it runs
rem from a plain command prompt or a double-click. The first configure of
rem each preset fetches the third-party code again, so it needs a network.
setlocal
cd /d "%~dp0.."
rem Windows' own find, not whatever else is called find on PATH first - Git
rem for Windows puts its Unix find there, which reads its arguments as paths.
set "FIND=%SystemRoot%\System32\find.exe"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Cannot find vswhere.exe - is Visual Studio installed?
    goto :failed
)
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Cannot find a Visual Studio with the C++ tools.
    goto :failed
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :failed

rem A running copy holds its exe open, and neither the deletion nor the
rem copy into dist\ can get past it.
tasklist /fi "imagename eq Spickzettel*" 2>nul | "%FIND%" /i "Spickzettel" >nul
if not errorlevel 1 (
    echo Spickzettel is running. Quit it first, then run this again.
    goto :failed
)

echo === Deleting the build trees and dist\
for %%d in (build\windows-debug build\windows-release build\windows-prerelease build\windows-demo dist) do (
    if exist "%%d" rmdir /s /q "%%d" || goto :failed
    if exist "%%d" (
        echo Could not delete %%d - is something in it open?
        goto :failed
    )
)

call :preset windows-msvc-debug test || goto :failed
call :preset windows-msvc-release test || goto :failed
call :preset windows-msvc-prerelease || goto :failed
call :preset windows-msvc-demo || goto :failed

echo.
echo === Done. In dist\:
dir /b dist
call :pause_if_double_clicked
exit /b 0

:preset
echo.
echo === %1: configure
cmake --preset %1 || exit /b 1
echo === %1: build
cmake --build --preset %1 || exit /b 1
if "%2"=="test" (
    echo === %1: test
    ctest --preset %1 || exit /b 1
)
exit /b 0

:failed
echo.
echo === FAILED
call :pause_if_double_clicked
exit /b 1

rem Keeps the window open to read, when there is a window that would close:
rem a double-click runs this as "cmd /c", a prompt someone typed into does not.
:pause_if_double_clicked
echo %cmdcmdline% | "%FIND%" /i "/c" >nul && (echo. & pause)
exit /b 0
