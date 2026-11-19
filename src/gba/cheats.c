/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/cheats.h>

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/string.h>
#include "gba/cheats/gameshark.h"
#include "gba/cheats/parv3.h"

#define MAX_LINE_LENGTH 128

DEFINE_VECTOR(GBACheatDataList, struct GBACheatData);

// Order in which "rompatch" lines were written, so they can be undone newest
// first and overlapping patches restore the exact original bytes
static uint32_t _romPatchSeq = 0;

static void _sync(struct mCheatDevice* device, struct GBACheatSet* removed, bool full);

static bool _active(const struct GBACheatSet* set) {
	return set->attached && set->d.enabled;
}

static void _addBreakpoint(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p || !cheats->hook) {
		return;
	}
	++cheats->hook->reentries;
	if (cheats->hook->reentries > 1) {
		return;
	}
	GBASetBreakpoint(device->p->board, &device->d, cheats->hook->address, cheats->hook->mode, &cheats->hook->patchedOpcode);
}

static void _removeBreakpoint(struct mCheatDevice* device, struct GBACheatSet* cheats) {
	if (!device->p || !cheats->hook) {
		return;
	}
	--cheats->hook->reentries;
	if (cheats->hook->reentries > 0) {
		return;
	}
	GBAClearBreakpoint(device->p->board, cheats->hook->address, cheats->hook->mode, cheats->hook->patchedOpcode);
}

static void GBACheatSetDeinit(struct mCheatSet* set);
static void GBACheatAddSet(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatRemoveSet(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatRefresh(struct mCheatSet* cheats, struct mCheatDevice* device);
static void GBACheatSetCopyProperties(struct mCheatSet* set, struct mCheatSet* oldSet);
static void GBACheatParseDirectives(struct mCheatSet*, const struct StringList* directives);
static void GBACheatDumpDirectives(struct mCheatSet*, struct StringList* directives);
static bool GBACheatAddLine(struct mCheatSet*, const char* line, int type);

static struct mCheatSet* GBACheatSetCreate(struct mCheatDevice* device, const char* name) {
	UNUSED(device);
	struct GBACheatSet* set = malloc(sizeof(*set));
	mCheatSetInit(&set->d, name);
	set->incompleteCheat = -1;
	set->incompletePatch = 0;
	set->currentBlock = -1;
	set->gsaVersion = 0;
	set->cbRngState = 0;
	set->cbMaster = 0;
	set->remainingAddresses = 0;
	set->hook = NULL;
	set->nAllocs = 0;
	GBACheatDataListInit(&set->allocData, 0);
	GBACheatDataListInit(&set->romData, 0);
	set->romTarget = NULL;
	set->romCrc = 0;
	set->attached = false;

	set->d.deinit = GBACheatSetDeinit;
	set->d.add = GBACheatAddSet;
	set->d.remove = GBACheatRemoveSet;

	set->d.addLine = GBACheatAddLine;
	set->d.copyProperties = GBACheatSetCopyProperties;

	set->d.parseDirectives = GBACheatParseDirectives;
	set->d.dumpDirectives = GBACheatDumpDirectives;

	set->d.refresh = GBACheatRefresh;

	return &set->d;
}

struct mCheatDevice* GBACheatDeviceCreate(void) {
	struct mCheatDevice* device = malloc(sizeof(*device));
	mCheatDeviceCreate(device);
	device->createSet = GBACheatSetCreate;
	return device;
}

static bool _romStillThere(struct GBACheatSet* set) {
	struct GBA* gba = set->romTarget;
	// Never "restore" bytes into a different ROM loaded in the meantime
	return gba && gba->memory.rom && gba->romCrc32 == set->romCrc;
}

static void _revertROMPatch(struct GBACheatSet* set, struct GBACheatData* patch) {
	if (patch->written && _romStillThere(set)) {
		GBAMemoryPatchROM(set->romTarget, patch->address, &patch->data[patch->size], patch->size, NULL);
	}
	patch->written = false;
	patch->applied = false;
}

static void GBACheatSetDeinit(struct mCheatSet* set) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) set;
	if (gbaset->hook) {
		--gbaset->hook->refs;
		if (gbaset->hook->refs == 0) {
			free(gbaset->hook);
		}
	}
	size_t i;
	// Freed while still in effect (mCheatDeviceClear): undo our ROM bytes
	for (i = GBACheatDataListSize(&gbaset->romData); i--;) {
		struct GBACheatData* patch = GBACheatDataListGetPointer(&gbaset->romData, i);
		if (patch->applied) {
			_revertROMPatch(gbaset, patch);
		}
		free(patch->data);
	}
	for (i = 0; i < GBACheatDataListSize(&gbaset->allocData); ++i) {
		free(GBACheatDataListGetPointer(&gbaset->allocData, i)->data);
	}
	GBACheatDataListDeinit(&gbaset->romData);
	GBACheatDataListDeinit(&gbaset->allocData);
}

