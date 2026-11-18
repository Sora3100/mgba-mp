/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/input.h>
#include <mgba-util/configuration.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/multipad.h>
#include <mgba/internal/gba/sio/lockstep.h>

#define IO(X) (0x04000000 | (X))
// Another unused I/O register that always takes the upstream open-bus path
#define IO_UNUSED_REFERENCE IO(0x190)

static struct mCore* _createCore(void) {
	struct mCore* core = GBACoreCreate();
	assert_non_null(core);
	assert_true(core->init(core));
	mCoreInitConfig(core, NULL);
	core->reset(core);
	return core;
}

static void _destroyCore(struct mCore* core) {
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
}

static uint32_t _load16(struct mCore* core, uint32_t address) {
	struct ARMCore* cpu = core->cpu;
	return cpu->memory.load16(cpu, address, NULL);
}

static uint32_t _load8(struct mCore* core, uint32_t address) {
	struct ARMCore* cpu = core->cpu;
	return cpu->memory.load8(cpu, address, NULL);
}

static uint32_t _load32(struct mCore* core, uint32_t address) {
	struct ARMCore* cpu = core->cpu;
	return cpu->memory.load32(cpu, address, NULL);
}

static void _setOption(struct mCore* core, const char* option, bool value) {
	mCoreConfigSetIntValue(&core->config, option, value);
	core->reloadConfigOption(core, option, NULL);
}

static void _assertUpstream(struct mCore* core) {
	uint32_t reference = _load16(core, IO_UNUSED_REFERENCE);
	uint32_t address;
	for (address = GBA_MP_REG_MAGIC; address < GBA_MP_REG_END; address += 2) {
		assert_int_equal(_load16(core, IO(address)), reference);
	}
}

static void _setFakePads(struct mCore* core) {
	struct GBA* gba = core->board;
	int player;
	for (player = 1; player <= GBA_MP_MAX_PLAYERS; ++player) {
		GBAMultipadSetKeys(gba, player, 0x1000 | (player * 0x41));
		GBAMultipadSetExtKeys(gba, player, 0x100 | (0x80 >> (player - 1)));
	}
}

M_TEST_DEFINE(disabledIsUpstream) {
	struct mCore* core = _createCore();
	_setFakePads(core);
	assert_int_not_equal(_load16(core, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC);
	_assertUpstream(core);
	// Extended buttons alone do not enable the interface
	_setOption(core, "extendedButtons", true);
	_assertUpstream(core);
	_destroyCore(core);
}

M_TEST_DEFINE(enabledRegisters) {
	struct mCore* core = _createCore();
	_setFakePads(core);
	_setOption(core, "multiController", true);

	assert_int_equal(_load16(core, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC);
	int player;
	for (player = 2; player <= GBA_MP_MAX_PLAYERS; ++player) {
		assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS + 2 * (player - 2))), (player * 0x41) & GBA_MP_KEY_MASK);
	}
	// Extended buttons off: extended words read 0
	for (player = 1; player <= GBA_MP_MAX_PLAYERS; ++player) {
		assert_int_equal(_load16(core, IO(GBA_MP_REG_EXT_KEYS + 2 * (player - 1))), 0);
	}
	// Gaps read 0
	assert_int_equal(_load16(core, IO(0x170)), 0);
	assert_int_equal(_load16(core, IO(0x182)), 0);

	_setOption(core, "extendedButtons", true);
	for (player = 1; player <= GBA_MP_MAX_PLAYERS; ++player) {
		assert_int_equal(_load16(core, IO(GBA_MP_REG_EXT_KEYS + 2 * (player - 1))), 0x80 >> (player - 1));
	}

	// 8- and 32-bit accesses
	assert_int_equal(_load8(core, IO(GBA_MP_REG_MAGIC)), 0x50);
	assert_int_equal(_load8(core, IO(GBA_MP_REG_MAGIC + 1)), 0x4D);
	assert_int_equal(_load32(core, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC | ((2 * 0x41) << 16));

	// Live values: a change is visible on the next read
	GBAMultipadSetKeys(core->board, 2, 0x0201);
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), 0x0201);

	// Writes are ignored
	struct ARMCore* cpu = core->cpu;
	int cycles = 0;
	cpu->memory.store16(cpu, IO(GBA_MP_REG_KEYS), 0x00FF, &cycles);
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), 0x0201);

	// Turning it back off restores upstream behaviour
	_setOption(core, "multiController", false);
	_assertUpstream(core);
	_destroyCore(core);
}

