// Conductor: a PicoLEDs Animator that keeps a prop in time with the show.
//
// - Frames are paced by show time (clockUs() is PicoComm's shared clock), so every prop running
//   the same frame rate draws its frames at the same instants.
// - Cues from the leader switch the effect on the first frame at or after the cue's time, on
//   every prop at once. Effects are made when their cue arrives, by a factory registered for
//   the effect ID, and deleted when the next one takes over, so only one is in memory at a time.
// - Until the first cue, and whenever the prop loses the leader (a RESUME_LOCAL cue), it plays
//   its own local playlist.
//
//     Conductor conductor(&canvas, node);
//     conductor.addEffect(EffectId::Starlight, [](Canvas *c, const Cue &) -> Animation * {
//         return new Starlight(c);
//     });
//     conductor.addLocal(EffectId::Starlight, 60000);
//     conductor.init();
//     while (true) conductor.step();
//
// Not in v1: CUE transitions other than a cut (fade-through-black and crossfade play as cuts).

#pragma once

#include <cstdint>

#include "Animator.h"
#include "picocomm/cue_source.h"
#include "picocomm/effects.h"
#include "picocomm/wire.h"

namespace picocomm {

class Conductor : public Animator {
   public:
    // Makes the animation for an effect from a cue's parameters. It's deleted by the Conductor.
    using Factory = Animation *(*)(Canvas *canvas, const Cue &cue);

    static constexpr int kMaxEffects = 32;
    static constexpr int kMaxLocal = 16;
    static constexpr int kMaxWaiting = 8;
    // A clock jump bigger than this (locking to the leader, a leader reboot) restarts the current
    // effect rather than letting it try to catch up on the time.
    static constexpr uint64_t kRestartJumpUs = 1000000;
    // How often frame counts and times go to the CueSource (FRAME_STATS), on this Pico's clock.
    static constexpr uint64_t kStatsIntervalUs = 1000000;

    Conductor(Canvas *canvas, CueSource &source, int fps = kDefaultFPS);
    ~Conductor() override;

    // Registers the effects this prop can play. The same list should go in the Node's config.
    void addEffect(EffectId effect, Factory make) { addEffect((uint16_t)effect, make); }
    void addEffect(uint16_t effect, Factory make);
    bool knowsEffect(uint16_t effect) const;

    // The local playlist: played in order, each for durationMs, when there's no leader.
    void addLocal(EffectId effect, uint32_t durationMs, const uint8_t *params = nullptr) {
        addLocal((uint16_t)effect, durationMs, params);
    }
    void addLocal(uint16_t effect, uint32_t durationMs, const uint8_t *params = nullptr);

    void init() override;
    bool step() override;

    // Show time when the leader's clock is locked, this Pico's own clock until then.
    uint64_t clockUs() override;

    bool playingLocal() const { return local_; }
    uint16_t currentEffect() const { return effect_; }
    uint16_t currentCueId() const { return cueId_; }
    uint8_t brightness();

   private:
    struct Registered {
        uint16_t effect;
        Factory make;
    };
    struct LocalEntry {
        uint16_t effect;
        uint32_t durationUs;
        uint8_t params[8];
    };

    void apply(const Cue &cue, uint64_t frameTime);
    bool play(uint16_t effect, const Cue &cue, uint64_t frameTime);
    void playLocal(int index, uint64_t frameTime);
    void updateBrightness(uint64_t frameTime);
    void reportFrameStats();

    CueSource &source_;
    Registered effects_[kMaxEffects];
    int effectCount_ = 0;
    LocalEntry localList_[kMaxLocal];
    int localCount_ = 0;

    Animation *current_ = nullptr;  // owned
    uint16_t effect_ = 0;
    uint16_t cueId_ = 0;
    bool local_ = true;
    int localIndex_ = 0;
    uint64_t localStartUs_ = 0;  // on this Pico's own clock, which never jumps

    Cue waiting_[kMaxWaiting];
    int waitingCount_ = 0;

    bool locked_ = false;
    uint64_t lastClock_ = 0;

    uint64_t worstFrameUs_ = 0;  // since the last stats report
    uint64_t nextStatsUs_ = 0;

    // Brightness ramp: from rampFrom_ at rampStartUs_ to rampTo_ over rampUs_ (show time).
    bool ramping_ = false;
    uint8_t rampFrom_ = 0;
    uint8_t rampTo_ = 0;
    uint64_t rampStartUs_ = 0;
    uint32_t rampUs_ = 0;
};

}  // namespace picocomm
