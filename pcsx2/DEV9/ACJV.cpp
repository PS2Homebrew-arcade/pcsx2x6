#include "common/Console.h"
#include "ACMACROS.h"
#include "ACJV.h"
#include "Config.h"
#include "Host.h"
#include "Input/InputManager.h"
#include "GS/GS.h"
#include "common/SettingsInterface.h"
#include <array>
#include <atomic>
#include <string>

enum ACJVCMD {
	UNKNOWN = -2, // unknown CMD, should fire up a warning for developer
	NONE = -1,  // Neutral state
	JVS_INIT0, // starts with 26 A3
	JVS_INIT1, // starts with 98 59
	JVS_JVS,   // starts with 6F 3E. this holds an actual JVS packet inside
};

bool ACJV::enabled = false;
#define JVS_PKTINDX_TO_ADDR(index) ((2 * (index & 0x3FFF)) + 0x12400000)

#define ANS(addr, what) case addr: return what
#define JVS_CHECK_ADDR_JVSPACKET(addr) ((addr & 0x00000F00) == 0x00000600) // when ACJV writes to 0x12406???.
void do_acjv_packet();

u16 RootAddr = 0x3E6F;
u16 Sent0D = 0, Sent0C = 0;

// R/W arrays are u8 because ACJV is processing (u8) arrays, but the MMIO is performed over (volatile u16*). leaving the higher byte empty
std::array<u8, ACJV_PACKETSIZE> rdbuf; // NAMCO_PCB ---> IOP
std::array<u8, ACJV_PACKETSIZE> wrbuf; // IOP --> NAMCO_PCB
inline u16* rdbuf_getu16() { // NAMCO_PCB ---> IOP
    return reinterpret_cast<u16*>(rdbuf.data());
}
inline const u16* wrbuf_getu16() { // IOP --> NAMCO_PCB
    return reinterpret_cast<const u16*>(wrbuf.data());
}

std::string BOARDS[] = {
	"namco ltd.;RAYS PCB;",
	"namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose",
	"namco ltd.;FCB;Ver1.02;JPN,TouchPanel&Multipurpose",
	"namco ltd.;TSS-I/O;Ver2.11;GUN-EXTENTION",
	"namco ltd.;MIU-I/O;Ver2.05;JPN,GUN-EXTENTION",
};
enum BOARDID ACJV::CurrentBoardID = RAYS_PCB;

static constexpr const char* BOARD_DISPLAY_NAMES[] = {
	"RAYS PCB",
	"FCA-1 (Multipurpose)",
	"FCB (Touch Panel)",
	"TSS-I/O (Gun Extension)",
	"MIU-I/O (Gun Extension)",
};

static constexpr u16 DEFAULT_DIP_SWITCH_STATE =
    (DIPS::VIDEO_VOLTAGE | DIPS::MONITOR_SYNCFREQ | DIPS::VIDEO_SYNC_SPLIT);

static constexpr const std::array<u16, ACJV::NUM_DIP_SWITCHES> s_dip_switch_masks = {{
	DIPS::TESTMODE,
	DIPS::VIDEO_VOLTAGE,
	DIPS::MONITOR_SYNCFREQ,
	DIPS::VIDEO_SYNC_SPLIT,
}};

static constexpr const std::array<ACJV::DIPSwitchInfo, ACJV::NUM_DIP_SWITCHES> s_dip_switch_info = {{
	{"TestMode", TRANSLATE_NOOP("JVS", "Test Mode"), "ToggleTestMode", false},
	{"VideoVoltage", TRANSLATE_NOOP("JVS", "Video Voltage"), "ToggleVideoVoltage", true},
	{"MonitorSyncFrequency", TRANSLATE_NOOP("JVS", "Monitor Sync Frequency"), "ToggleMonitorSyncFrequency", true},
	{"VideoSyncSplit", TRANSLATE_NOOP("JVS", "Video Sync Split"), "ToggleVideoSyncSplit", true},
}};

