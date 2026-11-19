/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QList>
#include <QPicture>
#include <QSet>
#include <QWidget>

#include <mgba/internal/gba/input.h>

class QComboBox;
class QTimer;

namespace QGBA {

class InputController;
class KeyEditor;

class GBAKeyEditor : public QWidget {
Q_OBJECT

public:
	GBAKeyEditor(InputController* controller, int type, const QString& profile = QString(), QWidget* parent = nullptr, int player = 1);

	int player() const { return m_player; }
	virtual ~GBAKeyEditor();

public slots:
	void setAll();
	void save();
#ifdef BUILD_SDL
	void updateJoysticks();
	// Re-reads the pad list and this player's pad without reassigning anything
	void refreshGamepads();
#endif

protected:
	virtual void resizeEvent(QResizeEvent*) override;
	virtual void paintEvent(QPaintEvent*) override;
	virtual bool event(QEvent*) override;
	virtual void closeEvent(QCloseEvent*) override;
	virtual bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
	void setNext();
	void refresh();
#ifdef BUILD_SDL
	void selectGamepad(int index);
#endif

private:
	static const qreal DPAD_CENTER_X;
	static const qreal DPAD_CENTER_Y;
	static const qreal DPAD_WIDTH;
	static const qreal DPAD_HEIGHT;

	void setLocation(QWidget* widget, qreal x, qreal y);

	void lookupBinding(const mInputMap*, KeyEditor*, int key);
	void bindKey(const KeyEditor*, int key);

	bool findFocus(KeyEditor* needle = nullptr);

#ifdef BUILD_SDL
	void lookupAxes(const mInputMap*);
	void lookupHats(const mInputMap*);
#endif

	KeyEditor* keyById(int);

	QComboBox* m_profileSelect = nullptr;
	QWidget* m_clear = nullptr;
	QWidget* m_buttons;
	KeyEditor* m_keyDU;
	KeyEditor* m_keyDD;
	KeyEditor* m_keyDL;
	KeyEditor* m_keyDR;
	KeyEditor* m_keySelect;
	KeyEditor* m_keyStart;
	KeyEditor* m_keyA;
	KeyEditor* m_keyB;
	KeyEditor* m_keyL;
	KeyEditor* m_keyR;
	QList<KeyEditor*> m_keyOrder;
	QList<KeyEditor*>::iterator m_currentKey;

	uint32_t m_type;
	QString m_profile;
	InputController* m_controller;
	int m_player;
	int m_shownGamepad = -2;

	QPicture m_background;
};

}
