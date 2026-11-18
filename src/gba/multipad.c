/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/multipad.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>

// Must match DRIVER_ID in sio/lockstep.c ('Lock'). Compared by value so this
// file does not depend on the lockstep implementation being linked in.
#define LOCKSTEP_DRIVER_ID 0x6B636F4C

void GBAMultipadInit(struct GBAMultipad* mp) {
	memset(mp, 0, sizeof(*mp));
}

bool GBAMultipadIsLinked(const struct GBA* gba) {
	const struct GBASIODriver* driver = gba->sio.driver;
	if (!driver || !driver->driverId) {
		return false;
	}
	if (driver->driverId(driver) != LOCKSTEP_DRIVER_ID) {
		return false;
	}
	const struct GBASIOLockstepDriver* lockstep = (const struct GBASIOLockstepDriver*) driver;
	if (!lockstep->coordinator) {
		return false;
	}
	return lockstep->coordinator->nAttached > 1;
}

bool GBAMultipadIsActive(const struct GBA* gba) {
	return gba->mp.enabled && !GBAMultipadIsLinked(gba);
}

bool GBAMultipadExtIsActive(const struct GBA* gba) {
	return gba->mp.extEnabled && !GBAMultipadIsLinked(gba);
}

void GBAMultipadSetEnabled(struct GBA* gba, bool enabled) {
	gba->mp.enabled = enabled;
}

void GBAMultipadSetExtEnabled(struct GBA* gba, bool enabled) {
	gba->mp.extEnabled = enabled;
}

void GBAMultipadSetKeys(struct GBA* gba, int player, uint16_t keys) {
	if (player < 2 || player > GBA_MP_MAX_PLAYERS) {
		return;
	}
	gba->mp.keys[player - 1] = keys & GBA_MP_KEY_MASK;
}

void GBAMultipadSetExtKeys(struct GBA* gba, int player, uint16_t keys) {
	if (player < 1 || player > GBA_MP_MAX_PLAYERS) {
		return;
	}
	gba->mp.extKeys[player - 1] = keys & GBA_MP_EXT_KEY_MASK;
}

uint16_t GBAMultipadGetKeys(const struct GBA* gba, int player) {
	if (player < 2 || player > GBA_MP_MAX_PLAYERS) {
		return 0;
	}
	return gba->mp.keys[player - 1];
}

uint16_t GBAMultipadGetExtKeys(const struct GBA* gba, int player) {
	if (player < 1 || player > GBA_MP_MAX_PLAYERS) {
		return 0;
	}
	return gba->mp.extKeys[player - 1];
}

bool GBAMultipadReadRegister(struct GBA* gba, uint32_t address, uint16_t* value) {
	address &= ~1U;
	if (address < GBA_MP_REG_MAGIC || address >= GBA_MP_REG_END) {
		return false;
	}
	if (!GBAMultipadIsActive(gba)) {
		return false;
	}
	if (address == GBA_MP_REG_MAGIC) {
		*value = GBA_MP_MAGIC;
		return true;
	}
	if (address >= GBA_MP_REG_KEYS && address < GBA_MP_REG_KEYS + 2 * (GBA_MP_MAX_PLAYERS - 1)) {
		int player = (address - GBA_MP_REG_KEYS) / 2 + 2;
		*value = gba->mp.keys[player - 1];
		return true;
	}
	if (address >= GBA_MP_REG_EXT_KEYS && address < GBA_MP_REG_EXT_KEYS + 2 * GBA_MP_MAX_PLAYERS) {
		int player = (address - GBA_MP_REG_EXT_KEYS) / 2 + 1;
		*value = gba->mp.extEnabled ? gba->mp.extKeys[player - 1] : 0;
		return true;
	}
	// Gaps (0x170, 0x182) inside the interface range
	*value = 0;
	return true;
}
