/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QStackedWidget;

namespace QGBA {

class ConfigController;
class ExtendedKeyEditor;
class GBAKeyEditor;
class InputController;

// Controllers settings page: a player selector at the top switches which
// player's gamepad bindings are edited. Player 1 is the regular mGBA input;
// players 2..8 are only used while "Enable multiple controllers" is on.
class ControllersView : public QWidget {
Q_OBJECT

public:
	ControllersView(ConfigController* config, InputController* controller, uint32_t type, const QString& profile, QWidget* parent = nullptr);

public slots:
	void save();

private slots:
	void selectPlayer(int index);
	void updatePlayers();
	void updateEnabled();

private:
	ConfigController* m_config;
	InputController* m_controller;
	uint32_t m_type;

	QComboBox* m_playerSelect;
	QCheckBox* m_multiController;
	QCheckBox* m_extendedButtons;
	QLabel* m_linkNote;
	QStackedWidget* m_pages;
	QList<GBAKeyEditor*> m_editors;
	QList<ExtendedKeyEditor*> m_extendedEditors;
};

}
