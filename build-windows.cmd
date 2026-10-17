@echo off
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul
if errorlevel 1 (
  echo Install MinGW-w64 and add its bin directory to PATH, then run this file again.
  exit /b 1
)
if not exist bin\Release mkdir bin\Release
windres -i res/RaptorScope.rc -o bin/Release/resources.o --include-dir=res
if errorlevel 1 exit /b 1
g++ -std=c++11 -O2 -static -static-libgcc -static-libstdc++ -Iinclude -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 src/main.cpp src/core/lzss.cpp src/core/color.cpp src/core/audio.cpp src/formats/dat.cpp src/formats/texture.cpp src/formats/mesh.cpp src/formats/tim.cpp src/formats/video.cpp src/formats/save_editor.cpp src/formats/scd.cpp src/formats/dc2_room.cpp src/formats/dc2_scd.cpp src/formats/dc2_model.cpp src/ui/app.cpp src/ui/panels.cpp bin/Release/resources.o -o bin/Release/RaptorScope-DC1-GLB.exe -mwindows -lcomctl32 -lgdi32 -lopengl32 -lglu32 -lwinmm -lcomdlg32 -lole32 -loleaut32 -luuid -ladvapi32 -lmsimg32 -lshell32
if errorlevel 1 exit /b 1
g++ -std=c++11 -O2 -static -static-libgcc -static-libstdc++ -Iinclude tools/dc1_export.cpp src/formats/dat.cpp src/formats/mesh.cpp src/formats/save_editor.cpp src/formats/dc2_model.cpp src/core/lzss.cpp -o bin/Release/dc1_export.exe
if errorlevel 1 exit /b 1
echo Built bin\Release\RaptorScope-DC1-GLB.exe and dc1_export.exe
