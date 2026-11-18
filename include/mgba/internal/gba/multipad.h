/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_MULTIPAD_H
#define GBA_MULTIPAD_H

#include <mgba-util/common.h>

CXX_GUARD_START

// Game-visible multi-controller interface (mgba-mp).
//
// The I/O range 0x04000160-0x04000183 is unused on real hardware. While the
// multi-controller option is enabled (and no link session is active) it reads:
//   0x04000160                 magic 0x4D50 ('MP')
//   0x04000162 + 2 * (n - 2)   buttons of player n (2..8), KEYINPUT bit layout, 1 = pressed
//   0x04000172 + 2 * (n - 1)   extended buttons of player n (1..8), see enum GBAExtKey,
//                              non-zero only while the extended-buttons option is enabled
// Everything else in the range reads 0. When the option is disabled, or while
// a link session is active, the whole range behaves exactly like upstream
// (unused I/O, open bus). Writes are always ignored.

#define GBA_MP_MAX_PLAYERS 8
#define GBA_MP_MAGIC 0x4D50
#define GBA_MP_REG_MAGIC 0x160
#define GBA_MP_REG_KEYS 0x162
#define GBA_MP_REG_EXT_KEYS 0x172
#define GBA_MP_REG_END 0x184
#define GBA_MP_KEY_MASK 0x03FF
#define GBA_MP_EXT_KEY_MASK 0x00FF

enum GBAExtKey {
	GBA_EXT_KEY_X = 0,
	GBA_EXT_KEY_Y = 1,
	GBA_EXT_KEY_ZL = 2,
	GBA_EXT_KEY_ZR = 3,
	GBA_EXT_KEY_L3 = 4,
	GBA_EXT_KEY_R3 = 5,
	GBA_EXT_KEY_GUIDE = 6,
	GBA_EXT_KEY_BACK = 7,
	GBA_EXT_KEY_MAX
};

struct GBAMultipad {
	bool enabled;
	bool extEnabled;
	// Index = player - 1. keys[0] is unused (player 1 is KEYINPUT).
	uint16_t keys[GBA_MP_MAX_PLAYERS];
	uint16_t extKeys[GBA_MP_MAX_PLAYERS];
};

struct GBA;

void GBAMultipadInit(struct GBAMultipad*);

bool GBAMultipadIsLinked(const struct GBA*);
bool GBAMultipadIsActive(const struct GBA*);
bool GBAMultipadExtIsActive(const struct GBA*);

void GBAMultipadSetEnabled(struct GBA*, bool enabled);
void GBAMultipadSetExtEnabled(struct GBA*, bool enabled);

// player is 1-based. Player 1 buttons are ignored (use the normal keypad).
void GBAMultipadSetKeys(struct GBA*, int player, uint16_t keys);
void GBAMultipadSetExtKeys(struct GBA*, int player, uint16_t keys);
uint16_t GBAMultipadGetKeys(const struct GBA*, int player);
uint16_t GBAMultipadGetExtKeys(const struct GBA*, int player);

// Returns true and fills *value if the register is served by the interface.
// Returns false if the access must fall through to upstream behaviour.
bool GBAMultipadReadRegister(struct GBA*, uint32_t address, uint16_t* value);

CXX_GUARD_END

#endif
