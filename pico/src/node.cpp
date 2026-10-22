#include "picocomm/pico/node.h"

#include <malloc.h>

#include <cstdio>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "picocomm/elector.h"

// Linker symbols bounding the heap.
extern "C" char __StackLimit, __bss_end__;

namespace picocomm {
namespace pico {

namespace {

Node *theNode = nullptr;

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint64_t kHousekeepingUs = 2000000;  // RSSI and link checks

void logLine(const char *line) { printf("[picocomm] %s\n", line); }

uint16_t freeRamKB() {
    // The heap runs from the end of .bss to the stack; subtract what malloc has handed out.
    struct mallinfo mi = mallinfo();
    size_t total = &__StackLimit - &__bss_end__;
    return (uint16_t)((total - mi.uordblks) / 1024);
}

// The Transport over a lwIP UDP pcb. Everything here runs on core 1.
class LwipTransport : public Transport {
   public:
    udp_pcb *pcb = nullptr;

    uint64_t nowUs() override { return time_us_64(); }

    void send(uint32_t ipv4, uint16_t port, const uint8_t *packet) override {
        ip_addr_t dst;
        ip_addr_set_ip4_u32(&dst, ipv4);
        sendTo(&dst, port, packet);
    }

    void broadcast(uint16_t port, const uint8_t *packet) override {
        ip_addr_t dst;
        ip_addr_set_ip4_u32(&dst, IPADDR_BROADCAST);
        sendTo(&dst, port, packet);
    }

   private:
    void sendTo(const ip_addr_t *dst, uint16_t port, const uint8_t *packet) {
        pbuf *p = pbuf_alloc(PBUF_TRANSPORT, kPacketSize, PBUF_RAM);
        if (p == nullptr) {
            return;
        }
        memcpy(p->payload, packet, kPacketSize);
        udp_sendto(pcb, p, dst, port);
        pbuf_free(p);
    }
};

class ElectorGlue : public ElectorApp {
   public:
    explicit ElectorGlue(Node &n) : node(n) {}
    bool knowsEffect(uint16_t effect) override { return node.knowsEffect(effect); }
    void deliverCue(const Cue &cue) override { node.pushCue(cue); }
    void clockChanged(const ClockModel &m) override { node.publishClock(m); }
    void clockLost() override { node.unpublishClock(); }
    void fillStatus(Status &s) override { node.fillStatus(s); }
    uint32_t unixTime(uint64_t showUs) override { return node.unixTime(showUs); }
    Node &node;
};

uint32_t random32() { return get_rand_32(); }

// Core 1's protocol object. Constructed once, on core 1, after the network is up.
LwipTransport transport;
Elector *elector = nullptr;

void onUdp(void *, udp_pcb *, pbuf *p, const ip_addr_t *addr, u16_t port) {
    // Receive timestamp first: it's the t2 (leader) or t4 (follower) of a sync exchange.
    uint64_t rxUs = time_us_64();
    uint8_t buf[kPacketSize];
    if (p->tot_len == kPacketSize && pbuf_copy_partial(p, buf, kPacketSize, 0) == kPacketSize) {
        uint32_t ip = ip4_addr_get_u32(ip_2_ip4(addr));
        if (elector != nullptr) {
            elector->onPacket(buf, kPacketSize, ip, port, rxUs);
        }
    }
    pbuf_free(p);
}

void core1Entry() { theNode->core1Main(); }

bool connectWifi(const NodeConfig &cfg) {
    bool open = cfg.password == nullptr || cfg.password[0] == '\0';
    printf("[picocomm] connecting to \"%s\"\n", cfg.ssid);
    int err = cyw43_arch_wifi_connect_timeout_ms(cfg.ssid, open ? nullptr : cfg.password,
                                                 open ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK,
                                                 kConnectTimeoutMs);
    if (err != 0) {
        printf("[picocomm] connect failed (%d)\n", err);
        return false;
    }
    // Power saving off: napping between AP beacons adds tens to hundreds of ms of jitter.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    char ip[16];
    ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), ip, sizeof(ip));
    printf("[picocomm] connected, address %s\n", ip);
    return true;
}

}  // namespace

const char *toString(NetState s) {
    switch (s) {
        case NetState::Starting: return "starting";
        case NetState::RadioFailed: return "radio failed";
        case NetState::Connecting: return "connecting";
        case NetState::Searching: return "searching";
        case NetState::Joining: return "joining";
        case NetState::Joined: return "joined";
        case NetState::Orphaned: return "orphaned";
        case NetState::Leading: return "leading";
    }
    return "?";
}

Node::Node(const NodeConfig &config) : config_(config) {
    queue_init(&cues_, sizeof(Cue), kCueQueueDepth);
    queue_init(&outbound_, sizeof(OutboundCue), kOutboundDepth);
}

void Node::start() {
    theNode = this;
    // A dedicated doorbell for the startup handshake (RP2350 hardware doorbells; see
    // waitForRadio()). Claimed for both cores.
    doorbell_ = multicore_doorbell_claim_unused(0b11, true);
    if (config_.role == Role::Leader) {
        // The leader's show time is its own clock: identity mapping, locked from the start.
        publishClock(ClockModel());
    }
    multicore_launch_core1(core1Entry);
}

bool Node::waitForRadio() {
    while (!multicore_doorbell_is_set_current_core(doorbell_)) {
        tight_loop_contents();
    }
    multicore_doorbell_clear_current_core(doorbell_);
    return state() != NetState::RadioFailed;
}

bool Node::toShowTime(uint64_t localUs, uint64_t &showUs) const {
    if (!locked_.load(std::memory_order_acquire)) {
        return false;
    }
    ClockModel m;
    clock_.read(m);
    showUs = m.toShow(localUs);
    return true;
}

