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

// A synthetic cartridge: every word is "mov r0, #1"
#define ROM_SIZE 0x20000
#define ROM_FILL 0xE3A00001

static int romSetup(void** state) {
	struct mCore* core = GBACoreCreate();
	assert_non_null(core);
	assert_true(core->init(core));
	mCoreInitConfig(core, NULL);
	uint8_t* rom = malloc(ROM_SIZE);
	size_t i;
	for (i = 0; i < ROM_SIZE; i += 4) {
		STORE_32LE(ROM_FILL, i, rom);
	}
	struct VFile* vf = VFileMemChunk(rom, ROM_SIZE);
	free(rom);
	assert_true(core->loadROM(core, vf));
	core->cheatDevice(core);
	core->reset(core);
	*state = core;
	return 0;
}

static void _parse(struct mCheatDevice* device, const char* text) {
	struct VFile* vf = VFileFromConstMemory(text, strlen(text));
	assert_true(mCheatParseFile(device, vf));
	vf->close(vf);
}

static char* _save(struct mCheatDevice* device) {
	struct VFile* vf = VFileMemChunk(NULL, 0);
	assert_true(mCheatSaveFile(device, vf));
	size_t size = vf->seek(vf, 0, SEEK_END);
	char* text = calloc(1, size + 1);
	vf->seek(vf, 0, SEEK_SET);
	assert_int_equal(vf->read(vf, text, size), size);
	vf->close(vf);
	return text;
}

// What the frontend does at the end of every frame
static void _frame(struct mCheatDevice* device) {
	size_t i;
	for (i = 0; i < mCheatSetsSize(&device->cheats); ++i) {
		mCheatRefresh(device, *mCheatSetsGetPointer(&device->cheats, i));
	}
}

static void _setPC(struct ARMCore* cpu, uint32_t pc, bool thumb) {
	cpu->cpsr.t = thumb;
	_ARMSetMode(cpu, thumb ? MODE_THUMB : MODE_ARM);
	cpu->gprs[ARM_PC] = pc;
	if (thumb) {
		ThumbWritePC(cpu);
	} else {
		ARMWritePC(cpu);
	}
}

static const char _hackFile[] =
	"!GSAv1 raw\n"
	"# Raw ROM patch\n"
	"6000F7E2 0000D10C\n"
	"!reset\n"
	"# Hack\n"
	"alloc 01000000 10000\n"
	"allocdata 01000000 4200A0E3 000081E5 1EFF2FE1\n"
	"allocdata 0x01000100 2420 0860 7047\n"
	"rompatch 08000100 000000EA\n"
	"rompatch 0x08000200 BE BA FE CA\n"
	"01000010:77\n"
	"!disabled\n"
	"# Plain RAM\n"
	"alloc 01100000 100\n";

M_TEST_DEFINE(dataParseErrors) {
	struct mCore* core = *state;
	struct mCheatDevice* device = core->cheatDevice(core);
	struct mCheatSet* set = device->createSet(device, NULL);
	// Data before the alloc line has no block to go into
	assert_false(set->addLine(set, "allocdata 01000000 00", GBA_CHEAT_AUTODETECT));
	assert_true(set->addLine(set, "alloc 01000000 100", GBA_CHEAT_AUTODETECT));
	assert_true(set->addLine(set, "allocdata 010000FF 01", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "allocdata 010000FF 0102", GBA_CHEAT_AUTODETECT)); // runs past the end
	assert_false(set->addLine(set, "allocdata 01000100 01", GBA_CHEAT_AUTODETECT)); // after the block
	assert_false(set->addLine(set, "allocdata 00FFFFFF 01", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "allocdata 01000000 123", GBA_CHEAT_AUTODETECT)); // odd digit count
	assert_false(set->addLine(set, "allocdata 01000000 1 2", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "allocdata 01000000 GG", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "allocdata 01000000", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "allocdata 01000000 ", GBA_CHEAT_AUTODETECT));
	assert_true(set->addLine(set, "ALLOCDATA 0x01000000 a0B1 c2", GBA_CHEAT_AUTODETECT));

	assert_true(set->addLine(set, "rompatch 08000000 00", GBA_CHEAT_AUTODETECT));
	assert_true(set->addLine(set, "rompatch 0DFFFFFF 00", GBA_CHEAT_AUTODETECT)); // last byte of a mirror
	assert_false(set->addLine(set, "rompatch 0DFFFFFF 0000", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "rompatch 07FFFFFF 00", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "rompatch 0E000000 00", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "rompatch 08000000", GBA_CHEAT_AUTODETECT));
	assert_false(set->addLine(set, "rompatch08000000 00", GBA_CHEAT_AUTODETECT));

	// A line longer than the cheat file reader takes is refused up front
	char line[mCHEAT_MAX_LINE_LENGTH + 8];
	strcpy(line, "rompatch 08000000 ");
	while (strlen(line) < mCHEAT_MAX_LINE_LENGTH - 3) {
		strcat(line, "00");
	}
	strcat(line, " 00");
	assert_int_equal(strlen(line), mCHEAT_MAX_LINE_LENGTH);
	assert_true(set->addLine(set, line, GBA_CHEAT_AUTODETECT));
	strcat(line, "00");
	assert_false(set->addLine(set, line, GBA_CHEAT_AUTODETECT));

	struct GBACheatSet* gbaset = (struct GBACheatSet*) set;
	assert_int_equal(GBACheatDataListSize(&gbaset->allocData), 2);
	const struct GBACheatData* data = GBACheatDataListGetConstPointer(&gbaset->allocData, 1);
	assert_int_equal(data->size, 3);
	assert_int_equal(data->data[0], 0xA0);
	assert_int_equal(data->data[2], 0xC2);
	assert_int_equal(GBACheatDataListSize(&gbaset->romData), 3);
	assert_int_equal(GBACheatDataListGetConstPointer(&gbaset->romData, 1)->address, 0x09FFFFFF);
	mCheatSetDeinit(set);
}

