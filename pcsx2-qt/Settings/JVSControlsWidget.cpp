// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/JVSControlsWidget.h"

#include "Settings/ControllerSettingsWindow.h"
#include "Settings/ControllerSettingWidgetBinder.h"
#include "Settings/InputBindingWidget.h"

#include "pcsx2/DEV9/ACJV.h"
#include "pcsx2/Host.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/USB/USB.h"

#include "QtHost.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtGui/QAction>
#include <QtGui/QCursor>
#include <QtGui/QKeyEvent>
#include <QtGui/QShowEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>

#include <utility>
#include <vector>

// Highlight bind button text
static constexpr const char* JVS_LABEL_HOVER_STYLE = "color: #4FC3F7; font-weight: bold;";
namespace {
class HoverHighlight final : public QObject
{
public:
	HoverHighlight(QWidget* button, QLabel* label) : QObject(button), m_label(label) { button->installEventFilter(this); }

protected:
	bool eventFilter(QObject* watched, QEvent* event) override
	{
		if (event->type() == QEvent::Enter)
			m_label->setStyleSheet(JVS_LABEL_HOVER_STYLE);
		else if (event->type() == QEvent::Leave)
			m_label->setStyleSheet(QString());
		return QObject::eventFilter(watched, event);
	}

private:
	QLabel* m_label;
};
} // namespace

// One "[label] [P1] [P2]" bind row; empty p2key = single column, HalfAxis for analog.
static void addBindRow(QWidget* parent, SettingsInterface* sif, QGridLayout* layout, int row,
	const QString& label, const std::string& p1key, const std::string& p2key,
	InputBindingInfo::Type bind_type = InputBindingInfo::Type::Button);

using JvsAutomapBinds = std::vector<std::pair<std::string, GenericInputBinding>>;
static void addJvsAutomapButton(JVSControlsWidget* w, ControllerSettingsWindow* dialog, QBoxLayout* layout,
	JvsAutomapBinds p1binds, JvsAutomapBinds p2binds);
static void addLightgunAutomapButton(JVSControlsWidget* w, ControllerSettingsWindow* dialog, QBoxLayout* layout);

JVSControlsWidget::JVSControlsWidget(QWidget* parent, ControllerSettingsWindow* dialog)
	: QWidget(parent)
	, m_dialog(dialog)
{
	m_ui.setupUi(this);

	// Show current JVS board in use
	for (int i = RAYS_PCB; i <= TAITO_BG3_IO_PCB; i++)
		m_ui.boardType->addItem(QString::fromUtf8(ACJV::GetBoardDisplayName(static_cast<BOARDID>(i))));
	refreshBoardType();

	bindDIPSwitchWidgets();
	bindSystemButtonWidgets();
	buildDrumPage();
	buildFightingPage();
	buildRacingPage();
	buildTwinstickPage();
	buildLightgunPage();
	buildStandardPage();
	buildTouchPage();

	// Settings view: analog tuning (deadzone/sensitivity) for the JVS steering and pedals.
	{
		SettingsInterface* sif = m_dialog->getProfileSettingsInterface();
		QGroupBox* group = new QGroupBox(tr("Analog (Steering / Pedals)"), this);
		QGridLayout* g = new QGridLayout(group);
		g->setColumnStretch(0, 1);
		int row = 0;
		const auto addDescription = [&](const QString& text) {
			QLabel* desc = new QLabel(text, group);
			desc->setWordWrap(true);
			g->addWidget(desc, row++, 0, 1, 2);
			g->addItem(new QSpacerItem(1, 10, QSizePolicy::Minimum, QSizePolicy::Fixed), row++, 0, 1, 2);
		};
		const auto addSpinRow = [&](const QString& label, const char* key, float def, double min, double max, const QString& description) {
			g->addWidget(new QLabel(label, group), row, 0);
			QDoubleSpinBox* spin = new QDoubleSpinBox(group);
			spin->setDecimals(0);
			spin->setSuffix(QStringLiteral("%"));
			spin->setMinimum(min);
			spin->setMaximum(max);
			spin->setMaximumWidth(110);
			ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, spin, ACJV::CONFIG_SECTION, key, def, 100.0f);
			g->addWidget(spin, row++, 1, Qt::AlignRight);
			addDescription(description);
		};
		addSpinRow(tr("Steering Deadzone"), "AnalogDeadzone", 0.0f, 0, 100,
			tr("Sets the steering deadzone, i.e. the fraction of wheel travel around the center that is ignored."));
		addSpinRow(tr("Steering Sensitivity"), "AnalogSensitivity", 1.33f, 1, 200,
			tr("Sets the steering axis scaling factor. A value between 130% and 140% suits most modern controllers."));
		addSpinRow(tr("Pedal Deadzone"), "TriggerDeadzone", 0.0f, 0, 100,
			tr("Sets the pedal deadzone, i.e. the fraction of accelerator/brake travel that is ignored."));
		QCheckBox* invert = new QCheckBox(tr("Invert Steering"), group);
		ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, invert, ACJV::CONFIG_SECTION, "InvertSteering", false);
		g->addWidget(invert, row++, 0, 1, 2);
		addDescription(tr("Reverses the steering direction. Enable if the wheel turns the opposite way in-game."));
		m_ui.settingsViewLayout->addWidget(group);
		m_ui.settingsViewLayout->addStretch(1);
	}

	m_ui.systemPageLayout->addStretch(1); // top-pack so the page doesn't inherit the tallest page's height

	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(m_dialog->getProfileSettingsInterface(), m_ui.suppressDaemon,
		"Arcade", "SuppressDaemon", true);

	// Item order MUST match the QStackedWidget page order in the .ui.
	m_ui.pageSelector->addItem(tr("System"));
	m_ui.pageSelector->addItem(tr("Fighting"));
	m_ui.pageSelector->addItem(tr("Racing"));
	m_ui.pageSelector->addItem(tr("Taiko Drum"));
	m_ui.pageSelector->addItem(tr("Twinstick"));
	m_ui.pageSelector->addItem(tr("Lightgun"));
	m_ui.pageSelector->addItem(tr("Standard"));
	m_ui.pageSelector->addItem(tr("Touch Panel"));
	connect(m_ui.pageSelector, QOverload<int>::of(&QComboBox::currentIndexChanged),
		m_ui.pageStack, &QStackedWidget::setCurrentIndex);

	connect(m_ui.viewBindings, &QToolButton::clicked, this, [this]() { m_ui.viewStack->setCurrentIndex(0); });
	connect(m_ui.viewSettings, &QToolButton::clicked, this, [this]() { m_ui.viewStack->setCurrentIndex(1); });
	connect(m_ui.viewMacros, &QToolButton::clicked, this, [this]() { m_ui.viewStack->setCurrentIndex(2); });

	connect(g_emu_thread, &EmuThread::onInputDevicesEnumerated, this,
		[this](const QList<QPair<QString, QString>>&) { refreshAimDevices(); });

	autoPickInputPage();
}

JVSControlsWidget::~JVSControlsWidget() = default;

void JVSControlsWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	refreshBoardType();
	refreshAimDevices();
	refreshMacrosPage();
	autoPickInputPage();
}

// Delete a layout's widgets and items immediately so a rebuilt page shows no stale widgets.
static void clearLayoutImmediate(QLayout* layout)
{
	QLayoutItem* item;
	while ((item = layout->takeAt(0)) != nullptr)
	{
		if (QWidget* w = item->widget())
			delete w;
		delete item;
	}
}

