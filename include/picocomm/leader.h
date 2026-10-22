// The leader side of the protocol (docs/protocol-design.md, sections 3 to 6 and 9): beacon, keep
// the roster, answer SYNC requests, send cues with ACK and resend, and serve the laptop.
//
// Portable C++ with no pico-sdk dependency, like Follower. Show time is the leader's own local clock
// (Transport::nowUs()) plus a fixed offset, which is 0 unless the leader took over from another one
// and carries on its show time (Elector, section 5).

#pragma once

#include <cstddef>
#include <cstdint>

#include "picocomm/effects.h"
#include "picocomm/transport.h"
#include "picocomm/wire.h"

namespace picocomm {

struct LeaderConfig {
    // Random per boot, so followers can tell the leader rebooted and show time started over.
    uint32_t generation = 0;
    // The leader is a prop too, and gets cues for its own group through LeaderApp::deliverCue.
    PropType propType = PropType::Unknown;
    uint8_t propIndex = 0;
    // Sent in every BEACON so other candidates know whether to defer (section 5).
    uint32_t priority = kAlwaysLead;
    // Show time minus local time. Every time the leader handles (timers, SYNC_RESP timestamps,
    // cue times) is show time.
    int64_t showOffsetUs = 0;
    LogFn log = nullptr;
};

class LeaderApp {
   public:
    virtual ~LeaderApp() = default;
    // A cue for the leader's own prop. Same contract as FollowerApp::deliverCue.
    virtual void deliverCue(const Cue &cue) = 0;
    // Wall-clock time at the given show time, as unix seconds, or 0 if not known (no RTC).
    virtual uint32_t unixTime(uint64_t /*showUs*/) { return 0; }
    // The same in microseconds, for the BEACON's unixFraction. Override this one if the app knows
    // the wall clock to better than a second.
    virtual uint64_t unixTimeUs(uint64_t showUs) { return (uint64_t)unixTime(showUs) * 1000000; }
    // The leader's own row in the roster: fps, missedFrames, rssi, effect, cueId.
    virtual void fillStatus(Status &/*status*/) {}
};

class Leader {
   public:
    static constexpr int kMaxFollowers = 16;
    static constexpr int kMaxPending = 4;  // unacknowledged cues per follower
    static constexpr uint64_t kBeaconIntervalUs = 1000000;
    static constexpr uint64_t kResendIntervalUs = 50000;
    // Stop resending a cue this long after its time (or after it was first sent, if later).
    static constexpr uint64_t kGiveUpUs = 2000000;
    static constexpr uint64_t kStaleUs = 6000000;
    static constexpr uint64_t kLostUs = 30000000;
    static constexpr uint32_t kDefaultLeadMs = 300;

    struct PendingCue {
        bool used = false;
        bool catchUp = false;  // sent on join to catch a late joiner up; lateness is expected
        Cue cue;
        uint64_t firstSentUs = 0;
        uint64_t lastSentUs = 0;
        uint64_t giveUpUs = 0;
        uint8_t tries = 0;
    };

    struct Member {
        bool used = false;
        uint8_t nodeId = 0;
        uint8_t boardId[8] = {};
        uint8_t propType = 0;
        uint8_t propIndex = 0;
        uint16_t fwVersion = 0;
        uint16_t catalogVersion = 0;
        uint16_t capabilities = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        NodeState state = NodeState::Joining;
        uint64_t lastHeardUs = 0;
        bool hasStatus = false;
        Status status;
        uint32_t cuesSent = 0;
        uint32_t cuesLate = 0;
        uint32_t cuesMissed = 0;  // gave up resending
        uint32_t cuesUnknown = 0;
        uint16_t lastLateCueId = 0;
        int32_t lastLateUs = 0;
        PendingCue pending[kMaxPending];
    };

    Leader(const LeaderConfig &config, Transport &transport, LeaderApp &app);

    void onPacket(const uint8_t *data, size_t len, uint32_t srcIp, uint16_t srcPort,
                  uint64_t rxUs);
    // Timers: beacons, resends, stale/lost detection. Call often.
    void poll();

    // Cues every prop in groupMask (including the leader's own prop, if its group is in the mask)
    // to do spec (kind, transition, effect, transitionMs, params; its cueId and atUs are ignored)
    // leadMs from now. All targets get the same cue ID and the same show time. Returns the cue ID.
    uint16_t cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs = kDefaultLeadMs);
    uint16_t cueAt(uint16_t groupMask, const Cue &spec, uint64_t atShowUs);

    uint64_t showTime() { return transport_.nowUs() + (uint64_t)config_.showOffsetUs; }
    uint32_t generation() const { return config_.generation; }
    // The leader's own address, for its roster row (lwIP order; see transport.h).
    void setOwnAddress(uint32_t ipv4) { ownIp_ = ipv4; }

    int memberSlots() const { return kMaxFollowers; }
    const Member &member(int slot) const { return members_[slot]; }
    int activeFollowers() const;  // joined and not lost

   private:
    void handleJoin(const uint8_t *data, uint32_t ip, uint16_t port, uint64_t rxUs);
    void handleSyncReq(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port,
                       uint64_t rxUs);
    void handleAck(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs);
    void handleStatus(const Header &h, const uint8_t *data, uint32_t ip, uint64_t rxUs);
    void handleQuery(const uint8_t *data, uint32_t ip, uint16_t port);
    void handleOverride(const Header &h, const uint8_t *data, uint32_t ip, uint16_t port);
    Member *findMember(uint8_t nodeId, uint32_t ip);
    void heard(Member &m, uint64_t rxUs);
    void queueCue(Member &m, const Cue &c, bool catchUp, uint64_t now);
    void sendCue(Member &m, PendingCue &p, uint64_t now);
    void sendBeacon(uint64_t now);
    Header header(uint8_t flags = 0);
    void log(const char *fmt, ...);

    LeaderConfig config_;
    Transport &transport_;
    LeaderApp &app_;
    uint32_t ownIp_ = 0;
    uint16_t seq_ = 0;
    uint16_t nextCueId_ = 1;
    uint16_t sceneId_ = 0;
    uint64_t nextBeaconUs_ = 0;

    // The last OVERRIDE, so a resend (the observer missed our ACK) isn't cued twice.
    uint32_t lastOverrideIp_ = 0;
    uint16_t lastOverridePort_ = 0;
    uint16_t lastOverrideSeq_ = 0;
    uint16_t lastOverrideCueId_ = 0;

    Member members_[kMaxFollowers];

    // The latest scene cue (effect, blackout, resume local) and brightness cue per group, resent
    // to a follower when it joins so a late joiner catches up.
    Cue sceneCue_[16];
    bool haveSceneCue_[16] = {};
    Cue brightnessCue_[16];
    bool haveBrightnessCue_[16] = {};
};

}  // namespace picocomm
