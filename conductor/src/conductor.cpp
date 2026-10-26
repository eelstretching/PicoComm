#include "picocomm/conductor.h"

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"

namespace picocomm {

namespace {

// EffectId::None and BLACKOUT cues: all pixels off.
class Blackout : public Animation {
   public:
    explicit Blackout(Canvas *canvas) : Animation(canvas, nullptr, 0) {}
    bool step() override {
        canvas->fill(RGB(0, 0, 0));
        return true;
    }
};

}  // namespace

Conductor::Conductor(Canvas *canvas, CueSource &source, int fps)
    : Animator(canvas, fps), source_(source) {}

Conductor::~Conductor() {
    animations.clear();
    delete current_;
}

void Conductor::addEffect(uint16_t effect, Factory make) {
    for (int i = 0; i < effectCount_; i++) {
        if (effects_[i].effect == effect) {
            effects_[i].make = make;
            return;
        }
    }
    if (effectCount_ < kMaxEffects) {
        effects_[effectCount_++] = {effect, make};
    }
}

bool Conductor::knowsEffect(uint16_t effect) const {
    if (effect == (uint16_t)EffectId::None) {
        return true;
    }
    for (int i = 0; i < effectCount_; i++) {
        if (effects_[i].effect == effect) {
            return true;
        }
    }
    return false;
}

void Conductor::addLocal(uint16_t effect, uint32_t durationMs, const uint8_t *params) {
    if (localCount_ == kMaxLocal) {
        return;
    }
    LocalEntry &e = localList_[localCount_++];
    e.effect = effect;
    e.durationUs = durationMs * 1000;
    memset(e.params, 0, sizeof(e.params));
    if (params != nullptr) {
        memcpy(e.params, params, sizeof(e.params));
    }
}

uint64_t Conductor::clockUs() {
    uint64_t show;
    if (source_.showTime(show)) {
        return show;
    }
    return time_us_64();
}

uint8_t Conductor::brightness() { return canvas->getRenderer()->getBrightness(); }

void Conductor::init() {
    uint64_t now = clockUs();
    lastClock_ = now;
    uint64_t show;
    locked_ = source_.showTime(show);
    uint64_t frame = now - now % usPerFrame;
    playLocal(0, frame);
}

bool Conductor::play(uint16_t effect, const Cue &cue, uint64_t frameTime) {
    Animation *next = nullptr;
    if (effect == (uint16_t)EffectId::None || cue.kind == CueKind::Blackout) {
        next = new Blackout(canvas);
        effect = (uint16_t)EffectId::None;
    } else {
        for (int i = 0; i < effectCount_; i++) {
            if (effects_[i].effect == effect) {
                next = effects_[i].make(canvas, cue);
                break;
            }
        }
    }
    if (next == nullptr) {
        printf("[conductor] no animation for effect 0x%04x\n", effect);
        return false;
    }
    // Only one effect in memory at a time: the old one goes as the new one starts.
    animations.clear();
    animations.push_back(next);
    pos = 0;
    delete current_;
    current_ = next;
    effect_ = effect;
    startAnimation(next, frameTime);
    return true;
}

void Conductor::playLocal(int index, uint64_t frameTime) {
    local_ = true;
    localStartUs_ = time_us_64();
    Cue c;
    c.kind = CueKind::Effect;
    c.atUs = frameTime;
    if (localCount_ == 0) {
        c.effect = (uint16_t)EffectId::None;
        play(c.effect, c, frameTime);
        return;
    }
    localIndex_ = index % localCount_;
    const LocalEntry &e = localList_[localIndex_];
    c.effect = e.effect;
    memcpy(c.params, e.params, sizeof(c.params));
    if (!play(e.effect, c, frameTime)) {
        play((uint16_t)EffectId::None, c, frameTime);
    }
}

void Conductor::apply(const Cue &cue, uint64_t frameTime) {
    switch (cue.kind) {
        case CueKind::Effect:
        case CueKind::Blackout:
            if (play(cue.kind == CueKind::Blackout ? (uint16_t)EffectId::None : cue.effect, cue,
                     frameTime)) {
                local_ = false;
                cueId_ = cue.cueId;
            }
            break;
        case CueKind::Brightness:
            ramping_ = true;
            rampFrom_ = brightness();
            rampTo_ = cue.params[0];
            rampStartUs_ = cue.atUs;
            rampUs_ = (uint32_t)cue.transitionMs * 1000;
            break;
        case CueKind::ResumeLocal:
            cueId_ = cue.cueId;
            if (!local_) {
                playLocal(localIndex_, frameTime);
            }
            break;
    }
}

void Conductor::updateBrightness(uint64_t frameTime) {
    if (!ramping_ || frameTime < rampStartUs_) {
        return;
    }
    uint64_t into = frameTime - rampStartUs_;
    uint8_t b = rampTo_;
    if (rampUs_ > 0 && into < rampUs_) {
        b = (uint8_t)(rampFrom_ + ((int)rampTo_ - (int)rampFrom_) * (int64_t)into / (int64_t)rampUs_);
    } else {
        ramping_ = false;
    }
    canvas->getRenderer()->setBrightness(b);
}

bool Conductor::step() {
    uint64_t now = clockUs();
    uint64_t frame = now - now % usPerFrame;

    // Locking to the leader (or a leader reboot) moves the clock a long way at once. Restart the
    // effect at the new time instead of letting it try to cover the gap in one step.
    uint64_t show;
    bool locked = source_.showTime(show);
    bool jumped = now > lastClock_ + kRestartJumpUs || now + kRestartJumpUs < lastClock_;
    if ((locked != locked_ || jumped) && current_ != nullptr) {
        startAnimation(current_, frame);
    }
    locked_ = locked;
    lastClock_ = now;

    Cue c;
    while (source_.popCue(c)) {
        if (waitingCount_ < kMaxWaiting) {
            waiting_[waitingCount_++] = c;
        } else {
            // Shouldn't happen (cues are a few a minute); play the oldest now to make room.
            apply(waiting_[0], frame);
            memmove(waiting_, waiting_ + 1, sizeof(Cue) * (kMaxWaiting - 1));
            waiting_[kMaxWaiting - 1] = c;
        }
    }

    // Cues take effect on the first frame starting at or after their time, oldest time first.
    while (true) {
        int due = -1;
        for (int i = 0; i < waitingCount_; i++) {
            if (waiting_[i].atUs <= frame && (due < 0 || waiting_[i].atUs < waiting_[due].atUs)) {
                due = i;
            }
        }
        if (due < 0) {
            break;
        }
        Cue d = waiting_[due];
        waiting_[due] = waiting_[--waitingCount_];
        apply(d, frame);
    }

    if (local_ && localCount_ > 1 &&
        time_us_64() - localStartUs_ >= localList_[localIndex_].durationUs) {
        playLocal(localIndex_ + 1, frame);
    }

    updateBrightness(frame);
    source_.reportFrame((uint8_t)getFPS(), (uint16_t)getMissedFrames(), effect_, cueId_);
    bool more = Animator::step();
    reportFrameStats();
    return more;
}

namespace {

// Microseconds into a u16 field. An average over no frames yet is NaN, which comes out as 0.
uint16_t clamp16(float us) {
    if (!(us > 0.0f)) {
        return 0;
    }
    return us >= 65535.0f ? 65535 : (uint16_t)(us + 0.5f);
}

}  // namespace

void Conductor::reportFrameStats() {
    if (frameWatch.lastTime > worstFrameUs_) {
        worstFrameUs_ = frameWatch.lastTime;
    }
    uint64_t now = time_us_64();
    if (now < nextStatsUs_) {
        return;
    }
    nextStatsUs_ = now + kStatsIntervalUs;
    FrameStats s;
    s.fps = (uint8_t)getFPS();
    s.avgFrameUs = clamp16(getAverageFrameTimeUS());
    s.worstFrameUs = clamp16((float)worstFrameUs_);
    s.avgStepUs = clamp16(getAverageStepTimeUS());
    s.avgShowUs = clamp16(getAverageShowTimeUS());
    s.frameCount = getFrameCount();
    s.missedFrames = getMissedFrames();
    source_.reportFrameStats(s);
    worstFrameUs_ = 0;
}

}  // namespace picocomm