// Macros
void JVSControlsWidget::refreshMacrosPage()
{
	clearLayoutImmediate(m_ui.macrosViewLayout);

	struct MacroLayout { QString name; std::string key; std::span<const InputBindingInfo> buttons; };
	std::vector<MacroLayout> layouts;
	for (const ACJV::LayoutInfo& fl : ACJV::GetFightingLayouts())
		layouts.push_back({QString::fromUtf8(fl.name), ACJV::LayoutKey(fl.buttons), fl.buttons});
	for (const ACJV::RacingLayoutInfo& rl : ACJV::GetRacingLayouts())
		layouts.push_back({QString::fromUtf8(rl.name), ACJV::LayoutKey(rl.buttons), rl.buttons});
	for (const ACJV::LayoutInfo& sl : ACJV::GetStandardLayouts())
		layouts.push_back({QString::fromUtf8(sl.name), ACJV::LayoutKey(sl.buttons), sl.buttons});

	// Dropdown to pick a layout's macros; build into one content widget so a refresh deletes it whole.
	QWidget* content = new QWidget(this);
	QVBoxLayout* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	m_ui.macrosViewLayout->addWidget(content);

	const std::string current = ACJV::GetCurrentLayoutKey();
	QComboBox* combo = new QComboBox(content);
	int currentIdx = 0;
	for (int k = 0; k < static_cast<int>(layouts.size()); k++)
	{
		QString label = layouts[k].name;
		if (!current.empty() && layouts[k].key == current)
		{
			label += tr(" (current)");
			currentIdx = k;
		}
		combo->addItem(label);
	}
	QHBoxLayout* header = new QHBoxLayout();
	header->addWidget(new QLabel(tr("Game:"), content));
	header->addWidget(combo, 1);
	contentLayout->addLayout(header);

	QWidget* rows = new QWidget(content);
	QVBoxLayout* rowsLayout = new QVBoxLayout(rows);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addWidget(rows);
	contentLayout->addStretch(1);

	if (!layouts.empty())
		buildMacroRows(rowsLayout, layouts[currentIdx].key, layouts[currentIdx].buttons);
	combo->setCurrentIndex(currentIdx);
	connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this, layouts, rowsLayout](int idx) {
			if (idx >= 0 && idx < static_cast<int>(layouts.size()))
				buildMacroRows(rowsLayout, layouts[idx].key, layouts[idx].buttons);
		});
}

void JVSControlsWidget::buildMacroRows(QVBoxLayout* rowsLayout, const std::string& layoutKey, std::span<const InputBindingInfo> lbuttons)
{
	clearLayoutImmediate(rowsLayout);
	if (layoutKey.empty())
		return;
	QWidget* rows = rowsLayout->parentWidget();
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();
	SettingsInterface& readsi = sif ? *sif : *Host::Internal::GetBaseSettingsLayer();
	for (int p = 0; p < 2; p++)
	{
		QGroupBox* group = new QGroupBox(tr("Player %1 Macros").arg(p + 1), rows);
		QGridLayout* g = new QGridLayout(group);
		for (u32 i = 0; i < ACJV::NUM_JVS_MACROS; i++)
		{
			const std::string trigKey = ACJV::MacroConfigKey(layoutKey, p, i, "");
			const std::string bindsKey = ACJV::MacroConfigKey(layoutKey, p, i, "Binds");
			g->addWidget(new QLabel(tr("Macro %1").arg(i + 1), group), static_cast<int>(i), 0);
			InputBindingWidget* trig = new InputBindingWidget(group, sif,
				InputBindingInfo::Type::Macro, ACJV::CONFIG_SECTION, trigKey);
			trig->setMinimumWidth(120);
			trig->setMaximumWidth(120);
			g->addWidget(trig, static_cast<int>(i), 1);

			const std::vector<std::string> selected = readsi.GetStringList(ACJV::CONFIG_SECTION, bindsKey.c_str());
			QWidget* box = new QWidget(group);
			QHBoxLayout* bl = new QHBoxLayout(box);
			bl->setContentsMargins(0, 0, 0, 0);
			for (const ACJV::JvsMacroSwitch& sw : ACJV::GetMacroSwitches())
			{
				const char* fn = nullptr; // this layout's function for the switch, if it uses it
				for (const InputBindingInfo& bi : lbuttons)
					if (static_cast<u16>(bi.bind_index) == sw.bit) { fn = bi.display_name; break; }
				if (!fn)
					continue; // only show the switches the layout actually uses (cleaner than all 6)
				QCheckBox* cb = new QCheckBox(QString::fromUtf8(sw.label) + QStringLiteral(" (") + QString::fromUtf8(fn) + QStringLiteral(")"), box);
				cb->setProperty("jvsbtn", QString::fromUtf8(sw.name));
				bool on = false;
				for (const std::string& s : selected)
					if (s == sw.name) { on = true; break; }
				cb->setChecked(on);
				bl->addWidget(cb);
				connect(cb, &QCheckBox::toggled, this, [this, bindsKey, box]() {
					std::vector<std::string> names;
					for (QCheckBox* c : box->findChildren<QCheckBox*>())
						if (c->isChecked())
							names.push_back(c->property("jvsbtn").toString().toStdString());
					const auto write = [&](SettingsInterface& si) {
						if (names.empty())
							si.DeleteValue(ACJV::CONFIG_SECTION, bindsKey.c_str());
						else
							si.SetStringList(ACJV::CONFIG_SECTION, bindsKey.c_str(), names);
					};
					if (m_dialog->isEditingGlobalSettings())
					{
						auto lock = Host::GetSettingsLock();
						write(*Host::Internal::GetBaseSettingsLayer());
					}
					else
					{
						SettingsInterface* si = m_dialog->getProfileSettingsInterface();
						write(*si);
						si->Save();
					}
					g_emu_thread->applySettings();
				});
			}
			bl->addStretch(1);
			g->addWidget(box, static_cast<int>(i), 2);
		}
		rowsLayout->addWidget(group);
	}
}

// The settings window is cached, so re-pick the running game's board each time the page shows.
void JVSControlsWidget::refreshBoardType()
{
	m_ui.boardType->setCurrentIndex(static_cast<int>(ACJV::GetCurrentBoardID()));
}

// Auto-select the page for the running game's mode, but only when the game changed.
void JVSControlsWidget::autoPickInputPage()
{
	const std::string& gameid = ACJV::GetGameId();
	if (gameid == m_autoPickedGameId)
		return;
	m_autoPickedGameId = gameid;
	// Page index in the combo/stack (must match the addItem order above).
	int page = 0;
	switch (ACJV::GetMode())
	{
		case JVS_MODE::FIGHTING:  page = 1; break;
		case JVS_MODE::DRIVE:     page = 2; break;
		case JVS_MODE::DRUM:      page = 3; break;
		case JVS_MODE::TWINSTICK: page = 4; break;
		case JVS_MODE::LIGHTGUN:  page = 5; break;
		case JVS_MODE::STANDARD:  page = 6; break;
		case JVS_MODE::TOUCH:     page = 7; break;
		default:                  page = 0; break;
	}
	m_ui.pageSelector->setCurrentIndex(page);
}

