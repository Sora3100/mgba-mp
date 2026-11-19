/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include "sdl-events.h"

#include <mgba/core/input.h>

// Pad-to-player assignment of the SDL event layer, driven with SDL virtual
// joysticks so no real controller is needed (real devices are disabled; if
// one still shows up the tests are skipped).

#if SDL_VERSION_ATLEAST(3, 0, 0)

#define SLOT_P1 0
#define SLOT_EXTRA 4

struct Fixture {
	struct mSDLEvents events;
	struct mSDLPlayer players[3];
	struct mInputMap maps[3];
	SDL_JoystickID pads[6];
	int nPads;
};

static const struct mInputPlatformInfo _info = {
	.platformName = "gba",
	.nKeys = 0,
	.hat = { -1, -1, -1, -1 },
};

static void _disableRealDevices(void) {
	static const char* const hints[] = {
		"SDL_XINPUT_ENABLED", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_DIRECTINPUT", "SDL_JOYSTICK_WGI",
		"SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_MFI", "SDL_JOYSTICK_IOKIT",
		"SDL_JOYSTICK_LINUX_DEVICES",
	};
	size_t i;
	for (i = 0; i < sizeof(hints) / sizeof(*hints); ++i) {
		SDL_SetHint(hints[i], "0");
	}
	// Linux/BSD evdev scanning can only be pointed somewhere empty
	SDL_SetHint("SDL_JOYSTICK_DEVICE", "/nonexistent");
}

static SDL_JoystickID _attachPad(struct Fixture* f, const char* name, Uint16 vendor) {
	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.vendor_id = vendor;
	desc.product_id = 0x0001;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name = name;
	SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
	assert_true(id != 0);
	f->pads[f->nPads++] = id;
	return id;
}

static SDL_JoystickID _padOf(const struct mSDLPlayer* player) {
	return player->joystick ? player->joystick->id : 0;
}

static struct Fixture* _setup(void) {
	_disableRealDevices();
	struct Fixture* f = calloc(1, sizeof(*f));
	assert_true(mSDLInitEvents(&f->events));
	if (SDL_JoystickListSize(&f->events.joysticks)) {
		return f;
	}
	int i;
	for (i = 0; i < 3; ++i) {
		mInputMapInit(&f->maps[i], &_info);
		f->players[i].bindings = &f->maps[i];
		assert_true(mSDLAttachPlayer(&f->events, &f->players[i], i ? SLOT_EXTRA + i - 1 : SLOT_P1));
		assert_null(f->players[i].joystick);
	}
	return f;
}

static void _teardown(struct Fixture* f) {
	int i;
	for (i = 0; i < 3; ++i) {
		if (f->players[i].bindings) {
			mSDLDetachPlayer(&f->events, &f->players[i]);
			mInputMapDeinit(&f->maps[i]);
		}
	}
	for (i = 0; i < f->nPads; ++i) {
		if (f->pads[i]) {
			SDL_DetachVirtualJoystick(f->pads[i]);
		}
	}
	mSDLUpdateJoysticks(&f->events, NULL);
	mSDLDeinitEvents(&f->events);
	free(f);
}

#define SKIP_IF_REAL_PADS(F) \
	if (SDL_JoystickListSize(&(F)->events.joysticks)) { \
		_teardown(F); \
		skip(); \
	}

