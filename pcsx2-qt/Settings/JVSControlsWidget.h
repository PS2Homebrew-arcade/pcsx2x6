// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_JVSControlsWidget.h"

#include <QtWidgets/QWidget>

#include <span>
#include <string>

class ControllerSettingsWindow;
class QShowEvent;
class QComboBox;
class QVBoxLayout;
class QLabel;
struct InputBindingInfo;

class JVSControlsWidget final : public QWidget
{
	Q_OBJECT

public:
	JVSControlsWidget(QWidget* parent, ControllerSettingsWindow* dialog);
	~JVSControlsWidget() override;

protected:
	void showEvent(QShowEvent* event) override;

private:
	void refreshBoardType();
	void autoPickInputPage();
	void bindDIPSwitchWidgets();
	void bindSystemButtonWidgets();
	void buildDrumPage();
	void buildFightingPage();
	void buildRacingPage();
	void buildTwinstickPage();
	void buildLightgunPage();
	void buildStandardPage();
	void buildTouchPage();
	void refreshAimDevices();
	void refreshMacrosPage(); // rebuild the Macros view (layout dropdown + per-layout rows)
	void buildMacroRows(QVBoxLayout* rowsLayout, const std::string& layoutKey, std::span<const InputBindingInfo> lbuttons);

	Ui::JVSControlsWidget m_ui;
	ControllerSettingsWindow* m_dialog;
	std::string m_autoPickedGameId;
	QComboBox* m_aimCombos[2] = {}; // [0] = USB1/P1, [1] = USB2/P2
	QComboBox* m_touchAimCombo = nullptr;
	QLabel* m_rawInputStatus = nullptr;
};