void JVSControlsWidget::bindDIPSwitchWidgets()
{
	QCheckBox* checks[] = {m_ui.testMode, m_ui.videoVoltage, m_ui.monitorSyncFrequency, m_ui.videoSyncSplit};
	InputBindingWidget* toggles[] = {m_ui.toggleTestMode, m_ui.toggleVideoVoltage, m_ui.toggleMonitorSyncFrequency, m_ui.toggleVideoSyncSplit};
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();
	const auto dips = ACJV::GetDIPSwitches();
	for (size_t i = 0; i < std::size(checks); i++)
	{
		checks[i]->setText(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, dips[i].display_name));
		ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, checks[i],
			ACJV::CONFIG_SECTION, dips[i].name, dips[i].default_value);
		toggles[i]->initialize(sif, InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, dips[i].toggle_bind_name);
	}
	m_ui.dipSwitchLayout->setRowStretch(4, 1);
}

void JVSControlsWidget::bindSystemButtonWidgets()
{
	QGridLayout* layout = m_ui.systemButtonsLayout;
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	// Universal inputs (same JVS bit in every game)
	layout->addWidget(new QLabel(tr("P1"), this), 0, 1, Qt::AlignHCenter);
	layout->addWidget(new QLabel(tr("P2"), this), 0, 2, Qt::AlignHCenter);
	addBindRow(this, sif, layout, 1, tr("Service"), "P1_Service", "P2_Service");
	addBindRow(this, sif, layout, 2, tr("Enter"),   "P1_Button1", "P2_Button1");
	addBindRow(this, sif, layout, 3, tr("Coin"),    "Coin1",      "Coin2");
	addBindRow(this, sif, layout, 4, tr("Start"),   "P1_Start",   "P2_Start");
	addBindRow(this, sif, layout, 5, tr("Up"),      "P1_Up",      "P2_Up");
	addBindRow(this, sif, layout, 6, tr("Down"),    "P1_Down",    "P2_Down");
	addBindRow(this, sif, layout, 7, tr("Left"),    "P1_Left",    "P2_Left");
	addBindRow(this, sif, layout, 8, tr("Right"),   "P1_Right",   "P2_Right");
	layout->setRowStretch(9, 1);
}

void JVSControlsWidget::buildDrumPage()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	QGroupBox* group = new QGroupBox(tr("Taiko Drum"), this);
	QGridLayout* g = new QGridLayout(group);
	g->addWidget(new QLabel(tr("P1"), group), 0, 1, Qt::AlignHCenter);
	g->addWidget(new QLabel(tr("P2"), group), 0, 2, Qt::AlignHCenter);
	// config keys must match the s_jvs_drum_bindings names in ACJV.cpp (which carry the real analog channels).
	addBindRow(group, sif, g, 1, tr("Don Left (inner, red)"),  "P1_DonLeft",  "P2_DonLeft");
	addBindRow(group, sif, g, 2, tr("Don Right (inner, red)"), "P1_DonRight", "P2_DonRight");
	addBindRow(group, sif, g, 3, tr("Ka Left (outer, blue)"),  "P1_KaLeft",   "P2_KaLeft");
	addBindRow(group, sif, g, 4, tr("Ka Right (outer, blue)"), "P1_KaRight",  "P2_KaRight");
	g->setColumnStretch(0, 1); // stretch the label column so the binds sit to the right

	QHBoxLayout* row = new QHBoxLayout();
	row->addWidget(group, 1);
	row->addStretch(1);
	m_ui.drumPageLayout->addLayout(row);
	m_ui.drumPageLayout->addStretch(1);

	// Opt-in automap default: Don (red) = Square/Triangle, Ka (blue) = Cross/Circle.
	addJvsAutomapButton(this, m_dialog, m_ui.drumPageLayout,
		{{"P1_DonLeft", GenericInputBinding::Square}, {"P1_DonRight", GenericInputBinding::Triangle},
			{"P1_KaLeft", GenericInputBinding::Cross}, {"P1_KaRight", GenericInputBinding::Circle}},
		{{"P2_DonLeft", GenericInputBinding::Square}, {"P2_DonRight", GenericInputBinding::Triangle},
			{"P2_KaLeft", GenericInputBinding::Cross}, {"P2_KaRight", GenericInputBinding::Circle}});
}

static void addBindRow(QWidget* parent, SettingsInterface* sif, QGridLayout* layout, int row,
	const QString& label, const std::string& p1key, const std::string& p2key,
	InputBindingInfo::Type bind_type)
{
	QLabel* desc = new QLabel(label, parent);
	layout->addWidget(desc, row, 0);
	InputBindingWidget* p1 = new InputBindingWidget(parent, sif, bind_type, ACJV::CONFIG_SECTION, p1key);
	p1->setMinimumWidth(140);
	p1->setMaximumWidth(140);
	layout->addWidget(p1, row, 1);
	new HoverHighlight(p1, desc);
	if (p2key.empty())
		return;
	InputBindingWidget* p2 = new InputBindingWidget(parent, sif, bind_type, ACJV::CONFIG_SECTION, p2key);
	p2->setMinimumWidth(140);
	p2->setMaximumWidth(140);
	layout->addWidget(p2, row, 2);
	new HoverHighlight(p2, desc);
}

// A 4-way movement cross on the given JVS keys; shared by the Fighting d-pad and Twinstick levers.
static void addMovementCompass(QWidget* parent, SettingsInterface* sif, QGridLayout* g, int colBase,
	const QString& header, const std::string& upKey, const std::string& downKey,
	const std::string& leftKey, const std::string& rightKey)
{
	QLabel* h = new QLabel(header, parent);
	h->setAlignment(Qt::AlignHCenter);
	g->addWidget(h, 0, colBase, 1, 3);
	const auto bind = [&](const std::string& key) {
		InputBindingWidget* w = new InputBindingWidget(parent, sif, InputBindingInfo::Type::Button,
			ACJV::CONFIG_SECTION, key);
		w->setMinimumWidth(130);
		w->setMaximumWidth(130);
		return w;
	};
	g->addWidget(bind(upKey),    1, colBase + 1);
	g->addWidget(bind(leftKey),  2, colBase + 0);
	g->addWidget(bind(rightKey), 2, colBase + 2);
	g->addWidget(bind(downKey),  3, colBase + 1);
}

