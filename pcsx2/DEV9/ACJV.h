#pragma once
#include <span>
#include "MemoryTypes.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"
#include "common/ARCADE.h"

class SettingsInterface;
struct InputBindingInfo;

#define ACJV_BASE_ADDDR 0x12400000
#define ACJV_RANGE      0x1240
#define ACJV_ADDR_CAP   0x8000 // ACJV.IRX r/w fun will do `(2 * (addr & 0x3FFF)`, meaning available range is `0x12400000` - `0x12407FFE`
#define ACJV_PACKETSIZE 0x300 // seems to be the amount of bytes that's always read/written by the game

#define ACJV_RDBASE 0x12404000 // acmeme access addr 0x2300
#define ACJV_WRBASE 0x12404600 // acmeme access addr 0x2000

#define ACJV_CTR_START 0x12416002 // set to 0 when ACJV.IRX runs
#define ACJV_CTR_STOP  0x12416000 // set to 0 during: JVFIRM upload begins, ACCORE starts, `acJvModuleStop()` is called

#define ACJV_RDWR_SIZELIMIT 0x4000 // ACJV.IRX read/write functions only consider 14 bits from addr

#define JVS_CMD_SUCCESS 0x1
#define JVS_REVISION 0x30 //Revision 3.0
#define JVS_VERSION 0x10 //Version 1.0
#define JVS_PLAYER_COUNT 2 


enum BOARDID {
	RAYS_PCB = 0,
	FCA_1_JPN_MULTIPURPOSE,
	FCB_JPN_TOUCHPANEL,
	TSS_GUN_EXTENTION,
	MIU_IO_JPN_GUN_EXTENTI
};

enum class JVS_MODE {
	DEFAULT,
	LIGHTGUN,
	FIGHTING,
	DRIVE,
	DRUM,
	TOUCH,
};

// Sw -> pad order per layout, mirroring each game's official PS2 port.
enum class FightingLayout {
	TEKKEN,     // Sw1,2,4,5: Square, Triangle, Cross, Circle         — TK4, TK5, TK5DR, SDBZ, PrideGP
	GUNDAM,     // Sw1,2,3,4: Square, Triangle, Cross, Circle         — YuYu + Gundam VS (zgundm/dx, SEED/Dest, GvG/NEXT)
	SIX_BUTTON, // Sw1-6:     Square, Triangle, L1, Cross, Circle, R1 — JAM
	SOULCAL,    // Sw1,2,3,4: Square, Triangle, Circle, Cross         — SC2, SC3, KN1, KN2, BAX, FUD
	BLOODYROAR, // Sw1,2,3,4: Square, Cross, Circle, Triangle         — BR3
};

#define JVS_WHEEL_CHANNEL_MAX 3
#define JVS_DRUM_CHANNEL_MAX 8

struct GunMapping {
    u16 pedal;
    u16 sensor;
    bool sensor_active_high;
    u16 p1_start;
    u16 p2_start;
    u16 p1_trigger;
    u16 p2_trigger;
};

namespace ACJV {
    enum : u32
    {
        NUM_DIP_SWITCHES = 4,
    };

    static constexpr const char* CONFIG_SECTION = "JVS";
    static constexpr const char* TRANSLATION_CONTEXT = "JVS";

    struct DIPSwitchInfo
    {
        const char* name;
        const char* display_name;
        const char* toggle_bind_name;
        bool default_value;
    };

    extern bool enabled;

    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);
    extern enum BOARDID CurrentBoardID;

    std::span<const DIPSwitchInfo> GetDIPSwitches();
    const DIPSwitchInfo& GetTestModeDIPSwitch();
    const DIPSwitchInfo& GetVideoVoltageDIPSwitch();
    const DIPSwitchInfo& GetMonitorSyncFrequencyDIPSwitch();
    const DIPSwitchInfo& GetVideoSyncSplitDIPSwitch();
    std::span<const InputBindingInfo> GetDIPSwitchBindings();
    std::span<const InputBindingInfo> GetButtonBindings();
    std::span<const InputBindingInfo> GetP2ButtonBindings();
    std::span<const InputBindingInfo> GetCoinBindings();
    void SetButtonState(u32 player, u16 mask, bool pressed);
    void InsertCoin(u32 slot);

    bool GetDIPSwitchState(u32 index);
    void SetDIPSwitchState(u32 index, bool enabled);
    void ToggleDIPSwitchState(u32 index);
    void LoadConfig(const SettingsInterface& si);
    void CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_settings = true, bool copy_bindings = true);
    void SetDefaultConfiguration(SettingsInterface& si);

    bool IsSuppressDaemonEnabled();
    const char* GetBoardDisplayName(BOARDID id);
    BOARDID GetCurrentBoardID();
    void SetMode(JVS_MODE mode);
    JVS_MODE GetMode();
    void SetScreenPos(u16 x, u16 y);
    void SetGameId(const std::string& gameid);
    const std::string& GetGameId();
    const GunMapping& GetGunMapping();

    bool IsSindenBorderEnabled();
    int GetSindenBorderMode();
    int GetSindenBorderThickness();
}


