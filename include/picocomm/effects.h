// The shared effect catalog: which effect IDs exist and what their 8 parameter bytes mean.
//
// A CUE names an effect by ID; each prop's firmware maps the IDs it supports to its own Animation
// objects (the MegaTree's Starlight, the arches' Leap, ...) and tells PicoComm which ones those
// are, so an unsupported ID is answered with ACK status UNKNOWN_EFFECT instead of being played.
//
// Rules for editing this file:
// - Never renumber or reuse an ID. Add new effects at the end of their block.
// - Bump kCatalogVersion whenever an ID is added or a parameter's meaning changes. JOIN carries the
//   version, so the leader can warn about a prop running an older catalog.

#pragma once

#include <cstdint>

namespace picocomm {

constexpr uint16_t kCatalogVersion = 1;

// Prop types. Each type is also a group: group bit n is prop type n, so "all arches" is the mask
// groupBit(PropType::Arch).
enum class PropType : uint8_t {
    Unknown = 0,
    MegaTree = 1,
    Topper = 2,
    Arch = 3,
    MiniTree = 4,
    Bench = 15,  // bench and test boards
};

constexpr uint16_t groupBit(PropType t) { return (uint16_t)(1u << (uint8_t)t); }
constexpr uint16_t kAllGroups = 0xffff;

enum class EffectId : uint16_t {
    // 0x0000 to 0x00ff: generic effects every prop should support.
    None = 0x0000,         // all pixels off
    SolidColor = 0x0001,   // params: [0..2] r, g, b
    Starlight = 0x0002,    // params: [0] palette, [1] speed (0 = default), [2..5] u32 random seed
    TwinkleFox = 0x0003,   // params: [0] palette, [1] density, [2] speed, [3..6] u32 random seed
    FillWipe = 0x0004,     // params: [0..2] r, g, b, [3] speed
    Chase = 0x0005,        // params: [0..2] r, g, b, [3] speed, [4] length

    // 0x0100 to 0x01ff: MegaTree and topper.
    TopperStarlight = 0x0100,  // params: as Starlight

    // 0x0200 to 0x02ff: arches.
    Leap = 0x0200,     // params: [0..2] r, g, b, [3] speed
    ArchRise = 0x0201, // params: [0..2] r, g, b, [3] speed
    Cylon = 0x0202,    // params: [0..2] r, g, b, [3] speed

    // 0xff00 to 0xffff: test patterns for the bench.
    TestFlash = 0xff00,  // params: [0..2] r, g, b; full strip on for 100 ms, then off
};

// Palette numbers used in params, in the order PicoLEDs' HolidayPalettes.h declares them.
enum class Palette : uint8_t {
    RedGreenWhite = 0,
    Holly = 1,
    RedWhite = 2,
    BlueWhite = 3,
    FairyLight = 4,
    Snow = 5,
    RetroC9 = 6,
    Ice = 7,
};

}  // namespace picocomm