M_TEST_DEFINE(dataRoundTrip) {
	struct mCore* core = *state;
	struct mCheatDevice* device = core->cheatDevice(core);
	_parse(device, _hackFile);
	assert_int_equal(mCheatSetsSize(&device->cheats), 3);
	char* saved = _save(device);
	static const char expected[] =
		"!GSAv1 raw\n"
		"# Raw ROM patch\n"
		"6000F7E2 0000D10C\n"
		"!GSAv1 raw\n"
		"# Hack\n"
		"alloc 01000000 10000\n"
		"allocdata 01000000 4200A0E3 000081E5 1EFF2FE1\n"
		"allocdata 0x01000100 2420 0860 7047\n"
		"rompatch 08000100 000000EA\n"
		"rompatch 0x08000200 BE BA FE CA\n"
		"01000010:77\n"
		"!disabled\n"
		"!GSAv1 raw\n"
		"# Plain RAM\n"
		"alloc 01100000 100\n";
	assert_string_equal(saved, expected);

	// Loading the saved file again gives the same file
	mCheatDeviceClear(device);
	_parse(device, saved);
	char* again = _save(device);
	assert_string_equal(again, expected);
	free(saved);
	free(again);

	// Adding a code to the set afterwards (like the cheats window) keeps it
	struct mCheatSet* hack = *mCheatSetsGetPointer(&device->cheats, 1);
	assert_true(mCheatAddLine(hack, "01000020:01", GBA_CHEAT_AUTODETECT));
	assert_true(mCheatAddLine(hack, "rompatch 08000300 11", GBA_CHEAT_AUTODETECT));
	saved = _save(device);
	assert_non_null(strstr(saved, "rompatch 0x08000200 BE BA FE CA\n01000010:77\n01000020:01\nrompatch 08000300 11\n"));
	free(saved);
}

M_TEST_DEFINE(dataLongLineSkipped) {
	struct mCore* core = *state;
	struct mCheatDevice* device = core->cheatDevice(core);
	// A line the reader cannot hold is dropped whole; its tail must not
	// turn into a separate code
	size_t length = 600;
	char* text = malloc(length + 200);
	strcpy(text, "# Set\nalloc 01000000 1000\nallocdata 01000000 ");
	while (strlen(text) < length) {
		strcat(text, "12345678");
	}
	strcat(text, "\nallocdata 01000000 AA\n");
	_parse(device, text);
	free(text);
	assert_int_equal(mCheatSetsSize(&device->cheats), 1);
	struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, 0);
	assert_int_equal(StringListSize(&set->lines), 2);
	assert_int_equal(mCheatListSize(&set->list), 0);
	assert_int_equal(mCheatPatchListSize(&set->romPatches), 0);
	assert_string_equal(*StringListGetPointer(&set->lines, 1), "allocdata 01000000 AA");

	// A line of exactly the maximum length still loads
	mCheatDeviceClear(device);
	text = malloc(mCHEAT_MAX_LINE_LENGTH + 64);
	strcpy(text, "# Set\nalloc 01000000 1000\nallocdata 01000000 ");
	while (strlen(text) < 26 + mCHEAT_MAX_LINE_LENGTH) {
		strcat(text, "0");
	}
	strcat(text, "\r\n");
	_parse(device, text);
	free(text);
	set = *mCheatSetsGetPointer(&device->cheats, 0);
	assert_int_equal(StringListSize(&set->lines), 2);
	assert_int_equal(strlen(*StringListGetPointer(&set->lines, 1)), mCHEAT_MAX_LINE_LENGTH);
}