// Shared page builder for Fighting and Standard: movement, per-layout buttons, automap.
template<typename LayoutT>
static void buildJvsLayoutPage(JVSControlsWidget* self, SettingsInterface* sif, ControllerSettingsWindow* dialog,
	QBoxLayout* pageLayout, std::span<const LayoutT> layouts)
{
	{
		QGroupBox* group = new QGroupBox(JVSControlsWidget::tr("Movement"), self);
		QGridLayout* g = new QGridLayout(group);
		g->setColumnStretch(0, 1);
		addMovementCompass(group, sif, g, 1, JVSControlsWidget::tr("P1"), "P1_Up", "P1_Down", "P1_Left", "P1_Right");
		g->setColumnStretch(4, 1);
		addMovementCompass(group, sif, g, 5, JVSControlsWidget::tr("P2"), "P2_Up", "P2_Down", "P2_Left", "P2_Right");
		g->setColumnStretch(8, 1);
		pageLayout->addWidget(group);
	}

	QGridLayout* grid = new QGridLayout();
	int idx = 0;
	for (const LayoutT& fl : layouts)
	{
		QGroupBox* group = new QGroupBox(QString::fromUtf8(fl.name), self);
		QGridLayout* g = new QGridLayout(group);
		g->addWidget(new QLabel(JVSControlsWidget::tr("P1"), group), 0, 1, Qt::AlignHCenter);
		if (!fl.one_player)
			g->addWidget(new QLabel(JVSControlsWidget::tr("P2"), group), 0, 2, Qt::AlignHCenter);
		int row = 1;
		for (const InputBindingInfo& b : fl.buttons)
		{
			const std::string base(b.name);
			addBindRow(group, sif, g, row++,
				QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, b.display_name),
				base + "_P1", fl.one_player ? std::string() : (base + "_P2"));
		}
		if (fl.note)
		{
			QLabel* note = new QLabel(QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, fl.note), group);
			note->setWordWrap(true);
			note->setStyleSheet("font-style: italic;");
			g->addWidget(note, row++, 0, 1, 3);
		}
		grid->addWidget(group, idx / 2, idx % 2);
		idx++;
	}
	pageLayout->addLayout(grid);
	pageLayout->addStretch(1);

	JvsAutomapBinds p1 = {{"P1_Up", GenericInputBinding::DPadUp}, {"P1_Down", GenericInputBinding::DPadDown},
		{"P1_Left", GenericInputBinding::DPadLeft}, {"P1_Right", GenericInputBinding::DPadRight}};
	JvsAutomapBinds p2 = {{"P2_Up", GenericInputBinding::DPadUp}, {"P2_Down", GenericInputBinding::DPadDown},
		{"P2_Left", GenericInputBinding::DPadLeft}, {"P2_Right", GenericInputBinding::DPadRight}};
	for (const LayoutT& fl : layouts)
		for (const InputBindingInfo& b : fl.buttons)
		{
			p1.emplace_back(std::string(b.name) + "_P1", b.generic_mapping);
			if (!fl.one_player)
				p2.emplace_back(std::string(b.name) + "_P2", b.generic_mapping);
		}
	addJvsAutomapButton(self, dialog, pageLayout, std::move(p1), std::move(p2));
}

void JVSControlsWidget::buildFightingPage()
{
	buildJvsLayoutPage(this, m_dialog->getProfileSettingsInterface(), m_dialog,
		m_ui.fightingPageLayout, ACJV::GetFightingLayouts());
}

void JVSControlsWidget::buildRacingPage()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	QGroupBox* wheel = new QGroupBox(tr("Steering && Pedals"), this);
	QGridLayout* g = new QGridLayout(wheel);
	{
		const auto cell = [&](int row, int col, const QString& label, const std::string& key, InputBindingInfo::Type type) {
			QLabel* l = new QLabel(label, wheel);
			l->setAlignment(Qt::AlignHCenter);
			g->addWidget(l, row, col);
			InputBindingWidget* bind = new InputBindingWidget(wheel, sif, type, ACJV::CONFIG_SECTION, key);
			bind->setMinimumWidth(140);
			bind->setMaximumWidth(140);
			g->addWidget(bind, row + 1, col, Qt::AlignHCenter);
			new HoverHighlight(bind, l);
		};
		cell(0, 0, tr("Shift Down"),     "Racing_ShiftDown_P1", InputBindingInfo::Type::Button);
		cell(0, 1, tr("Accelerator"),    "Gas",                 InputBindingInfo::Type::HalfAxis);
		cell(0, 2, tr("Shift Up"),       "Racing_ShiftUp_P1",   InputBindingInfo::Type::Button);
		cell(2, 0, tr("Steering Left"),  "SteerLeft",           InputBindingInfo::Type::HalfAxis);
		cell(2, 2, tr("Steering Right"), "SteerRight",          InputBindingInfo::Type::HalfAxis);
		cell(4, 1, tr("Brake"),          "Brake",               InputBindingInfo::Type::HalfAxis);
		cell(6, 2, tr("View Change"),    "Racing_View_P1",      InputBindingInfo::Type::Button);
	}

	std::vector<QGroupBox*> sections;
	for (const ACJV::RacingLayoutInfo& rl : ACJV::GetRacingLayouts())
	{
		if (ACJV::LayoutKey(rl.buttons) == "Racing")
			continue;
		QGroupBox* group = new QGroupBox(QString::fromUtf8(rl.name), this);
		QGridLayout* gg = new QGridLayout(group);
		int row = 0;
		for (const InputBindingInfo& b : rl.buttons)
			addBindRow(group, sif, gg, row++,
				QCoreApplication::translate(ACJV::TRANSLATION_CONTEXT, b.display_name),
				std::string(b.name) + "_P1", "");
		gg->setColumnStretch(0, 1);
		sections.push_back(group);
	}

	QGridLayout* grid = new QGridLayout();
	grid->addWidget(wheel, 0, 0, Qt::AlignTop);
	for (int i = 0; i < static_cast<int>(sections.size()); i++)
		grid->addWidget(sections[i], 0, i + 1, Qt::AlignTop);
	for (int c = 0; c <= static_cast<int>(sections.size()); c++)
		grid->setColumnStretch(c, 1);
	m_ui.racingPageLayout->addLayout(grid);
	m_ui.racingPageLayout->addStretch(1);

	JvsAutomapBinds binds = {{"SteerLeft", GenericInputBinding::LeftStickLeft},
		{"SteerRight", GenericInputBinding::LeftStickRight},
		{"Gas", GenericInputBinding::R2}, {"Brake", GenericInputBinding::L2}};
	for (const ACJV::RacingLayoutInfo& rl : ACJV::GetRacingLayouts())
		for (const InputBindingInfo& b : rl.buttons)
			binds.emplace_back(std::string(b.name) + "_P1", b.generic_mapping);
	addJvsAutomapButton(this, m_dialog, m_ui.racingPageLayout, std::move(binds), {});
}

// Twinstick (Zoids) page: two lever compasses plus triggers and buttons (1 player).
void JVSControlsWidget::buildTwinstickPage()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	QGroupBox* levers = new QGroupBox(tr("Levers"), this);
	QGridLayout* g = new QGridLayout(levers);
	g->setColumnStretch(0, 1);
	addMovementCompass(levers, sif, g, 1, tr("Left Lever"),
		"P1_LLeverUp", "P1_LLeverDown", "P1_LLeverLeft", "P1_LLeverRight");
	g->setColumnStretch(4, 1);
	addMovementCompass(levers, sif, g, 5, tr("Right Lever"),
		"P1_RLeverUp", "P1_RLeverDown", "P1_RLeverLeft", "P1_RLeverRight");
	g->setColumnStretch(8, 1);
	m_ui.twinstickPageLayout->addWidget(levers);

	QGroupBox* btns = new QGroupBox(tr("Triggers && Buttons"), this); // && -> literal & (Qt treats & as a mnemonic)
	QGridLayout* bg = new QGridLayout(btns);
	addBindRow(btns, sif, bg, 0, tr("Left Trigger"),  "P1_LTrigger", std::string(), InputBindingInfo::Type::HalfAxis);
	addBindRow(btns, sif, bg, 1, tr("Right Trigger"), "P1_RTrigger", std::string(), InputBindingInfo::Type::HalfAxis);
	addBindRow(btns, sif, bg, 2, tr("Left Button"),   "P1_LButton",  std::string());
	addBindRow(btns, sif, bg, 3, tr("Right Button"),  "P1_RButton",  std::string());
	bg->setColumnStretch(0, 1);
	QHBoxLayout* brow = new QHBoxLayout();
	brow->addWidget(btns, 1);
	brow->addStretch(1);
	m_ui.twinstickPageLayout->addLayout(brow);
	m_ui.twinstickPageLayout->addStretch(1);

	// Automap from the twin-stick defaults; Start/Service live on the System page, so skip them.
	JvsAutomapBinds binds;
	for (const InputBindingInfo& b : ACJV::GetTwinstickBindings())
	{
		const std::string name(b.name);
		if (name == "P1_Start" || name == "P1_Service" || b.generic_mapping == GenericInputBinding::Unknown)
			continue;
		binds.emplace_back(b.name, b.generic_mapping);
	}
	addJvsAutomapButton(this, m_dialog, m_ui.twinstickPageLayout, std::move(binds), {});
}

