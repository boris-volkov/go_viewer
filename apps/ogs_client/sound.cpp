#include "sound.hpp"
#include <cmath>
#include <cstdlib>
#include <vector>

static const float PI2 = 6.28318530718f;

bool Sound::init() {
    if (dev_) return true;
    if (!(SDL_WasInit(SDL_INIT_AUDIO)) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return false;

    SDL_AudioSpec want{}, have{};
    want.freq     = rate_;
    want.format   = AUDIO_F32SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = nullptr;  // queued mode — we push samples with SDL_QueueAudio

    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    rate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);  // start playback (silent until something queues)
    return true;
}

void Sound::shutdown() {
    if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
}

void Sound::queue(const float* samples, int count) {
    if (!dev_ || count <= 0) return;
    SDL_QueueAudio(dev_, samples, (Uint32)count * sizeof(float));
}

// Rising two-tone chime: C5 then G5, each a sine with a soft attack and
// exponential decay so it doesn't click at the edges.
void Sound::play_game_start() {
    if (!dev_) return;
    const float vol = 0.22f;
    struct Note { float freq, dur; };
    const Note notes[] = { {523.25f, 0.11f}, {783.99f, 0.18f} };

    std::vector<float> buf;
    for (const Note& nt : notes) {
        int n = (int)(nt.dur * rate_);
        for (int i = 0; i < n; i++) {
            float t   = (float)i / rate_;
            float env = std::exp(-t * 14.0f) * std::min(1.0f, t * 400.0f);
            buf.push_back(vol * env * std::sin(PI2 * nt.freq * t));
        }
    }
    queue(buf.data(), (int)buf.size());
}

// Go-stone "clack": a few milliseconds of white-noise burst (the impact) plus a
// sharply damped sine ping (the slate/glass resonance). The opponent's stones
// ring slightly lower so the two sides are distinguishable by ear.
void Sound::play_stone(bool mine) {
    if (!dev_) return;
    unsigned rng = (unsigned)SDL_GetTicks() * 2654435761u + 12345u;
    // No two stones strike identically: vary the ring frequency (±8%), overall
    // loudness (±20%) and ring length a little per click, like real slate stones
    // landing with different force on different spots of the board.
    auto frand = [&rng]() {  // uniform in [-1, 1]
        rng = rng * 1664525u + 1013904223u;
        return (float)(rng >> 8) / 8388608.0f - 1.0f;
    };
    const float vol   = 0.30f  * (1.0f + 0.08f * frand());
    const float ping  = 700.0f * (1.0f + 0.03f * frand());
    const float ring  = 180.0f * (1.0f + 0.06f * frand());  // ping decay rate
    (void)mine;  // both sides click alike, per Boris — param kept for easy re-split
    const float dur   = 0.055f;
    int n = (int)(dur * rate_);

    std::vector<float> buf((size_t)n);
    for (int i = 0; i < n; i++) {
        float t = (float)i / rate_;
        // Impact: white noise dying out within ~8ms
        float noise = frand() * std::exp(-t * 400.0f);
        // Resonance: damped ping, ~15ms tail
        float body  = std::sin(PI2 * ping * t) * std::exp(-t * ring);
        buf[(size_t)i] = vol * (0.70f * noise + 0.60f * body);
    }
    queue(buf.data(), n);
}
