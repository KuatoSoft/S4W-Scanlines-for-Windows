@echo off
setlocal
title S4W - Build Release v1.2

set PROJ_DIR=D:\S4W 1.1
set DIST_DIR=D:\S4W v1.2
set HOOK_DIR=%PROJ_DIR%\Hook

echo.
echo ============================================================
echo   S4W Release Builder
echo ============================================================
echo.

:: ── Step 1: Compile hook DLLs (x64 + x86) ────────────────────────────────
echo [1/3] Compiling hook DLLs...
call "%HOOK_DIR%\do_compile.bat"
if not exist "%HOOK_DIR%\S4W_Hook.dll"     ( echo FAIL: S4W_Hook.dll not built    & goto :error )
if not exist "%HOOK_DIR%\S4W_Hook_x86.dll" ( echo FAIL: S4W_Hook_x86.dll not built & goto :error )
echo       Hook DLLs: OK

:: ── Step 2: dotnet publish (single-file, self-contained) ──────────────────
echo [2/3] Publishing S4W (single-file self-contained)...
dotnet publish "%PROJ_DIR%\S4W.csproj" ^
    -c Release ^
    -r win-x64 ^
    --self-contained true ^
    -p:PublishSingleFile=true ^
    -p:IncludeNativeLibrariesForSelfExtract=true ^
    -p:EnableCompressionInSingleFile=true ^
    -p:S4W_RELEASE=1 ^
    -o "%PROJ_DIR%\dist_tmp" ^
    --nologo -v quiet
if errorlevel 1 ( echo FAIL: dotnet publish failed & goto :error )
if not exist "%PROJ_DIR%\dist_tmp\S4W.exe" ( echo FAIL: S4W.exe not produced & goto :error )
echo       dotnet publish: OK

:: ── Step 3: Assemble clean distribution folder ────────────────────────────
echo [3/3] Assembling "%DIST_DIR%"...
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

copy /y "%PROJ_DIR%\dist_tmp\S4W.exe"           "%DIST_DIR%\S4W.exe"
copy /y "%PROJ_DIR%\[S4W]_User Guide.pdf"       "%DIST_DIR%\[S4W]_User Guide.pdf"

mkdir "%DIST_DIR%\Hook"
copy /y "%HOOK_DIR%\S4W_Hook.dll"               "%DIST_DIR%\Hook\S4W_Hook.dll"
copy /y "%HOOK_DIR%\S4W_Hook_x86.dll"           "%DIST_DIR%\Hook\S4W_Hook_x86.dll"
copy /y "%HOOK_DIR%\S4W_Injector_x86.exe"       "%DIST_DIR%\Hook\S4W_Injector_x86.exe"

:: Cleanup temp publish folder
rmdir /s /q "%PROJ_DIR%\dist_tmp"

echo.
echo ============================================================
echo   Distribution ready: %DIST_DIR%
echo ============================================================
dir /b "%DIST_DIR%"
echo.
echo Done!
goto :end

:error
echo.
echo BUILD FAILED. See errors above.
exit /b 1

:end
endlocal