// One crosshair config box (image path + scale + color) bound to the given settings keys.
static QGroupBox* makeCrosshairBox(JVSControlsWidget* w, SettingsInterface* sif, const QString& title,
	const std::string& section, const std::string& pathKey, const std::string& scaleKey, const std::string& colorKey)
{
	QGroupBox* box = new QGroupBox(title, w);
	QGridLayout* cg = new QGridLayout(box);
	cg->setColumnStretch(1, 1);

	QLineEdit* path = new QLineEdit(box);
	QPushButton* browse = new QPushButton(JVSControlsWidget::tr("Browse..."), box);
	ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, path, section, pathKey, "");
	QObject::connect(browse, &QPushButton::clicked, w, [w, path]() {
		const QString p(QDir::toNativeSeparators(QFileDialog::getOpenFileName(w, JVSControlsWidget::tr("Select Crosshair Image"))));
		if (!p.isEmpty())
			path->setText(p);
	});
	QHBoxLayout* ph = new QHBoxLayout();
	ph->addWidget(path, 1);
	ph->addWidget(browse);
	cg->addWidget(new QLabel(JVSControlsWidget::tr("Image"), box), 0, 0);
	cg->addLayout(ph, 0, 1);

	QDoubleSpinBox* scale = new QDoubleSpinBox(box);
	scale->setMinimum(1);
	scale->setMaximum(1000);
	scale->setSingleStep(1);
	scale->setDecimals(0);
	scale->setSuffix(QStringLiteral("%"));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileFloat(sif, scale, section, scaleKey, 1.0f, 100.0f);
	cg->addWidget(new QLabel(JVSControlsWidget::tr("Scale"), box), 1, 0);
	cg->addWidget(scale, 1, 1);

	QLineEdit* color = new QLineEdit(box);
	ControllerSettingWidgetBinder::BindWidgetToInputProfileString(sif, color, section, colorKey, "#ffffff");
	cg->addWidget(new QLabel(JVSControlsWidget::tr("Color"), box), 2, 0);
	cg->addWidget(color, 2, 1);
	return box;
}

// Fill an aim-device combo: system pointers first, then bindable controllers; select `current`.
static void populateAimCombo(QComboBox* combo, ControllerSettingsWindow* dialog, const QString& current)
{
	combo->blockSignals(true);
	combo->clear();
	// with RawInput, one entry per mouse; otherwise the single merged system pointer
	const auto raw_mice = InputManager::EnumerateRawPointerDevices();
	if (raw_mice.empty())
	{
		combo->addItem(JVSControlsWidget::tr("Mouse (system)"), QString::fromStdString(InputManager::GetPointerDeviceName(0)));
	}
	else
	{
		for (const auto& [vidpid, display_name] : raw_mice)
		{
			const std::optional<u32> idx = InputManager::GetPointerIndexForRawDevice(vidpid);
			if (idx.has_value())
				combo->addItem(QString::fromStdString(display_name), QString::fromStdString(InputManager::GetPointerDeviceName(idx.value())));
		}
	}
	for (const QPair<QString, QString>& dev : dialog->getDeviceList())
	{
		// Skip the "Mouse"/"Keyboard" pseudo-devices: no aim stick, and the mouse is already the pointer above.
		if (dev.first == QLatin1String("Mouse") || dev.first == QLatin1String("Keyboard"))
			continue;
		// Raw mice are already listed above with their pointer slot.
		if (dev.first.startsWith(QLatin1String("RawMouse-")) || dev.first.startsWith(QLatin1String("EvdevMouse-")))
			continue;
		combo->addItem(JVSControlsWidget::tr("%1 (Stick)").arg(dev.second), dev.first);
	}
	const int idx = combo->findData(current);
	combo->setCurrentIndex((idx >= 0) ? idx : 0);
	combo->blockSignals(false);
}

// Store the chosen aim device and mirror its left stick into the 4 relative-aim binding keys
// (cleared when the device is a system pointer, i.e. the mouse).
static void applyAimDevice(JVSControlsWidget* w, ControllerSettingsWindow* dialog, const QString& dev,
	const std::string& section, const char* pointerKey, const std::array<std::string, 4>& axisKeys)
{
	dialog->setStringValue(section.c_str(), pointerKey, dev.toUtf8().constData());
	InputManager::GenericInputBindingMapping mapping; // stays empty for a system pointer -> clears the stick binds
	if (!dev.startsWith(QStringLiteral("Pointer-")))
		mapping = InputManager::GetGenericBindingMapping(dev.toStdString());
	static constexpr GenericInputBinding stick[4] = {GenericInputBinding::LeftStickLeft,
		GenericInputBinding::LeftStickRight, GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown};
	for (int i = 0; i < 4; i++)
	{
		std::string host;
		for (const auto& [gg, h] : mapping)
			if (gg == stick[i])
				host = h;
		if (host.empty())
			dialog->clearSettingValue(section.c_str(), axisKeys[i].c_str());
		else
			dialog->setStringValue(section.c_str(), axisKeys[i].c_str(), host.c_str());
	}
	for (InputBindingWidget* bw : w->findChildren<InputBindingWidget*>())
		bw->reloadBinding();
	g_emu_thread->applySettings();
}