M_TEST_DEFINE(configAtLoad) {
	struct mCore* core = GBACoreCreate();
	assert_non_null(core);
	assert_true(core->init(core));
	mCoreInitConfig(core, NULL);
	struct mCoreConfig config;
	mCoreConfigInit(&config, NULL);
	mCoreConfigSetIntValue(&config, "multiController", 1);
	core->loadConfig(core, &config);
	mCoreConfigDeinit(&config);
	core->reset(core);
	assert_int_equal(_load16(core, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC);
	_destroyCore(core);
}

static void _lockstepNop(struct mLockstepUser* user) {
	UNUSED(user);
}

M_TEST_DEFINE(linkPlayIsUpstream) {
	struct mCore* core1 = _createCore();
	struct mCore* core2 = _createCore();
	_setFakePads(core1);
	_setFakePads(core2);
	_setOption(core1, "multiController", true);
	_setOption(core1, "extendedButtons", true);
	_setOption(core2, "multiController", true);

	struct mLockstepUser user = {
		.sleep = _lockstepNop,
		.wake = _lockstepNop,
		// No preference: the coordinator assigns IDs in attach order
		.requestedId = NULL,
	};
	struct GBASIOLockstepCoordinator coordinator;
	struct GBASIOLockstepDriver driver1;
	struct GBASIOLockstepDriver driver2;
	GBASIOLockstepCoordinatorInit(&coordinator);
	GBASIOLockstepDriverCreate(&driver1, &user);
	GBASIOLockstepDriverCreate(&driver2, &user);

	// A single core on a lockstep is not a link session
	GBASIOLockstepCoordinatorAttach(&coordinator, &driver1);
	core1->setPeripheral(core1, mPERIPH_GBA_LINK_PORT, &driver1.d);
	assert_int_equal(GBASIOLockstepCoordinatorAttached(&coordinator), 1);
	assert_false(GBAMultipadIsLinked(core1->board));
	assert_int_equal(_load16(core1, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC);

	// Second core attached: both read upstream
	GBASIOLockstepCoordinatorAttach(&coordinator, &driver2);
	core2->setPeripheral(core2, mPERIPH_GBA_LINK_PORT, &driver2.d);
	assert_int_equal(GBASIOLockstepCoordinatorAttached(&coordinator), 2);
	assert_true(GBAMultipadIsLinked(core1->board));
	assert_true(GBAMultipadIsLinked(core2->board));
	_assertUpstream(core1);
	_assertUpstream(core2);

	// Link ends: re-enabled automatically
	core2->setPeripheral(core2, mPERIPH_GBA_LINK_PORT, NULL);
	GBASIOLockstepCoordinatorDetach(&coordinator, &driver2);
	assert_int_equal(GBASIOLockstepCoordinatorAttached(&coordinator), 1);
	assert_false(GBAMultipadIsLinked(core1->board));
	assert_int_equal(_load16(core1, IO(GBA_MP_REG_MAGIC)), GBA_MP_MAGIC);
	assert_int_equal(_load16(core1, IO(GBA_MP_REG_EXT_KEYS)), 0x80);

	core1->setPeripheral(core1, mPERIPH_GBA_LINK_PORT, NULL);
	GBASIOLockstepCoordinatorDetach(&coordinator, &driver1);
	GBASIOLockstepCoordinatorDeinit(&coordinator);
	_destroyCore(core1);
	_destroyCore(core2);
}

// A gamepad-style binding type (the Qt frontend uses SDL GameController ids,
// type 'SDLC'). Button and axis ids below follow the SDL GameController enum.
#define PAD_TYPE 0x53444C43
enum {
	PAD_A = 0, PAD_B = 1, PAD_BACK = 4, PAD_START = 6, PAD_LSHOULDER = 9, PAD_RSHOULDER = 10,
	PAD_UP = 11, PAD_DOWN = 12, PAD_LEFT = 13, PAD_RIGHT = 14, PAD_BUTTONS = 26
};
enum { PAD_LEFTX = 0, PAD_LEFTY = 1, PAD_AXES = 6 };

// What the frontend does for one player each frame: map the pad's buttons and
// axes through that player's own map into GBA key bits.
static uint16_t _mapPad(const struct mInputMap* map, const bool* buttons, const int16_t* axes) {
	uint16_t keys = 0;
	int i;
	for (i = 0; i < PAD_BUTTONS; ++i) {
		if (!buttons[i]) {
			continue;
		}
		int key = mInputMapKey(map, PAD_TYPE, i);
		if (key >= 0) {
			keys |= 1 << key;
		}
	}
	for (i = 0; i < PAD_AXES; ++i) {
		int key = mInputMapAxis(map, PAD_TYPE, i, axes[i]);
		if (key >= 0) {
			keys |= 1 << key;
		}
	}
	return keys;
}

M_TEST_DEFINE(secondPadFeedsPlayer2) {
	struct mCore* core = _createCore();
	_setOption(core, "multiController", true);

	// Player 2's own bindings, loaded from its own config section like the
	// frontend does ("gba.p2.input.SDLC"), with a layout that differs from
	// player 1's (A and B swapped) so a mix-up would show
	struct mInputPlatformInfo info = GBAInputInfo;
	info.platformName = "gba.p2";
	struct Configuration config;
	ConfigurationInit(&config);
	const char* section = "gba.p2.input.SDLC";
	ConfigurationSetIntValue(&config, section, "keyA", PAD_A);
	ConfigurationSetIntValue(&config, section, "keyB", PAD_B);
	ConfigurationSetIntValue(&config, section, "keySelect", PAD_BACK);
	ConfigurationSetIntValue(&config, section, "keyStart", PAD_START);
	ConfigurationSetIntValue(&config, section, "keyL", PAD_LSHOULDER);
	ConfigurationSetIntValue(&config, section, "keyR", PAD_RSHOULDER);
	ConfigurationSetIntValue(&config, section, "keyUp", PAD_UP);
	ConfigurationSetIntValue(&config, section, "keyDown", PAD_DOWN);
	ConfigurationSetIntValue(&config, section, "keyLeft", PAD_LEFT);
	ConfigurationSetIntValue(&config, section, "keyRight", PAD_RIGHT);
	ConfigurationSetValue(&config, section, "axisLeftAxis", "-0");
	ConfigurationSetIntValue(&config, section, "axisLeftValue", -12288);
	ConfigurationSetValue(&config, section, "axisRightAxis", "+0");
	ConfigurationSetIntValue(&config, section, "axisRightValue", 12288);
	ConfigurationSetValue(&config, section, "axisUpAxis", "-1");
	ConfigurationSetIntValue(&config, section, "axisUpValue", -12288);
	ConfigurationSetValue(&config, section, "axisDownAxis", "+1");
	ConfigurationSetIntValue(&config, section, "axisDownValue", 12288);
	struct mInputMap map;
	mInputMapInit(&map, &info);
	assert_true(mInputMapLoad(&map, PAD_TYPE, &config));

	static const struct {
		int button;
		uint16_t key;
	} buttons[] = {
		{ PAD_A, 1 << GBA_KEY_A }, { PAD_B, 1 << GBA_KEY_B }, { PAD_BACK, 1 << GBA_KEY_SELECT },
		{ PAD_START, 1 << GBA_KEY_START }, { PAD_RIGHT, 1 << GBA_KEY_RIGHT }, { PAD_LEFT, 1 << GBA_KEY_LEFT },
		{ PAD_UP, 1 << GBA_KEY_UP }, { PAD_DOWN, 1 << GBA_KEY_DOWN }, { PAD_RSHOULDER, 1 << GBA_KEY_R },
		{ PAD_LSHOULDER, 1 << GBA_KEY_L },
	};
	bool pad[PAD_BUTTONS] = { false };
	int16_t axes[PAD_AXES] = { 0 };
	size_t i;
	for (i = 0; i < sizeof(buttons) / sizeof(*buttons); ++i) {
		memset(pad, 0, sizeof(pad));
		pad[buttons[i].button] = true;
		GBAMultipadSetKeys(core->board, 2, _mapPad(&map, pad, axes));
		// The game sees exactly that bit, 1 = pressed, KEYINPUT order
		assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), buttons[i].key);
		// Player 1's keypad and player 3 are untouched
		assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS + 2)), 0);
		assert_int_equal(_load16(core, IO(0x130)), 0x3FF);
	}

	// Stick as d-pad, beyond the dead zone only
	memset(pad, 0, sizeof(pad));
	axes[PAD_LEFTX] = -20000;
	axes[PAD_LEFTY] = 20000;
	GBAMultipadSetKeys(core->board, 2, _mapPad(&map, pad, axes));
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), (1 << GBA_KEY_LEFT) | (1 << GBA_KEY_DOWN));
	axes[PAD_LEFTX] = 8000;
	axes[PAD_LEFTY] = -8000;
	GBAMultipadSetKeys(core->board, 2, _mapPad(&map, pad, axes));
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), 0);

	// Released pad reads 0
	axes[PAD_LEFTX] = 0;
	axes[PAD_LEFTY] = 0;
	GBAMultipadSetKeys(core->board, 2, _mapPad(&map, pad, axes));
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), 0);

	// Survives a reset (host state, not emulated state)
	GBAMultipadSetKeys(core->board, 2, 1 << GBA_KEY_A);
	core->reset(core);
	assert_int_equal(_load16(core, IO(GBA_MP_REG_KEYS)), 1 << GBA_KEY_A);

	mInputMapDeinit(&map);
	ConfigurationDeinit(&config);
	_destroyCore(core);
}

M_TEST_SUITE_DEFINE(GBAMultipad,
	cmocka_unit_test(secondPadFeedsPlayer2),
	cmocka_unit_test(disabledIsUpstream),
	cmocka_unit_test(enabledRegisters),
	cmocka_unit_test(configAtLoad),
	cmocka_unit_test(linkPlayIsUpstream))
