/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QList>
#include <QWidget>

namespace QGBA {

class InputController;
class KeyEditor;

// Bindings of the extended host buttons (X, Y, ZL, ZR, L3, R3, Guide, Back)
// of one player. They are exposed to the game through the multi-controller
// registers only, never through KEYINPUT.
class ExtendedKeyEditor : public QWidget {
Q_OBJECT

public:
	ExtendedKeyEditor(InputController* controller, uint32_t type, int player, QWidget* parent = nullptr);

	int player() const { return m_player; }

public slots:
	void refresh();
	void save();
	void resetDefaults();

private:
	InputController* m_controller;
	uint32_t m_type;
	int m_player;
	QList<KeyEditor*> m_keys;
};

}
