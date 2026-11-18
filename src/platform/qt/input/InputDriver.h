/* Copyright (c) 2013-2023 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "input/GamepadHatEvent.h"

#include <QList>
#include <QString>
#include <QObject>

#include <memory>

struct mInputMap;
struct mRotationSource;
struct mRumble;

namespace QGBA {

class ConfigController;
class Gamepad;
class InputController;
class KeySource;

class InputDriver : public QObject {
Q_OBJECT

public:
	InputDriver(QObject* parent = nullptr);
	virtual ~InputDriver() = default;

	virtual void setPlayerId(int id);
	int playerId() const { return m_playerId; }

	virtual uint32_t type() const = 0;
	virtual QString visibleName() const = 0;
	virtual QString currentProfile() const = 0;

	virtual bool supportsPolling() const;
	virtual bool supportsGamepads() const;
	virtual bool supportsSensors() const;

	virtual void loadConfiguration(ConfigController*);
	virtual void saveConfiguration(ConfigController*);

	virtual void bindDefaults(InputController*);

	virtual bool update() = 0;

	virtual QList<KeySource*> connectedKeySources() const;
	virtual QList<std::shared_ptr<Gamepad>> connectedGamepads() const;

	virtual int activeKeySourceIndex() const;
	virtual int activeGamepadIndex() const;

	KeySource* activeKeySource();
	std::shared_ptr<Gamepad> activeGamepad();

	virtual void setActiveKeySource(int);
	virtual void setActiveGamepad(int);

	virtual void registerTiltAxisX(int axis);
	virtual void registerTiltAxisY(int axis);
	virtual void registerGyroAxisX(int axis);
	virtual void registerGyroAxisY(int axis);
	virtual void registerGyroAxisZ(int axis);

	virtual float gyroSensitivity() const;
	virtual void setGyroSensitivity(float sensitivity);

	virtual mRumble* rumble();
	virtual mRotationSource* rotationSource();

	// Multi-controller support. Players are 1-based; player 1 is the
	// driver's normal active gamepad.
	virtual void setPlayerMap(int player, mInputMap* map);
	virtual void setExtraPlayersEnabled(bool enabled);
	virtual bool extraPlayersEnabled() const;
	virtual QList<bool> playerButtons(int player);
	virtual QList<int16_t> playerAxes(int player);
	virtual QList<GamepadHatEvent::Direction> playerHats(int player);
	virtual QString playerGamepadName(int player);
	virtual int playerGamepadIndex(int player);
	virtual void setPlayerGamepad(int player, int index);
	virtual void bindPlayerDefaults(mInputMap* map);
	virtual void bindExtendedDefaults(mInputMap* map);

private:
	int m_playerId = -1;
};

}
