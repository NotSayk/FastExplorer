@echo off
rem Compile FastExplorer avec MSVC.
rem   build.bat            release optimisee     -> build\release\FastExplorer.exe
rem   build.bat debug      debug                 -> build\debug\FastExplorer.exe
rem   build.bat asan       debug + AddressSanitizer -> build\asan\FastExplorer.exe
rem   build.bat test       compile et lance les tests sous AddressSanitizer
setlocal
pushd "%~dp0"

set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo MSVC introuvable.
  goto :fail
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"
set "OPT="
if /i "%MODE%"=="release" set "OPT=/O2 /GL /Gy /MT /DNDEBUG" & set "LOPT=/LTCG /OPT:REF /OPT:ICF"
if /i "%MODE%"=="debug"   set "OPT=/Od /Zi /MTd /D_DEBUG"     & set "LOPT=/DEBUG"
if /i "%MODE%"=="asan"    set "OPT=/Od /Zi /MD /fsanitize=address" & set "LOPT=/DEBUG /INCREMENTAL:NO"
if /i "%MODE%"=="test"    set "OPT=/Od /Zi /MD /fsanitize=address" & set "LOPT=/DEBUG /INCREMENTAL:NO"
if not defined OPT (
  echo Mode inconnu : %MODE%
  goto :fail
)

set "COMMON=/nologo /std:c++20 /EHsc /W4 /WX /utf-8 /permissive- /MP /DUNICODE /D_UNICODE"
set "OUT=build\%MODE%"
if not exist "%OUT%" mkdir "%OUT%"

if /i "%MODE%"=="test" goto :test

cl %COMMON% %OPT% src\*.cpp /Fo%OUT%\ /Fd%OUT%\ /Fe%OUT%\FastExplorer.exe /link /SUBSYSTEM:WINDOWS %LOPT% || goto :fail
if /i "%MODE%"=="asan" call :copyasan
goto :ok

:test
cl %COMMON% %OPT% /Isrc tests\tests.cpp src\paths.cpp src\listing.cpp src\nt.cpp src\jobs.cpp src\shellmenu.cpp src\icons.cpp src\watch.cpp src\fileops.cpp src\fastcopy.cpp src\dragdrop.cpp src\navpane.cpp src\pins.cpp src\crumbs.cpp src\foldersize.cpp /Fo%OUT%\ /Fd%OUT%\ /Fe%OUT%\tests.exe /link /SUBSYSTEM:CONSOLE %LOPT% || goto :fail
call :copyasan
"%OUT%\tests.exe" || goto :fail
goto :ok

:copyasan
rem Le runtime ASan est une DLL : on la copie a cote de l'exe pour qu'il se lance hors de l'invite VS.
for /f "delims=" %%f in ('where clang_rt.asan_dynamic-x86_64.dll 2^>nul') do (
  copy /y "%%f" "%OUT%\" >nul
  goto :eof
)
echo Avertissement : runtime ASan introuvable.
goto :eof

:ok
popd
exit /b 0

:fail
popd
exit /b 1
