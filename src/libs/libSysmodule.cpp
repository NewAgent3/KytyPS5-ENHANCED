#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

// Sysmodule ids from the PS4 SDK (sceSysmodule.h). The emulator links the
// functionality of most modules directly into the main binary, so loading
// always succeeds; the id table exists to validate and log guest requests.
constexpr uint16_t MODULE_NET                = 0x1;
constexpr uint16_t MODULE_HTTP               = 0x2;
constexpr uint16_t MODULE_SSL                = 0x4;
constexpr uint16_t MODULE_NP_COMMON          = 0x5;
constexpr uint16_t MODULE_NP_SNS             = 0x6;
constexpr uint16_t MODULE_NP_AUTH            = 0x7;
constexpr uint16_t MODULE_NP_UTIL            = 0x8;
constexpr uint16_t MODULE_NPWebApi           = 0x9;
constexpr uint16_t MODULE_NP_SCORE_RANKING   = 0xa;
constexpr uint16_t MODULE_NP_MATCHING2       = 0xb;
constexpr uint16_t MODULE_NP_TROPHY          = 0xc;
constexpr uint16_t MODULE_NP_MESSAGE         = 0xd;
constexpr uint16_t MODULE_NP_FRIEND          = 0xe;
constexpr uint16_t MODULE_NP_PROFILE_UI      = 0xf;
constexpr uint16_t MODULE_NP_AUTHORIZATION   = 0x10;
constexpr uint16_t MODULE_WEB                = 0x12;
constexpr uint16_t MODULE_SPREMOTEPLAY       = 0x13;
constexpr uint16_t MODULE_GAMELIVESTREAMING  = 0x14;
constexpr uint16_t MODULE_COMPANIONAPP       = 0x15;
constexpr uint16_t MODULE_GAMECUSTOMCONTROLDATA = 0x16;
constexpr uint16_t MODULE_USBSTORAGE         = 0x18;
constexpr uint16_t MODULE_APPCHECK           = 0x1b;
constexpr uint16_t MODULE_SMARTCARD          = 0x1d;
constexpr uint16_t MODULE_NETCTL             = 0x20;
constexpr uint16_t MODULE_SCENE_A           = 0x21;
constexpr uint16_t MODULE_SAMPLE_SESSION     = 0x22;
constexpr uint16_t MODULE_REQUESTSECURITY    = 0x23;
constexpr uint16_t MODULE_USERPROFILES       = 0x24;
constexpr uint16_t MODULE_SYSTEMAGC          = 0x25;
constexpr uint16_t MODULE_UPDATE            = 0x26;
constexpr uint16_t MODULE_AUDIO3D           = 0x27;
constexpr uint16_t MODULE_SYSTEMLOGGER      = 0x28;
constexpr uint16_t MODULE_NP_SNS_FACEBOOK   = 0x29;
constexpr uint16_t MODULE_FACE              = 0x2b;
constexpr uint16_t MODULE_SMART             = 0x2e;
constexpr uint16_t MODULE_SURF              = 0x30;
constexpr uint16_t MODULE_AUTH              = 0x31;
constexpr uint16_t MODULE_FIT               = 0x33;
constexpr uint16_t MODULE_NP_DECIDIOUTILITY = 0x34;
constexpr uint16_t MODULE_RTC               = 0x35;
constexpr uint16_t MODULE_NP_EULA           = 0x36;
constexpr uint16_t MODULE_VE_VIDEO_EDITOR   = 0x37;
constexpr uint16_t MODULE_VE_VIDEO_SEARCH   = 0x38;
constexpr uint16_t MODULE_CURSOR            = 0x3b;
constexpr uint16_t MODULE_MESSAGE_DIALOG    = 0x3c;
constexpr uint16_t MODULE_IME_DIALOG        = 0x3d;
constexpr uint16_t MODULE_NP_PUSH_NOTIFICATION = 0x3e;
constexpr uint16_t MODULE_PSM_IROHA         = 0x40;
constexpr uint16_t MODULE_SPELLCHECK       = 0x43;
constexpr uint16_t MODULE_WEB_BROWSER      = 0x44;
constexpr uint16_t MODULE_NP_ACTIVITYFEED  = 0x46;
constexpr uint16_t MODULE_VIDEOOUT_UI      = 0x47;
constexpr uint16_t MODULE_AVPLAYER         = 0x48;
constexpr uint16_t MODULE_NP_SIGNALING     = 0x49;
constexpr uint16_t MODULE_KOREAN_IME       = 0x4a;
constexpr uint16_t MODULE_CHINESE_IME      = 0x4b;
constexpr uint16_t MODULE_MOVE             = 0x4c;
constexpr uint16_t MODULE_NP_PROFILE       = 0x4f;
constexpr uint16_t MODULE_SYSTEM_SETTINGS_DIALOG = 0x51;
constexpr uint16_t MODULE_DATA_MESSAGE_DIALOG = 0x52;
constexpr uint16_t MODULE_NP_UNIFIED_FRIEND = 0x53;
constexpr uint16_t MODULE_PRIME            = 0x55;
constexpr uint16_t MODULE_MOVE_CONTROLLER  = 0x56;
constexpr uint16_t MODULE_IME              = 0x5b;
constexpr uint16_t MODULE_CONTENT_DELETE   = 0x5c;
constexpr uint16_t MODULE_PERF            = 0x5d;
constexpr uint16_t MODULE_NP_TUS           = 0x61;
constexpr uint16_t MODULE_BGFT            = 0x64;
constexpr uint16_t MODULE_SBLIB           = 0x65;
constexpr uint16_t MODULE_NP_PARTY        = 0x67;
constexpr uint16_t MODULE_SYSTEM_NOTIFICATIONS = 0x68;
constexpr uint16_t MODULE_NP_SONYENTITLEMENT = 0x69;
constexpr uint16_t MODULE_CLOUD_CLIENT_DAEMON = 0x6a;
constexpr uint16_t MODULE_SERVICE_ACTIVITY = 0x6b;
constexpr uint16_t MODULE_JOBSTATUS        = 0x6c;
constexpr uint16_t MODULE_NP_SNS_TWITTER   = 0x70;
constexpr uint16_t MODULE_RINGDHT          = 0x71;
constexpr uint16_t MODULE_NP              = 0x72;
constexpr uint16_t MODULE_SETC            = 0x74;
constexpr uint16_t MODULE_SYSCHECK        = 0x75;