// Lightgun (GunCon2) page: bind the gun's USB1/USB2 guncon2_* button keys directly here.
void JVSControlsWidget::buildLightgunPage()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	// One row: label + P1/P2 binds to guncon2_<name>.
	const auto gunRow = [&](QWidget* box, QGridLayout* gl, int r, const QString& label, const char* name,
		InputBindingInfo::Type type) {
		QLabel* desc = new QLabel(label, box);
		gl->addWidget(desc, r, 0);
		const std::string key = USB::GetConfigSubKey("guncon2", name);
		const auto cell = [&](int col, const char* section) {
			InputBindingWidget* wdg = new InputBindingWidget(box, sif, type, section, key);
			wdg->setMinimumWidth(140);
			wdg->setMaximumWidth(140);
			gl->addWidget(wdg, r, col);
			new HoverHighlight(wdg, desc);
		};
		cell(1, "USB1");
		cell(2, "USB2");
	};
	const auto header = [&](QGridLayout* gl, QWidget* box) {
		gl->addWidget(new QLabel(tr("P1"), box), 0, 1, Qt::AlignHCenter);
		gl->addWidget(new QLabel(tr("P2"), box), 0, 2, Qt::AlignHCenter);
	};

	QLabel* aim = new QLabel(tr("Pick each player's aim device below: the mouse, or a controller's stick. Point off-screen to reload."), this);
	aim->setStyleSheet("font-style: italic;"); // info, not a control
	m_ui.lightgunPageLayout->addWidget(aim);

	QGroupBox* aimGroup = new QGroupBox(tr("Aim Device"), this);
	QGridLayout* ag = new QGridLayout(aimGroup);
	header(ag, aimGroup);
	ag->addWidget(new QLabel(tr("Device"), aimGroup), 1, 0);
	// Chosen device stored in guncon2_Pointer; a controller auto-binds its left stick to Relative Aim.
	const auto makeAimCombo = [&](int col, const char* section) {
		QComboBox* combo = new QComboBox(aimGroup);
		combo->setMinimumWidth(160);
		m_aimCombos[col - 1] = combo;
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, section, combo](int) {
			const QString dev = combo->currentData().toString();
			if (dev.isEmpty())
				return;
			applyAimDevice(this, m_dialog, dev, section, "guncon2_Pointer",
				{USB::GetConfigSubKey("guncon2", "RelativeLeft"), USB::GetConfigSubKey("guncon2", "RelativeRight"),
					USB::GetConfigSubKey("guncon2", "RelativeUp"), USB::GetConfigSubKey("guncon2", "RelativeDown")});
		});
		ag->addWidget(combo, 1, col, Qt::AlignLeft);
	};
	makeAimCombo(1, "USB1");
	makeAimCombo(2, "USB2");
	QPushButton* refreshBtn = new QPushButton(tr("Refresh"), aimGroup);
	connect(refreshBtn, &QPushButton::clicked, this, []() {
		g_emu_thread->reloadInputDevices();
		g_emu_thread->enumerateInputDevices(); // queued after the re-scan; onInputDevicesEnumerated repopulates the combos
	});
	ag->addWidget(refreshBtn, 1, 3);
#if defined(_WIN32) || defined(__linux__)
	m_rawInputStatus = new QLabel(aimGroup);
	ag->addWidget(m_rawInputStatus, 2, 0, 1, 4);
#endif
	refreshAimDevices();
	ag->setColumnStretch(0, 1);
	QGroupBox* cabinet = new QGroupBox(tr("Cabinet settings"), this);
	QVBoxLayout* cl = new QVBoxLayout(cabinet);
	QCheckBox* link2p = new QCheckBox(tr("Link as 2P (right side)"), cabinet);
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, link2p, ACJV::CONFIG_SECTION, "LightgunLinkAs2P", false);
	cl->addWidget(link2p);
	cl->addStretch(1);
	QHBoxLayout* top = new QHBoxLayout();
	top->addWidget(aimGroup, 1);
	top->addWidget(cabinet, 1);
	m_ui.lightgunPageLayout->addLayout(top);

	QGroupBox* group = new QGroupBox(tr("Light gun buttons"), this);
	QGridLayout* g = new QGridLayout(group);
	QLabel* calibNote = new QLabel(tr("Reminder: calibrate in-game before playing. Most light gun games won't "
		"work correctly if you don't. For Vampire Night, you need to calibrate both players."), group);
	calibNote->setWordWrap(true);
	calibNote->setStyleSheet("font-style: italic;");
	g->addWidget(calibNote, 0, 0, 1, 3);
	// P1/P2 header on its own row (row 0 is taken by the reminder).
	g->addWidget(new QLabel(tr("P1"), group), 1, 1, Qt::AlignHCenter);
	g->addWidget(new QLabel(tr("P2"), group), 1, 2, Qt::AlignHCenter);
	gunRow(group, g, 2, tr("Trigger"),    "Trigger", InputBindingInfo::Type::Button);
	gunRow(group, g, 3, tr("Foot Pedal"), "A",       InputBindingInfo::Type::Button);
	g->setColumnStretch(0, 1);
	g->setRowStretch(4, 1); // absorb the extra height beside the taller Relative Aim box

	QGroupBox* rel = new QGroupBox(tr("Relative Aim (stick)"), this);
	QGridLayout* rg = new QGridLayout(rel);
	header(rg, rel);
	gunRow(rel, rg, 1, tr("Up"),    "RelativeUp",    InputBindingInfo::Type::HalfAxis);
	gunRow(rel, rg, 2, tr("Down"),  "RelativeDown",  InputBindingInfo::Type::HalfAxis);
	gunRow(rel, rg, 3, tr("Left"),  "RelativeLeft",  InputBindingInfo::Type::HalfAxis);
	gunRow(rel, rg, 4, tr("Right"), "RelativeRight", InputBindingInfo::Type::HalfAxis);
	rg->setColumnStretch(0, 1);

	QHBoxLayout* bottom = new QHBoxLayout();
	bottom->addWidget(group, 1);
	bottom->addWidget(rel, 1);
	m_ui.lightgunPageLayout->addLayout(bottom);

	// Per-player crosshair image (guncon2_cursor_* keys); clearing the path = system cursor.
	const auto makeCrosshair = [&](const char* section, const QString& title) -> QGroupBox* {
		return makeCrosshairBox(this, sif, title, section, USB::GetConfigSubKey("guncon2", "cursor_path"),
			USB::GetConfigSubKey("guncon2", "cursor_scale"), USB::GetConfigSubKey("guncon2", "cursor_color"));
	};
	QHBoxLayout* crosshairRow = new QHBoxLayout();
	crosshairRow->addWidget(makeCrosshair("USB1", tr("Crosshair (P1)")), 1);
	crosshairRow->addWidget(makeCrosshair("USB2", tr("Crosshair (P2)")), 1);
	m_ui.lightgunPageLayout->addLayout(crosshairRow);

	// Sinden lightgun border-friendly
	QGroupBox* sinden = new QGroupBox(tr("Sinden Border"), this);
	QGridLayout* sg = new QGridLayout(sinden);
	sg->setColumnStretch(1, 1);
	QCheckBox* sEnabled = new QCheckBox(tr("Show white border"), sinden);
	ControllerSettingWidgetBinder::BindWidgetToInputProfileBool(sif, sEnabled, ACJV::CONFIG_SECTION, "SindenBorderEnabled", false);
	sg->addWidget(sEnabled, 0, 0, 1, 2);
	QComboBox* sMode = new QComboBox(sinden);
	sMode->addItem(tr("4:3"));        // mode 0 = GS draw rect (the aspect-correct game image)
	sMode->addItem(tr("Fullscreen")); // mode 1 = whole window
	ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, sMode, ACJV::CONFIG_SECTION, "SindenBorderMode", 0);
	sg->addWidget(new QLabel(tr("Aspect ratio"), sinden), 1, 0);
	sg->addWidget(sMode, 1, 1);
	QSpinBox* sThickness = new QSpinBox(sinden);
	sThickness->setMinimum(1);
	sThickness->setMaximum(100);
	sThickness->setSuffix(tr(" px"));
	ControllerSettingWidgetBinder::BindWidgetToInputProfileInt(sif, sThickness, ACJV::CONFIG_SECTION, "SindenBorderThickness", 10);
	sg->addWidget(new QLabel(tr("Thickness"), sinden), 2, 0);
	sg->addWidget(sThickness, 2, 1);
	QHBoxLayout* sindenRow = new QHBoxLayout();
	sindenRow->addWidget(sinden, 1); // left half of the row, like the Crosshair box above
	sindenRow->addStretch(1);
	m_ui.lightgunPageLayout->addLayout(sindenRow);

	m_ui.lightgunPageLayout->addStretch(1);

	addLightgunAutomapButton(this, m_dialog, m_ui.lightgunPageLayout);
}

