#pragma once
#include <SDL2/SDL.h>

// Tiny synthesized-audio helper on top of SDL2's queued audio API — no files,
// no mixer library. Sounds are generated as sample buffers on demand and handed
// to SDL_QueueAudio. Queued sounds play back-to-back rather than mixed, which is
// fine for short one-shot clicks and chimes.
//
// All methods are safe no-ops if init() failed (e.g. no audio device present).
class Sound {
public:
    bool init();        // opens the default audio device; call once at startup
    void shutdown();

    void play_game_start();          // rising two-tone chime
    // Stone "clack". Currently not called from anywhere — Boris muted stone sounds
    // (hard to design one that survives 300 repetitions per game); kept for later.
    void play_stone(bool mine);

private:
    SDL_AudioDeviceID dev_  = 0;
    int               rate_ = 48000;

    void queue(const float* samples, int count);
};