static void GBACheatAddSet(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	_addBreakpoint(device, gbaset);
	gbaset->attached = true;
	_sync(device, NULL, false);
}

static void GBACheatRemoveSet(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	_removeBreakpoint(device, gbaset);
	gbaset->attached = false;
	_sync(device, gbaset, true);
}

static const char* _skipSpace(const char* str) {
	while (isspace((int) str[0])) {
		++str;
	}
	return str;
}

static const char* _parseAllocNumber(const char* str, uint32_t* out) {
	str = _skipSpace(str);
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
		str += 2;
	}
	if (!isxdigit((int) str[0])) {
		return NULL;
	}
	char* end;
	unsigned long value = strtoul(str, &end, 16);
	if (value > 0xFFFFFFFFUL) {
		return NULL;
	}
	*out = value;
	return end;
}

// Syntax: "alloc <address> <size>", both in hex (optional 0x prefix)
bool GBACheatAddAllocLine(struct GBACheatSet* cheats, const char* line) {
	line = _skipSpace(line);
	if (strncasecmp(line, "alloc", 5) != 0 || !isspace((int) line[5])) {
		return false;
	}
	uint32_t base;
	uint32_t size;
	uint32_t aligned;
	line = _parseAllocNumber(&line[5], &base);
	if (!line || !isspace((int) line[0])) {
		return false;
	}
	line = _parseAllocNumber(line, &size);
	if (!line || _skipSpace(line)[0]) {
		return false;
	}
	if (!GBACheatRAMValidate(base, size, &aligned)) {
		return false;
	}
	size_t i;
	for (i = 0; i < cheats->nAllocs; ++i) {
		if (cheats->allocs[i].base == base && cheats->allocs[i].size == aligned) {
			return true;
		}
	}
	if (cheats->nAllocs >= GBA_CHEAT_RAM_MAX_BLOCKS) {
		return false;
	}
	cheats->allocs[cheats->nAllocs].base = base;
	cheats->allocs[cheats->nAllocs].size = aligned;
	++cheats->nAllocs;
	return true;
}

static bool _allocWanted(struct mCheatDevice* device, uint32_t base, uint32_t size) {
	size_t s;
	for (s = 0; s < mCheatSetsSize(&device->cheats); ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		if (!_active(set)) {
			continue;
		}
		size_t i;
		for (i = 0; i < set->nAllocs; ++i) {
			if (set->allocs[i].base == base && set->allocs[i].size == size) {
				return true;
			}
		}
	}
	return false;
}