enum JVS { //https://github.com/TheOnlyJoey/openjvs/wiki/Command-list
    /// broadcast commands (meant for all active nodes, sent to node ID FF)
    RESET = 0xF0, // [REQUIRED]
    SET_NODE_ADDRESS = 0xF1, // [REQUIRED]
    SETUP_COMMS = 0xF2,
    /// initialization commands
    READ_ID_DATA = 0x10, // [REQUIRED]
    GET_CMDFORMAT_REV = 0x11, // [REQUIRED] get command format revision
    GET_REVISION = 0x12, // [REQUIRED] Get JVS revision
    GET_SUPP_COMM_VER= 0x13, // [REQUIRED] get supported communications versions
    GET_SLAVE_FEAT = 0x14, // [REQUIRED] indicates which commands of this enum are supported
    CONVEY_ID_MAINBOARD = 0x15, // convey ID of main board
    /// I/O commands
    READ_INP_SWITCH = 0x20, //read switch inputs
    READ_INP_COIN = 0x21, //read coin inputs
    READ_INP_ANALOG = 0x22, //read analog inputs
    READ_INP_ROTATORY = 0x23, //read rotary inputs
    READ_INP_KEYCODE = 0x24, //read key code input?
    READ_INP_SCREENPOS = 0x25, //read screen position input (duckhunt gun style)
    READ_INP_GENERAL_PURPOSE = 0x26, //read general-purpose (unrelated to player) input
    UNKNOWN_2e = 0x2e, //something with payouts?
    RETRANSIT_DATA_ON_FAIL = 0x2f, // [REQUIRED] request to retransmit data in case of checksum failure
    DECREASE_COIN_NUM = 0x30, //decrease number of coins
    OUTPUT_NUM_PAYOUT = 0x31, //output the number of payouts?
    OUTPUT_GENERAL = 0x32, //general-purpose output
    PUTPUT_ANALOG = 0x33, //analog output
    OUTPUT_CHAR_DATA = 0x34, //output character data, e.g. to LCD
    OUTPUT_COIN_NUM = 0x35, //output the total number of coins?
    SUBSTRACT_PAYOUT = 0x36, //subtract payouts?
    OUTPUT_GENERAL_PURPOSE2 = 0x37, //general-purpose output 2
    OUTPUT_GENERAL_PURPOSE3 = 0x38, //general-purpose output 3
    /// Manufacturer-specific
    //60-7F	 	reserved for manufacturer-specific commands
};

enum COINCOND { // Coin Slot condition
			COIN_NORMAL=0,// normal
			COIN_JAMMED,  // coin jam
			COIN_DISCON,  // counter disconnected
			COIN_BUSY,    // busy
};

enum DIPS {
    TESTMODE = 0x80,
    VIDEO_VOLTAGE = 0x40,
    MONITOR_SYNCFREQ = 0x20,
    VIDEO_SYNC_SPLIT = 0x10,
};

// To improve and correct when adding more JVS controls from games
enum JVSButton : u16 {
    JVS_BTN_START   = 0x80,
    JVS_BTN_SERVICE = 0x40,
    JVS_BTN_UP      = 0x20,
    JVS_BTN_DOWN    = 0x10,
    JVS_BTN_LEFT    = 0x08,
    JVS_BTN_RIGHT   = 0x04,
    JVS_BTN_1       = 0x02,
    JVS_BTN_2       = 0x01,
    JVS_BTN_3       = 0x8000,
    JVS_BTN_4       = 0x4000,
    JVS_BTN_5       = 0x2000,
    JVS_BTN_6       = 0x1000,
};


#define JVS_SYNC 0xE0
