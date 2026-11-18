/* Copyright (c) 2013-2023 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "input/SDLInputDriver.h"
#include "input/moc_SDLInputDriver.cpp"

#include "ConfigController.h"
#include "InputController.h"

#include <QSet>

#include <mgba/internal/gba/multipad.h>

#include <algorithm>

using namespace QGBA;

int s_sdlInited = 0;
QReadWriteLock s_eventsRwLock;
mSDLEvents s_sdlEvents;

void SDL::suspendScreensaver() {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (s_sdlInited) {
		mSDLSuspendScreensaver(&s_sdlEvents);
	}
#endif
}

void SDL::resumeScreensaver() {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (s_sdlInited) {
		mSDLResumeScreensaver(&s_sdlEvents);
	}
#endif
}

void SDL::setScreensaverSuspendable(bool suspendable) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (s_sdlInited) {
		mSDLSetScreensaverSuspendable(&s_sdlEvents, suspendable);
	}
#endif
}

SDLInputDriver::SDLInputDriver(InputController* controller, QObject* parent)
	: InputDriver(parent)
	, m_controller(controller)
{
	QWriteLocker locker(&s_eventsRwLock);
	if (s_sdlInited == 0) {
		mSDLInitEvents(&s_sdlEvents);
	}
	++s_sdlInited;
	m_sdlPlayer.bindings = m_controller->map();

	for (size_t i = 0; i < SDL_JoystickListSize(&s_sdlEvents.joysticks); ++i) {
		// Can't use make_shared here due to friend restrictions
		m_gamepads.append(std::shared_ptr<SDLGamepad>(new SDLGamepad(this, i)));
	}

#if SDL_VERSION_ATLEAST(2, 0, 0)
	// Throttle the gamepad update timer to only update once a second
	m_gamepadTimer.setInterval(1000);
	connect(&m_gamepadTimer, &QTimer::timeout, this, &SDLInputDriver::updateGamepads);
#endif
}

SDLInputDriver::~SDLInputDriver() {
	QWriteLocker locker(&s_eventsRwLock);
	for (int i = 0; i < EXTRA_PLAYERS; ++i) {
		if (m_extraAttached[i]) {
			mSDLDetachPlayer(&s_sdlEvents, &m_extraPlayers[i]);
			m_extraAttached[i] = false;
		}
	}
	if (m_playerAttached) {
		mSDLDetachPlayer(&s_sdlEvents, &m_sdlPlayer);
	}

	--s_sdlInited;
	if (s_sdlInited == 0) {
		mSDLDeinitEvents(&s_sdlEvents);
	}
}

void SDLInputDriver::setPlayerId(int id) {
	InputDriver::setPlayerId(id);
	if (m_playerAttached) {
		QWriteLocker locker(&s_eventsRwLock);
		mSDLPlayerChangeId(&s_sdlEvents, &m_sdlPlayer, id);
	}
}

bool SDLInputDriver::supportsPolling() const {
	// XXX: SDL_PumpEvents can cause the runloop to re-enter, at least on Windows
	// So to avoid re-entering the SDL polling in the meantime, we have to reject
	// polling while locked.
	if (!s_eventsRwLock.tryLockForRead(0)) {
		return false;
	}
	s_eventsRwLock.unlock();
	return true;
}

bool SDLInputDriver::supportsGamepads() const {
	return true;
}

bool SDLInputDriver::supportsSensors() const {
	return true;
}

QString SDLInputDriver::currentProfile() const {
	if (m_sdlPlayer.joystick) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
		return SDL_JoystickName(m_sdlPlayer.joystick->joystick);
#else
		return SDL_JoystickName(SDL_JoystickIndex(m_sdlPlayer.joystick->joystick));
#endif
	}
	return {};
}

void SDLInputDriver::loadConfiguration(ConfigController* config) {
	m_config = config;

	QWriteLocker locker(&s_eventsRwLock);
	mSDLEventsLoadConfig(&s_sdlEvents, config->input());
	if (!m_playerAttached) {
		m_playerAttached = mSDLAttachPlayer(&s_sdlEvents, &m_sdlPlayer, playerId());
	}
	if (m_playerAttached) {
		mSDLPlayerLoadConfig(&m_sdlPlayer, config->input());
	}
}

void SDLInputDriver::saveConfiguration(ConfigController* config) {
	if (m_playerAttached) {
		mSDLPlayerSaveConfig(&m_sdlPlayer, config->input());
	}
}

void SDLInputDriver::bindDefaults(InputController* controller) {
	mSDLInitBindingsGBA(controller->map());
}

mRumble* SDLInputDriver::rumble() {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (m_playerAttached) {
		return &m_sdlPlayer.rumble.d.d;
	}
#endif
	return nullptr;
}

mRotationSource* SDLInputDriver::rotationSource() {
	if (m_playerAttached) {
		return &m_sdlPlayer.rotation.d;
	}
	return nullptr;
}


bool SDLInputDriver::update() {
	if (!m_playerAttached) {
		return false;
	}

#if SDL_VERSION_ATLEAST(2, 0, 0)
	if (!m_gamepadTimer.isActive()) {
		m_gamepadTimer.start();
	}
#endif
	SDL_JoystickUpdate();

	return true;
}

QList<std::shared_ptr<Gamepad>> SDLInputDriver::connectedGamepads() const {
	QList<std::shared_ptr<Gamepad>> pads;
	for (auto& pad : m_gamepads) {
		pads.append(pad);
	}
	return pads;
}

#if SDL_VERSION_ATLEAST(2, 0, 0)
void SDLInputDriver::updateGamepads() {
	QSignalBlocker blocker(&m_gamepadTimer);
	// XXX: SDL_PumpEvents can cause the runloop to re-enter, at least on Windows
	// So to avoid re-entering the SDL polling in the meantime, we have to reject
	// polling while locked.
	if (!s_eventsRwLock.tryLockForWrite(0)) {
		return;
	}
	if (m_config) {
		mSDLUpdateJoysticks(&s_sdlEvents, m_config->input());
	}

	QSet<size_t> knownGamepads;
	for (int i = 0; i < m_gamepads.size(); ++i) {
		auto& gamepad = m_gamepads.at(i);
		if (gamepad->updateIndex()) {
			knownGamepads.insert(gamepad->m_index);
			continue;
		}
		m_gamepads.removeAt(i);
		--i;
	}

	for (size_t i = 0; i < SDL_JoystickListSize(&s_sdlEvents.joysticks); ++i) {
		if (knownGamepads.contains(i)) {
			continue;
		}
		// Can't use make_shared here due to friend restrictions
		m_gamepads.append(std::shared_ptr<SDLGamepad>(new SDLGamepad(this, i)));
	}
	std::sort(m_gamepads.begin(), m_gamepads.end(), [](const auto& a, const auto& b) {
		return a->m_index < b->m_index;
	});
	s_eventsRwLock.unlock();
}
#endif

int SDLInputDriver::activeGamepadIndex() const {
	return m_sdlPlayer.joystick ? m_sdlPlayer.joystick->index : 0;
}

void SDLInputDriver::setActiveGamepad(int index) {
	setPlayerGamepad(1, index);
}

mSDLPlayer* SDLInputDriver::sdlPlayer(int player) {
	if (player == 1) {
		return m_playerAttached ? &m_sdlPlayer : nullptr;
	}
	if (player < 2 || player > EXTRA_PLAYERS + 1) {
		return nullptr;
	}
	return m_extraAttached[player - 2] ? &m_extraPlayers[player - 2] : nullptr;
}

void SDLInputDriver::setPlayerMap(int player, mInputMap* map) {
	if (player < 2 || player > EXTRA_PLAYERS + 1) {
		return;
	}
	m_extraMaps[player - 2] = map;
	m_extraPlayers[player - 2].bindings = map;
}

void SDLInputDriver::setExtraPlayersEnabled(bool enabled) {
	QWriteLocker locker(&s_eventsRwLock);
	m_extraEnabled = enabled;
	for (int i = 0; i < EXTRA_PLAYERS; ++i) {
		if (enabled && !m_extraAttached[i] && m_extraMaps[i] && m_playerAttached) {
			// Attached in player order after player 1, so pads are handed out
			// in SDL device order (plug-in order) at startup. Hotplugged pads go
			// to the lowest SDL player slot without one (see mSDLUpdateJoysticks).
			m_extraPlayers[i].bindings = m_extraMaps[i];
			m_extraAttached[i] = mSDLAttachPlayer(&s_sdlEvents, &m_extraPlayers[i], EXTRA_PLAYER_BASE + i);
		} else if (!enabled && m_extraAttached[i]) {
			mSDLDetachPlayer(&s_sdlEvents, &m_extraPlayers[i]);
			m_extraPlayers[i].joystick = nullptr;
			m_extraAttached[i] = false;
		}
	}
}

bool SDLInputDriver::extraPlayersEnabled() const {
	return m_extraEnabled;
}

QList<bool> SDLInputDriver::playerButtons(int player) {
	QReadLocker locker(&s_eventsRwLock);
	mSDLPlayer* sdl = sdlPlayer(player);
	if (!sdl || !sdl->joystick) {
		return {};
	}

	QList<bool> buttons;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_GameController* controller = sdl->joystick->controller;
	if (!controller) {
		return {};
	}
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
		buttons.append(SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(i)));
	}
#else
	SDL_Joystick* joystick = sdl->joystick->joystick;
	int numButtons = SDL_JoystickNumButtons(joystick);
	for (int i = 0; i < numButtons; ++i) {
		buttons.append(SDL_JoystickGetButton(joystick, i));
	}
#endif
	return buttons;
}

QList<int16_t> SDLInputDriver::playerAxes(int player) {
	QReadLocker locker(&s_eventsRwLock);
	mSDLPlayer* sdl = sdlPlayer(player);
	if (!sdl || !sdl->joystick) {
		return {};
	}

	QList<int16_t> axes;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_GameController* controller = sdl->joystick->controller;
	if (!controller) {
		return {};
	}
	for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
		axes.append(SDL_GameControllerGetAxis(controller, static_cast<SDL_GameControllerAxis>(i)));
	}
#else
	SDL_Joystick* joystick = sdl->joystick->joystick;
	int numAxes = SDL_JoystickNumAxes(joystick);
	for (int i = 0; i < numAxes; ++i) {
		axes.append(SDL_JoystickGetAxis(joystick, i));
	}
#endif
	return axes;
}

QList<GamepadHatEvent::Direction> SDLInputDriver::playerHats(int player) {
	QList<GamepadHatEvent::Direction> hats;
#if !SDL_VERSION_ATLEAST(2, 0, 0)
	QReadLocker locker(&s_eventsRwLock);
	mSDLPlayer* sdl = sdlPlayer(player);
	if (!sdl || !sdl->joystick) {
		return {};
	}
	SDL_Joystick* joystick = sdl->joystick->joystick;
	int numHats = SDL_JoystickNumHats(joystick);
	for (int i = 0; i < numHats; ++i) {
		hats.append(static_cast<GamepadHatEvent::Direction>(SDL_JoystickGetHat(joystick, i)));
	}
#else
	UNUSED(player);
#endif
	return hats;
}

QString SDLInputDriver::playerGamepadName(int player) {
	QReadLocker locker(&s_eventsRwLock);
	mSDLPlayer* sdl = sdlPlayer(player);
	if (!sdl || !sdl->joystick) {
		return {};
	}
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return QString::fromUtf8(SDL_JoystickName(sdl->joystick->joystick));
#else
	return QString::fromUtf8(SDL_JoystickName(SDL_JoystickIndex(sdl->joystick->joystick)));
#endif
}

int SDLInputDriver::playerGamepadIndex(int player) {
	QReadLocker locker(&s_eventsRwLock);
	mSDLPlayer* sdl = sdlPlayer(player);
	if (!sdl || !sdl->joystick) {
		return -1;
	}
	return sdl->joystick->index;
}

void SDLInputDriver::setPlayerGamepad(int player, int index) {
	QWriteLocker locker(&s_eventsRwLock);
	mSDLPlayer* target = sdlPlayer(player);
	if (!target || index < 0 || static_cast<size_t>(index) >= SDL_JoystickListSize(&s_sdlEvents.joysticks)) {
		return;
	}
	SDL_JoystickCombo* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, index);
	if (m_extraEnabled) {
		// Swap with whichever of our players currently holds that pad
		for (int p = 1; p <= EXTRA_PLAYERS + 1; ++p) {
			mSDLPlayer* other = sdlPlayer(p);
			if (other && other != target && other->joystick == joystick) {
				other->joystick = target->joystick;
			}
		}
	}
	target->joystick = joystick;
}

void SDLInputDriver::bindPlayerDefaults(mInputMap* map) {
	mSDLInitBindingsGBA(map);
}

void SDLInputDriver::bindExtendedDefaults(mInputMap* map) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_X, GBA_EXT_KEY_X);
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_Y, GBA_EXT_KEY_Y);
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_LEFTSTICK, GBA_EXT_KEY_L3);
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_RIGHTSTICK, GBA_EXT_KEY_R3);
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_GUIDE, GBA_EXT_KEY_GUIDE);
	mInputBindKey(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_BUTTON_BACK, GBA_EXT_KEY_BACK);

	// Triggers rest at 0 and only go positive
	struct mInputAxis description = { GBA_EXT_KEY_ZL, -1, 0x4000, INT16_MIN };
	mInputBindAxis(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_AXIS_TRIGGERLEFT, &description);
	description = { GBA_EXT_KEY_ZR, -1, 0x4000, INT16_MIN };
	mInputBindAxis(map, SDL_BINDING_CONTROLLER, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, &description);
#else
	mInputBindKey(map, SDL_BINDING_BUTTON, 2, GBA_EXT_KEY_X);
	mInputBindKey(map, SDL_BINDING_BUTTON, 3, GBA_EXT_KEY_Y);
#endif
}

void SDLInputDriver::registerTiltAxisX(int axis) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.axisX = axis;
	}
}

void SDLInputDriver::registerTiltAxisY(int axis) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.axisY = axis;
	}
}

void SDLInputDriver::registerGyroAxisX(int axis) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.gyroX = axis;
		if (m_sdlPlayer.rotation.gyroY == axis) {
			m_sdlPlayer.rotation.gyroZ = axis;
		} else {
			m_sdlPlayer.rotation.gyroZ = -1;
		}
	}
}

void SDLInputDriver::registerGyroAxisY(int axis) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.gyroY = axis;
		if (m_sdlPlayer.rotation.gyroX == axis) {
			m_sdlPlayer.rotation.gyroZ = axis;
		} else {
			m_sdlPlayer.rotation.gyroZ = -1;
		}
	}
}

void SDLInputDriver::registerGyroAxisZ(int axis) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.gyroZ = axis;
		m_sdlPlayer.rotation.gyroX = -1;
		m_sdlPlayer.rotation.gyroY = -1;
	}
}

float SDLInputDriver::gyroSensitivity() const {
	if (m_playerAttached) {
		return m_sdlPlayer.rotation.gyroSensitivity;
	}
	return 0;
}

void SDLInputDriver::setGyroSensitivity(float sensitivity) {
	if (m_playerAttached) {
		m_sdlPlayer.rotation.gyroSensitivity = sensitivity;
	}
}

SDLGamepad::SDLGamepad(SDLInputDriver* driver, int index, QObject* parent)
	: Gamepad(driver, parent)
	, m_index(index)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
#if SDL_VERSION_ATLEAST(2, 24, 0)
	SDL_GUIDToString(SDL_JoystickGetGUID(joystick), m_guid, sizeof(m_guid));
#else
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), m_guid, sizeof(m_guid));
#endif
	m_id = SDL_JoystickInstanceID(joystick);
	m_visibleName = SDL_JoystickName(joystick);
#if SDL_VERSION_ATLEAST(2, 0, 14)
	m_serial = SDL_JoystickGetSerial(joystick);
#endif
#else
	m_visibleName = SDL_JoystickName(SDL_JoystickIndex(joystick));
#endif
}

QList<bool> SDLGamepad::currentButtons() const {
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return {};
	}

	QList<bool> buttons;

#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_GameController* controller = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->controller;
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
		buttons.append(SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(i)));
	}
#else
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	int numButtons = SDL_JoystickNumButtons(joystick);
	for (int i = 0; i < numButtons; ++i) {
		buttons.append(SDL_JoystickGetButton(joystick, i));
	}
#endif

	return buttons;
}

QList<int16_t> SDLGamepad::currentAxes() const {
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return {};
	}

	QList<int16_t> axes;

#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_GameController* controller = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->controller;
	for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
		axes.append(SDL_GameControllerGetAxis(controller, static_cast<SDL_GameControllerAxis>(i)));
	}
#else
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	int numAxes = SDL_JoystickNumAxes(joystick);
	for (int i = 0; i < numAxes; ++i) {
		axes.append(SDL_JoystickGetAxis(joystick, i));
	}
#endif

	return axes;
}

QList<GamepadHatEvent::Direction> SDLGamepad::currentHats() const {
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return {};
	}

	QList<GamepadHatEvent::Direction> hats;

#if !SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	int numHats = SDL_JoystickNumHats(joystick);
	for (int i = 0; i < numHats; ++i) {
		hats.append(static_cast<GamepadHatEvent::Direction>(SDL_JoystickGetHat(joystick, i)));
	}
#endif

	return hats;
}

QString SDLGamepad::buttonHumanName(int button) const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return {};
	}

	SDL_GameController* controller = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->controller;
	const char* name = mSDLButtonName(controller, static_cast<SDL_GameControllerButton>(button));
	if (name) {
		return QString::fromUtf8(name);
	}
#endif
	return QString::number(button);
}

QString SDLGamepad::axisHumanName(int axis) const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return {};
	}

	SDL_GameController* controller = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->controller;
	const char* name = mSDLAxisName(controller, static_cast<SDL_GameControllerAxis>(axis));
	if (name) {
		return QString::fromUtf8(name);
	}
#endif
	return QString::number(axis);
}

int SDLGamepad::buttonCount() const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_CONTROLLER_BUTTON_MAX;
#else
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return -1;
	}
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	return SDL_JoystickNumButtons(joystick);
#endif
}

int SDLGamepad::axisCount() const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_CONTROLLER_AXIS_MAX;
#else
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return -1;
	}
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	return SDL_JoystickNumAxes(joystick);
#endif
}

int SDLGamepad::hatCount() const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return 0;
#else
	QReadLocker locker(&s_eventsRwLock);
	if (!verify()) {
		return -1;
	}
	SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, m_index)->joystick;
	return SDL_JoystickNumHats(joystick);
#endif
}

QString SDLGamepad::name() const {
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return m_guid;
#else
	return m_visibleName;
#endif
}

QString SDLGamepad::visibleName() const {
	return m_visibleName;
}

QString SDLGamepad::serial() const {
	return m_serial;
}

#if SDL_VERSION_ATLEAST(2, 0, 0)
bool SDLGamepad::updateIndex() {
	for (size_t i = 0; i < SDL_JoystickListSize(&s_sdlEvents.joysticks); ++i) {
		SDL_Joystick* joystick = SDL_JoystickListGetPointer(&s_sdlEvents.joysticks, i)->joystick;
		SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
		if (id == m_id) {
			m_index = i;
			return true;
		}

#if SDL_VERSION_ATLEAST(2, 0, 0)
		char guid[34]{};
#if SDL_VERSION_ATLEAST(2, 24, 0)
		SDL_GUIDToString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
#else
		SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
#endif
		if (QLatin1String(guid) != QLatin1String(m_guid)) {
			continue;
		}

#if SDL_VERSION_ATLEAST(2, 0, 14)
		QString serial = SDL_JoystickGetSerial(joystick);
		if (m_serial == serial) {
			m_index = i;
			return true;
		}
#endif
#endif
	}
	return false;
}
#endif

bool SDLGamepad::verify() const {
	return m_index < SDL_JoystickListSize(&s_sdlEvents.joysticks);
}
