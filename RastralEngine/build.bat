@echo off
setlocal

set "GLEW_DIR=C:\glew-2.1.0"
set "GLEW_INC=%GLEW_DIR%\include"
set "GLEW_LIB=%GLEW_DIR%\lib\Release\x64"
set "GLEW_BIN=%GLEW_DIR%\bin\Release\x64"

set "OUTDIR=build"
set "OUTEXE=MusicDirector.exe"

set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "MAIN=%SRC%\Main.cpp"
set INC=/I "%ROOT%Include"

set "CFG=%~1"
if /I "%CFG%"=="" set "CFG=debug"
if /I "%CFG%"=="clean" goto :CLEAN

where cl >nul 2>&1
if errorlevel 1 (
  echo [!] MSVC 'cl' not found. Use the "x64 Native Tools Command Prompt for VS".
  exit /b 1
)

if not exist "%MAIN%" (
  echo [x] Could not find "%MAIN%".
  echo     Expected: ^<repo^>\src\Main.cpp relative to this script.
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if /I "%CFG%"=="release" (
  set "CFLAGS=/nologo /std:c++17 /EHsc /O2 /DNDEBUG /MD"
  set "LFLAGS=/link"
) else (
  set "CFLAGS=/nologo /std:c++17 /EHsc /Zi /Od /MDd"
  set "LFLAGS=/link /DEBUG:FULL"
)

set "CFLAGS=%CFLAGS% /DWIN32_LEAN_AND_MEAN /DNOMINMAX"

set "GLEW_LIBNAME="
set "USE_GLEW_STATIC=0"
if exist "%GLEW_LIB%\glew32s.lib" (
  set "GLEW_LIBNAME=glew32s.lib"
  set "USE_GLEW_STATIC=1"
) else if exist "%GLEW_LIB%\glew32.lib" (
  set "GLEW_LIBNAME=glew32.lib"
) else (
  echo [x] Could not find GLEW libs in "%GLEW_LIB%".
  exit /b 1
)

if "%USE_GLEW_STATIC%"=="1" (
  set "CFLAGS=%CFLAGS% /DGLEW_STATIC"
)

set "CFLAGS=%CFLAGS% %INC%"

echo.
echo === Building %CFG% ===
echo   cl %CFLAGS% ^
 /I"%GLEW_INC%"  ^
 /Fe:"%OUTDIR%\%OUTEXE%" /Fo"%OUTDIR%\\" ^
 "%MAIN%" ^
 %LFLAGS% /LIBPATH:"%GLEW_LIB%" %GLEW_LIBNAME% opengl32.lib gdi32.lib user32.lib
echo.

cl %CFLAGS% /I"%GLEW_INC%" /IInclude /Fe:"%OUTDIR%\%OUTEXE%" /Fo"%OUTDIR%\\" "%MAIN%" %LFLAGS% /LIBPATH:"%GLEW_LIB%" %GLEW_LIBNAME% opengl32.lib gdi32.lib user32.lib
if errorlevel 1 (
  echo.
  echo [x] Build failed.
  exit /b 1
)

if "%USE_GLEW_STATIC%"=="0" (
  if exist "%GLEW_BIN%\glew32.dll" (
    copy /Y "%GLEW_BIN%\glew32.dll" "%OUTDIR%\" >nul
  ) else (
    echo [!] Warning: dynamic GLEW selected but "%GLEW_BIN%\glew32.dll" not found.
  )
)

echo.
echo [✓] Built "%OUTDIR%\%OUTEXE%"
exit /b 0