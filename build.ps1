mkdir -f build | out-null
cp raylib/raylib.dll .

if ($args[0] -eq "web") {
    emcc -o ./build/game.html main.c -Os -Wall ./raylib/libraylib.a -I./arena -I./raylib -L./raylib -s USE_GLFW=3 -DPLATFORM_WEB -std=c23 --shell-file ./raylib/minshell.html --preload-file=./assets/
} else {
	clang -MJ compile_commands.json -I./arena -I./raylib -L./raylib -lraylib -o main.exe main.c
}