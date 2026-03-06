@echo off
set "PATH=C:\MinGW\bin;%PATH%"

echo ==========================================
echo      SkipFrames Build
echo ==========================================

:: 1. Compile DLL
echo [1/3] Compiling DLL...
g++ skipframes_dll.cpp sayi_bilen.cpp -o skipframes-v1.8.0.dll -shared -static -m32 -O3 -DNDEBUG -fno-exceptions -fno-rtti -fmerge-all-constants -lpsapi -luser32 -lkernel32 -static-libgcc -static-libstdc++ -s
if %errorlevel% neq 0 (
    echo [ERROR] DLL Compilation Failed!
    pause
    exit /b %errorlevel%
)
            
:: 2. Embed DLL into Header
echo [2/3] Embedding DLL into Header...
powershell -ExecutionPolicy Bypass -File embed_v2.ps1
if not exist embedded_dll.h (
    echo [ERROR] Embedding Failed!
    pause
    exit /b 1
)

:: 3. Compile Injector (EXE)
echo [3/3] Compiling Injector...
if exist skipframes-v1.8.0.exe del skipframes-v1.8.0.exe
g++ skipframes.cpp -o skipframes-v1.8.0.exe -static -m32 -O3 -DNDEBUG -fno-exceptions -fno-rtti -fmerge-all-constants -luser32 -lkernel32 -static-libgcc -static-libstdc++ -s
if %errorlevel% neq 0 (
    echo [ERROR] EXE Compilation Failed!
    pause
    exit /b %errorlevel%
)

echo.
echo ==========================================
echo      BUILD SUCCESS: skipframes-v1.8.0.exe
echo ==========================================
echo.
pause