struct ModuleIdName {
	uint16_t    id;
	const char* name;
};

constexpr ModuleIdName MODULE_ID_NAMES[] = {
    {MODULE_NET, "Net"},
    {MODULE_HTTP, "Http"},
    {MODULE_SSL, "Ssl"},
    {MODULE_NP_COMMON, "NpCommon"},
    {MODULE_NP_MATCHING2, "NpMatching2"},
    {MODULE_NP_TROPHY, "NpTrophy"},
    {MODULE_NP_MESSAGE, "NpMessage"},
    {MODULE_NP_FRIEND, "NpFriend"},
    {MODULE_NPWebApi, "NpWebApi"},
    {MODULE_WEB, "Web"},
    {MODULE_SPREMOTEPLAY, "RemotePlay"},
    {MODULE_GAMELIVESTREAMING, "GameLiveStreaming"},
    {MODULE_COMPANIONAPP, "CompanionApp"},
    {MODULE_APPCHECK, "AppCheck"},
    {MODULE_NETCTL, "NetCtl"},
    {MODULE_USERPROFILES, "UserProfiles"},
    {MODULE_SYSTEMAGC, "SystemAgc"},
    {MODULE_AUDIO3D, "Audio3d"},
    {MODULE_FACE, "Face"},
    {MODULE_AUTH, "Auth"},
    {MODULE_RTC, "Rtc"},
    {MODULE_MESSAGE_DIALOG, "MessageDialog"},
    {MODULE_IME_DIALOG, "ImeDialog"},
    {MODULE_IME, "Ime"},
    {MODULE_NP, "Np"},
    {MODULE_BGFT, "Bgft"},
};

constexpr int SYSMODULE_ERROR_UNKNOW_MODULE = -2136969215; // 0x805F0001

const char* ModuleIdToName(uint16_t id) {
	for (const auto& entry: MODULE_ID_NAMES) {
		if (entry.id == id) {
			return entry.name;
		}
	}
	return nullptr;
}

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

static KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	const auto* name = ModuleIdToName(id);
	LOGF("\t id = %d (%s)\n", static_cast<int>(id), name != nullptr ? name : "unknown");

	if (name == nullptr) {
		// Unknown module ids fail on the real console; reject them so games do
		// not depend on behavior of modules the emulator does not implement.
		LOGF("SysmoduleLoadModule: unknown module id %d\n", static_cast<int>(id));
		return SYSMODULE_ERROR_UNKNOW_MODULE;
	}

	return OK;
}

static KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	const auto* name = ModuleIdToName(id);
	LOGF("\t id = %d (%s)\n", static_cast<int>(id), name != nullptr ? name : "unknown");

	return OK;
}

static KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2,
                                                            int arg3, int* ret) {
	PRINT_NAME();

	const auto* name = ModuleIdToName(id);
	LOGF("\t id = %d (%s)\n"
	     "\t arg1 = %d arg2 = %d arg3 = %d\n",
	     static_cast<int>(id), name != nullptr ? name : "unknown", arg1, arg2, arg3);

	EXIT_IF(arg1 != 0);
	EXIT_IF(arg2 != 0);
	EXIT_IF(arg3 != 0);
	EXIT_IF(ret == nullptr);

	*ret = 0;

	return OK;
}

static KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	const auto* name = ModuleIdToName(id);
	LOGF("\t id = %d (%s)\n", static_cast<int>(id), name != nullptr ? name : "unknown");

	return OK;
}

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1) {
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
}

} // namespace Libs