static constexpr const std::array<InputBindingInfo, ACJV::NUM_DIP_SWITCHES> s_dip_switch_bindings = {{
	{s_dip_switch_info[0].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Test Mode"), nullptr, InputBindingInfo::Type::Button, 0, GenericInputBinding::Unknown},
	{s_dip_switch_info[1].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Video Voltage"), nullptr, InputBindingInfo::Type::Button, 1, GenericInputBinding::Unknown},
	{s_dip_switch_info[2].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Monitor Sync Frequency"), nullptr, InputBindingInfo::Type::Button, 2, GenericInputBinding::Unknown},
	{s_dip_switch_info[3].toggle_bind_name, TRANSLATE_NOOP("JVS", "Toggle Video Sync Split"), nullptr, InputBindingInfo::Type::Button, 3, GenericInputBinding::Unknown},
}};

static constexpr const std::array<InputBindingInfo, 12> s_jvs_p1_button_bindings = {{
	{"P1_Up",      TRANSLATE_NOOP("JVS", "P1 Up"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,      GenericInputBinding::DPadUp},
	{"P1_Down",    TRANSLATE_NOOP("JVS", "P1 Down"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,    GenericInputBinding::DPadDown},
	{"P1_Left",    TRANSLATE_NOOP("JVS", "P1 Left"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,    GenericInputBinding::DPadLeft},
	{"P1_Right",   TRANSLATE_NOOP("JVS", "P1 Right"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT,   GenericInputBinding::DPadRight},
	{"P1_Button1", TRANSLATE_NOOP("JVS", "P1 Button 1"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1,       GenericInputBinding::Square},
	{"P1_Button2", TRANSLATE_NOOP("JVS", "P1 Button 2"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2,       GenericInputBinding::Triangle},
	{"P1_Button3", TRANSLATE_NOOP("JVS", "P1 Button 3"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3,       GenericInputBinding::Unknown},
	{"P1_Button4", TRANSLATE_NOOP("JVS", "P1 Button 4"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4,       GenericInputBinding::Cross},
	{"P1_Button5", TRANSLATE_NOOP("JVS", "P1 Button 5"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_5,       GenericInputBinding::Circle},
	{"P1_Button6", TRANSLATE_NOOP("JVS", "P1 Button 6"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_6,       GenericInputBinding::Unknown},
	{"P1_Start",   TRANSLATE_NOOP("JVS", "P1 Start"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_START,   GenericInputBinding::Start},
	{"P1_Service", TRANSLATE_NOOP("JVS", "P1 Service"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_SERVICE, GenericInputBinding::Select},
}};

static constexpr const std::array<InputBindingInfo, 12> s_jvs_p2_button_bindings = {{
	{"P2_Up",      TRANSLATE_NOOP("JVS", "P2 Up"),       nullptr, InputBindingInfo::Type::Button, JVS_BTN_UP,      GenericInputBinding::DPadUp},
	{"P2_Down",    TRANSLATE_NOOP("JVS", "P2 Down"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_DOWN,    GenericInputBinding::DPadDown},
	{"P2_Left",    TRANSLATE_NOOP("JVS", "P2 Left"),     nullptr, InputBindingInfo::Type::Button, JVS_BTN_LEFT,    GenericInputBinding::DPadLeft},
	{"P2_Right",   TRANSLATE_NOOP("JVS", "P2 Right"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_RIGHT,   GenericInputBinding::DPadRight},
	{"P2_Button1", TRANSLATE_NOOP("JVS", "P2 Button 1"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_1,       GenericInputBinding::Square},
	{"P2_Button2", TRANSLATE_NOOP("JVS", "P2 Button 2"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_2,       GenericInputBinding::Triangle},
	{"P2_Button3", TRANSLATE_NOOP("JVS", "P2 Button 3"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_3,       GenericInputBinding::Unknown},
	{"P2_Button4", TRANSLATE_NOOP("JVS", "P2 Button 4"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_4,       GenericInputBinding::Cross},
	{"P2_Button5", TRANSLATE_NOOP("JVS", "P2 Button 5"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_5,       GenericInputBinding::Circle},
	{"P2_Button6", TRANSLATE_NOOP("JVS", "P2 Button 6"), nullptr, InputBindingInfo::Type::Button, JVS_BTN_6,       GenericInputBinding::Unknown},
	{"P2_Start",   TRANSLATE_NOOP("JVS", "P2 Start"),    nullptr, InputBindingInfo::Type::Button, JVS_BTN_START,   GenericInputBinding::Start},
	{"P2_Service", TRANSLATE_NOOP("JVS", "P2 Service"),  nullptr, InputBindingInfo::Type::Button, JVS_BTN_SERVICE, GenericInputBinding::Select},
}};

// Per-layout face button default inputs (BTN1-6), mirroring each game's
// official PS2 port pad. Some games share the same layout. Rows follow FightingLayout order.
static constexpr GenericInputBinding s_fighting_face_buttons[][6] = {
	// BTN1,                       BTN2,                          BTN3,                         BTN4,                          BTN5,                         BTN6
	{GenericInputBinding::Square, GenericInputBinding::Triangle, GenericInputBinding::Unknown, GenericInputBinding::Cross,    GenericInputBinding::Circle,  GenericInputBinding::Unknown}, // TEKKEN
	{GenericInputBinding::Square, GenericInputBinding::Triangle, GenericInputBinding::Cross,   GenericInputBinding::Circle,   GenericInputBinding::Unknown, GenericInputBinding::Unknown}, // GUNDAM (YuYu + Gundam VS)
	{GenericInputBinding::Square, GenericInputBinding::Triangle, GenericInputBinding::L1,      GenericInputBinding::Cross,    GenericInputBinding::Circle,  GenericInputBinding::R1},      // SIX_BUTTON
	{GenericInputBinding::Square, GenericInputBinding::Triangle, GenericInputBinding::Circle,  GenericInputBinding::Cross,    GenericInputBinding::Unknown, GenericInputBinding::Unknown}, // SOULCAL
	{GenericInputBinding::Square, GenericInputBinding::Cross,    GenericInputBinding::Circle,  GenericInputBinding::Triangle, GenericInputBinding::Unknown, GenericInputBinding::Unknown}, // BLOODYROAR
};

static std::array<InputBindingInfo, 12> s_active_p1_bindings;
static std::array<InputBindingInfo, 12> s_active_p2_bindings;

// Copy the base P1/P2 tables, then set the default pad button of BTN1-6 from
// the game's layout row. Binding table indices:
//   [0]=Up    [1]=Down  [2]=Left   [3]=Right
//   [4]=BTN1  [5]=BTN2  [6]=BTN3   [7]=BTN4
//   [8]=BTN5  [9]=BTN6  [10]=Start [11]=Service
static void UpdateFightingBindings(FightingLayout layout)
{
	s_active_p1_bindings = s_jvs_p1_button_bindings;
	s_active_p2_bindings = s_jvs_p2_button_bindings;
	const auto& face = s_fighting_face_buttons[static_cast<int>(layout)];
	constexpr int BTN1_INDEX = 4;
	for (int i = 0; i < 6; i++)
	{
		s_active_p1_bindings[BTN1_INDEX + i].generic_mapping = face[i];
		s_active_p2_bindings[BTN1_INDEX + i].generic_mapping = face[i];
	}
}

static constexpr const std::array<InputBindingInfo, 2> s_jvs_coin_bindings = {{
	{"Coin1", TRANSLATE_NOOP("JVS", "Insert Coin P1"), nullptr, InputBindingInfo::Type::Button, 0, GenericInputBinding::Unknown},
	{"Coin2", TRANSLATE_NOOP("JVS", "Insert Coin P2"), nullptr, InputBindingInfo::Type::Button, 1, GenericInputBinding::Unknown},
}};

static u16 s_dip_switch_state = DEFAULT_DIP_SWITCH_STATE;
static bool s_suppress_daemon = true;
static std::atomic<bool> s_sinden_border_enabled{false};
static std::atomic<int> s_sinden_border_mode{0};
static std::atomic<int> s_sinden_border_thickness{10};
static std::string s_gameid;

std::span<const ACJV::DIPSwitchInfo> ACJV::GetDIPSwitches()
{
	return s_dip_switch_info;
}

const ACJV::DIPSwitchInfo& ACJV::GetTestModeDIPSwitch()
{
	return s_dip_switch_info[0];
}

const ACJV::DIPSwitchInfo& ACJV::GetVideoVoltageDIPSwitch()
{
	return s_dip_switch_info[1];
}

const ACJV::DIPSwitchInfo& ACJV::GetMonitorSyncFrequencyDIPSwitch()
{
	return s_dip_switch_info[2];
}

const ACJV::DIPSwitchInfo& ACJV::GetVideoSyncSplitDIPSwitch()
{
	return s_dip_switch_info[3];
}

bool ACJV::IsSuppressDaemonEnabled()
{
	return s_suppress_daemon;
}

const char* ACJV::GetBoardDisplayName(BOARDID id)
{
	if (id >= RAYS_PCB && id <= MIU_IO_JPN_GUN_EXTENTI)
		return BOARD_DISPLAY_NAMES[id];
	return "Unknown";
}

BOARDID ACJV::GetCurrentBoardID()
{
	return CurrentBoardID;
}

std::span<const InputBindingInfo> ACJV::GetDIPSwitchBindings()
{
	return s_dip_switch_bindings;
}

static JVS_MODE m_jvsMode = JVS_MODE::DEFAULT;

std::span<const InputBindingInfo> ACJV::GetButtonBindings()
{
	if (m_jvsMode == JVS_MODE::FIGHTING)
		return s_active_p1_bindings;
	return s_jvs_p1_button_bindings;
}

std::span<const InputBindingInfo> ACJV::GetP2ButtonBindings()
{
	if (m_jvsMode == JVS_MODE::FIGHTING)
		return s_active_p2_bindings;
	return s_jvs_p2_button_bindings;
}

std::span<const InputBindingInfo> ACJV::GetCoinBindings()
{
	return s_jvs_coin_bindings;
}

bool ACJV::GetDIPSwitchState(u32 index)
{
	return (index < s_dip_switch_masks.size()) && ((s_dip_switch_state & s_dip_switch_masks[index]) != 0);
}

void ACJV::SetDIPSwitchState(u32 index, bool enabled)
{
	if (index >= s_dip_switch_masks.size())
		return;

	const u16 mask = s_dip_switch_masks[index];
	if (enabled)
		s_dip_switch_state |= mask;
	else
		s_dip_switch_state &= ~mask;
}

void ACJV::ToggleDIPSwitchState(u32 index)
{
	if (index >= s_dip_switch_masks.size())
		return;

	const u16 mask = s_dip_switch_masks[index];
	s_dip_switch_state ^= mask;
}

void ACJV::LoadConfig(const SettingsInterface& si)
{
	u16 state = 0;
	for (u32 i = 0; i < s_dip_switch_info.size(); i++)
	{
		const DIPSwitchInfo& dip_switch = s_dip_switch_info[i];
		if (si.GetBoolValue(CONFIG_SECTION, dip_switch.name, dip_switch.default_value))
			state |= s_dip_switch_masks[i];
	}
	s_dip_switch_state = state;
	s_suppress_daemon = si.GetBoolValue(CONFIG_SECTION, "SuppressDaemon", true);
	s_sinden_border_enabled = si.GetBoolValue(CONFIG_SECTION, "SindenBorderEnabled", false);
	s_sinden_border_mode = si.GetIntValue(CONFIG_SECTION, "SindenBorderMode", 0);
	s_sinden_border_thickness = si.GetIntValue(CONFIG_SECTION, "SindenBorderThickness", 10);
}

void ACJV::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_settings, bool copy_bindings)
{
	if (copy_settings)
	{
		for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
			dest_si->CopyBoolValue(src_si, CONFIG_SECTION, dip_switch.name);
		dest_si->CopyBoolValue(src_si, CONFIG_SECTION, "SuppressDaemon");
		dest_si->CopyBoolValue(src_si, CONFIG_SECTION, "SindenBorderEnabled");
		dest_si->CopyIntValue(src_si, CONFIG_SECTION, "SindenBorderMode");
		dest_si->CopyIntValue(src_si, CONFIG_SECTION, "SindenBorderThickness");
	}

	if (copy_bindings)
	{
		for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, dip_switch.toggle_bind_name);
		for (const InputBindingInfo& bi : s_jvs_p1_button_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_jvs_p2_button_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
		for (const InputBindingInfo& bi : s_jvs_coin_bindings)
			dest_si->CopyStringListValue(src_si, CONFIG_SECTION, bi.name);
	}
}

void ACJV::SetDefaultConfiguration(SettingsInterface& si)
{
	si.ClearSection(CONFIG_SECTION);
	for (const DIPSwitchInfo& dip_switch : s_dip_switch_info)
		si.SetBoolValue(CONFIG_SECTION, dip_switch.name, dip_switch.default_value);
	si.SetBoolValue(CONFIG_SECTION, "SuppressDaemon", true);
	si.SetBoolValue(CONFIG_SECTION, "SindenBorderEnabled", false);
	si.SetIntValue(CONFIG_SECTION, "SindenBorderMode", 0);
	si.SetIntValue(CONFIG_SECTION, "SindenBorderThickness", 10);
}

u16 ACJV::Read16(u32 addr) {
    if (addr >= ACJV_RDBASE && addr < 0x124045FE) {
        int x = (addr - ACJV_RDBASE)/2;
		if (x == 2 || x == 3 || x == 4) return rdbuf.at(x)|1;// initial polling expects these addrs to not be zero
        return (u16)rdbuf.at(x);
    } else if ((addr == 0x124045FE)) {
		return (u16)rdbuf.at((addr - ACJV_RDBASE)/2);
	}
	return 0;
}

void ACJV::Write16(u32 addr, u16 val) {
    if (addr >= ACJV_WRBASE && addr < 0x12404BFE) { //0x124048FE
        u32 x = (addr - ACJV_WRBASE)/2;
        wrbuf[x] = val;
    } else if (addr == 0x12404BFE) {
       wrbuf[(addr -  ACJV_WRBASE)/2] = val;
		do_acjv_packet();
	}
}

#define JVS_ASSERT(x) if (!(x)) Console.WriteLn("## ASSERT ## %s:%s:%d %s", __FILE__, __FUNCTION__, __LINE__, #x);

// JVS bus state — volatile runtime values, reset on game switch (see SetGameId)
static u16 m_jvsSystemButtonState = 0;
static u16 m_jvsButtonState[JVS_PLAYER_COUNT] = {};
static u8 m_testButtonState = 0;
static u16 m_coin1 = 0;
static u16 m_coin2 = 0;
static u16 m_jvsScreenPosX = 0;
static u16 m_jvsScreenPosY = 0;
static float m_jvsLightgunDX = -1.0f;  // normalized display X (-1 = off-screen)
static float m_jvsLightgunDY = -1.0f;  // normalized display Y (-1 = off-screen)
static u16 m_jvsWheelChannels[JVS_WHEEL_CHANNEL_MAX] = {};
static u16 m_jvsDrumChannels[JVS_DRUM_CHANNEL_MAX] = {};

// Per-game JVS button mapping for lightgun games, keyed by NM game ID (see issue #9).
// Field order: pedal, sensor, sensor_active_high, p1_start, p2_start, p1_trigger, p2_trigger
// Each value is a JVS bit from JVSButton enum. 0 = not used for this game.
// This table serves as template for future per-game configs (fighting, driving, drum, etc).
static const GunMapping s_default_gun_mapping = {JVS_BTN_3, JVS_BTN_RIGHT, false, 0, 0, JVS_BTN_2, 0};
static const std::map<std::string, GunMapping> s_gun_mappings = {
	{"NM00003", {0,            0x200,         true,  JVS_BTN_3,  JVS_BTN_6, JVS_BTN_2,    JVS_BTN_5}}, // Vampire Night
	{"NM00012", {JVS_BTN_6,    0,             false, 0,          0,          JVS_BTN_2,    0}},          // Time Crisis 3
	{"NM00021", {JVS_BTN_3,    JVS_BTN_RIGHT, false, 0,          0,          JVS_BTN_LEFT, 0}},          // Cobra The Arcade
	{"NM00032", {JVS_BTN_3,    JVS_BTN_RIGHT, false, 0,          0,          JVS_BTN_LEFT, 0}},          // Time Crisis 4
};
static const GunMapping* m_gunMapping = &s_default_gun_mapping;

static const std::map<std::string, FightingLayout> s_fighting_layouts = {
	{"NM00004", FightingLayout::TEKKEN},     // Tekken 4
	{"NM00019", FightingLayout::TEKKEN},     // Tekken 5 / 5.1
	{"NM00026", FightingLayout::TEKKEN},     // Tekken 5 DR
	{"NM00007", FightingLayout::SOULCAL},    // Soul Calibur II
	{"NM00031", FightingLayout::SOULCAL},    // Soul Calibur III
	{"NM00002", FightingLayout::BLOODYROAR}, // Bloody Roar 3
	{"NM00048", FightingLayout::SOULCAL},    // Fate Unlimited Codes
	{"NM00027", FightingLayout::TEKKEN},     // Super Dragon Ball Z
	{"NM00029", FightingLayout::SOULCAL},    // Kinnikuman MGP
	{"NM00035", FightingLayout::GUNDAM},     // YuYu Hakusho
	{"NM00040", FightingLayout::SOULCAL},    // Kinnikuman MGP 2
	{"NM00011", FightingLayout::TEKKEN},     // Pride GP 2003
	{"NM00018", FightingLayout::SIX_BUTTON}, // Capcom Fighting Jam
	{"NM00042", FightingLayout::SOULCAL},    // Sengoku Basara X
	{"NM00013", FightingLayout::GUNDAM},     // Z-Gundam: A.E.U.G. vs Titans
	{"NM00017", FightingLayout::GUNDAM},     // Z-Gundam: A.E.U.G. vs Titans DX
	{"NM00024", FightingLayout::GUNDAM},     // Gundam SEED: Federation vs Z.A.F.T.
	{"NM00034", FightingLayout::GUNDAM},     // Gundam SEED Destiny: Federation vs Z.A.F.T. II
	{"NM00043", FightingLayout::GUNDAM},     // Gundam vs Gundam
	{"NM00052", FightingLayout::GUNDAM},     // Gundam vs Gundam NEXT
};

// Gamepad input -> JVS button state: set or clear a button bit for a player
void ACJV::SetButtonState(u32 player, u16 mask, bool pressed)
{
	if (player >= JVS_PLAYER_COUNT)
		return;
	if (pressed)
		m_jvsButtonState[player] |= mask;
	else
		m_jvsButtonState[player] &= ~mask;
}

// Gamepad coin button -> increment JVS coin counter for P1 (slot 0) or P2 (slot 1)
void ACJV::InsertCoin(u32 slot)
{
	if (slot == 0)
		m_coin1++;
	else if (slot == 1)
		m_coin2++;
}

void ACJV::SetMode(JVS_MODE mode)
{
	m_jvsMode = mode;
}

JVS_MODE ACJV::GetMode()
{
	return m_jvsMode;
}

bool ACJV::IsSindenBorderEnabled()
{
	return s_sinden_border_enabled;
}

int ACJV::GetSindenBorderMode()
{
	return s_sinden_border_mode;
}

int ACJV::GetSindenBorderThickness()
{
	return s_sinden_border_thickness;
}

void ACJV::SetScreenPos(u16 x, u16 y)
{
	m_jvsScreenPosX = x;
	m_jvsScreenPosY = y;
}

// Called from VMManager on game boot. Resets all JVS state and selects per-game I/O config.
const std::string& ACJV::GetGameId() { return s_gameid; }

void ACJV::SetGameId(const std::string& gameid)
{
	s_gameid = gameid;
	// Clean slate: zero all input state on game switch within the emulator
	m_coin1 = 0;
	m_coin2 = 0;
	m_jvsButtonState[0] = 0;
	m_jvsButtonState[1] = 0;
	m_jvsSystemButtonState = 0;
	m_testButtonState = 0;
	m_jvsScreenPosX = 0;
	m_jvsScreenPosY = 0;
	m_jvsLightgunDX = -1.0f;
	m_jvsLightgunDY = -1.0f;
	std::memset(m_jvsWheelChannels, 0, sizeof(m_jvsWheelChannels));
	std::memset(m_jvsDrumChannels, 0, sizeof(m_jvsDrumChannels));

	// Select per-game gun mapping, or fall back to default
	auto it = s_gun_mappings.find(gameid);
	if (it != s_gun_mappings.end())
	{
		m_gunMapping = &it->second;
		Console.WriteLn("ACJV: gun mapping for %s: p1_trigger=0x%04X pedal=0x%04X sensor=0x%04X", gameid.c_str(), it->second.p1_trigger, it->second.pedal, it->second.sensor);
	}
	else
		m_gunMapping = &s_default_gun_mapping;

	auto fit = s_fighting_layouts.find(gameid);
	if (fit != s_fighting_layouts.end())
	{
		constexpr const char* layout_names[] = {"tekken", "gundam", "6-button", "soulcalibur", "bloodyroar"};
		Console.WriteLn("ACJV: fighting layout for %s: %s", gameid.c_str(), layout_names[static_cast<int>(fit->second)]);
		UpdateFightingBindings(fit->second);
	}

	// TC3 has 3 I/O boards: TSS-I/O (white flash), MIU-I/O (640x224), RAYS PCB (0xFFFF).
	// MIU-I/O chosen: no flash artifact, calibration uses JVS trigger debounce directly.
	// RAYS PCB calibration uses DMA protocol (cmd 0x70) that bypasses our JVS handler.
	if (gameid == "NM00012")
		CurrentBoardID = MIU_IO_JPN_GUN_EXTENTI;
	else
		CurrentBoardID = RAYS_PCB;
}

const GunMapping& ACJV::GetGunMapping()
{
	return *m_gunMapping;
}

static void UpdateLightgunFromMouse()
{
	const auto& [mx, my] = InputManager::GetPointerAbsolutePosition(0);
	float dx, dy;
	GSTranslateWindowToDisplayCoordinates(mx, my, &dx, &dy);
	constexpr float edge_margin = 0.01f;
	bool on_screen = (dx >= 0.0f && dy >= 0.0f && dx < (1.0f - edge_margin) && dy < (1.0f - edge_margin));
	if (on_screen)
	{
		m_jvsLightgunDX = dx;
		m_jvsLightgunDY = dy;
		m_jvsScreenPosX = static_cast<u16>((1.0f - dx) * 0xFFFF);
		m_jvsScreenPosY = static_cast<u16>(dy * 0xFFFF);
	}
	else
	{
		m_jvsLightgunDX = -1.0f;
		m_jvsLightgunDY = -1.0f;
		m_jvsScreenPosX = 0;
		m_jvsScreenPosY = 0;
	}
	const auto& gm = ACJV::GetGunMapping();
	if (gm.sensor)
		ACJV::SetButtonState(0, gm.sensor, gm.sensor_active_high ? on_screen : !on_screen);
}

void do_jvs_packet(const u8* input, u8* output) {
	input++;
	u8 inDest = *input++;
	u8 inSize = *input++;
	u8 outSize = 0;
	u32 inWorkChecksum = inDest + inSize;
	inSize--;

	(*output++) = JVS_SYNC;
	(*output++) = 0x00; //Master ID?
	u8* dstSize = output++;
	(*dstSize) = 1;
	(*output++) = JVS_CMD_SUCCESS;
	while(inSize != 0) {
		u8 cmd = (*input++);
		inSize--;
		inWorkChecksum += cmd;
		switch(cmd) {
		case JVS::RESET: {
			JVS_ASSERT(inSize != 0);
			u8 param = (*input++);
			JVS_ASSERT(param == 0xD9);
			inSize--;
			inWorkChecksum += param;
		}
		break;
		case JVS::READ_ID_DATA: {
			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize)++;
			const char* boardName = BOARDS[ACJV::CurrentBoardID].c_str();
			size_t length = strlen(boardName);

			for(int i = 0; i < length + 1; i++)
			{
				(*output++) = boardName[i];
				(*dstSize)++;
			}
		}
		break;
		case JVS::SET_NODE_ADDRESS: {
			JVS_ASSERT(inSize != 0);
			u8 param = (*input++);
			inSize--;
			inWorkChecksum += param;
			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize)++;
		}
		break;
		case JVS::GET_CMDFORMAT_REV: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = 0x13; //Revision 1.3
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_REVISION: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = JVS_REVISION;
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_SUPP_COMM_VER: {
			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = JVS_VERSION;
			(*dstSize) += 2;
		}
		break;
		case JVS::GET_SLAVE_FEAT: {
			(*output++) = JVS_CMD_SUCCESS;

			(*output++) = 0x02;             //Coin input
			(*output++) = JVS_PLAYER_COUNT; //2 Coin slots
			(*output++) = 0x00;
			(*output++) = 0x00;

			(*output++) = 0x01;             //Switch input
			(*output++) = JVS_PLAYER_COUNT; //2 players
			(*output++) = 0x10;             //16 switches
			(*output++) = 0x00;
			// TODO: driving games (e.g. Wangan Midnight)
#if 0
			if(m_jvsMode == JVS_MODE::DRIVE)
			{
				(*output++) = 0x03;                  //Analog Input
				(*output++) = JVS_WHEEL_CHANNEL_MAX; //Channel Count (3 channels)
				(*output++) = 0x10;                  //Bits (16 bits)
				(*output++) = 0x00;
				(*dstSize) += 4;
			}
			else
#endif
			if(m_jvsMode == JVS_MODE::LIGHTGUN)
			{
				(*output++) = 0x06; //Screen Pos Input
				(*output++) = 0x10; //X pos bits
				(*output++) = 0x10; //Y pos bits
				(*output++) = m_gunMapping->p2_trigger ? 0x02 : 0x01; // gun count: 2 if P2 trigger defined (Vampire Night), else 1

				//GPIO for recoil
				(*output++) = 0x12; //GPIO output
				(*output++) = 0x10; //slot(?) count
				(*output++) = 0x00;
				(*output++) = 0x00;

				//Time Crisis 4 reads from analog input to determine screen position
				(*output++) = 0x03; //Analog Input
				(*output++) = 0x02; //Channel Count (2 channels)
				(*output++) = 0x10; //Bits (16 bits)
				(*output++) = 0x00;

				(*dstSize) += 12;
			}
			// TODO: drum games (e.g. Taiko no Tatsujin)
#if 0
			else if(m_jvsMode == JVS_MODE::DRUM)
			{
				(*output++) = 0x03;                 //Analog Input
				(*output++) = JVS_DRUM_CHANNEL_MAX; //Channel Count (8 channels)
				(*output++) = 0x0A;                 //Bits (10 bits)
				(*output++) = 0x00;

				(*dstSize) += 4;
			}
#endif
			// TODO: touch panel games
#if 0
			else if(m_jvsMode == JVS_MODE::TOUCH)
			{
				(*output++) = 0x06; //Screen Pos Input
				(*output++) = 0x10; //X pos bits
				(*output++) = 0x10; //Y pos bits
				(*output++) = 0x01; //channels

				(*dstSize) += 4;
			}
#endif
			(*output++) = 0x00; //End of features

			(*dstSize) += 10;
		}
		break;
		case JVS::CONVEY_ID_MAINBOARD:
		{
			while(1)
			{
				u8 value = (*input++);
				JVS_ASSERT(inSize != 0);
				inSize--;
				inWorkChecksum += value;
				if(value == 0) break;
			}
		}
		break;
		case JVS::READ_INP_SWITCH:
		{
			JVS_ASSERT(inSize >= 2);
			u8 playerCount = (*input++);
			u8 byteCount = (*input++);
			JVS_ASSERT(playerCount >= 1);
			JVS_ASSERT(playerCount <= JVS_PLAYER_COUNT);
			JVS_ASSERT(byteCount == 2);
			inWorkChecksum += playerCount;
			inWorkChecksum += byteCount;
			inSize -= 2;

			(*output++) = JVS_CMD_SUCCESS;
			(*output++) = m_testButtonState|(s_dip_switch_state & TESTMODE);
			//(*output++) = (m_jvsSystemButtonState == 0x03) ? 0x80 : 0;  //Test

			(*output++) = static_cast<u8>(m_jvsButtonState[0]);      //Player 1
			(*output++) = static_cast<u8>(m_jvsButtonState[0] >> 8); //Player 1
			(*dstSize) += 4;

			//if (m_jvsButtonState[0])
			//	Console.WriteLn("JVS P1 buttons: %04X coin:%d", m_jvsButtonState[0], m_coin1);

			if(playerCount == 2)
			{
				(*output++) = static_cast<u8>(m_jvsButtonState[1]);      //Player 2
				(*output++) = static_cast<u8>(m_jvsButtonState[1] >> 8); //Player 2
				(*dstSize) += 2;
			}
		}
		break;
		case JVS::READ_INP_COIN:
		{
			JVS_ASSERT(inSize != 0);
			u8 slotCount = (*input++);
			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			inWorkChecksum += slotCount;
			inSize--;
			u8 slot1Condition = COIN_NORMAL; // see enum COINCOND
			u8 slot2Condition = COIN_NORMAL; // see enum COINCOND

			(*output++) = JVS_CMD_SUCCESS;

			(*output++) = static_cast<u8>(((m_coin1 >> 8) & 0x3f) | (slot1Condition << 6)); //Coin 1 MSB + slot1condition
			(*output++) = static_cast<u8>(m_coin1 & 0x00ff);                                //Coin 1 LSB

			(*dstSize) += 3;

			if(slotCount == 2)
			{
				(*output++) = static_cast<u8>(((m_coin2 >> 8) & 0x3f) | (slot2Condition << 6)); //Coin 2 MSB + slot2condition
				(*output++) = static_cast<u8>(m_coin2 & 0x00ff);                                //Coin 2 LSB

				(*dstSize) += 2;
			}
		}
		break;
		case JVS::OUTPUT_COIN_NUM: // actually never received this jvs cmd
		{
			JVS_ASSERT(inSize >= 3);
			u8 slotCount = (*input++);
			u8 amountMSB = (*input++);
			u8 amountLSB = (*input++);

			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			//inWorkChecksum += slotCount;
			inSize -= 3;

			if(slotCount == 1) m_coin1 += (amountMSB << 8) + amountLSB;
			if(slotCount == 2) m_coin2 += (amountMSB << 8) + amountLSB;

			(*output++) = JVS_CMD_SUCCESS;

			(*dstSize) += 1;
		}
		break;
		case JVS::DECREASE_COIN_NUM: // actually never received this jvs cmd
		{
			JVS_ASSERT(inSize >= 3);
			u8 slotCount = (*input++);
			u8 amountMSB = (*input++);
			u8 amountLSB = (*input++);

			JVS_ASSERT(slotCount >= 1);
			JVS_ASSERT(slotCount <= 2);
			//inWorkChecksum += slotCount;
			inSize -= 3;

			if(slotCount == 1) m_coin1 -= (amountMSB << 8) + amountLSB;
			if(slotCount == 2) m_coin2 -= (amountMSB << 8) + amountLSB;

			(*output++) = JVS_CMD_SUCCESS;

			(*dstSize) += 1;
		}
		break;
		case JVS::READ_INP_ANALOG:
		{
			JVS_ASSERT(inSize != 0);
			u8 channel = (*input++);
			inWorkChecksum += channel;
			inSize--;

			(*output++) = JVS_CMD_SUCCESS;

			// TC4 reads screen position from analog channels instead of SCREENPOS
			if(m_jvsMode == JVS_MODE::LIGHTGUN)
			{
				JVS_ASSERT(channel == 2);
				UpdateLightgunFromMouse();
				(*output++) = static_cast<u8>(m_jvsScreenPosX >> 8); //Pos X MSB
				(*output++) = static_cast<u8>(m_jvsScreenPosX);      //Pos X LSB
				(*output++) = static_cast<u8>(m_jvsScreenPosY >> 8); //Pos Y MSB
				(*output++) = static_cast<u8>(m_jvsScreenPosY);      //Pos Y LSB
			}
			else if(m_jvsMode == JVS_MODE::DRUM)
			{
				JVS_ASSERT(channel == JVS_DRUM_CHANNEL_MAX);
				for(int i = 0; i < JVS_DRUM_CHANNEL_MAX; i++)
				{
					(*output++) = static_cast<u8>(m_jvsDrumChannels[i] >> 8);
					(*output++) = static_cast<u8>(m_jvsDrumChannels[i]);
				}
			}
			else if(m_jvsMode == JVS_MODE::DRIVE)
			{
				for(int i = 0; i < JVS_WHEEL_CHANNEL_MAX; i++)
				{
					(*output++) = static_cast<u8>(m_jvsWheelChannels[i] >> 8);
					(*output++) = static_cast<u8>(m_jvsWheelChannels[i]);
				}
			}

			(*dstSize) += (2 * channel) + 1;
		}
		break;
		case JVS::READ_INP_SCREENPOS:
		{
			JVS_ASSERT(inSize != 0);
			u8 channel = (*input++);
			inWorkChecksum += channel;
			inSize--;

			if(m_jvsMode == JVS_MODE::LIGHTGUN)
				UpdateLightgunFromMouse();

			(*output++) = JVS_CMD_SUCCESS;

			// Screen position scaling depends on I/O board:
			// - MIU-I/O (TC3): native 640x224, Y inverted (bottom-up)
			// - RAYS PCB (TC4, Cobra, VPN): full 16-bit range 0xFFFF, Y inverted (bottom-up)
			// pos=0 means off-screen in JVS, so on-screen values are clamped to minimum 1
			u16 posX = 0, posY = 0;
			if(m_jvsMode == JVS_MODE::LIGHTGUN && m_jvsLightgunDX >= 0.0f)
			{
				const float scaleX = (ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI) ? 640.0f : 0xFFFF;
				const float scaleY = (ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI) ? 224.0f : 0xFFFF;
				posX = static_cast<u16>(m_jvsLightgunDX * scaleX);
				if (ACJV::CurrentBoardID == RAYS_PCB || ACJV::CurrentBoardID == MIU_IO_JPN_GUN_EXTENTI)
					posY = static_cast<u16>((1.0f - m_jvsLightgunDY) * scaleY);
				else
					posY = static_cast<u16>(m_jvsLightgunDY * scaleY);
				if (posX == 0) posX = 1;
				if (posY == 0) posY = 1;
			}
			for (u8 ch = 0; ch < channel; ch++)
			{
				(*output++) = static_cast<u8>(posX >> 8);
				(*output++) = static_cast<u8>(posX);
				(*output++) = static_cast<u8>(posY >> 8);
				(*output++) = static_cast<u8>(posY);
			}

			(*dstSize) += 1 + (4 * channel);
		}
		break;
		// GPIO output — game sends byte values to control physical outputs (e.g. gun recoil solenoids).
		// Byte 1 = P1 recoil: value >= 0x50 means recoil triggered, value 0xC0 observed during fire.
		// TODO: forward p1Recoil to serial port / USB for real lightgun recoil hardware
		case JVS::OUTPUT_GENERAL:
		{
			JVS_ASSERT(inSize >= 2);

			u8 bytecount = (*input++);
			inWorkChecksum += bytecount;
			inSize--;

			for(int i = 1; i <= bytecount; i++)
			{
				u8 gpvalue = (*input++);
				inWorkChecksum += gpvalue;
				inSize--;

				if(i == 1)
				{
					int p1Recoil = (gpvalue >= 0x50) ? 1 : 0;
					(void)p1Recoil;
				}
			}

			(*output++) = JVS_CMD_SUCCESS;
			(*dstSize) += 1;
		}
		break;
		default:
			//Unknown command
			// Console.Error("ACJV::%s: unknown JVS CMD 0x%X", __FUNCTION__, cmd);
			break;
		}
	}
	u8 inChecksum = (*input);
	// if (inChecksum != (inWorkChecksum & 0xFF))
	//     Console.Warning("ACJV::%s: checksum mismatch: %02X | %02X", __FUNCTION__, inChecksum, inWorkChecksum&0xFF);
}


// based on https://github.com/search?q=repo%3Ajpd002/Play-%20CSys246%3A%3AProcessJvsPacket&type=code by Jean-Philip Desjardins
void do_acjv_packet() {
	const u16* wr16 = wrbuf_getu16();
	u16* rd16 = rdbuf_getu16();
	rd16[0] = wr16[0];
	u16 RootPacketID = wr16[8];
	if(rd16[0] == 0x3E6F) {
		rd16[1]      = 0x208;        // unconfirmed: firmware version
		rd16[0x14]   = RootPacketID; // Xored with value at 0x10 in send packet, needs to be the same
		rd16[0x21]   = wr16[0x0D];
		rd16[0x30]  = s_dip_switch_state; // here the game polls the dip switch values?
		u16 PacketID = wr16[0x0C];
		if(PacketID != 0) {
			if(wrbuf[0x122] == JVS_SYNC) {
				do_jvs_packet(&wrbuf[0x122], &rdbuf[0x15A]);
			} else {
				do_jvs_packet(&wrbuf[0x22],  &rdbuf[0x5A]);
			}
			rd16[0x20] = PacketID;
		}
		rd16[0x15] = 0x5210;
		rd16[0x16] = 0x5210;
		rd16[0x17] = 0x5210;
	}
}