// Parses "<keyword> <address> <hex bytes>"; the bytes are pairs of hex
// digits, optionally separated by whitespace. Allocates factor * size bytes.
static bool _parseDataLine(const char* line, const char* keyword, size_t factor, uint32_t* address, uint32_t* size, uint8_t** data) {
	// Longer lines would not survive saving and reloading the cheat file
	if (strlen(line) > mCHEAT_MAX_LINE_LENGTH) {
		return false;
	}
	size_t keywordLength = strlen(keyword);
	line = _skipSpace(line);
	if (strncasecmp(line, keyword, keywordLength) != 0 || !isspace((int) line[keywordLength])) {
		return false;
	}
	line = _parseAllocNumber(&line[keywordLength], address);
	if (!line || !isspace((int) line[0])) {
		return false;
	}
	size_t count = 0;
	const char* bytes = line;
	while (true) {
		bytes = _skipSpace(bytes);
		if (!bytes[0]) {
			break;
		}
		if (!isxdigit((int) bytes[0]) || !isxdigit((int) bytes[1])) {
			return false;
		}
		bytes += 2;
		++count;
	}
	if (!count) {
		return false;
	}
	uint8_t* out = calloc(factor, count);
	if (!out) {
		return false;
	}
	size_t i;
	bytes = line;
	for (i = 0; i < count; ++i) {
		bytes = _skipSpace(bytes);
		out[i] = (hexDigit(bytes[0]) << 4) | hexDigit(bytes[1]);
		bytes += 2;
	}
	*size = count;
	*data = out;
	return true;
}

// Syntax: "allocdata <address> <hex bytes>": initial contents for an
// "alloc" block of the same set (the alloc line must come first)
bool GBACheatAddAllocDataLine(struct GBACheatSet* cheats, const char* line) {
	uint32_t address;
	uint32_t size;
	uint8_t* data;
	if (!_parseDataLine(line, "allocdata", 1, &address, &size, &data)) {
		return false;
	}
	size_t i;
	for (i = 0; i < cheats->nAllocs; ++i) {
		const struct GBACheatAlloc* alloc = &cheats->allocs[i];
		if (address - alloc->base < alloc->size && size <= alloc->size - (address - alloc->base)) {
			break;
		}
	}
	if (i == cheats->nAllocs) {
		mLOG(CHEATS, ERROR, "allocdata 0x%08X+0x%X does not fit in an alloc block of its cheat", address, size);
		free(data);
		return false;
	}
	struct GBACheatData* entry = GBACheatDataListAppend(&cheats->allocData);
	memset(entry, 0, sizeof(*entry));
	entry->address = address;
	entry->size = size;
	entry->data = data;
	return true;
}

// Syntax: "rompatch <address> <hex bytes>": replaces ROM bytes while the set
// is enabled. Any ROM mirror (0x08000000-0x0DFFFFFF) is accepted.
bool GBACheatAddROMPatchLine(struct GBACheatSet* cheats, const char* line) {
	uint32_t address;
	uint32_t size;
	uint8_t* data;
	if (!_parseDataLine(line, "rompatch", 2, &address, &size, &data)) {
		return false;
	}
	uint32_t region = address >> BASE_OFFSET;
	uint32_t offset = address & (GBA_SIZE_ROM0 - 1);
	if (region < GBA_REGION_ROM0 || region > GBA_REGION_ROM2_EX || size > GBA_SIZE_ROM0 - offset) {
		mLOG(CHEATS, ERROR, "rompatch 0x%08X+0x%X is outside the cartridge ROM", address, size);
		free(data);
		return false;
	}
	struct GBACheatData* entry = GBACheatDataListAppend(&cheats->romData);
	memset(entry, 0, sizeof(*entry));
	entry->address = GBA_BASE_ROM0 | offset;
	entry->size = size;
	entry->data = data;
	return true;
}

static bool _dataDirty(const struct GBACheatDataList* list, bool wanted) {
	size_t i;
	for (i = 0; i < GBACheatDataListSize(list); ++i) {
		if (GBACheatDataListGetConstPointer(list, i)->applied != wanted) {
			return true;
		}
	}
	return false;
}

struct _ROMPatchRef {
	struct GBACheatSet* set;
	struct GBACheatData* patch;
};

static int _newestFirst(const void* a, const void* b) {
	const struct _ROMPatchRef* x = a;
	const struct _ROMPatchRef* y = b;
	if (x->patch->seq == y->patch->seq) {
		return 0;
	}
	return x->patch->seq < y->patch->seq ? 1 : -1;
}

