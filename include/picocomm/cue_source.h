// What a prop's animation loop needs from PicoComm, as an interface so it can be faked in tests.
// picocomm::pico::Node implements it on a Pico.

#pragma once

#include <cstdint>

#include "picocomm/wire.h"

namespace picocomm {

class CueSource {
   public:
    virtual ~CueSource() = default;
    // Current show time. False until the clock is locked to the leader's.
    virtual bool showTime(uint64_t &showUs) const = 0;
    // Next cue, if any, in arrival order.
    virtual bool popCue(Cue &cue) = 0;
    // What's on screen, for the leader's roster.
    virtual void reportFrame(uint8_t fps, uint16_t missedFrames, uint16_t effect,
                             uint16_t cueId) = 0;
    // How the animation loop is doing (frame counts and times), for an observer's QUERY(stats).
    // Every second or so is plenty. nodeId, nodeCount, uptimeS and tempC are filled in later.
    virtual void reportFrameStats(const FrameStats & /*stats*/) {}
};

}  // namespace picocomm
