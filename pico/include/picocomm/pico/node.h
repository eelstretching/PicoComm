// PicoComm on a Pico 2 W: the network runs on core 1 (cyw43 + lwIP in poll mode, in a tight
// loop), core 0 keeps rendering and asks for show time and cues. docs/protocol-design.md,
// section 10.
//
// Typical prop firmware:
//
//     picocomm::pico::NodeConfig cfg;
//     cfg.role = picocomm::pico::Role::Candidate;  // or Follower, or a fixed Leader
//     cfg.leaderPriority = 6400;                   // e.g. the prop's LED count
//     cfg.propType = picocomm::PropType::Arch;
//     cfg.propIndex = 3;
//     cfg.ssid = "show"; cfg.password = "...";
//     cfg.effects = myEffects; cfg.effectCount = n;
//     picocomm::pico::Node node(cfg);
//     node.start();          // launches core 1
//     node.waitForRadio();   // BEFORE canvas.setup(): cyw43 must claim its PIO state machine first
//     canvas.setup();
//     while (true) {
//         uint64_t show;
//         if (node.showTime(show)) { ...frame pacing on show time... }
//         picocomm::Cue cue;
//         while (node.popCue(cue)) { ...act on it at cue.atUs... }
//         if (node.leading()) { ...run the show script: node.cue(...)... }
//     }
//
// Only one Node can exist (core 1 has one entry point).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pico/util/queue.h"

#include "picocomm/cue_source.h"
#include "picocomm/effects.h"
#include "picocomm/seqlock.h"
#include "picocomm/sync.h"
#include "picocomm/wire.h"

namespace picocomm {
namespace pico {

// Leader: always leads (BEACON priority kAlwaysLead). Follower: never leads. Candidate: enters the
// leader election with leaderPriority (docs/protocol-design.md, section 5).
enum class Role : uint8_t { Leader, Follower, Candidate };

// What core 1 is doing, for a status light or the serial log.
enum class NetState : uint8_t {
    Starting,
    RadioFailed,  // cyw43_arch_init() failed; nothing will happen
    Connecting,   // joining the WiFi network
    Searching,    // follower: waiting for a beacon
    Joining,      // follower: sent JOIN
    Joined,       // following: in the show
    Orphaned,     // following: lost the leader, playing the local playlist, still searching
    Leading,      // leading: beaconing, running the show
};

const char *toString(NetState s);

struct NodeConfig {
    Role role = Role::Follower;
    // Candidates only: the higher number leads (by convention the prop's LED count). 0 is taken as
    // 1, since 0 means "never lead".
    uint32_t leaderPriority = 0;
    PropType propType = PropType::Unknown;
    uint8_t propIndex = 0;
    uint16_t fwVersion = 0;
    const char *ssid = "";
    const char *password = "";  // empty for an open network
    // Effects this prop can play; a cue for anything else is refused with UNKNOWN_EFFECT. nullptr
    // accepts every effect. The array must outlive the Node.
    const uint16_t *effects = nullptr;
    size_t effectCount = 0;
    // Print protocol events (joins, losses, clock steps, late cues) over stdio.
    bool logEvents = true;
};

class Node : public CueSource {
   public:
    static constexpr int kCueQueueDepth = 8;
    static constexpr int kOutboundDepth = 8;

    explicit Node(const NodeConfig &config);

    // ---- Core 0 ----

    // Launches core 1. Call once.
    void start();
    // Blocks until core 1 has finished cyw43_arch_init(). Call before canvas.setup(), so the LED
    // renderer can't take the PIO state machine cyw43 needs (the same race OfficeSign's
    // BtReadySignal handles). Returns false if the radio failed to start.
    bool waitForRadio();

    // Current show time. False until the clock is locked to the leader's (always true on the
    // leader once started). Never blocks.
    bool showTime(uint64_t &showUs) const override;
    // Show time at a given local time_us_64(), for converting timestamps.
    bool toShowTime(uint64_t localUs, uint64_t &showUs) const;

    // Next cue, if any. Cues come out in arrival order; act on each at cue.atUs (show time), or
    // at once if that has passed.
    bool popCue(Cue &cue) override;

    // What core 0 is doing, sent to the leader in STATUS (and shown by `picocomm watch`).
    void reportFrame(uint8_t fps, uint16_t missedFrames, uint16_t effect, uint16_t cueId) override;

    // Whether this node is leading right now (a Candidate can start or stop at any time).
    bool leading() const { return state() == NetState::Leading; }

    // Leader only: cue every prop in groupMask (including this one) leadMs from now. Queued to
    // core 1; returns false if that queue is full. Cues still queued when this node stops leading
    // are dropped.
    bool cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs = 300);
    // Tell the node the wall-clock time (from the DS3231) so its BEACONs carry it while it leads.
    void setWallClock(uint32_t unixSeconds, uint64_t atShowUs);

    NetState state() const { return state_.load(std::memory_order_relaxed); }
    int8_t rssi() const { return rssi_.load(std::memory_order_relaxed); }
    uint32_t cuesDropped() const { return cuesDropped_.load(std::memory_order_relaxed); }

    // ---- Core 1 internals (public for the glue's callbacks; not for prop code) ----

    struct OutboundCue {
        uint16_t groupMask;
        uint32_t leadMs;
        Cue spec;
    };

    void core1Main();
    void publishClock(const ClockModel &m) { clock_.write(m); locked_.store(true); }
    void unpublishClock() { locked_.store(false); }
    void pushCue(const Cue &c);
    void fillStatus(Status &s) const;
    bool knowsEffect(uint16_t effect) const;
    uint32_t unixTime(uint64_t showUs) const;
    const NodeConfig &config() const { return config_; }

   private:
    NodeConfig config_;
    int doorbell_ = -1;
    queue_t cues_;
    queue_t outbound_;
    SeqLock<ClockModel> clock_;
    std::atomic<bool> locked_{false};
    std::atomic<NetState> state_{NetState::Starting};
    std::atomic<int8_t> rssi_{0};
    std::atomic<uint32_t> cuesDropped_{0};
    std::atomic<uint32_t> frameStats_{0};  // fps | missedFrames << 8
    std::atomic<uint32_t> playing_{0};     // effect | cueId << 16
    struct WallClock {
        uint32_t unixSeconds;
        uint64_t showUs;
    };
    SeqLock<WallClock> wallClock_;
};

}  // namespace pico
}  // namespace picocomm
