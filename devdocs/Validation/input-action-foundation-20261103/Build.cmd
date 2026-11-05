@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\Sandbox\Codex\Tools\cmake-3.31.10\cmake-3.31.10-windows-x86_64\bin\cmake.exe" -S C:/Sandbox/Codex/Workspaces/gargantuan-input -B C:/Sandbox/Codex/Builds/gargantuan-input/msvc1444-release -G Ninja -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_C_COMPILER=cl.exe -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DGARGANTUAN_TRACY=OFF -DGARGANTUAN_WITH_GNS=OFF -DGARGANTUAN_BUILD_RENDERER_BENCHMARKS=ON -DGARGANTUAN_BUILD_SERIALIZATION_BENCHMARKS=ON -DGARGANTUAN_GLSLC_EXECUTABLE=C:/Sandbox/Codex/Cache/gargantuan/shaderc/glslc.exe -DCMAKE_MAKE_PROGRAM=C:/Strawberry/c/bin/ninja.exe
if errorlevel 1 exit /b 1
"C:\Sandbox\Codex\Tools\cmake-3.31.10\cmake-3.31.10-windows-x86_64\bin\cmake.exe" --build C:/Sandbox/Codex/Builds/gargantuan-input/msvc1444-release --parallel 8 --target gargantuan_input_action_tests gargantuan_input_action_benchmark gargantuan_player_runtime_tests gargantuan_platform_input_boundary_tests gargantuan_foundation_tests gargantuan_interaction_foundation_tests gargantuan_gui_foundation_tests
exit /b %errorlevel%
