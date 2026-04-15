@echo off
set MSVC=D:\Visual Studio  2026\VC\Tools\MSVC\14.42.34433
set SDK=D:\Windows Kits\10\Lib\10.0.22621.0
set SDKINC=D:\Windows Kits\10\Include\10.0.22621.0

"%MSVC%\bin\Hostx64\x64\cl.exe" /O2 /LD /EHsc /W3 /DNDEBUG ^
  /I"%MSVC%\include" ^
  /I"%SDKINC%\um" /I"%SDKINC%\shared" /I"%SDKINC%\ucrt" ^
  "D:\S4W 1.1\Hook\S4W_Hook.cpp" ^
  /Fe:"D:\S4W 1.1\Hook\S4W_Hook.dll" ^
  /link d3d11.lib d3d9.lib dxgi.lib d3dcompiler.lib user32.lib ole32.lib opengl32.lib gdi32.lib ^
  /LIBPATH:"%SDK%\um\x64" ^
  /LIBPATH:"%MSVC%\lib\x64" ^
  /LIBPATH:"%SDK%\ucrt\x64" ^
  /DEF:NUL
if errorlevel 1 (echo COMPILE_FAIL) else (echo COMPILE_OK)

"%MSVC%\bin\Hostx64\x86\cl.exe" /O2 /LD /EHsc /W3 /DNDEBUG ^
  /I"%MSVC%\include" ^
  /I"%SDKINC%\um" /I"%SDKINC%\shared" /I"%SDKINC%\ucrt" ^
  "D:\S4W 1.1\Hook\S4W_Hook.cpp" ^
  /Fe:"D:\S4W 1.1\Hook\S4W_Hook_x86.dll" ^
  /link d3d11.lib d3d9.lib dxgi.lib d3dcompiler.lib user32.lib ole32.lib opengl32.lib gdi32.lib ^
  /LIBPATH:"%SDK%\um\x86" ^
  /LIBPATH:"%MSVC%\lib\x86" ^
  /LIBPATH:"%SDK%\ucrt\x86" ^
  /DEF:NUL /MACHINE:X86
if errorlevel 1 (echo COMPILE_X86_FAIL) else (echo COMPILE_X86_OK)

del /q "D:\S4W 1.1\Hook\S4W_Hook.obj" "D:\S4W 1.1\Hook\S4W_Hook.lib" "D:\S4W 1.1\Hook\S4W_Hook.exp" 2>nul
del /q "D:\S4W 1.1\Hook\S4W_Hook_x86.obj" "D:\S4W 1.1\Hook\S4W_Hook_x86.lib" "D:\S4W 1.1\Hook\S4W_Hook_x86.exp" 2>nul
