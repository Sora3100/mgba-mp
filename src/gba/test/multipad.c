/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
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

M_TEST_SUITE_DEFINE(GBAMultipad,
	cmocka_unit_test(disabledIsUpstream),
	cmocka_unit_test(enabledRegisters),
	cmocka_unit_test(configAtLoad),
	cmocka_unit_test(linkPlayIsUpstream))