static size_t _collectApplied(struct GBACheatSet* set, struct _ROMPatchRef* refs, size_t n) {
	size_t i;
	for (i = 0; i < GBACheatDataListSize(&set->romData); ++i) {
		struct GBACheatData* patch = GBACheatDataListGetPointer(&set->romData, i);
		if (patch->applied) {
			if (refs) {
				refs[n].set = set;
				refs[n].patch = patch;
			}
			++n;
		}
	}
	return n;
}

// Takes every "rompatch" off, newest first, so the ROM is exactly as it was
static void _revertAllROMPatches(struct mCheatDevice* device, struct GBACheatSet* removed) {
	size_t nSets = mCheatSetsSize(&device->cheats);
	size_t n = removed ? _collectApplied(removed, NULL, 0) : 0;
	size_t s;
	for (s = 0; s < nSets; ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		if (set != removed) {
			n = _collectApplied(set, NULL, n);
		}
	}
	if (!n) {
		return;
	}
	struct _ROMPatchRef* refs = malloc(n * sizeof(*refs));
	if (!refs) {
		return;
	}
	n = removed ? _collectApplied(removed, refs, 0) : 0;
	for (s = 0; s < nSets; ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		if (set != removed) {
			n = _collectApplied(set, refs, n);
		}
	}
	qsort(refs, n, sizeof(*refs), _newestFirst);
	size_t i;
	for (i = 0; i < n; ++i) {
		_revertROMPatch(refs[i].set, refs[i].patch);
	}
	free(refs);
}

static void _applyAllROMPatches(struct mCheatDevice* device, struct GBA* gba) {
	size_t s;
	for (s = 0; s < mCheatSetsSize(&device->cheats); ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		if (!_active(set)) {
			continue;
		}
		set->romTarget = gba;
		set->romCrc = gba->romCrc32;
		size_t i;
		for (i = 0; i < GBACheatDataListSize(&set->romData); ++i) {
			struct GBACheatData* patch = GBACheatDataListGetPointer(&set->romData, i);
			// Without a ROM nothing is written, and nothing will be restored
			patch->written = GBAMemoryPatchROM(gba, patch->address, patch->data, patch->size, &patch->data[patch->size]);
			patch->applied = true;
			patch->seq = ++_romPatchSeq;
		}
	}
}

static bool _dataInBlock(const struct GBACheatData* data, const struct GBACheatRAMBlock* block) {
	uint32_t offset = data->address - block->base;
	return offset < block->size && data->size <= block->size - offset;
}

// Rebuilds the reset image of every mapped block from the "allocdata" lines
// of the enabled sets (in file order, later lines win) and puts the data in:
// a block mapped just now gets the whole image; a block that was already
// mapped only gets the lines that just came into effect; a block whose
// contents came from a savestate keeps them.
static void _loadAllocData(struct mCheatDevice* device, struct GBA* gba) {
	size_t nSets = mCheatSetsSize(&device->cheats);
	size_t b;
	size_t s;
	size_t i;
	for (b = 0; b < GBA_CHEAT_RAM_MAX_BLOCKS; ++b) {
		struct GBACheatRAMBlock* block = &gba->memory.cheatRam[b];
		if (!block->data) {
			continue;
		}
		uint8_t* image = NULL;
		for (s = 0; s < nSets; ++s) {
			struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
			if (!_active(set)) {
				continue;
			}
			for (i = 0; i < GBACheatDataListSize(&set->allocData); ++i) {
				const struct GBACheatData* data = GBACheatDataListGetConstPointer(&set->allocData, i);
				if (!_dataInBlock(data, block)) {
					continue;
				}
				if (!image) {
					image = calloc(1, block->size);
					if (!image) {
						break;
					}
				}
				memcpy(&image[data->address - block->base], data->data, data->size);
			}
		}
		GBAMemorySetCheatRAMImage(gba, block, image);
		if (block->restored) {
			free(image);
			continue;
		}
		if (block->fresh) {
			if (image) {
				GBAMemoryWriteCheatRAM(gba, block->base, image, block->size);
			}
			free(image);
			continue;
		}
		free(image);
		for (s = 0; s < nSets; ++s) {
			struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
			if (!_active(set)) {
				continue;
			}
			for (i = 0; i < GBACheatDataListSize(&set->allocData); ++i) {
				const struct GBACheatData* data = GBACheatDataListGetConstPointer(&set->allocData, i);
				if (!data->applied && _dataInBlock(data, block)) {
					GBAMemoryWriteCheatRAM(gba, data->address, data->data, data->size);
				}
			}
		}
	}
}

