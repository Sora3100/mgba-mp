/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ControllersView.h"
#include "moc_ControllersView.cpp"

#include "ConfigController.h"
#include "ExtendedKeyEditor.h"
#include "GBAKeyEditor.h"
#include "InputController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

using namespace QGBA;

ControllersView::ControllersView(ConfigController* config, InputController* controller, uint32_t type, const QString& profile, QWidget* parent)
	: QWidget(parent)
	, m_config(config)
	, m_controller(controller)
	, m_type(type)
{
	QVBoxLayout* layout = new QVBoxLayout(this);

	// Player selector, centred at the top of the page
	QHBoxLayout* selectorRow = new QHBoxLayout;
	selectorRow->addStretch();
	QLabel* selectorLabel = new QLabel(tr("Controller:"));
	m_playerSelect = new QComboBox;
	m_playerSelect->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	selectorLabel->setBuddy(m_playerSelect);
	selectorRow->addWidget(selectorLabel);
	selectorRow->addWidget(m_playerSelect);
	selectorRow->addStretch();
	layout->addLayout(selectorRow);

	QHBoxLayout* toggleRow = new QHBoxLayout;
	toggleRow->addStretch();
	m_multiController = new QCheckBox(tr("Enable multiple controllers"));
	m_multiController->setToolTip(tr("Players 2-8 each use their own gamepad, assigned in plug-in order"));
	m_extendedButtons = new QCheckBox(tr("Enable extended buttons"));
	m_extendedButtons->setToolTip(tr("Expose X, Y, ZL, ZR, L3, R3, Guide and Back to games that support them"));
	toggleRow->addWidget(m_multiController);
	toggleRow->addWidget(m_extendedButtons);
	toggleRow->addStretch();
	layout->addLayout(toggleRow);

	m_linkNote = new QLabel(tr("Multiple controllers and extended buttons are disabled while link play is active."));
	m_linkNote->setAlignment(Qt::AlignHCenter);
	m_linkNote->setWordWrap(true);
	layout->addWidget(m_linkNote);

	m_pages = new QStackedWidget;
	for (int player = 1; player <= InputController::MAX_CONTROLLERS; ++player) {
		QWidget* page = new QWidget;
		QVBoxLayout* pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(0, 0, 0, 0);
		GBAKeyEditor* editor = new GBAKeyEditor(controller, type, player == 1 ? profile : QString(), nullptr, player);
		ExtendedKeyEditor* extended = new ExtendedKeyEditor(controller, type, player);
		pageLayout->addWidget(editor, 1);
		pageLayout->addWidget(extended);
		m_editors.append(editor);
		m_extendedEditors.append(extended);
		m_pages->addWidget(page);
	}
	layout->addWidget(m_pages, 1);

	m_multiController->setChecked(config->getOption("multiController").toInt());
	m_extendedButtons->setChecked(config->getOption("extendedButtons").toInt());

	connect(m_playerSelect, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ControllersView::selectPlayer);
	connect(m_multiController, &QAbstractButton::toggled, this, &ControllersView::updateEnabled);
	connect(m_extendedButtons, &QAbstractButton::toggled, this, &ControllersView::updateEnabled);
	connect(controller, &InputController::playersChanged, this, &ControllersView::updatePlayers);

	updatePlayers();
	updateEnabled();
}

void ControllersView::selectPlayer(int index) {
	if (index < 0) {
		return;
	}
	m_pages->setCurrentIndex(index);
	m_editors[index]->updateJoysticks();
}

void ControllersView::updatePlayers() {
	int current = m_playerSelect->currentIndex();
	bool wasBlocked = m_playerSelect->blockSignals(true);
	m_playerSelect->clear();
	for (int player = 1; player <= InputController::MAX_CONTROLLERS; ++player) {
		QString pad = m_controller->playerGamepadName(player, m_type);
		if (pad.isEmpty()) {
			pad = tr("no controller");
		}
		m_playerSelect->addItem(tr("Player %1 (%2)").arg(player).arg(pad));
	}
	m_playerSelect->setCurrentIndex(current < 0 ? 0 : current);
	m_playerSelect->blockSignals(wasBlocked);
}

void ControllersView::updateEnabled() {
	bool linked = InputController::isLinked();
	bool multi = m_multiController->isChecked() && !linked;
	m_multiController->setEnabled(!linked);
	m_extendedButtons->setEnabled(!linked);
	m_linkNote->setVisible(linked);

	// Players 2..8 can always be edited, but are only used while the option is on
	m_playerSelect->setEnabled(multi || m_controller->multiControllerActive());
	if (!m_playerSelect->isEnabled() && m_playerSelect->currentIndex() != 0) {
		m_playerSelect->setCurrentIndex(0);
	}

	bool extended = m_extendedButtons->isChecked() && !linked;
	for (ExtendedKeyEditor* editor : m_extendedEditors) {
		editor->setVisible(extended);
	}
}

void ControllersView::save() {
	for (GBAKeyEditor* editor : m_editors) {
		editor->save();
	}
	for (ExtendedKeyEditor* editor : m_extendedEditors) {
		editor->save();
	}
	if (!InputController::isLinked()) {
		m_config->setOption("multiController", m_multiController->isChecked());
		m_config->updateOption("multiController");
		m_config->setOption("extendedButtons", m_extendedButtons->isChecked());
		m_config->updateOption("extendedButtons");
	}
}
