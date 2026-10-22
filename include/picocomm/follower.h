// The follower side of the protocol (docs/protocol-design.md, sections 4 to 6): find the leader
// from its BEACON, JOIN, keep the clock locked with SYNC bursts, report STATUS, and accept CUEs.
//
// Portable C++ with no pico-sdk dependency: everything comes in through onPacket() and poll(), and
// goes out through the Transport and the FollowerApp. On a Pico both are called from core 1's
// network loop (pico/); in the host tests from the simulator.

#pragma once

#include <cstddef>
#include <cstdint>

#include "picocomm/effects.h"
#include "picocomm/sync.h"
#include "picocomm/transport.h"
#include "picocomm/wire.h"

namespace picocomm {

struct FollowerConfig {
    uint8_t boardId[8] = {};
    PropType propType = PropType::Unknown;
    uint8_t propIndex = 0;
    uint16_t fwVersion = 0;
    uint16_t capabilities = 0;
    LogFn log = nullptr;
};

// What the follower needs from the prop it runs on.
class FollowerApp {
   public:
    virtual ~FollowerApp() = default;
    // Whether this prop can play the effect. Unknown effects are refused with UNKNOWN_EFFECT and
    // the prop keeps playing what it was playing.
    virtual bool knowsEffect(uint16_t effect) = 0;
    // A cue to act on at show time cue.atUs (or right away, if that has already passed). Each cue
    // ID is delivered once, however many times the leader resends it.
    virtual void deliverCue(const Cue &cue) = 0;
    // The local-to-show-time mapping changed. Called after every lock, step, and slew update.
    virtual void clockChanged(const ClockModel &model) = 0;
    // Show time is no longer known (the leader rebooted and show time started over). Until the
    // next clockChanged(), the old mapping is meaningless.
    virtual void clockLost() {}
    // Fills in what only the prop knows: fps, missedFrames, rssi, freeRamKB, and effect/cueId if it
    // tracks what's actually on screen (they default to the last cue accepted).
    virtual void fillStatus(Status &/*status*/) {}
};

class Follower {
   public:
    // Timing (section 4 to 6 of the design).
    static constexpr uint64_t kBurstIntervalUs = 2000000;
    static constexpr int kBurstSize = 4;
    static constexpr uint64_t kBurstSpacingUs = 20000;
    // A burst is finished this long after it started; later responses are ignored.
    static constexpr uint64_t kBurstWindowUs = 260000;
    static constexpr uint64_t kStatusIntervalUs = 2000000;
    static constexpr uint64_t kJoinRetryUs = 1000000;
    static constexpr uint64_t kOrphanTimeoutUs = 10000000;
    static constexpr int kRecentCues = 8;
    // A new leader whose BEACON shows a show time this close to ours took over and carried on the
    // old show time (section 5): keep the clock running and just re-lock to the new leader.
    static constexpr uint64_t kHandoverToleranceUs = 100000;

    enum class State : uint8_t {
        Searching,  // waiting for a beacon
        Joining,    // sent JOIN, waiting for WELCOME
        Joined,     // syncing, accepting cues
    };

    Follower(const FollowerConfig &config, Transport &transport, FollowerApp &app);

    // A received datagram, with the local time it arrived (taken as early as possible).
    void onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                  uint64_t rxUs);
    // Timers: join retries, sync bursts, status, orphan detection, clock slewing. Call often.
    void poll();

    State state() const { return state_; }
    bool locked() const { return clock_.locked(); }
    bool orphaned() const { return orphaned_; }
    uint8_t nodeId() const { return nodeId_; }
    uint32_t leaderIp() const { return leaderIp_; }
    uint32_t leaderGeneration() const { return generation_; }
    const ClockDiscipline &clock() const { return clock_; }
    uint64_t showTime(uint64_t localUs) const { return clock_.published().toShow(localUs); }
    uint32_t lastSyncDelayUs() const { return lastDelayUs_; }
    // Starts the clock locked to a mapping we already trust (a leader stepping down passes its own
    // show clock), so the first sync slews to the new leader instead of stepping, if they are close.
    void seedClock(const ClockModel &model);
    // Local time we last heard from the leader (WELCOME or SYNC_RESP).
    uint64_t lastLeaderUs() const { return lastLeaderUs_; }

    struct Stats {
        uint32_t syncSamples = 0;
        uint32_t bursts = 0;
        uint32_t steps = 0;
        uint32_t cuesAccepted = 0;
        uint32_t cueDuplicates = 0;
        uint32_t cuesLate = 0;
        uint32_t orphanings = 0;
        uint32_t leaderReboots = 0;
        uint32_t handovers = 0;  // new leaders that carried on the show time
    };
    const Stats &stats() const { return stats_; }

   private:
    struct RecentCue {
        uint16_t cueId;
        AckStatus status;
        int32_t lateByUs;
    };

    void handleBeacon(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port,
                      uint64_t rxUs);
    void handleWelcome(const uint8_t *data, uint32_t ip, uint64_t rxUs);
    void handleSyncResp(const uint8_t *data, uint32_t ip, uint64_t rxUs);
    void handleCue(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs);
    void newLeader(const Beacon &b, uint64_t rxUs);
    void sendJoin(uint64_t now);
    void sendSyncReq();
    void sendStatus(uint64_t now);
    void sendAck(uint16_t ackedSeq, const RecentCue &r);
    void finishBurst(uint64_t now);
    void orphan(uint64_t now);
    Header header();
    void log(const char *fmt, ...);

    FollowerConfig config_;
    Transport &transport_;
    FollowerApp &app_;

    State state_ = State::Searching;
    bool orphaned_ = false;
    bool haveGeneration_ = false;
    bool forceStep_ = false;
    uint32_t generation_ = 0;
    uint32_t leaderIp_ = 0;
    uint16_t leaderPort_ = kPort;
    uint8_t nodeId_ = kUnassignedId;
    uint16_t seq_ = 0;

    uint64_t lastJoinUs_ = 0;
    uint64_t lastLeaderUs_ = 0;  // last SYNC_RESP or WELCOME
    uint64_t nextBurstUs_ = 0;
    uint64_t burstStartUs_ = 0;
    int burstSent_ = 0;
    bool burstActive_ = false;
    uint64_t nextStatusUs_ = 0;
    uint64_t lastSampleUs_ = 0;
    uint32_t lastDelayUs_ = 0;

    BurstFilter burst_;
    DriftFit fit_;
    ClockDiscipline clock_;

    RecentCue recent_[kRecentCues] = {};
    int recentCount_ = 0;
    int recentNext_ = 0;
    uint16_t lastCueId_ = 0;
    uint16_t lastEffect_ = 0;

    Stats stats_;
};

}  // namespace picocomm