static bool _cpuInBlock(struct GBA* gba, const struct GBACheatRAMBlock* block) {
	// PC runs up to two instructions ahead of the one executing
	return gba->cpu->gprs[ARM_PC] - block->base < block->size + 2 * WORD_SIZE_ARM;
}

// Makes cheat RAM and "rompatch" bytes match the enabled sets.
// Order: ROM patches come off first, then unused blocks are unmapped (back to
// open bus), new blocks are mapped and filled with their "allocdata", and
// only then the ROM patches go back on, so a ROM hook never points into a
// block that is missing or still empty. All of this runs between two CPU
// instructions; a block the CPU is executing from right now stays mapped
// until it has left it (checked again on every refresh).
static void _sync(struct mCheatDevice* device, struct GBACheatSet* removed, bool full) {
	if (!device->p || device->p->platform(device->p) != mPLATFORM_GBA) {
		return;
	}
	struct GBA* gba = device->p->board;
	size_t nSets = mCheatSetsSize(&device->cheats);
	bool romDirty = false;
	bool dataDirty = false;
	if (removed) {
		romDirty = _dataDirty(&removed->romData, false);
		dataDirty = _dataDirty(&removed->allocData, false);
	}
	size_t s;
	size_t i;
	for (s = 0; s < nSets; ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		romDirty = romDirty || _dataDirty(&set->romData, _active(set));
		dataDirty = dataDirty || _dataDirty(&set->allocData, _active(set));
	}

	if (romDirty) {
		_revertAllROMPatches(device, removed);
	}

	// Adding a set never makes a block unwanted. Skipping the unmap there also
	// keeps blocks (and their savestate contents) alive while a cheat file is
	// re-parsed set by set after mCheatDeviceClear, e.g. when a savestate
	// carrying its cheats is loaded; leftovers go on the next refresh.
	for (i = 0; full && i < GBA_CHEAT_RAM_MAX_BLOCKS; ++i) {
		struct GBACheatRAMBlock* block = &gba->memory.cheatRam[i];
		if (block->data && !_allocWanted(device, block->base, block->size)) {
			if (_cpuInBlock(gba, block)) {
				mLOG(CHEATS, DEBUG, "Cheat RAM 0x%08X is executing; unmapping it later", block->base);
				continue;
			}
			GBAMemoryUnmapCheatRAM(gba, block->base, block->size);
		}
	}
	for (s = 0; s < nSets; ++s) {
		struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
		if (!_active(set)) {
			continue;
		}
		for (i = 0; i < set->nAllocs; ++i) {
			GBAMemoryMapCheatRAM(gba, set->allocs[i].base, set->allocs[i].size);
		}
	}
	bool fresh = false;
	for (i = 0; i < GBA_CHEAT_RAM_MAX_BLOCKS; ++i) {
		if (gba->memory.cheatRam[i].data && gba->memory.cheatRam[i].fresh) {
			fresh = true;
		}
	}
	if (dataDirty || fresh) {
		_loadAllocData(device, gba);
		for (s = 0; s < nSets; ++s) {
			struct GBACheatSet* set = (struct GBACheatSet*) *mCheatSetsGetPointer(&device->cheats, s);
			for (i = 0; i < GBACheatDataListSize(&set->allocData); ++i) {
				GBACheatDataListGetPointer(&set->allocData, i)->applied = _active(set);
			}
		}
		if (removed) {
			for (i = 0; i < GBACheatDataListSize(&removed->allocData); ++i) {
				GBACheatDataListGetPointer(&removed->allocData, i)->applied = false;
			}
		}
	}
	// "restored" (contents from a savestate) lasts until the next refresh or
	// removal, so sets re-added in between do not overwrite the state's data
	for (i = 0; i < GBA_CHEAT_RAM_MAX_BLOCKS; ++i) {
		gba->memory.cheatRam[i].fresh = false;
		if (full) {
			gba->memory.cheatRam[i].restored = false;
		}
	}

	if (romDirty) {
		_applyAllROMPatches(device, gba);
	}
}

