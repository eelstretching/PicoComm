// Leader election (docs/protocol-design.md, section 5): every prop runs an Elector, which is a
// Follower or a Leader as the election goes, and switches between them.
//
// Each node has a leader priority. Props with an RTC and the show script enter with a number that
// means something (by default their LED count, so the MegaTree wins); 0 never leads, and
// kAlwaysLead is a fixed leader that never steps down. The rank is (priority, IPv4 address): the
// higher priority wins and ties go to the higher address. Election is by BEACON alone:
//
// - At boot a candidate follows for kListenUs (plus a random 0 to 1 s, so boards powered on
//   together don't all claim at once). If it heard no leader, it starts leading.
// - A leader that hears a higher-ranked leader's BEACON steps down and follows it. So two leaders
//   (two boards booted together, or the network came back after a split) settle within a beacon.
// - A candidate that outranks the leader it follows takes over once its clock is locked, carrying
//   on the show time. So a MegaTree that reboots mid-show takes the show back without a jump.
// - Everyone follows the highest-ranked leader they hear. When it goes quiet for
//   kLeaderSilentUs (plus the same jitter), the best candidate left takes over, carrying on the
//   show time from its own locked clock; followers see the new leader's BEACON and re-lock.
//
// Portable C++ like Leader and Follower. The Leader and Follower live inside the Elector (no heap);
// whichever is not in use is destroyed, so each role change starts that side fresh.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "picocomm/effects.h"
#include "picocomm/follower.h"
#include "picocomm/leader.h"
#include "picocomm/transport.h"
#include "picocomm/wire.h"

namespace picocomm {

struct ElectorConfig {
    // kNeverLead (0) only ever follows; kAlwaysLead leads from boot and never steps down.
    uint32_t priority = kNeverLead;
    uint8_t boardId[8] = {};
    PropType propType = PropType::Unknown;
    uint8_t propIndex = 0;
    uint16_t fwVersion = 0;
    uint16_t capabilities = 0;
    // This node's address (lwIP order), for the tiebreak and to ignore its own beacons.
    uint32_t ownIp = 0;
    // Random numbers for leader generations and the jitter. Must not return the same sequence on
    // every board (on a Pico, get_rand_32()).
    uint32_t (*random32)() = nullptr;
    LogFn log = nullptr;
};

// The prop's side of both roles. clockChanged() also carries the leader's own clock (local time
// plus the show offset) whenever this node starts leading.
class ElectorApp : public FollowerApp, public LeaderApp {
   public:
    // This node started (true) or stopped (false) leading. A leader runs the show script.
    virtual void leadingChanged(bool /*leading*/) {}
};

class Elector {
   public:
    static constexpr uint64_t kListenUs = 3000000;
    static constexpr uint64_t kLeaderSilentUs = 8000000;
    static constexpr uint32_t kJitterUs = 1000000;

    Elector(const ElectorConfig &config, Transport &transport, ElectorApp &app);

    void onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                  uint64_t rxUs);
    void poll();

    bool leading() const { return leader_.has_value(); }
    // The active side; exactly one of them is non-null.
    Leader *leader() { return leader_ ? &*leader_ : nullptr; }
    Follower *follower() { return follower_ ? &*follower_ : nullptr; }
    const Follower *follower() const { return follower_ ? &*follower_ : nullptr; }

    // Leader only: see Leader::cue. Returns 0 (no cue) when following.
    uint16_t cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs = Leader::kDefaultLeadMs);

    // The leader this node follows (0 if none heard), and how many times it took over or stepped
    // down.
    uint32_t leaderIp() const { return leaderIp_; }
    uint32_t takeovers() const { return takeovers_; }
    uint32_t stepDowns() const { return stepDowns_; }

   private:
    struct Rank {
        uint32_t priority;
        uint32_t ip;
        bool operator>(const Rank &o) const {
            return priority != o.priority ? priority > o.priority : ip > o.ip;
        }
    };

    bool candidate() const { return config_.priority != kNeverLead; }
    Rank ownRank() const { return {config_.priority, config_.ownIp}; }
    void heardBeacon(const Beacon &b, uint32_t ip, uint64_t now);
    void lead(uint64_t now, const char *why);
    // seed: the show clock to keep running until the first sync (a leader stepping down).
    void follow(const ClockModel *seed = nullptr);
    void log(const char *fmt, ...);

    ElectorConfig config_;
    Transport &transport_;
    ElectorApp &app_;
    std::optional<Leader> leader_;
    std::optional<Follower> follower_;

    uint32_t jitterUs_ = 0;
    uint64_t startUs_ = 0;
    // The highest-ranked leader heard lately: the one we follow.
    uint32_t leaderIp_ = 0;
    Rank leaderRank_{0, 0};
    uint64_t leaderHeardUs_ = 0;

    uint32_t takeovers_ = 0;
    uint32_t stepDowns_ = 0;
};

}  // namespace picocomm