// Touch panel page: pointer device (mouse, or a controller stick via relative aim), touch press, crosshair.
void JVSControlsWidget::buildTouchPage()
{
	SettingsInterface* sif = m_dialog->getProfileSettingsInterface();

	QLabel* info = new QLabel(tr("Move the pointer with the mouse or a controller stick, and hold the Touch button "
		"to press the panel. A tap is press then release inside the target."), this);
	info->setWordWrap(true);
	info->setStyleSheet("font-style: italic;");
	m_ui.touchPageLayout->addWidget(info);

	// Pointer device: mouse, or a controller whose left stick fills the relative-aim binds below.
	QGroupBox* aimGroup = new QGroupBox(tr("Pointer Device"), this);
	QGridLayout* ag = new QGridLayout(aimGroup);
	ag->addWidget(new QLabel(tr("Device"), aimGroup), 0, 0);
	QComboBox* combo = new QComboBox(aimGroup);
	combo->setMinimumWidth(160);
	m_touchAimCombo = combo;
	connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo](int) {
		const QString dev = combo->currentData().toString();
		if (dev.isEmpty())
			return;
		applyAimDevice(this, m_dialog, dev, ACJV::CONFIG_SECTION, "TouchPointer",
			{"TouchRelativeLeft", "TouchRelativeRight", "TouchRelativeUp", "TouchRelativeDown"});
	});
	ag->addWidget(combo, 0, 1, Qt::AlignLeft);
	ag->setColumnStretch(1, 1);
	QHBoxLayout* top = new QHBoxLayout();
	top->addWidget(aimGroup, 1);
	top->addStretch(1);
	m_ui.touchPageLayout->addLayout(top);

	QGroupBox* touchGroup = new QGroupBox(tr("Touch"), this);
	QGridLayout* tg = new QGridLayout(touchGroup);
	QLabel* pressLabel = new QLabel(tr("Touch (tap)"), touchGroup);
	tg->addWidget(pressLabel, 0, 0);
	InputBindingWidget* press = new InputBindingWidget(touchGroup, sif, InputBindingInfo::Type::Button, ACJV::CONFIG_SECTION, "TouchPress");
	press->setMinimumWidth(140);
	press->setMaximumWidth(140);
	tg->addWidget(press, 0, 1);
	new HoverHighlight(press, pressLabel);
	QLabel* pressNote = new QLabel(tr("Left mouse button when unbound."), touchGroup);
	pressNote->setStyleSheet("font-style: italic;");
	tg->addWidget(pressNote, 1, 0, 1, 2);
	tg->setColumnStretch(0, 1);
	tg->setRowStretch(2, 1);

	QGroupBox* rel = new QGroupBox(tr("Relative Aim (stick)"), this);
	QGridLayout* rg = new QGridLayout(rel);
	const auto relRow = [&](int r, const QString& label, const char* key) {
		QLabel* desc = new QLabel(label, rel);
		rg->addWidget(desc, r, 0);
		InputBindingWidget* wdg = new InputBindingWidget(rel, sif, InputBindingInfo::Type::HalfAxis, ACJV::CONFIG_SECTION, key);
		wdg->setMinimumWidth(140);
		wdg->setMaximumWidth(140);
		rg->addWidget(wdg, r, 1);
		new HoverHighlight(wdg, desc);
	};
	relRow(0, tr("Up"), "TouchRelativeUp");
	relRow(1, tr("Down"), "TouchRelativeDown");
	relRow(2, tr("Left"), "TouchRelativeLeft");
	relRow(3, tr("Right"), "TouchRelativeRight");
	rg->setColumnStretch(0, 1);

	QHBoxLayout* mid = new QHBoxLayout();
	mid->addWidget(touchGroup, 1);
	mid->addWidget(rel, 1);
	m_ui.touchPageLayout->addLayout(mid);

	// Crosshair for the touch pointer (needed to see where a controller stick points).
	QHBoxLayout* crossRow = new QHBoxLayout();
	crossRow->addWidget(makeCrosshairBox(this, sif, tr("Crosshair"), ACJV::CONFIG_SECTION,
		"TouchCursorPath", "TouchCursorScale", "TouchCursorColor"), 1);
	crossRow->addStretch(1);
	m_ui.touchPageLayout->addLayout(crossRow);

	m_ui.touchPageLayout->addStretch(1);
}

namespace
{
// Community easter egg: press 3 three times on the Standard page to light up "Button 3".
class Button3Egg final : public QObject
{
public:
	Button3Egg(QLabel* label, QWidget* page)
		: QObject(label)
		, m_label(label)
		, m_page(page)
	{
		qApp->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject*, QEvent* event) override
	{
		if (event->type() != QEvent::KeyPress)
			return false;
		const QKeyEvent* ke = static_cast<QKeyEvent*>(event);
		// An app-level filter sees the same key event once per widget it propagates through.
		if (ke->key() != Qt::Key_3 || ke->isAutoRepeat() || ke->timestamp() == m_last_press ||
			m_timer != 0 || !m_page->isVisible())
			return false;
		m_presses = (ke->timestamp() - m_last_press > 1200) ? 1 : m_presses + 1;
		m_last_press = ke->timestamp();
		if (m_presses == 3)
			m_timer = startTimer(50);
		return false;
	}

	void timerEvent(QTimerEvent*) override
	{
		if (!m_page->isVisible())
		{
			killTimer(m_timer);
			m_timer = 0;
			m_presses = 0;
			m_label->setStyleSheet(QString());
			return;
		}
		m_hue = (m_hue + 10) % 360;
		m_label->setStyleSheet(
			QStringLiteral("color: %1; font-weight: bold;").arg(QColor::fromHsv(m_hue, 255, 255).name()));
	}

private:
	QLabel* m_label;
	QWidget* m_page;
	quint64 m_last_press = 0;
	int m_presses = 0;
	int m_timer = 0;
	int m_hue = 0;
};
} // namespace

void JVSControlsWidget::buildStandardPage()
{
	buildJvsLayoutPage(this, m_dialog->getProfileSettingsInterface(), m_dialog,
		m_ui.standardPageLayout, ACJV::GetStandardLayouts());

	for (QLabel* label : m_ui.standardPage->findChildren<QLabel*>())
	{
		if (label->text() == QStringLiteral("Button 3"))
		{
			new Button3Egg(label, m_ui.standardPage);
			return;
		}
	}
}

// (Re)fill the lightgun Aim Device combos; devices enumerate asynchronously, so this reruns on show.
void JVSControlsWidget::refreshAimDevices()
{
	const char* sections[2] = {"USB1", "USB2"};
	for (int i = 0; i < 2; i++)
	{
		if (!m_aimCombos[i])
			continue;
		populateAimCombo(m_aimCombos[i], m_dialog, QString::fromStdString(m_dialog->getStringValue(
			sections[i], "guncon2_Pointer", InputManager::GetPointerDeviceName(0).c_str())));
	}
	if (m_touchAimCombo)
	{
		populateAimCombo(m_touchAimCombo, m_dialog, QString::fromStdString(m_dialog->getStringValue(
			ACJV::CONFIG_SECTION, "TouchPointer", InputManager::GetPointerDeviceName(0).c_str())));
	}
	if (m_rawInputStatus)
	{
#ifdef _WIN32
		const QString label = tr("Raw Input:");
#else
		const QString label = tr("Evdev:");
#endif
		m_rawInputStatus->setText(InputManager::IsUsingRawInput()
			? QStringLiteral("%1 <b><span style='color:#2ecc71;'>ON</span></b>").arg(label)
			: QStringLiteral("%1 <b>OFF</b>").arg(label));
	}
}

