@echo off
REM ---------------------------------------------------------------------------
REM Build SteamOverlayFix.exe with MSVC.
REM
REM Run this from a "x64 Native Tools Command Prompt for VS", or just run it and
REM it will try to locate and load the build environment itself.
REM
REM MinGW alternative:
REM   windres SteamOverlayFix.rc -O coff -o SteamOverlayFix.res
REM   g++ -O2 -Wall -Wextra -mwindows -static -static-libgcc -static-libstdc++ -s ^
REM       SteamOverlayFix.cpp SteamOverlayFix.res -o SteamOverlayFix.exe ^
REM       -lcomctl32 -ldwmapi -lgdi32 -lshell32 -ladvapi32 -luser32
REM
REM   -static keeps the exe standalone; the watcher runs as SYSTEM at boot with
REM   no MinGW on the PATH. -s strips symbols (794 KB -> 376 KB).
REM   Do not add -municode: it expects wWinMain, this source defines WinMain.
REM ---------------------------------------------------------------------------
setlocal

where cl.exe >nul 2>&1
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%VSWHERE%" (
    echo ERROR: MSVC not found. Open a "x64 Native Tools Command Prompt for VS"
    echo        and re-run this script, or install Visual Studio Build Tools.
    exit /b 1
  )
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath`) do set "VSPATH=%%i"
  if not defined VSPATH (
    echo ERROR: no Visual Studio C++ toolset installed.
    exit /b 1
  )
  call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
)

echo Compiling resources...
rc /nologo /fo SteamOverlayFix.res SteamOverlayFix.rc
if errorlevel 1 exit /b 1

echo Compiling...
cl /nologo /W4 /EHsc /O2 /DUNICODE /D_UNICODE /MT ^
   SteamOverlayFix.cpp SteamOverlayFix.res ^
   /link /SUBSYSTEM:WINDOWS /OUT:SteamOverlayFix.exe
if errorlevel 1 exit /b 1

del /q SteamOverlayFix.obj SteamOverlayFix.res >nul 2>&1
echo.
echo Built SteamOverlayFix.exe
endlocal