bool Node::showTime(uint64_t &showUs) const { return toShowTime(time_us_64(), showUs); }

bool Node::popCue(Cue &cue) { return queue_try_remove(&cues_, &cue); }

void Node::reportFrame(uint8_t fps, uint16_t missedFrames, uint16_t effect, uint16_t cueId) {
    frameStats_.store(fps | ((uint32_t)missedFrames << 8), std::memory_order_relaxed);
    playing_.store(effect | ((uint32_t)cueId << 16), std::memory_order_relaxed);
}

bool Node::cue(uint16_t groupMask, const Cue &spec, uint32_t leadMs) {
    OutboundCue o{groupMask, leadMs, spec};
    return queue_try_add(&outbound_, &o);
}

void Node::setWallClock(uint32_t unixSeconds, uint64_t atShowUs) {
    wallClock_.write({unixSeconds, atShowUs});
}

uint32_t Node::unixTime(uint64_t showUs) const {
    WallClock w;
    if (!wallClock_.read(w) || w.unixSeconds == 0) {
        return 0;
    }
    return w.unixSeconds + (uint32_t)((int64_t)(showUs - w.showUs) / 1000000);
}

void Node::pushCue(const Cue &c) {
    if (!queue_try_add(&cues_, &c)) {
        cuesDropped_.fetch_add(1, std::memory_order_relaxed);
        if (config_.logEvents) {
            printf("[picocomm] cue queue full, dropped cue %u\n", c.cueId);
        }
    }
}

bool Node::knowsEffect(uint16_t effect) const {
    if (config_.effects == nullptr) {
        return true;
    }
    for (size_t i = 0; i < config_.effectCount; i++) {
        if (config_.effects[i] == effect) {
            return true;
        }
    }
    return false;
}

void Node::fillStatus(Status &s) const {
    uint32_t frame = frameStats_.load(std::memory_order_relaxed);
    uint32_t playing = playing_.load(std::memory_order_relaxed);
    s.fps = (uint8_t)frame;
    s.missedFrames = (uint16_t)(frame >> 8);
    if (playing != 0) {
        s.effect = (uint16_t)playing;
        s.cueId = (uint16_t)(playing >> 16);
    }
    s.rssi = rssi_.load(std::memory_order_relaxed);
    s.freeRamKB = freeRamKB();
}

void Node::core1Main() {
    // cyw43 is initialized here so its interrupts land on core 1, never on the renderer's core.
    bool radioOk = cyw43_arch_init() == 0;
    state_.store(radioOk ? NetState::Connecting : NetState::RadioFailed);
    multicore_doorbell_set_other_core(doorbell_);  // core 0 may now claim PIO state machines
    if (!radioOk) {
        printf("[picocomm] cyw43_arch_init failed\n");
        while (true) {
            tight_loop_contents();
        }
    }
    cyw43_arch_enable_sta_mode();
    while (!connectWifi(config_)) {
        sleep_ms(2000);
    }

    transport.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    ip_set_option(transport.pcb, SOF_BROADCAST);
    // Followers bind the protocol port too: that's where the leader's beacons are broadcast.
    udp_bind(transport.pcb, IP_ANY_TYPE, kPort);
    udp_recv(transport.pcb, onUdp, nullptr);

    static ElectorGlue glue(*this);
    ElectorConfig ec;
    switch (config_.role) {
        case Role::Leader: ec.priority = kAlwaysLead; break;
        case Role::Follower: ec.priority = kNeverLead; break;
        case Role::Candidate:
            ec.priority = config_.leaderPriority == kNeverLead ? 1 : config_.leaderPriority;
            break;
    }
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    static_assert(sizeof(id.id) == 8, "board ID is 8 bytes");
    memcpy(ec.boardId, id.id, 8);
    ec.propType = config_.propType;
    ec.propIndex = config_.propIndex;
    ec.fwVersion = config_.fwVersion;
    ec.ownIp = ip4_addr_get_u32(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
    ec.random32 = random32;
    ec.log = config_.logEvents ? logLine : nullptr;
    static Elector e(ec, transport, glue);
    elector = &e;

    uint64_t nextHousekeeping = 0;
    while (true) {
        // Poll mode: lwIP and the cyw43 driver run only here, so a packet's receive callback runs
        // within a loop iteration of its arrival.
        cyw43_arch_poll();

        OutboundCue o;
        while (queue_try_remove(&outbound_, &o)) {
            if (elector->cue(o.groupMask, o.spec, o.leadMs) == 0 && config_.logEvents) {
                printf("[picocomm] not leading, dropped a cue from core 0\n");
            }
        }
        elector->poll();
        NetState s = NetState::Leading;
        if (const Follower *f = elector->follower()) {
            s = NetState::Searching;
            if (f->orphaned()) {
                s = NetState::Orphaned;
            } else if (f->state() == Follower::State::Joining) {
                s = NetState::Joining;
            } else if (f->state() == Follower::State::Joined) {
                s = NetState::Joined;
            }
        }
        state_.store(s, std::memory_order_relaxed);

        uint64_t now = time_us_64();
        if (now >= nextHousekeeping) {
            nextHousekeeping = now + kHousekeepingUs;
            int32_t rssi = 0;
            if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) {
                rssi_.store((int8_t)(rssi < -128 ? -128 : rssi), std::memory_order_relaxed);
            }
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
                // The protocol objects keep their state; followers free-run on their last fit
                // and orphan after 10 s, just as if the leader had gone quiet.
                NetState before = state();
                state_.store(NetState::Connecting);
                printf("[picocomm] WiFi link down, reconnecting\n");
                while (!connectWifi(config_)) {
                    sleep_ms(2000);
                }
                state_.store(before);
            }
        }
    }
}

}  // namespace pico
}  // namespace picocomm