static void applyJvsAutomap(JVSControlsWidget* w, ControllerSettingsWindow* dialog, const QString& device,
	const JvsAutomapBinds& binds, const char* section = ACJV::CONFIG_SECTION)
{
	const auto mapping = InputManager::GetGenericBindingMapping(device.toStdString());
	if (mapping.empty())
	{
		QMessageBox::critical(w, JVSControlsWidget::tr("Automatic Mapping"),
			JVSControlsWidget::tr("No default mapping is available for device '%1'.").arg(device));
		return;
	}

	const auto apply = [&](SettingsInterface& si) {
		for (const auto& [key, generic] : binds)
		{
			if (generic == GenericInputBinding::Unknown)
				continue;
			for (const auto& [g, host] : mapping)
			{
				if (g == generic)
				{
					si.SetStringValue(section, key.c_str(), host.c_str());
					break;
				}
			}
		}
	};

	if (dialog->isEditingGlobalSettings())
	{
		{
			auto lock = Host::GetSettingsLock();
			apply(*Host::Internal::GetBaseSettingsLayer());
		}
		Host::CommitBaseSettingChanges();
	}
	else
	{
		apply(*dialog->getProfileSettingsInterface());
		dialog->getProfileSettingsInterface()->Save();
		g_emu_thread->reloadInputBindings();
	}

	for (InputBindingWidget* widget : w->findChildren<InputBindingWidget*>())
		widget->reloadBinding();
	g_emu_thread->applySettings();
}

static QString jvsDeviceLabel(const QPair<QString, QString>& dev)
{
	return (dev.first.compare(dev.second, Qt::CaseInsensitive) == 0)
		? dev.first : QStringLiteral("%1: %2").arg(dev.first).arg(dev.second);
}

static void showJvsAutomapMenu(JVSControlsWidget* w, ControllerSettingsWindow* dialog,
	const JvsAutomapBinds& p1binds, const JvsAutomapBinds& p2binds)
{
	QMenu menu(w);
	bool added = false;
	if (p2binds.empty())
	{
		for (const QPair<QString, QString>& dev : dialog->getDeviceList())
		{
			const QString device = dev.first;
			const QString label = jvsDeviceLabel(dev);
			QObject::connect(menu.addAction(label), &QAction::triggered, w,
				[w, dialog, device, p1binds]() { applyJvsAutomap(w, dialog, device, p1binds); });
			added = true;
		}
	}
	else
	{
		QMenu* sub[2] = {menu.addMenu(JVSControlsWidget::tr("Map Player 1 from...")),
			menu.addMenu(JVSControlsWidget::tr("Map Player 2 from..."))};
		const JvsAutomapBinds* binds[2] = {&p1binds, &p2binds};
		for (const QPair<QString, QString>& dev : dialog->getDeviceList())
		{
			const QString device = dev.first;
			const QString label = jvsDeviceLabel(dev);
			for (int p = 0; p < 2; p++)
			{
				const JvsAutomapBinds* b = binds[p];
				QObject::connect(sub[p]->addAction(label), &QAction::triggered, w,
					[w, dialog, device, b]() { applyJvsAutomap(w, dialog, device, *b); });
			}
			added = true;
		}
	}
	if (!added)
		menu.addAction(JVSControlsWidget::tr("No devices available"))->setEnabled(false);
	menu.exec(QCursor::pos());
}

static void clearJvsPageBindings(JVSControlsWidget* w, QBoxLayout* layout)
{
	if (QMessageBox::question(w, JVSControlsWidget::tr("Clear Mapping"),
			JVSControlsWidget::tr("Clear all input bindings on this page?")) != QMessageBox::Yes)
		return;
	if (QWidget* page = layout->parentWidget())
		for (InputBindingWidget* widget : page->findChildren<InputBindingWidget*>())
			widget->clearBinding();
}

static void addMappingButtonRow(JVSControlsWidget* w, QBoxLayout* layout, QPushButton* automap)
{
	QPushButton* clear = new QPushButton(JVSControlsWidget::tr("Clear Mapping"), w);
	QObject::connect(clear, &QPushButton::clicked, w, [w, layout]() { clearJvsPageBindings(w, layout); });
	QHBoxLayout* row = new QHBoxLayout();
	row->addWidget(automap, 1);
	row->addWidget(clear);
	layout->insertLayout(0, row);
}

static void addJvsAutomapButton(JVSControlsWidget* w, ControllerSettingsWindow* dialog, QBoxLayout* layout,
	JvsAutomapBinds p1binds, JvsAutomapBinds p2binds)
{
	QPushButton* btn = new QPushButton(JVSControlsWidget::tr("Automatic Mapping (PS2 default layout)"), w);
	QObject::connect(btn, &QPushButton::clicked, w,
		[w, dialog, p1binds, p2binds]() { showJvsAutomapMenu(w, dialog, p1binds, p2binds); });
	addMappingButtonRow(w, layout, btn);
}

static void showLightgunAutomapMenu(JVSControlsWidget* w, ControllerSettingsWindow* dialog, const JvsAutomapBinds& binds)
{
	QMenu menu(w);
	QMenu* sub[2] = {menu.addMenu(JVSControlsWidget::tr("Map Player 1 from...")),
		menu.addMenu(JVSControlsWidget::tr("Map Player 2 from..."))};
	const char* sections[2] = {"USB1", "USB2"};
	bool added = false;
	for (const QPair<QString, QString>& dev : dialog->getDeviceList())
	{
		const QString device = dev.first;
		const QString label = jvsDeviceLabel(dev);
		for (int p = 0; p < 2; p++)
		{
			const char* section = sections[p];
			QObject::connect(sub[p]->addAction(label), &QAction::triggered, w,
				[w, dialog, device, binds, section]() { applyJvsAutomap(w, dialog, device, binds, section); });
		}
		added = true;
	}
	if (!added)
		menu.addAction(JVSControlsWidget::tr("No devices available"))->setEnabled(false);
	menu.exec(QCursor::pos());
}

static void addLightgunAutomapButton(JVSControlsWidget* w, ControllerSettingsWindow* dialog, QBoxLayout* layout)
{
	// GunCon2 defaults (guncon2_* keys); Start and Coins are on the System page, so skip them.
	JvsAutomapBinds binds;
	for (const InputBindingInfo& bi : USB::GetDeviceBindings("guncon2", 0))
	{
		const std::string_view name(bi.name);
		if (name == "Start" || name == "Select")
			continue;
		if (bi.generic_mapping != GenericInputBinding::Unknown)
			binds.emplace_back(USB::GetConfigSubKey("guncon2", bi.name), bi.generic_mapping);
	}
	QPushButton* btn = new QPushButton(JVSControlsWidget::tr("Automatic Mapping (PS2 default layout)"), w);
	QObject::connect(btn, &QPushButton::clicked, w,
		[w, dialog, binds]() { showLightgunAutomapMenu(w, dialog, binds); });
	addMappingButtonRow(w, layout, btn);
}

#include "moc_JVSControlsWidget.cpp"