void GBACheatSyncAllocations(struct mCheatDevice* device) {
	_sync(device, NULL, true);
}

static bool GBACheatAddAutodetect(struct GBACheatSet* set, uint32_t op1, uint32_t op2) {
	uint32_t o1 = op1;
	uint32_t o2 = op2;
	char line[18] = "XXXXXXXX XXXXXXXX";
	snprintf(line, sizeof(line), "%08X %08X", op1, op2);

	int nextProbability;
	int maxProbability = INT_MIN;
	switch (set->gsaVersion) {
	case 0:
		// Try to detect GameShark version
		GBACheatDecryptGameShark(&o1, &o2, GBACheatGameSharkSeeds);
		nextProbability = GBACheatGameSharkProbability(o1, o2);
		o1 = op1;
		o2 = op2;
		if (nextProbability > maxProbability) {
			maxProbability = nextProbability;
			GBACheatSetGameSharkVersion(set, GBA_GS_GSAV1);
		}

		GBACheatDecryptGameShark(&o1, &o2, GBACheatProActionReplaySeeds);
		nextProbability = GBACheatProActionReplayProbability(o1, o2);
		if (nextProbability > maxProbability) {
			maxProbability = nextProbability;
			GBACheatSetGameSharkVersion(set, GBA_GS_PARV3);
		}

		nextProbability = GBACheatGameSharkProbability(op1, op2);
		if (nextProbability > maxProbability) {
			maxProbability = nextProbability;
			GBACheatSetGameSharkVersion(set, GBA_GS_GSAV1_RAW);
		}

		nextProbability = GBACheatProActionReplayProbability(op1, op2);
		if (nextProbability > maxProbability) {
			maxProbability = nextProbability;
			GBACheatSetGameSharkVersion(set, GBA_GS_PARV3_RAW);
		}

		if (set->gsaVersion < 3) {
			return GBACheatAddGameShark(set, op1, op2);
		} else {
			return GBACheatAddProActionReplay(set, op1, op2);
		}
		break;
	case 1:
	case 2:
		return GBACheatAddGameShark(set, o1, o2);
	case 3:
	case 4:
		return GBACheatAddProActionReplay(set, o1, o2);
	}
	return false;
}

bool GBACheatAddVBALine(struct GBACheatSet* cheats, const char* line) {
	uint32_t address;
	uint8_t op;
	uint32_t value = 0;
	int width = 0;
	const char* lineNext = hex32(line, &address);
	if (!lineNext) {
		return false;
	}
	if (lineNext[0] != ':') {
		return false;
	}
	++lineNext;
	while (width < 4) {
		lineNext = hex8(lineNext, &op);
		if (!lineNext) {
			break;
		}
		value <<= 8;
		value |= op;
		++width;
	}
	if (width == 0 || width == 3) {
		return false;
	}

	if (address < GBA_BASE_ROM0 || address >= GBA_BASE_SRAM) {
		struct mCheat* cheat = mCheatListAppend(&cheats->d.list);
		memset(cheat, 0, sizeof(*cheat));
		cheat->address = address;
		cheat->operandOffset = 0;
		cheat->addressOffset = 0;
		cheat->repeat = 1;
		cheat->type = CHEAT_ASSIGN;
		cheat->width = width;
		cheat->operand = value;
	} else {
		struct mCheatPatch* patch = mCheatPatchListAppend(&cheats->d.romPatches);
		memset(patch, 0, sizeof(*patch));
		patch->width = width;
		patch->address = address;
		patch->segment = 0;
		patch->value = value;
		patch->check = false;
	}
	return true;
}