M_TEST_DEFINE(dataLoadAndExecute) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatDevice* device = core->cheatDevice(core);
	_parse(device, _hackFile);
	_frame(device);

	// Initial contents, the rest zero, other codes still write into it
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0xE3A00042);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 8, NULL), 0xE12FFF1E);
	assert_int_equal(cpu->memory.load16(cpu, BLOCK + 0x104, NULL), 0x4770);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 0x10, NULL), 0x77);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x200, NULL), 0);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0xFFFC, NULL), 0);
	// The disabled set's block is not there
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, 0x01100000));

	// ARM: mov r0, #0x42; str r0, [r1]; bx lr
	cpu->gprs[0] = 0;
	cpu->gprs[1] = 0x03000100;
	cpu->gprs[ARM_LR] = 0x03000000;
	cpu->memory.store32(cpu, 0x03000000, 0xEAFFFFFE, NULL);
	_setPC(cpu, BLOCK, false);
	ARMRun(cpu);
	ARMRun(cpu);
	ARMRun(cpu);
	assert_int_equal(cpu->memory.load32(cpu, 0x03000100, NULL), 0x42);
	assert_int_equal(cpu->gprs[ARM_PC], 0x03000000 + WORD_SIZE_ARM);

	// Thumb: movs r0, #0x24; str r0, [r1]; bx lr
	_setPC(cpu, BLOCK + 0x100, true);
	ARMRun(cpu);
	ARMRun(cpu);
	ARMRun(cpu);
	assert_int_equal(cpu->memory.load32(cpu, 0x03000100, NULL), 0x24);
	assert_int_equal(cpu->gprs[ARM_PC], 0x03000000 + WORD_SIZE_ARM);
}

M_TEST_DEFINE(dataEnableDisableReset) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatDevice* device = core->cheatDevice(core);
	_parse(device, _hackFile);
	_frame(device);
	struct mCheatSet* hack = *mCheatSetsGetPointer(&device->cheats, 1);

	// The game changes the block; a reset brings back the initial contents
	cpu->memory.store32(cpu, BLOCK, 0x11111111, NULL);
	cpu->memory.store32(cpu, BLOCK + 0x400, 0x22222222, NULL);
	core->reset(core);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0xE3A00042);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x400, NULL), 0);

	// Off: unmapped. On again: fresh block with the data
	hack->enabled = false;
	_frame(device);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	_assertOpenBus(cpu, BLOCK);
	hack->enabled = true;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 4, NULL), 0xE5810000);

	// Removing the set unmaps it
	_setPC(cpu, 0x03000000, false);
	_removeSet(core, hack);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	_assertOpenBus(cpu, BLOCK);
}

M_TEST_DEFINE(dataSharedBlock) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatDevice* device = core->cheatDevice(core);
	_parse(device,
		"# A\n"
		"alloc 01000000 100\n"
		"allocdata 01000000 01\n"
		"!disabled\n"
		"# B\n"
		"alloc 01000000 100\n"
		"allocdata 01000004 02\n");
	_frame(device);
	struct mCheatSet* b = *mCheatSetsGetPointer(&device->cheats, 1);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK, NULL), 1);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 4, NULL), 0);
	cpu->memory.store8(cpu, BLOCK + 8, 9, NULL);

	// Enabling a second set on a mapped block writes its lines and keeps
	// the rest of the block
	b->enabled = true;
	_frame(device);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 4, NULL), 2);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 8, NULL), 9);
	core->reset(core);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK, NULL), 1);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 4, NULL), 2);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 8, NULL), 0);

	// A line added to an enabled set takes effect right away
	assert_true(mCheatAddLine(b, "allocdata 01000010 33", GBA_CHEAT_AUTODETECT));
	b->refresh(b, device);
	assert_int_equal(cpu->memory.load8(cpu, BLOCK + 0x10, NULL), 0x33);
}

