// What the protocol state machines need from the outside world: a clock and a way to send.
//
// On a Pico this is lwIP plus time_us_64() (pico/), in the host tests it is a simulated network
// with simulated clocks (tests/sim.h). IPv4 addresses are uint32_t in lwIP's ip4_addr_t.addr form:
// the four octets in memory order, so 10.0.0.7 is 0x0700000a on a little-endian machine.

#pragma once

#include <cstdint>

namespace picocomm {

class Transport {
   public:
    virtual ~Transport() = default;
    // This node's local clock in microseconds (time_us_64() on a Pico). Must never go backwards.
    virtual uint64_t nowUs() = 0;
    // Sends one kPacketSize-byte packet.
    virtual void send(uint32_t ipv4, uint16_t port, const uint8_t *packet) = 0;
    virtual void broadcast(uint16_t port, const uint8_t *packet) = 0;
};

// Optional printf-style hook for one-line event logs (joins, losses, late cues). On a Pico this is
// usually printf over USB.
using LogFn = void (*)(const char *line);

}  // namespace picocomm
