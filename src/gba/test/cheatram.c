/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/cheats.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>

#define BLOCK 0x01000000
// Never mapped in these tests: reads take the upstream open-bus path
#define UNMAPPED 0x01800000

static int cheatRamSetup(void** state) {
	struct mCore* core = GBACoreCreate();
	assert_non_null(core);
	assert_true(core->init(core));
	mCoreInitConfig(core, NULL);
	core->cheatDevice(core);
	core->reset(core);
	*state = core;
	return 0;
}

static int cheatRamTeardown(void** state) {
	struct mCore* core = *state;
	if (!core) {
		return 0;
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return 0;
}

static struct mCheatSet* _allocSet(struct mCore* core, const char* line) {
	struct mCheatDevice* device = core->cheatDevice(core);
	struct mCheatSet* set = device->createSet(device, "alloc");
	assert_true(mCheatAddLine(set, line, GBA_CHEAT_AUTODETECT));
	mCheatAddSet(device, set);
	return set;
}

static void _removeSet(struct mCore* core, struct mCheatSet* set) {
	struct mCheatDevice* device = core->cheatDevice(core);
	mCheatRemoveSet(device, set);
	mCheatSetDeinit(set);
}

static void _assertOpenBus(struct ARMCore* cpu, uint32_t address) {
	// Same open-bus value as an address that was never mapped
	assert_int_equal(cpu->memory.load32(cpu, address, NULL), cpu->memory.load32(cpu, UNMAPPED, NULL));
	assert_int_equal(cpu->memory.load16(cpu, address, NULL), cpu->memory.load16(cpu, UNMAPPED, NULL));
	assert_int_equal(cpu->memory.load8(cpu, address + 1, NULL), cpu->memory.load8(cpu, UNMAPPED + 1, NULL));
}

M_TEST_DEFINE(rejectsBadBlocks) {
	struct mCore* core = *state;
	struct mCheatDevice* device = core->cheatDevice(core);
	struct mCheatSet* set = device->createSet(device, NULL);
	assert_false(set->addLine(set, "alloc 02000000 100", GBA_CHEAT_AUTODETECT)); // not region 1
	assert_false(set->addLine(set, "alloc 01000010 100", GBA_CHEAT_AUTODETECT)); // misaligned
	assert_false(set->addLine(set, "alloc 01000000 200000", GBA_CHEAT_AUTODETECT)); // > 1 MiB
	assert_false(set->addLine(set, "alloc 01000000 0", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "alloc 01000000", GBA_CHEAT_AUTODETECT));
	assert_true(set->addLine(set, "ALLOC 0x01100000 0x30", GBA_CHEAT_AUTODETECT)); // rounded to 0x40
	assert_int_equal(((struct GBACheatSet*) set)->allocs[0].size, 0x40);
	mCheatSetDeinit(set);
}

M_TEST_DEFINE(loadStoreOnOff) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;

	// Off: upstream behaviour, writes are dropped
	cpu->memory.store32(cpu, BLOCK, 0x12345678, NULL);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	_assertOpenBus(cpu, BLOCK);

	struct mCheatSet* set = _allocSet(core, "alloc 01000000 400");
	assert_non_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK + 0x3FF));
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK + 0x400));

	// On: zero-filled RAM with 8/16/32-bit access
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0);
	cpu->memory.store32(cpu, BLOCK, 0x12345678, NULL);
	cpu->memory.store16(cpu, BLOCK + 4, 0xBEEF, NULL);
	cpu->memory.store8(cpu, BLOCK + 7, 0xA5, NULL);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0x12345678);
	assert_int_equal(cpu->memory.load16(cpu, BLOCK + 2, NULL), 0x1234);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 1, NULL), 0x56);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 4, NULL), 0xA500BEEF);
	assert_int_equal(core->rawRead32(core, BLOCK, -1), 0x12345678);
	core->rawWrite16(core, BLOCK + 8, -1, 0x4321);
	assert_int_equal(cpu->memory.load16(cpu, BLOCK + 8, NULL), 0x4321);

	// Load/store multiple
	cpu->gprs[0] = 0x11111111;
	cpu->gprs[1] = 0x22222222;
	cpu->memory.storeMultiple(cpu, BLOCK + 0x20, 0x3, LSM_IA, NULL);
	cpu->gprs[0] = 0;
	cpu->gprs[1] = 0;
	cpu->memory.loadMultiple(cpu, BLOCK + 0x20, 0x3, LSM_IA, NULL);
	assert_int_equal(cpu->gprs[0], 0x11111111);
	assert_int_equal(cpu->gprs[1], 0x22222222);

	// Disabling the cheat unmaps the block
	set->enabled = false;
	mCheatRefresh(core->cheatDevice(core), set);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	_assertOpenBus(cpu, BLOCK);

	// Enabling maps fresh, zeroed RAM
	set->enabled = true;
	mCheatRefresh(core->cheatDevice(core), set);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0);

	_removeSet(core, set);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	_assertOpenBus(cpu, BLOCK);
}