M_TEST_DEFINE(dataSavestate) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatDevice* device = core->cheatDevice(core);
	_parse(device, _hackFile);
	_frame(device);
	// A variable the hack keeps in its block
	cpu->memory.store32(cpu, BLOCK + 0x800, 0x5A5A5A5A, NULL);

	struct mStateExtdata extdata;
	mStateExtdataInit(&extdata);
	assert_true(core->saveExtraState(core, &extdata));
	cpu->memory.store32(cpu, BLOCK + 0x800, 0, NULL);
	cpu->memory.store32(cpu, BLOCK, 0, NULL);
	assert_true(core->loadExtraState(core, &extdata));
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x800, NULL), 0x5A5A5A5A);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0xE3A00042);

	// A savestate with cheats reloads the cheat file after the memory: the
	// block keeps the state's contents instead of being re-initialised
	cpu->memory.store32(cpu, BLOCK + 0x800, 0, NULL);
	assert_true(core->loadExtraState(core, &extdata));
	mCheatDeviceClear(device);
	_parse(device, _hackFile);
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x800, NULL), 0x5A5A5A5A);
	assert_int_equal(cpu->memory.load16(cpu, 0x08000200, NULL), 0xBABE);

	// A state from before the hack: the block starts from its initial data
	struct mStateExtdata empty;
	mStateExtdataInit(&empty);
	assert_true(core->loadExtraState(core, &empty));
	assert_int_equal(cpu->memory.load32(cpu, BLOCK + 0x800, NULL), 0);
	assert_int_equal(cpu->memory.load32(cpu, BLOCK, NULL), 0xE3A00042);
	mStateExtdataDeinit(&empty);
	mStateExtdataDeinit(&extdata);
}

M_TEST_DEFINE(romPatchApplyRevert) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatDevice* device = core->cheatDevice(core);
	uint8_t* pristine = malloc(ROM_SIZE);
	memcpy(pristine, gba->memory.rom, ROM_SIZE);

	_parse(device, _hackFile);
	_frame(device);
	// Bus reads of every width and every ROM mirror see the patches
	assert_int_equal(cpu->memory.load32(cpu, 0x08000100, NULL), 0xEA000000);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000200, NULL), 0xCAFEBABE);
	assert_int_equal(cpu->memory.load16(cpu, 0x0A000202, NULL), 0xCAFE);
	assert_int_equal(cpu->memory.load8(cpu, 0x0C000203, NULL), 0xCA);
	assert_int_equal(core->rawRead32(core, 0x08000200, -1), 0xCAFEBABE);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000204, NULL), ROM_FILL);
	// The GSA raw patch in the same file works too
	assert_int_equal(cpu->memory.load16(cpu, 0x0801EFC4, NULL), 0xD10C);

	// Disabling puts back exactly the original bytes; the GSA patch stays
	struct mCheatSet* hack = *mCheatSetsGetPointer(&device->cheats, 1);
	hack->enabled = false;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000100, NULL), ROM_FILL);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000200, NULL), ROM_FILL);
	assert_int_equal(cpu->memory.load16(cpu, 0x0801EFC4, NULL), 0xD10C);
	struct mCheatSet* raw = *mCheatSetsGetPointer(&device->cheats, 0);
	raw->enabled = false;
	_frame(device);
	assert_memory_equal(gba->memory.rom, pristine, ROM_SIZE);

	hack->enabled = true;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000200, NULL), 0xCAFEBABE);
	_removeSet(core, hack);
	assert_memory_equal(gba->memory.rom, pristine, ROM_SIZE);
	free(pristine);
}

M_TEST_DEFINE(romPatchOverlap) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatDevice* device = core->cheatDevice(core);
	uint8_t* pristine = malloc(ROM_SIZE);
	memcpy(pristine, gba->memory.rom, ROM_SIZE);
	_parse(device,
		"# A\n"
		"rompatch 08000400 11111111 11111111\n"
		"rompatch 08000402 2222\n"
		"# B\n"
		"rompatch 08000404 33333333\n"
		"# C\n"
		"rompatch 08000800 44\n");
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000400, NULL), 0x22221111);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000404, NULL), 0x33333333);
	assert_int_equal(cpu->memory.load8(cpu, 0x08000800, NULL), 0x44);

	// Turning off the first set leaves the later sets' bytes in place
	struct mCheatSet* a = *mCheatSetsGetPointer(&device->cheats, 0);
	struct mCheatSet* b = *mCheatSetsGetPointer(&device->cheats, 1);
	a->enabled = false;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000400, NULL), ROM_FILL);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000404, NULL), 0x33333333);
	assert_int_equal(cpu->memory.load8(cpu, 0x08000800, NULL), 0x44);
	a->enabled = true;
	b->enabled = false;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000404, NULL), 0x11111111);
	b->enabled = true;
	_frame(device);

	// Clearing the device (savestate with cheats, reloading the file)
	// undoes everything even without remove callbacks
	mCheatDeviceClear(device);
	assert_memory_equal(gba->memory.rom, pristine, ROM_SIZE);
	free(pristine);
}