bool GBACheatAddLine(struct mCheatSet* set, const char* line, int type) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;
	const char* keyword = _skipSpace(line);
	if (strncasecmp(keyword, "allocdata", 9) == 0) {
		return GBACheatAddAllocDataLine(cheats, line);
	}
	if (strncasecmp(keyword, "rompatch", 8) == 0) {
		return GBACheatAddROMPatchLine(cheats, line);
	}
	if (strncasecmp(keyword, "alloc", 5) == 0) {
		return GBACheatAddAllocLine(cheats, line);
	}
	switch (type) {
	case GBA_CHEAT_AUTODETECT:
		break;
	case GBA_CHEAT_CODEBREAKER:
		return GBACheatAddCodeBreakerLine(cheats, line);
	case GBA_CHEAT_GAMESHARK:
		return GBACheatAddGameSharkLine(cheats, line);
	case GBA_CHEAT_PRO_ACTION_REPLAY:
		return GBACheatAddProActionReplayLine(cheats, line);
	case GBA_CHEAT_VBA:
		return GBACheatAddVBALine(cheats, line);
	default:
		return false;
	}

	uint32_t op1;
	uint16_t op2;
	uint16_t op3;
	const char* lineNext = hex32(line, &op1);
	if (!lineNext) {
		return false;
	}
	if (lineNext[0] == ':') {
		return GBACheatAddVBALine(cheats, line);
	}
	while (isspace((int) lineNext[0])) {
		++lineNext;
	}
	lineNext = hex16(lineNext, &op2);
	if (!lineNext) {
		return false;
	}
	if (!lineNext[0] || isspace((int) lineNext[0])) {
		return GBACheatAddCodeBreaker(cheats, op1, op2);
	}
	lineNext = hex16(lineNext, &op3);
	if (!lineNext) {
		return false;
	}
	uint32_t realOp2 = op2;
	realOp2 <<= 16;
	realOp2 |= op3;
	return GBACheatAddAutodetect(cheats, op1, realOp2);
}

static void GBACheatRefresh(struct mCheatSet* cheats, struct mCheatDevice* device) {
	struct GBACheatSet* gbaset = (struct GBACheatSet*) cheats;
	if (cheats->enabled && gbaset->hook && !gbaset->hook->reentries) {
		_addBreakpoint(device, gbaset);
	}
	// Once per frame (from the first set) follows enable/disable of any set;
	// a set whose own lines are not in effect as they should be syncs at once
	if ((mCheatSetsSize(&device->cheats) && *mCheatSetsGetPointer(&device->cheats, 0) == cheats) ||
	    _dataDirty(&gbaset->romData, _active(gbaset)) || _dataDirty(&gbaset->allocData, _active(gbaset))) {
		_sync(device, NULL, true);
	}
}

static void GBACheatSetCopyProperties(struct mCheatSet* set, struct mCheatSet* oldSet) {
	struct GBACheatSet* newSet = (struct GBACheatSet*) set;
	struct GBACheatSet* gbaset = (struct GBACheatSet*) oldSet;
	newSet->gsaVersion = gbaset->gsaVersion;
	memcpy(newSet->gsaSeeds, gbaset->gsaSeeds, sizeof(newSet->gsaSeeds));
	newSet->cbRngState = gbaset->cbRngState;
	newSet->cbMaster = gbaset->cbMaster;
	memcpy(newSet->cbSeeds, gbaset->cbSeeds, sizeof(newSet->cbSeeds));
	memcpy(newSet->cbTable, gbaset->cbTable, sizeof(newSet->cbTable));
	if (gbaset->hook) {
		if (newSet->hook) {
			--newSet->hook->refs;
			if (newSet->hook->refs == 0) {
				free(newSet->hook);
			}
		}
		newSet->hook = gbaset->hook;
		++newSet->hook->refs;
	}
}

