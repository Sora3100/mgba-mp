/* Copyright (c) 2013-2023 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "InputController.h"
#include "moc_InputController.cpp"

#include "ConfigController.h"
#include "input/Gamepad.h"
#include "input/GamepadButtonEvent.h"
#include "InputProfile.h"
#include "LogController.h"
#include "utils.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>
#ifdef BUILD_QT_MULTIMEDIA
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
#include <QCameraInfo>
#include <QVideoSurfaceFormat>
#else
#include <QCameraDevice>
#include <QMediaDevices>
#endif
#endif

#include <mgba/core/interface.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/multipad.h>
#include <mgba-util/configuration.h>

#include <cstdio>

using namespace QGBA;

int InputController::s_claimedPlayers = 0;
QList<InputController*> InputController::s_controllers;

static const char* s_extendedKeyNames[GBA_EXT_KEY_MAX] = {
	"X",
	"Y",
	"ZL",
	"ZR",
	"L3",
	"R3",
	"Guide",
	"Back",
};

// Each player gets its own config sections: "gba.pN.input.*" for the GBA
// buttons of players 2..8, "gba.pN.ext.input.*" for the extended buttons.
static char s_playerPlatformNames[InputController::MAX_CONTROLLERS][16];
static char s_extendedPlatformNames[InputController::MAX_CONTROLLERS][16];
static mInputPlatformInfo s_playerInfo[InputController::MAX_CONTROLLERS];
static mInputPlatformInfo s_extendedInfo[InputController::MAX_CONTROLLERS];

static void _initPlayerInfo() {
	static bool inited = false;
	if (inited) {
		return;
	}
	inited = true;
	for (int i = 0; i < InputController::MAX_CONTROLLERS; ++i) {
		snprintf(s_playerPlatformNames[i], sizeof(s_playerPlatformNames[i]), "gba.p%i", i + 1);
		snprintf(s_extendedPlatformNames[i], sizeof(s_extendedPlatformNames[i]), "gba.p%i.ext", i + 1);
		s_playerInfo[i] = GBAInputInfo;
		s_playerInfo[i].platformName = s_playerPlatformNames[i];
		s_extendedInfo[i].platformName = s_extendedPlatformNames[i];
		s_extendedInfo[i].keyId = s_extendedKeyNames;
		s_extendedInfo[i].nKeys = GBA_EXT_KEY_MAX;
		s_extendedInfo[i].hat = { -1, -1, -1, -1 };
	}
}

InputController::InputController(QWidget* topLevel, QObject* parent)
	: QObject(parent)
	, m_playerId(claimPlayer())
	, m_topLevel(topLevel)
	, m_focusParent(topLevel)
{
	mInputMapInit(&m_inputMap, &GBAInputInfo);
	_initPlayerInfo();
	for (int i = 0; i < MAX_CONTROLLERS; ++i) {
		mInputMapInit(&m_playerMaps[i], &s_playerInfo[i]);
		mInputMapInit(&m_extendedMaps[i], &s_extendedInfo[i]);
	}

	// A second window means a link session: free the extra players' pads
	// before the new window claims its own
	for (InputController* other : s_controllers) {
		other->updateExtraPlayers();
	}
	s_controllers.append(this);

	connect(&m_gamepadTimer, &QTimer::timeout, [this]() {
		for (auto& driver : m_inputDrivers) {
			if (driver->supportsPolling() && driver->supportsGamepads()) {
				testGamepad(driver->type());
			}
		}
		update();
	});

	m_gamepadTimer.setInterval(POLL_INTERVAL_MS);
	m_gamepadTimer.start();

#ifdef BUILD_QT_MULTIMEDIA
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	m_captureSession.setVideoSink(&m_videoSink);
	connect(&m_videoSink, &QVideoSink::videoFrameChanged, &m_videoDumper, &VideoDumper::present);
#endif
	connect(&m_videoDumper, &VideoDumper::imageAvailable, this, &InputController::setCamImage);
#endif

	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_X, GBA_KEY_A);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Z, GBA_KEY_B);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_A, GBA_KEY_L);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_S, GBA_KEY_R);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Return, GBA_KEY_START);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Backspace, GBA_KEY_SELECT);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Up, GBA_KEY_UP);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Down, GBA_KEY_DOWN);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Left, GBA_KEY_LEFT);
	mInputBindKey(&m_inputMap, KEYBOARD, Qt::Key_Right, GBA_KEY_RIGHT);


#ifdef M_CORE_GBA
	m_lux.p = this;
	m_lux.sample = [](GBALuminanceSource* context) {
		InputControllerLux* lux = static_cast<InputControllerLux*>(context);
		lux->value = 0xFF - lux->p->m_luxValue;
	};

	m_lux.readLuminance = [](GBALuminanceSource* context) {
		InputControllerLux* lux = static_cast<InputControllerLux*>(context);
		return lux->value;
	};
	setLuminanceLevel(0);
#endif

	m_image.p = this;
	m_image.startRequestImage = [](mImageSource* context, unsigned w, unsigned h, int) {
		InputControllerImage* image = static_cast<InputControllerImage*>(context);
		image->w = w;
		image->h = h;
		if (image->image.isNull()) {
			image->image.load(":/res/no-cam.png");
		}
#ifdef BUILD_QT_MULTIMEDIA
		image->p->m_cameraActive = true;
		QByteArray camera = image->p->m_config->getQtOption("camera").toByteArray();
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		if (!camera.isNull()) {
			image->p->m_cameraDevice = camera;
		}
#else
		if (!camera.isNull()) {
			for (const auto& cam : QMediaDevices::videoInputs()) {
				if (cam.id() == camera) {
					image->p->m_cameraDevice = cam;
				}
			}
		}
#endif
		QMetaObject::invokeMethod(image->p, "setupCam");
#endif
	};

	m_image.stopRequestImage = [](mImageSource* context) {
#ifdef BUILD_QT_MULTIMEDIA
		InputControllerImage* image = static_cast<InputControllerImage*>(context);
		image->p->m_cameraActive = false;
		QMetaObject::invokeMethod(image->p, "teardownCam");
#else
		UNUSED(context);
#endif
	};

	m_image.requestImage = [](mImageSource* context, const void** buffer, size_t* stride, mColorFormat* format) {
		InputControllerImage* image = static_cast<InputControllerImage*>(context);
		QSize size;
		{
			QMutexLocker locker(&image->mutex);
			if (image->outOfDate) {
				image->resizedImage = image->image.scaled(image->w, image->h, Qt::KeepAspectRatioByExpanding);
				image->resizedImage = image->resizedImage.convertToFormat(QImage::Format_RGB16);
				image->outOfDate = false;
			}
		}
		size = image->resizedImage.size();
		const uint16_t* bits = reinterpret_cast<const uint16_t*>(image->resizedImage.constBits());
		if (size.width() > image->w) {
			bits += (size.width() - image->w) / 2;
		}
		if (size.height() > image->h) {
			bits += ((size.height() - image->h) / 2) * size.width();
		}
		*buffer = bits;
		*stride = image->resizedImage.bytesPerLine() / sizeof(*bits);
		*format = mCOLOR_RGB565;
	};
}

InputController::~InputController() {
	s_controllers.removeAll(this);
	// Release our pads before the remaining windows reclaim them
	m_inputDrivers.clear();
	freePlayer(m_playerId);
	for (InputController* other : s_controllers) {
		other->updateExtraPlayers();
	}

	mInputMapDeinit(&m_inputMap);
	for (int i = 0; i < MAX_CONTROLLERS; ++i) {
		mInputMapDeinit(&m_playerMaps[i]);
		mInputMapDeinit(&m_extendedMaps[i]);
	}
}

void InputController::addInputDriver(std::shared_ptr<InputDriver> driver) {
	m_inputDrivers[driver->type()] = driver;
	driver->setPlayerId(m_playerId);
	if (!m_sensorDriver && driver->supportsSensors()) {
		m_sensorDriver = driver->type();
	}
}

void InputController::setConfiguration(ConfigController* config) {
	m_config = config;
	m_multiController = config->getOption("multiController").toInt();
	m_extendedButtons = config->getOption("extendedButtons").toInt();
	loadConfiguration(KEYBOARD);
	for (auto& driver : m_inputDrivers) {
		driver->loadConfiguration(config);
		if (driver->supportsGamepads()) {
			loadPlayerConfiguration(driver->type());
			for (int player = 2; player <= MAX_CONTROLLERS; ++player) {
				driver->setPlayerMap(player, playerMap(player));
			}
		}
	}
	m_multiController = config->getOption("multiController").toInt();
	m_extendedButtons = config->getOption("extendedButtons").toInt();
	updateExtraPlayers();
}

struct _AxisKeySearch {
	int key;
	bool found;
};

static void _findAxisKey(int, const mInputAxis* description, void* user) {
	_AxisKeySearch* search = static_cast<_AxisKeySearch*>(user);
	if (description->highDirection == search->key || description->lowDirection == search->key) {
		search->found = true;
	}
}

struct _AxisFill {
	mInputMap* map;
	uint32_t type;
};

static void _fillAxis(int axis, const mInputAxis* description, void* user) {
	_AxisFill* fill = static_cast<_AxisFill*>(user);
	if (!mInputQueryAxis(fill->map, fill->type, axis)) {
		mInputBindAxis(fill->map, fill->type, axis, description);
	}
}

// Auto-fill: everything that is unbound gets its default. A key with no
// button and no axis gets its default button (unless that button is already
// used for something else), and every default axis that is free is bound, so
// the d-pad directions end up on both the pad's d-pad and its left stick.
static void _fillMissing(mInputMap* map, const mInputMap* defaults, uint32_t type, int nKeys) {
	for (int key = 0; key < nKeys; ++key) {
		if (mInputQueryBinding(map, type, key) >= 0) {
			continue;
		}
		_AxisKeySearch search{key, false};
		mInputEnumerateAxes(map, type, _findAxisKey, &search);
		if (search.found) {
			continue;
		}
		int button = mInputQueryBinding(defaults, type, key);
		if (button >= 0 && mInputMapKey(map, type, button) < 0) {
			mInputBindKey(map, type, button, key);
		}
	}
	_AxisFill fill{map, type};
	mInputEnumerateAxes(defaults, type, _fillAxis, &fill);
}

void InputController::fillPlayerDefaults(mInputMap* map, uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver || !driver->supportsGamepads()) {
		return;
	}
	mInputMap defaults;
	mInputMapInit(&defaults, map->info);
	driver->bindPlayerDefaults(&defaults);
	_fillMissing(map, &defaults, type, map->info->nKeys);
	mInputMapDeinit(&defaults);
}

void InputController::loadPlayerConfiguration(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver || !m_config) {
		return;
	}
	for (int player = 2; player <= MAX_CONTROLLERS; ++player) {
		mInputMap* map = playerMap(player);
		mInputMapLoad(map, type, m_config->input());
		fillPlayerDefaults(map, type);
	}
	for (int player = 1; player <= MAX_CONTROLLERS; ++player) {
		mInputMapLoad(extendedMap(player), type, m_config->input());
	}
	fillExtendedDefaults(type);
}

// With extended buttons on, every unbound extended button gets its default,
// also right when the toggle is switched on.
void InputController::fillExtendedDefaults(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver || !driver->supportsGamepads() || !m_extendedButtons) {
		return;
	}
	for (int player = 1; player <= MAX_CONTROLLERS; ++player) {
		mInputMap* map = extendedMap(player);
		mInputMap defaults;
		mInputMapInit(&defaults, map->info);
		driver->bindExtendedDefaults(&defaults);
		_fillMissing(map, &defaults, type, map->info->nKeys);
		mInputMapDeinit(&defaults);
	}
}

mInputMap* InputController::playerMap(int player) {
	if (player <= 1 || player > MAX_CONTROLLERS) {
		return &m_inputMap;
	}
	return &m_playerMaps[player - 1];
}

mInputMap* InputController::extendedMap(int player) {
	if (player < 1 || player > MAX_CONTROLLERS) {
		player = 1;
	}
	return &m_extendedMaps[player - 1];
}

InputMapper InputController::playerMapper(int player, uint32_t type) {
	return InputMapper(playerMap(player), type);
}

InputMapper InputController::extendedMapper(int player, uint32_t type) {
	return InputMapper(extendedMap(player), type);
}

void InputController::savePlayerConfiguration(int player, uint32_t type) {
	if (!m_config) {
		return;
	}
	if (player > 1) {
		mInputMapSave(playerMap(player), type, m_config->input());
	}
	mInputMapSave(extendedMap(player), type, m_config->input());
	m_config->write();
}

void InputController::setMultiControllerEnabled(bool enabled) {
	if (m_multiController == enabled) {
		return;
	}
	m_multiController = enabled;
	updateExtraPlayers();
}

void InputController::setExtendedButtonsEnabled(bool enabled) {
	m_extendedButtons = enabled;
	for (auto& driver : m_inputDrivers) {
		if (driver->supportsGamepads()) {
			fillExtendedDefaults(driver->type());
		}
	}
	emit playersChanged();
}

bool InputController::isLinked() {
	int claimed = 0;
	for (int i = 0; i < MAX_GBAS; ++i) {
		if (s_claimedPlayers & (1 << i)) {
			++claimed;
		}
	}
	return claimed > 1;
}

bool InputController::multiControllerActive() const {
	return m_multiController && m_playerId == 0 && !isLinked();
}

bool InputController::extendedButtonsActive() const {
	return m_extendedButtons && multiControllerActive();
}

void InputController::updateExtraPlayers() {
	bool active = multiControllerActive();
	for (auto& driver : m_inputDrivers) {
		driver->setExtraPlayersEnabled(active);
	}
	if (!active) {
		setEventPlayer(1);
	}
	emit playersChanged();
}

void InputController::pollPlayers(uint16_t* keys, uint16_t* extendedKeys) {
	bool active = multiControllerActive();
	bool extended = extendedButtonsActive();
	for (int player = 1; player <= MAX_CONTROLLERS; ++player) {
		keys[player - 1] = 0;
		extendedKeys[player - 1] = 0;
		if (!active || (player == 1 && !extended)) {
			continue;
		}
		for (auto& driver : m_inputDrivers) {
			if (!driver->supportsGamepads()) {
				continue;
			}
			QList<bool> buttons = driver->playerButtons(player);
			QList<int16_t> axes = driver->playerAxes(player);
			QList<GamepadHatEvent::Direction> hats = driver->playerHats(player);
			if (buttons.isEmpty() && axes.isEmpty() && hats.isEmpty()) {
				continue;
			}
			if (player > 1) {
				InputMapper im(playerMapper(player, driver->type()));
				keys[player - 1] |= im.mapKeys(buttons) | im.mapAxes(axes) | im.mapHats(hats);
			}
			if (extended) {
				InputMapper im(extendedMapper(player, driver->type()));
				extendedKeys[player - 1] |= im.mapKeys(buttons) | im.mapAxes(axes) | im.mapHats(hats);
			}
		}
	}
}

QString InputController::playerGamepadName(int player, uint32_t type) {
	if (!type) {
		type = m_gamepadDriver;
	}
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return {};
	}
	return driver->playerGamepadName(player);
}

int InputController::playerGamepadIndex(int player, uint32_t type) {
	if (!type) {
		type = m_gamepadDriver;
	}
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return -1;
	}
	return driver->playerGamepadIndex(player);
}

void InputController::setPlayerGamepad(int player, uint32_t type, int index) {
	if (!type) {
		type = m_gamepadDriver;
	}
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return;
	}
	driver->setPlayerGamepad(player, index);
	emit playersChanged();
}

void InputController::setEventPlayer(int player) {
	if (player < 1 || player > MAX_CONTROLLERS || (player > 1 && !multiControllerActive())) {
		player = 1;
	}
	QWriteLocker l(&m_eventsLock);
	m_eventPlayer = player;
}

QList<bool> InputController::eventButtons(uint32_t type) {
	if (m_eventPlayer > 1) {
		auto driver = m_inputDrivers.value(type);
		return driver ? driver->playerButtons(m_eventPlayer) : QList<bool>();
	}
	std::shared_ptr<Gamepad> pad = gamepad(type);
	return pad ? pad->currentButtons() : QList<bool>();
}

QList<int16_t> InputController::eventAxes(uint32_t type) {
	if (m_eventPlayer > 1) {
		auto driver = m_inputDrivers.value(type);
		return driver ? driver->playerAxes(m_eventPlayer) : QList<int16_t>();
	}
	std::shared_ptr<Gamepad> pad = gamepad(type);
	return pad ? pad->currentAxes() : QList<int16_t>();
}

QList<GamepadHatEvent::Direction> InputController::eventHats(uint32_t type) {
	if (m_eventPlayer > 1) {
		auto driver = m_inputDrivers.value(type);
		return driver ? driver->playerHats(m_eventPlayer) : QList<GamepadHatEvent::Direction>();
	}
	std::shared_ptr<Gamepad> pad = gamepad(type);
	return pad ? pad->currentHats() : QList<GamepadHatEvent::Direction>();
}

bool InputController::loadConfiguration(uint32_t type) {
	bool loaded = mInputMapLoad(&m_inputMap, type, m_config->input());
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return false;
	}
	if (!loaded) {
		driver->bindDefaults(this);
	}
	fillPlayerDefaults(&m_inputMap, type);
	driver->loadConfiguration(m_config);
	return loaded;
}

bool InputController::loadProfile(uint32_t type, const QString& profile) {
	if (profile.isEmpty()) {
		return false;
	}
	bool loaded = mInputProfileLoad(&m_inputMap, type, m_config->input(), profile.toUtf8().constData());
	if (!loaded) {
		const InputProfile* ip = InputProfile::findProfile(profile);
		if (ip) {
			ip->apply(this);
			loaded = true;
		}
	}
	fillPlayerDefaults(&m_inputMap, type);
	emit profileLoaded(profile);
	return loaded;
}

void InputController::saveConfiguration() {
	saveConfiguration(KEYBOARD);
	for (auto& driver : m_inputDrivers) {
		driver->saveConfiguration(m_config);
	}
	m_config->write();
}

void InputController::saveConfiguration(uint32_t type) {
	mInputMapSave(&m_inputMap, type, m_config->input());
	auto driver = m_inputDrivers.value(type);
	if (driver) {
		driver->saveConfiguration(m_config);
	}
	m_config->write();
}

void InputController::saveProfile(uint32_t type, const QString& profile) {
	if (profile.isEmpty()) {
		return;
	}
	mInputProfileSave(&m_inputMap, type, m_config->input(), profile.toUtf8().constData());
	m_config->write();
}

QString InputController::profileForType(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return {};
	}
	return driver->currentProfile();
}

void InputController::setGamepadDriver(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver || !driver->supportsGamepads()) {
		return;
	}
	m_gamepadDriver = type;
}

QStringList InputController::connectedGamepads(uint32_t type) const {
	if (!type) {
		type = m_gamepadDriver;
	}
	const auto& driver = m_inputDrivers.value(type);
	if (!driver) {
		return {};
	}

	QStringList pads;
	for (auto& pad : driver->connectedGamepads()) {
		pads.append(pad->visibleName());
	}
	return pads;
}

int InputController::gamepadIndex(uint32_t type) const {
	if (!type) {
		type = m_gamepadDriver;
	}
	const auto& driver = m_inputDrivers.value(type);
	if (!driver) {
		return -1;
	}
	return driver->activeGamepadIndex();
}

void InputController::setGamepad(uint32_t type, int index) {
	if (!type) {
		type = m_gamepadDriver;
	}
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return;
	}
	driver->setActiveGamepad(index);
}

void InputController::setGamepad(int index) {
	setGamepad(0, index);
}

void InputController::setPreferredGamepad(uint32_t type, int index) {
	if (!m_config) {
		return;
	}
	if (!type) {
		type = m_gamepadDriver;
	}
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return;
	}

	auto pads = driver->connectedGamepads();
	if (index >= pads.count()) {
		return;
	}

	QString name = pads[index]->name();
	if (name.isEmpty()) {
		return;
	}
	mInputSetPreferredDeviceType(m_config->input(), "gba", type, m_playerId, name.toUtf8().constData());

	QString serial = pads[index]->serial();
	if (serial.isEmpty()) {
		return;
	}
	mInputSetPreferredDeviceSerial(m_config->input(), "gba", type, m_playerId, serial.toUtf8().constData());
}

void InputController::setPreferredGamepad(int index) {
	setPreferredGamepad(0, index);
}

InputMapper InputController::mapper(uint32_t type) {
	return InputMapper(&m_inputMap, type);
}

InputMapper InputController::mapper(InputDriver* driver) {
	return InputMapper(&m_inputMap, driver->type());
}

InputMapper InputController::mapper(InputSource* source) {
	return InputMapper(&m_inputMap, source->type());
}

void InputController::setSensorDriver(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver || !driver->supportsSensors()) {
		return;
	}
	m_sensorDriver = type;
}


mRumble* InputController::rumble() {
	auto driver = m_inputDrivers.value(m_sensorDriver);
	if (driver) {
		return driver->rumble();
	}
	return nullptr;
}

mRotationSource* InputController::rotationSource() {
	auto driver = m_inputDrivers.value(m_sensorDriver);
	if (driver) {
		return driver->rotationSource();
	}
	return nullptr;
}

int InputController::mapKeyboard(int key) const {
	return mInputMapKey(&m_inputMap, KEYBOARD, key);
}

void InputController::update() {
	for (auto& driver : m_inputDrivers) {
		QString profile = profileForType(driver->type());
		driver->update();
		QString newProfile = profileForType(driver->type());
		if (profile != newProfile) {
			loadProfile(driver->type(), newProfile);
		}
	}
	emit updated();
}

int InputController::pollEvents() {
	int activeButtons = 0;
	for (auto& pad : gamepads()) {
		InputMapper im(mapper(pad.get()));
		activeButtons |= im.mapKeys(pad->currentButtons());
		activeButtons |= im.mapAxes(pad->currentAxes());
		activeButtons |= im.mapHats(pad->currentHats());
	}
	QReadLocker l(&m_eventsLock);
	for (int i = 0; i < GBA_KEY_MAX; ++i) {
		if ((activeButtons & (1 << i)) && hasPendingEvent(i)) {
			activeButtons ^= 1 << i;
		}
	}
	return activeButtons;
}

std::shared_ptr<Gamepad> InputController::gamepad(uint32_t type) {
	auto driver = m_inputDrivers.value(type);
	if (!driver) {
		return nullptr;
	}
	if (!driver->supportsGamepads()) {
		return nullptr;
	}

	return driver->activeGamepad();
}

QList<std::shared_ptr<Gamepad>> InputController::gamepads() {
	QList<std::shared_ptr<Gamepad>> pads;
	for (auto& driver : m_inputDrivers) {
		if (!driver->supportsGamepads()) {
			continue;
		}
		std::shared_ptr<Gamepad> pad = driver->activeGamepad();
		if (pad) {
			pads.append(pad);
		}
	}
	return pads;
}

QSet<int> InputController::activeGamepadButtons(uint32_t type) {
	QSet<int> activeButtons;
	auto allButtons = eventButtons(type);
	for (int i = 0; i < allButtons.size(); ++i) {
		if (allButtons[i]) {
			activeButtons.insert(i);
		}
	}
	return activeButtons;
}

QSet<QPair<int, GamepadAxisEvent::Direction>> InputController::activeGamepadAxes(uint32_t type) {
	QSet<QPair<int, GamepadAxisEvent::Direction>> activeAxes;
	InputMapper im(mapper(type));
	auto allAxes = eventAxes(type);
	for (int i = 0; i < allAxes.size(); ++i) {
		if (allAxes[i] - im.axisCenter(i) >= im.axisThreshold(i)) {
			activeAxes.insert(qMakePair(i, GamepadAxisEvent::POSITIVE));
			continue;
		}
		if (allAxes[i] - im.axisCenter(i) <= -im.axisThreshold(i)) {
			activeAxes.insert(qMakePair(i, GamepadAxisEvent::NEGATIVE));
			continue;
		}
	}
	return activeAxes;
}

QSet<QPair<int, GamepadHatEvent::Direction>> InputController::activeGamepadHats(uint32_t type) {
	QSet<QPair<int, GamepadHatEvent::Direction>> activeHats;
	auto allHats = eventHats(type);
	for (int i = 0; i < allHats.size(); ++i) {
		if (allHats[i] != GamepadHatEvent::CENTER) {
			activeHats.insert(qMakePair(i, allHats[i]));
		}
	}
	return activeHats;
}

void InputController::testGamepad(uint32_t type) {
	QWriteLocker l(&m_eventsLock);
	auto activeAxes = activeGamepadAxes(type);
	auto oldAxes = m_activeAxes;
	m_activeAxes = activeAxes;

	auto activeButtons = activeGamepadButtons(type);
	auto oldButtons = m_activeButtons;
	m_activeButtons = activeButtons;

	auto activeHats = activeGamepadHats(type);
	auto oldHats = m_activeHats;
	m_activeHats = activeHats;

	if (!QApplication::focusWidget()) {
		return;
	}

	activeAxes.subtract(oldAxes);
	oldAxes.subtract(m_activeAxes);

	for (auto& axis : m_activeAxes) {
		bool newlyAboveThreshold = activeAxes.contains(axis);
		if (newlyAboveThreshold) {
			GamepadAxisEvent* event = new GamepadAxisEvent(axis.first, axis.second, newlyAboveThreshold, type, this);
			postPendingEvent(event->platformKey());
			sendGamepadEvent(event);
			if (!event->isAccepted()) {
				clearPendingEvent(event->platformKey());
			}
		}
	}
	for (auto& axis : oldAxes) {
		GamepadAxisEvent* event = new GamepadAxisEvent(axis.first, axis.second, false, type, this);
		clearPendingEvent(event->platformKey());
		sendGamepadEvent(event);
	}

	if (!QApplication::focusWidget()) {
		return;
	}

	activeButtons.subtract(oldButtons);
	oldButtons.subtract(m_activeButtons);

	for (int button : activeButtons) {
		GamepadButtonEvent* event = new GamepadButtonEvent(GamepadButtonEvent::Down(), button, type, this);
		postPendingEvent(event->platformKey());
		sendGamepadEvent(event);
		if (!event->isAccepted()) {
			clearPendingEvent(event->platformKey());
		}
	}
	for (int button : oldButtons) {
		GamepadButtonEvent* event = new GamepadButtonEvent(GamepadButtonEvent::Up(), button, type, this);
		clearPendingEvent(event->platformKey());
		sendGamepadEvent(event);
	}

	activeHats.subtract(oldHats);
	oldHats.subtract(m_activeHats);

	for (auto& hat : activeHats) {
		GamepadHatEvent* event = new GamepadHatEvent(GamepadHatEvent::Down(), hat.first, hat.second, type, this);
		postPendingEvents(event->platformKeys());
		sendGamepadEvent(event);
		if (!event->isAccepted()) {
			clearPendingEvents(event->platformKeys());
		}
	}
	for (auto& hat : oldHats) {
		GamepadHatEvent* event = new GamepadHatEvent(GamepadHatEvent::Up(), hat.first, hat.second, type, this);
		clearPendingEvents(event->platformKeys());
		sendGamepadEvent(event);
	}
}

void InputController::sendGamepadEvent(QEvent* event) {
	QWidget* focusWidget = nullptr;
	if (m_focusParent) {
		focusWidget = m_focusParent->focusWidget();
		if (!focusWidget) {
			focusWidget = m_focusParent;
		}
	} else {
		focusWidget = QApplication::focusWidget();
	}
	QApplication::postEvent(focusWidget, event, Qt::HighEventPriority);
}

void InputController::postPendingEvent(int key) {
	m_pendingEvents.insert(key);
}

void InputController::clearPendingEvent(int key) {
	m_pendingEvents.remove(key);
}

void InputController::postPendingEvents(int keys) {
	for (int i = 0; keys; ++i, keys >>= 1) {
		if (keys & 1) {
			m_pendingEvents.insert(i);
		}
	}
}

void InputController::clearPendingEvents(int keys) {
	for (int i = 0; keys; ++i, keys >>= 1) {
		if (keys & 1) {
			m_pendingEvents.remove(i);
		}
	}
}

bool InputController::hasPendingEvent(int key) const {
	return m_pendingEvents.contains(key);
}

int InputController::claimPlayer() {
	for (int i = 0; i < MAX_GBAS; ++i) {
		if (!(s_claimedPlayers & (1 << i))) {
			s_claimedPlayers |= 1 << i;
			return i;
		}
	}
	qFatal("Can't claim 5th player. Please report this bug.");
}

void InputController::freePlayer(int player) {
	s_claimedPlayers &= ~(1 << player);
}

void InputController::stealFocus(QWidget* focus) {
	m_focusParent = focus;
}

void InputController::releaseFocus(QWidget* focus) {
	if (focus == m_focusParent) {
		m_focusParent = m_topLevel;
	}
}

void InputController::loadCamImage(const QString& path) {
	setCamImage(QImage(path));
}

void InputController::setCamImage(const QImage& image) {
	if (image.isNull()) {
		return;
	}
	QMutexLocker locker(&m_image.mutex);
	m_image.image = image;
	m_image.resizedImage = QImage();
	m_image.outOfDate = true;
}

QList<QPair<QByteArray, QString>> InputController::listCameras() const {
	QList<QPair<QByteArray, QString>> out;
#ifdef BUILD_QT_MULTIMEDIA
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	for (const auto& cam : QCameraInfo::availableCameras()) {
		out.append(qMakePair(cam.deviceName().toLatin1(), cam.description()));
#else
	for (const auto& cam : QMediaDevices::videoInputs()) {
		out.append(qMakePair(cam.id(), cam.description()));
#endif
	}
#endif
	return out;
}

void InputController::increaseLuminanceLevel() {
	setLuminanceLevel(m_luxLevel + 1);
}

void InputController::decreaseLuminanceLevel() {
	setLuminanceLevel(m_luxLevel - 1);
}

void InputController::setLuminanceLevel(int level) {
	int value = 0x16;
	level = clamp(level, 0, 10);
	if (level > 0) {
		value += GBA_LUX_LEVELS[level - 1];
	}
	setLuminanceValue(value);
}

void InputController::setLuminanceValue(uint8_t value) {
	m_luxValue = value;
	value = std::max<int>(value - 0x16, 0);
	m_luxLevel = 10;
	for (int i = 0; i < 10; ++i) {
		if (value < GBA_LUX_LEVELS[i]) {
			m_luxLevel = i;
			break;
		}
	}
	emit luminanceValueChanged(m_luxValue);
}

void InputController::setupCam() {
#ifdef BUILD_QT_MULTIMEDIA
	if (m_config->getQtOption("cameraDriver").toInt() != static_cast<int>(CameraDriver::QT_MULTIMEDIA)) {
		return;
	}

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	if (!m_camera) {
		m_camera = std::make_unique<QCamera>(m_cameraDevice);
		connect(m_camera.get(), &QCamera::statusChanged, this, &InputController::prepareCamSettings, Qt::QueuedConnection);
	}
	if (m_camera->status() == QCamera::UnavailableStatus) {
		m_camera.reset();
		return;
	}
	m_camera->setCaptureMode(QCamera::CaptureVideo);
	m_camera->setViewfinder(&m_videoDumper);
	m_camera->load();
#else
	if (!m_camera) {
		m_camera = std::make_unique<QCamera>(m_cameraDevice);
		m_captureSession.setCamera(m_camera.get());
	}
	prepareCamFormat();
#endif
#endif
}

#ifdef BUILD_QT_MULTIMEDIA
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
void InputController::prepareCamSettings(QCamera::Status status) {
	if (status != QCamera::LoadedStatus || m_camera->state() == QCamera::ActiveState) {
		return;
	}
	prepareCamFormat();
}
#endif

void InputController::prepareCamFormat() {
	QSize size(1280, 720);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	QCameraViewfinderSettings settings;
	auto cameraRes = m_camera->supportedViewfinderResolutions(settings);
	for (auto& cameraSize : cameraRes) {
		if (cameraSize.width() < m_image.w || cameraSize.height() < m_image.h) {
			continue;
		}
		if (cameraSize.width() <= size.width() && cameraSize.height() <= size.height()) {
			size = cameraSize;
		}
	}
	settings.setResolution(size);

	auto cameraFormats = m_camera->supportedViewfinderPixelFormats(settings);
	auto goodFormats = m_videoDumper.supportedPixelFormats();
	bool goodFormatFound = false;
	for (const auto& goodFormat : goodFormats) {
		if (cameraFormats.contains(goodFormat)) {
			settings.setPixelFormat(goodFormat);
			goodFormatFound = true;
			break;
		}
	}
	if (!goodFormatFound) {
		LOG(QT, WARN) << tr("Could not find a valid camera format!");
		for (const auto& format : cameraFormats) {
			LOG(QT, WARN) << tr("Camera supported format: %1").arg(format);
		}
	}
	m_camera->setViewfinderSettings(settings);
#else
	bool goodFormatFound = false;
	auto goodFormats = m_videoDumper.supportedPixelFormats();
	QCameraFormat bestFormat;
	for (const auto& format : m_cameraDevice.videoFormats()) {
		if (!goodFormats.contains(format.pixelFormat())) {
			continue;
		}
		if (format.resolution().width() <= size.width() && format.resolution().height() <= size.height()) {
			size = format.resolution();
			bestFormat = format;
			goodFormatFound = true;
		}
	}
	if (!goodFormatFound) {
		LOG(QT, WARN) << tr("Could not find a valid camera format!");
	}
	m_camera->setCameraFormat(bestFormat);
#endif
	m_camera->start();
}
#endif

void InputController::teardownCam() {
#ifdef BUILD_QT_MULTIMEDIA
	if (m_camera) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		m_camera->unload();
#else
		m_captureSession.setCamera(nullptr);
#endif
		m_camera.reset();
	}
#endif
}

void InputController::setCamera(const QByteArray& name) {
#ifdef BUILD_QT_MULTIMEDIA
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	if (m_cameraDevice == name) {
		return;
	}
	m_cameraDevice = name;
	if (m_camera && m_camera->state() == QCamera::ActiveState) {
		teardownCam();
	}
#else
	if (m_cameraDevice.id() == name) {
		return;
	}
	for (const auto& cam : QMediaDevices::videoInputs()) {
		if (cam.id() == name) {
			m_cameraDevice = cam;
		}
	}
#endif
	if (m_cameraActive) {
		setupCam();
	}
#else
	UNUSED(name);
#endif
}
