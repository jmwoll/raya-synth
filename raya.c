// raya — ambient modular synth environment (raylib + C99)

#include "raylib.h"

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

static void frame(void)
{
    BeginDrawing();
    ClearBackground((Color){ 7, 11, 10, 255 });
    const char *title = "R A Y A";
    int size = 40;
    int w = MeasureText(title, size);
    DrawText(title, (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - size / 2,
             size, (Color){ 220, 235, 228, 255 });
    EndDrawing();
}

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "raya");

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(frame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) frame();
#endif

    CloseWindow();
    return 0;
}