static void GBACheatParseDirectives(struct mCheatSet* set, const struct StringList* directives) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;
	size_t d;
	for (d = 0; d < StringListSize(directives); ++d) {
		const char* directive = *StringListGetConstPointer(directives, d);
		if (strcmp(directive, "GSAv1") == 0) {
			GBACheatSetGameSharkVersion(cheats, GBA_GS_GSAV1);
			continue;
		}
		if (strcmp(directive, "GSAv1 raw") == 0) {
			GBACheatSetGameSharkVersion(cheats, GBA_GS_GSAV1_RAW);
			continue;
		}
		if (strcmp(directive, "PARv3") == 0) {
			GBACheatSetGameSharkVersion(cheats, GBA_GS_PARV3);
			continue;
		}
		if (strcmp(directive, "PARv3 raw") == 0) {
			GBACheatSetGameSharkVersion(cheats, GBA_GS_PARV3_RAW);
			continue;
		}
		// "!alloc <address> <size>" applies to every following set until
		// "!reset"; it is stored as an "alloc" line so saving keeps it
		size_t before = cheats->nAllocs;
		if (GBACheatAddAllocLine(cheats, directive) && cheats->nAllocs != before) {
			*StringListAppend(&set->lines) = strdup(directive);
			continue;
		}
	}
}

static void GBACheatDumpDirectives(struct mCheatSet* set, struct StringList* directives) {
	struct GBACheatSet* cheats = (struct GBACheatSet*) set;

	// TODO: Check previous directives
	size_t d;
	for (d = 0; d < StringListSize(directives); ++d) {
		free(*StringListGetPointer(directives, d));
	}
	StringListClear(directives);

	char** directive;
	switch (cheats->gsaVersion) {
	case 1:
		directive = StringListAppend(directives);
		*directive = strdup("GSAv1");
		break;
	case 2:
		directive = StringListAppend(directives);
		*directive = strdup("GSAv1 raw");
		break;
	case 3:
		directive = StringListAppend(directives);
		*directive = strdup("PARv3");
		break;
	case 4:
		directive = StringListAppend(directives);
		*directive = strdup("PARv3 raw");
		break;
	}
}

int GBACheatAddressIsReal(uint32_t address) {
	switch (address >> BASE_OFFSET) {
	case GBA_REGION_BIOS:
		return -0x80;
		break;
	case GBA_REGION_EWRAM:
		if ((address & OFFSET_MASK) > GBA_SIZE_EWRAM) {
			return -0x40;
		}
		return 0x20;
	case GBA_REGION_IWRAM:
		if ((address & OFFSET_MASK) > GBA_SIZE_IWRAM) {
			return -0x40;
		}
		return 0x20;
	case GBA_REGION_IO:
		if ((address & OFFSET_MASK) > GBA_SIZE_IO) {
			return -0x80;
		}
		return 0x10;
	case GBA_REGION_OAM:
		if ((address & OFFSET_MASK) > GBA_SIZE_OAM) {
			return -0x80;
		}
		return -0x8;
	case GBA_REGION_VRAM:
		if ((address & OFFSET_MASK) > GBA_SIZE_VRAM) {
			return -0x80;
		}
		return -0x8;
	case GBA_REGION_PALETTE_RAM:
		if ((address & OFFSET_MASK) > GBA_SIZE_PALETTE_RAM) {
			return -0x80;
		}
		return -0x8;
	case GBA_REGION_ROM0:
	case GBA_REGION_ROM0_EX:
	case GBA_REGION_ROM1:
	case GBA_REGION_ROM1_EX:
	case GBA_REGION_ROM2:
	case GBA_REGION_ROM2_EX:
		return -0x8;
	case GBA_REGION_SRAM:
	case GBA_REGION_SRAM_MIRROR:
		if ((address & OFFSET_MASK) > GBA_SIZE_FLASH512) {
			return -0x80;
		}
		return -0x8;
	default:
		return -0xC0;
	}
}