M_TEST_DEFINE(romPatchFetch) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct mCheatDevice* device = core->cheatDevice(core);

	// The CPU already fetched "mov r0, #1" at 0x08000100 when the patch
	// arrives: it must execute the patched "mov r0, #0x42"
	_setPC(cpu, 0x08000100, false);
	struct mCheatSet* set = device->createSet(device, "arm");
	assert_true(mCheatAddLine(set, "rompatch 08000100 4200A0E3", GBA_CHEAT_AUTODETECT));
	mCheatAddSet(device, set);
	cpu->gprs[0] = 0;
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 0x42);
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 1);
	_removeSet(core, set);

	// Thumb in a ROM mirror: "movs r0, #0x77" over "movs r1, r0" (0x0001)
	set = device->createSet(device, "thumb");
	assert_true(mCheatAddLine(set, "rompatch 08000300 7720", GBA_CHEAT_AUTODETECT));
	set->enabled = false;
	mCheatAddSet(device, set);
	_setPC(cpu, 0x0A000300, true);
	set->enabled = true;
	_frame(device);
	cpu->gprs[0] = 5;
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 0x77);

	// Reverting under an already fetched patched opcode
	_setPC(cpu, 0x0A000300, true);
	set->enabled = false;
	_frame(device);
	cpu->gprs[0] = 5;
	cpu->gprs[1] = 0;
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 5);
	assert_int_equal(cpu->gprs[1], 5);
	_removeSet(core, set);
}

M_TEST_DEFINE(romPatchIntoBlock) {
	struct mCore* core = *state;
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCheatDevice* device = core->cheatDevice(core);
	// A ROM hook jumping into code that comes with the cheat:
	// 08000000: ldr pc, [pc, #-4] -> 0x01000000; block: mov r0, #0x42; b .
	_parse(device,
		"# Hook\n"
		"alloc 01000000 100\n"
		"allocdata 01000000 4200A0E3 FEFFFFEA\n"
		"rompatch 08000000 04F01FE5 00000001\n");
	_frame(device);
	_setPC(cpu, 0x08000000, false);
	cpu->gprs[0] = 0;
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[ARM_PC], BLOCK + WORD_SIZE_ARM);
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 0x42);

	// Disabling while the CPU runs in the block: the ROM hook goes away at
	// once, the block stays until the CPU has left it
	struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, 0);
	set->enabled = false;
	_frame(device);
	assert_int_equal(cpu->memory.load32(cpu, 0x08000000, NULL), ROM_FILL);
	assert_non_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	ARMRun(cpu);
	_setPC(cpu, 0x08000000, false);
	_frame(device);
	assert_null(GBAMemoryCheatRAMLookup(&gba->memory, BLOCK));
	ARMRun(cpu);
	assert_int_equal(cpu->gprs[0], 1);
}

M_TEST_DEFINE(romPatchNoRom) {
	struct mCore* core = *state;
	struct mCheatDevice* device = core->cheatDevice(core);
	// No cartridge: the set loads, the patch is simply not written
	struct mCheatSet* set = device->createSet(device, "patch");
	assert_true(mCheatAddLine(set, "rompatch 08000000 01", GBA_CHEAT_AUTODETECT));
	mCheatAddSet(device, set);
	_frame(device);
	assert_false(GBACheatDataListGetConstPointer(&((struct GBACheatSet*) set)->romData, 0)->written);
	_removeSet(core, set);
}

M_TEST_SUITE_DEFINE(GBACheatRAM,
	cmocka_unit_test_setup_teardown(dataParseErrors, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataRoundTrip, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataLongLineSkipped, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataLoadAndExecute, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataEnableDisableReset, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataSharedBlock, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dataSavestate, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(romPatchApplyRevert, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(romPatchOverlap, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(romPatchFetch, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(romPatchIntoBlock, romSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(romPatchNoRom, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(rejectsBadBlocks, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(loadStoreOnOff, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(execute, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(dma, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(cheatWritesAndDirective, cheatRamSetup, cheatRamTeardown),
	cmocka_unit_test_setup_teardown(savestate, cheatRamSetup, cheatRamTeardown))
