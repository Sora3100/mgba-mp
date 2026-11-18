/* Copyright (c) 2026 mgba-mp contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ExtendedKeyEditor.h"
#include "moc_ExtendedKeyEditor.cpp"

#include "input/InputDriver.h"
#include "input/InputMapper.h"
#include "InputController.h"
#include "KeyEditor.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

#include <mgba/internal/gba/multipad.h>

using namespace QGBA;

ExtendedKeyEditor::ExtendedKeyEditor(InputController* controller, uint32_t type, int player, QWidget* parent)
	: QWidget(parent)
	, m_controller(controller)
	, m_type(type)
	, m_player(player)
{
	static const char* const names[GBA_EXT_KEY_MAX] = {
		QT_TR_NOOP("X"),
		QT_TR_NOOP("Y"),
		QT_TR_NOOP("ZL / L2"),
		QT_TR_NOOP("ZR / R2"),
		QT_TR_NOOP("L3"),
		QT_TR_NOOP("R3"),
		QT_TR_NOOP("Guide / Home"),
		QT_TR_NOOP("Back"),
	};

	QGridLayout* layout = new QGridLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	QLabel* title = new QLabel(tr("Extended buttons (player %1)").arg(player));
	layout->addWidget(title, 0, 0, 1, 8, Qt::AlignHCenter);
	for (int i = 0; i < GBA_EXT_KEY_MAX; ++i) {
		KeyEditor* key = new KeyEditor(this);
		key->setInputController(controller);
		m_keys.append(key);
		int row = 1 + i / 4;
		int column = (i % 4) * 2;
		layout->addWidget(new QLabel(tr(names[i])), row, column, Qt::AlignRight);
		layout->addWidget(key, row, column + 1);
	}
	QPushButton* defaults = new QPushButton(tr("Restore defaults"));
	connect(defaults, &QAbstractButton::pressed, this, &ExtendedKeyEditor::resetDefaults);
	layout->addWidget(defaults, 3, 0, 1, 8, Qt::AlignHCenter);

	refresh();
}

void ExtendedKeyEditor::refresh() {
	const mInputMap* map = m_controller->extendedMap(m_player);
	for (int i = 0; i < m_keys.count(); ++i) {
		m_keys[i]->setValueButton(mInputQueryBinding(map, m_type, i));
	}
	mInputEnumerateAxes(map, m_type, [](int axis, const mInputAxis* description, void* user) {
		ExtendedKeyEditor* self = static_cast<ExtendedKeyEditor*>(user);
		if (description->highDirection >= 0 && description->highDirection < self->m_keys.count()) {
			self->m_keys[description->highDirection]->setValueAxis(axis, GamepadAxisEvent::POSITIVE);
		}
		if (description->lowDirection >= 0 && description->lowDirection < self->m_keys.count()) {
			self->m_keys[description->lowDirection]->setValueAxis(axis, GamepadAxisEvent::NEGATIVE);
		}
	}, this);
}

void ExtendedKeyEditor::save() {
	InputMapper mapper = m_controller->extendedMapper(m_player, m_type);
	mapper.unbindAllAxes();
	for (int i = 0; i < m_keys.count(); ++i) {
		const KeyEditor* key = m_keys[i];
		if (key->axis() >= 0) {
			mapper.bindAxis(key->axis(), key->direction(), i);
		}
		mapper.bindKey(key->value(), i);
	}
	m_controller->savePlayerConfiguration(m_player, m_type);
}

void ExtendedKeyEditor::resetDefaults() {
	mInputMap* map = m_controller->extendedMap(m_player);
	InputDriver* driver = m_controller->gamepadDriver();
	if (!driver) {
		return;
	}
	InputMapper mapper(map, m_type);
	mapper.unbindAllKeys();
	mapper.unbindAllAxes();
	driver->bindExtendedDefaults(map);
	refresh();
}
