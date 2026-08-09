// raylib-on-vulkan example: audio - generated sine wave sound
#include "raylib.h"

#include <math.h>
#include <stdlib.h>

int main(void)
{
    const char *framesEnv = getenv("RAYVK_FRAMES");
    int maxFrames = (framesEnv != NULL)? atoi(framesEnv) : 0;

    InitWindow(800, 450, "rayvulkan - audio");
    InitAudioDevice();
    SetTargetFPS(60);

    // Build a 0.5s 440 Hz sine Wave in memory
    Wave wave = {
        .frameCount = 22050,
        .sampleRate = 44100,
        .sampleSize = 32,
        .channels = 1,
    };
    float *samples = (float *)MemAlloc(wave.frameCount*sizeof(float));
    for (unsigned int i = 0; i < wave.frameCount; i++)
        samples[i] = 0.3f*sinf(2.0f*3.14159265f*440.0f*(float)i/(float)wave.sampleRate);
    wave.data = samples;

    Sound beep = LoadSoundFromWave(wave);

    int frames = 0;
    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_SPACE)) PlaySound(beep);

        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText("press SPACE to beep", 280, 200, 20, LIGHTGRAY);
            DrawText(IsAudioDeviceReady()? "audio device: ready" : "audio device: NOT READY", 280, 240, 20,
                     IsAudioDeviceReady()? DARKGREEN : RED);
        EndDrawing();

        frames++;
        if ((maxFrames > 0) && (frames >= maxFrames)) break;
    }

    UnloadSound(beep);
    UnloadWave(wave);          // frees samples
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