M_TEST_DEFINE(plugOrderFillsLowestFreePlayer) {
	struct Fixture* f = _setup();
	SKIP_IF_REAL_PADS(f);
	SDL_JoystickID a = _attachPad(f, "Pad A", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_null(f->players[1].joystick);

	SDL_JoystickID b = _attachPad(f, "Pad B", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_int_equal(_padOf(&f->players[1]), b);
	assert_null(f->players[2].joystick);

	// Unplugging player 1's pad frees its slot only; the next pad goes there
	SDL_DetachVirtualJoystick(a);
	f->pads[0] = 0;
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_null(f->players[0].joystick);
	assert_int_equal(_padOf(&f->players[1]), b);
	SDL_JoystickID c = _attachPad(f, "Pad C", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), c);
	assert_int_equal(_padOf(&f->players[1]), b);
	_teardown(f);
}

M_TEST_DEFINE(repeatedAddEventDoesNotShareAPad) {
	struct Fixture* f = _setup();
	SKIP_IF_REAL_PADS(f);
	SDL_JoystickID a = _attachPad(f, "Pad A", 0x1111);
	SDL_JoystickID b = _attachPad(f, "Pad B", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_int_equal(_padOf(&f->players[1]), b);

	// A second "added" event for a pad that is already open and in use (as
	// SDL can send for devices opened at startup) must not hand it to the
	// next player without a pad
	SDL_Event event;
	SDL_zero(event);
	event.type = SDL_EVENT_JOYSTICK_ADDED;
	event.jdevice.which = a;
	assert_true(SDL_PushEvent(&event));
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_int_equal(_padOf(&f->players[1]), b);
	assert_null(f->players[2].joystick);
	_teardown(f);
}

M_TEST_DEFINE(preferredMatchKeepsHandlingQueuedPads) {
	struct Fixture* f = _setup();
	SKIP_IF_REAL_PADS(f);
	SDL_JoystickID a = _attachPad(f, "Pad A", 0x2222);
	SDL_JoystickID b = _attachPad(f, "Pad B", 0x3333);
	char guid[34];
	SDL_GUIDToString(SDL_GetJoystickGUIDForID(a), guid, sizeof(guid));
	f->events.preferredJoysticks[SLOT_P1].type = guid;

	// Both pads are queued before the update: the preferred match for player 1
	// must not leave the second pad waiting
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_int_equal(_padOf(&f->players[1]), b);
	f->events.preferredJoysticks[SLOT_P1].type = NULL;
	f->events.preferredJoysticks[SLOT_P1].serial = NULL;
	_teardown(f);
}

M_TEST_DEFINE(assignFreeRefillsPlayersWithoutPad) {
	struct Fixture* f = _setup();
	SKIP_IF_REAL_PADS(f);
	SDL_JoystickID a = _attachPad(f, "Pad A", 0x1111);
	SDL_JoystickID b = _attachPad(f, "Pad B", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	struct mSDLPlayer* order[] = { &f->players[0], &f->players[1], &f->players[2] };

	// Nothing free: nothing changes
	assert_int_equal(mSDLAssignFreeJoysticks(&f->events, order, 3), 0);
	assert_int_equal(_padOf(&f->players[0]), a);
	assert_int_equal(_padOf(&f->players[1]), b);
	assert_null(f->players[2].joystick);

	// Player 2 lost its pad (e.g. handed it to player 1 in a swap): the pad
	// nobody holds goes back to the lowest player without one
	f->players[0].joystick = f->players[1].joystick;
	f->players[1].joystick = NULL;
	assert_int_equal(mSDLAssignFreeJoysticks(&f->events, order, 3), 1);
	assert_int_equal(_padOf(&f->players[0]), b);
	assert_int_equal(_padOf(&f->players[1]), a);
	assert_null(f->players[2].joystick);

	// Detached players are left alone
	mSDLDetachPlayer(&f->events, &f->players[2]);
	SDL_JoystickID c = _attachPad(f, "Pad C", 0x1111);
	mSDLUpdateJoysticks(&f->events, NULL);
	assert_null(f->players[2].joystick);
	assert_int_equal(mSDLAssignFreeJoysticks(&f->events, order, 3), 0);
	assert_null(f->players[2].joystick);
	assert_true(mSDLAttachPlayer(&f->events, &f->players[2], SLOT_EXTRA + 1));
	assert_int_equal(_padOf(&f->players[2]), c);
	_teardown(f);
}

M_TEST_SUITE_DEFINE(SDLEvents,
	cmocka_unit_test(plugOrderFillsLowestFreePlayer),
	cmocka_unit_test(repeatedAddEventDoesNotShareAPad),
	cmocka_unit_test(preferredMatchKeepsHandlingQueuedPads),
	cmocka_unit_test(assignFreeRefillsPlayersWithoutPad))

#else

M_TEST_DEFINE(needsSDL3) {
	skip();
}

M_TEST_SUITE_DEFINE(SDLEvents,
	cmocka_unit_test(needsSDL3))

#endif
