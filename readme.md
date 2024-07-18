# GameJam: PizzaJam

This is my submission for the [Pizza Jam (July 24)](https://itch.io/jam/pizza-jam-pizza-prize-12). The submission was written in Odin and raylib, but then I rewrote the game in C to use emscripten to build it to HTML5.

![s0](/preview/s0.png)
![s1](/preview/s1.png)

## Build:

Requirements:
- clang
- emscripten envs

```
.\build.ps1 // native
.\build.ps1 web // builds the HTML5 version and places it in the build folder.
```