M_TEST_DEFINE(execute) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatSet* set = _allocSet(core, "alloc 01000000 200");

	// ARM: mov r0, #0x42; str r0, [r1]; b .
	cpu->memory.store32(cpu, BLOCK + 0x00, 0xE3A00042, NULL);
	cpu->memory.store32(cpu, BLOCK + 0x04, 0xE5810000, NULL);
	cpu->memory.store32(cpu, BLOCK + 0x08, 0xEAFFFFFE, NULL);
	// Thumb: movs r0, #0x24; str r0, [r1]; b .
	cpu->memory.store16(cpu, BLOCK + 0x100, 0x2024, NULL);
	cpu->memory.store16(cpu, BLOCK + 0x102, 0x6008, NULL);
	cpu->memory.store16(cpu, BLOCK + 0x104, 0xE7FE, NULL);

	cpu->gprs[0] = 0;
	cpu->gprs[1] = 0x03000100;
	cpu->cpsr.t = 0;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->gprs[ARM_PC] = BLOCK;
	ARMWritePC(cpu);
	assert_int_equal(gba->memory.activeRegion, GBA_REGION_CHEAT_RAM);
	ARMRun(cpu);
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 0x42);
	assert_int_equal(cpu->memory.load32(cpu, 0x03000100, NULL), 0x42);
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[ARM_PC], BLOCK + 0x08 + WORD_SIZE_ARM);

	cpu->cpsr.t = 1;
	_ARMSetMode(cpu, MODE_THUMB);
	cpu->gprs[ARM_PC] = BLOCK + 0x100;
	ThumbWritePC(cpu);
	ARMRun(cpu);
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 0x24);
	assert_int_equal(cpu->memory.load32(cpu, 0x03000100, NULL), 0x24);

	// Off: the same jump is an invalid region again
	cpu->cpsr.t = 0;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->gprs[ARM_PC] = 0x03000000;
	ARMWritePC(cpu);
	_removeSet(core, set);
	gba->hardCrash = false;
	cpu->memory.setActiveRegion(cpu, BLOCK);
	assert_int_equal(gba->memory.activeRegion, -1);
}

M_TEST_DEFINE(dma) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatSet* set = _allocSet(core, "alloc 01000000 100");
	int i;
	for (i = 0; i < 4; ++i) {
		cpu->memory.store32(cpu, BLOCK + i * 4, 0xC0DE0000 | i, NULL);
	}
	// Keep the CPU busy in IWRAM: b .
	cpu->memory.store32(cpu, 0x03000000, 0xEAFFFFFE, NULL);
	cpu->cpsr.t = 0;
	_ARMSetMode(cpu, MODE_ARM);
	cpu->gprs[ARM_PC] = 0x03000000;
	ARMWritePC(cpu);

	// DMA3: cheat RAM -> IWRAM, 4 words, then IWRAM -> cheat RAM
	cpu->memory.store32(cpu, 0x040000D4, BLOCK, NULL);
	cpu->memory.store32(cpu, 0x040000D8, 0x03000200, NULL);
	cpu->memory.store32(cpu, 0x040000DC, 0x84000004, NULL);
	for (i = 0; i < 64; ++i) {
		ARMRun(cpu);
	}
	for (i = 0; i < 4; ++i) {
		assert_int_equal(cpu->memory.load32(cpu, 0x03000200 + i * 4, NULL), 0xC0DE0000 | i);
	}
	cpu->memory.store32(cpu, 0x040000D4, 0x03000200, NULL);
	cpu->memory.store32(cpu, 0x040000D8, BLOCK + 0x80, NULL);
	cpu->memory.store32(cpu, 0x040000DC, 0x84000004, NULL);
	for (i = 0; i < 64; ++i) {
		ARMRun(cpu);
	}
	for (i = 0; i < 4; ++i) {
		assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x80 + i * 4, NULL), 0xC0DE0000 | i);
	}
	_removeSet(core, set);
}

M_TEST_DEFINE(cheatWritesAndDirective) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatDevice* device = core->cheatDevice(core);
	static const char file[] =
		"!alloc 0x01100000 0x20\n"
		"# Uses the block\n"
		"01100004:DEADBEEF\n";
	struct VFile* vf = VFileFromConstMemory(file, sizeof(file) - 1);
	assert_true(mCheatParseFile(device, vf));
	vf->close(vf);
	assert_int_equal(mCheatSetsSize(&device->cheats), 1);
	struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, 0);
	assert_string_equal(*StringListGetPointer(&set->lines, 0), "alloc 0x01100000 0x20");

	mCheatRefresh(device, set);
	assert_int_equal(cpu->memory.load32(cpu, 0x01100004, NULL), 0xDEADBEEF);
	assert_int_equal(cpu->memory.load32(cpu, 0x01100000, NULL), 0);

	set->enabled = false;
	mCheatRefresh(device, set);
	_assertOpenBus(cpu, 0x01100004);
	_removeSet(core, set);
}

M_TEST_DEFINE(savestate) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatSet* set = _allocSet(core, "alloc 01000000 40");
	cpu->memory.store32(cpu, BLOCK + 0x10, 0xCAFEBABE, NULL);

	struct mStateExtdata extdata;
	mStateExtdataInit(&extdata);
	assert_true(core->saveExtraState(core, &extdata));

	cpu->memory.store32(cpu, BLOCK + 0x10, 0x01020304, NULL);
	assert_true(core->loadExtraState(core, &extdata));
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x10, NULL), 0xCAFEBABE);

	// Loaded while the cheat is off: the contents wait for the cheat
	_removeSet(core, set);
	assert_true(core->loadExtraState(core, &extdata));
	_assertOpenBus(cpu, BLOCK + 0x10);
	set = _allocSet(core, "alloc 01000000 40");
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x10, NULL), 0xCAFEBABE);

	// A state without the block zeroes it
	struct mStateExtdata empty;
	mStateExtdataInit(&empty);
	assert_true(core->loadExtraState(core, &empty));
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x10, NULL), 0);
	mStateExtdataDeinit(&empty);

	mStateExtdataDeinit(&extdata);
	_removeSet(core, set);
}

M_TEST_SUITE_DEFINE(GBACheatRAM,
	cmocka_unit_test_setup_teardown(rejectsBadBlocks, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(loadStoreOnOff, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(execute, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dma, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(cheatWritesAndDirective, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(savestate, cheatRamSetup, cheatRamTeardown))
