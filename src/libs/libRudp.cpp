#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

LIB_VERSION("Rudp", 1, "Rudp", 1, 1);

namespace Rudp {

// The PS4/PS5 Rudp module implements Sony's reliable-UDP transport used by
// matchmaking and peer-to-peer gameplay. Games only drive it through the
// event handler, so the emulator models a minimal context table that keeps
// handler state and reports errors through the same event ids.
using RudpEventHandler = void (*)(int ctx_id, int event_id, int error_code, void* arg);
using RudpSendHandler  = void (*)(int ctx_id, int event_id, void* packet, int packet_size,
                                  void* arg);

constexpr int RUDP_ERROR_INVALID_CTX_ID     = -2145386495; // 0x80293001
constexpr int RUDP_ERROR_INVALID_ARGUMENT   = -2145386494; // 0x80293002
constexpr int RUDP_ERROR_CTX_ALREADY_EXISTS = -2145386493; // 0x80293003
constexpr int RUDP_ERROR_CTX_NOT_FOUND      = -2145386492; // 0x80293004
constexpr int RUDP_ERROR_NOT_INITIALIZED    = -2145386491; // 0x80293005

// Event ids reported through RudpEventHandler (see PS4 Rudp NID documentation).
constexpr int RUDP_EVENT_SEND_READY     = 0;
constexpr int RUDP_EVENT_RECV_READY     = 1;
constexpr int RUDP_EVENT_ERROR          = 2;
constexpr int RUDP_EVENT_DISCONNECTED   = 3;

constexpr int RUDP_MAX_CONTEXTS = 16;

struct RudpContext {
	bool     created     = false;
	uint32_t local_port  = 0;
	uint32_t remote_port = 0;
	uint32_t max_packet_size = 0;
	bool     dtls_enabled   = false;
};

static RudpEventHandler g_event_handler = nullptr;
static RudpSendHandler  g_send_handler  = nullptr;
static void*            g_event_arg     = nullptr;
static bool             g_initialized   = false;
static RudpContext      g_contexts[RUDP_MAX_CONTEXTS] = {};

static bool IsValidContext(int ctx_id) {
	return ctx_id >= 0 && ctx_id < RUDP_MAX_CONTEXTS && g_contexts[ctx_id].created;
}

static void Notify(int ctx_id, int event_id, int error_code) {
	if (g_event_handler != nullptr) {
		g_event_handler(ctx_id, event_id, error_code, g_event_arg);
	}
}

static KYTY_SYSV_ABI int RudpInit(void* mem_pool, int mem_pool_size) {
	PRINT_NAME();

	LOGF("\t mem_pool      = 0x%016" PRIx64 "\n"
	     "\t mem_pool_size = %d\n",
	     reinterpret_cast<uint64_t>(mem_pool), mem_pool_size);

	if (mem_pool == nullptr || mem_pool_size <= 0) {
		return RUDP_ERROR_INVALID_ARGUMENT;
	}

	g_initialized = true;

	return OK;
}

static KYTY_SYSV_ABI int RudpEnableInternalIOThread(uint32_t stack_size, uint32_t priority) {
	PRINT_NAME();

	LOGF("\t stack_size = %" PRIu32 "\n"
	     "\t priority   = %" PRIu32 "\n",
	     stack_size, priority);

	return OK;
}

static KYTY_SYSV_ABI int RudpSetEventHandler(RudpEventHandler handler, void* arg) {
	PRINT_NAME();

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t arg     = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(handler), reinterpret_cast<uint64_t>(arg));

	g_event_handler = handler;
	g_event_arg     = arg;

	return OK;
}

static KYTY_SYSV_ABI int RudpSetSendHandler(RudpSendHandler handler, void* arg) {
	PRINT_NAME();

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t arg     = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(handler), reinterpret_cast<uint64_t>(arg));

	g_send_handler = handler;
	g_event_arg    = arg;

	return OK;
}

static KYTY_SYSV_ABI int RudpCreateContext(int* ctx_id) {
	PRINT_NAME();

	if (ctx_id == nullptr) {
		return RUDP_ERROR_INVALID_ARGUMENT;
	}
	if (!g_initialized) {
		return RUDP_ERROR_NOT_INITIALIZED;
	}

	for (int id = 0; id < RUDP_MAX_CONTEXTS; id++) {
		if (!g_contexts[id].created) {
			g_contexts[id]         = {};
			g_contexts[id].created = true;
			*ctx_id                = id;
			LOGF("\t created ctx_id = %d\n", id);
			return OK;
		}
	}

	LOGF("\t no free Rudp contexts\n");
	return RUDP_ERROR_CTX_ALREADY_EXISTS;
}

static KYTY_SYSV_ABI int RudpDeleteContext(int ctx_id) {
	PRINT_NAME();

	LOGF("\t ctx_id = %d\n", ctx_id);

	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}

	g_contexts[ctx_id]         = {};
	g_contexts[ctx_id].created = false;
	Notify(ctx_id, RUDP_EVENT_DISCONNECTED, OK);

	return OK;
}

static KYTY_SYSV_ABI int RudpSetOption(int ctx_id, int option, int value) {
	PRINT_NAME();

	LOGF("\t ctx_id = %d\n"
	     "\t option = %d\n"
	     "\t value  = %d\n",
	     ctx_id, option, value);

	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}
	// Options are transport tuning knobs; accept and ignore them.
	(void)value;

	return OK;
}

static KYTY_SYSV_ABI int RudpBind(int ctx_id, uint32_t local_port) {
	PRINT_NAME();

	LOGF("\t ctx_id     = %d\n"
	     "\t local_port = %" PRIu32 "\n",
	     ctx_id, local_port);

	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}

	g_contexts[ctx_id].local_port = local_port;

	return OK;
}

static KYTY_SYSV_ABI int RudpConnect(int ctx_id, uint64_t remote_address, uint32_t remote_port,
                                     uint32_t max_packet_size, bool dtls_enabled) {
	PRINT_NAME();

	LOGF("\t ctx_id          = %d\n"
	     "\t remote_address  = 0x%016" PRIx64 "\n"
	     "\t remote_port     = %" PRIu32 "\n"
	     "\t max_packet_size = %" PRIu32 "\n"
	     "\t dtls_enabled    = %s\n",
	     ctx_id, remote_address, remote_port, max_packet_size,
	     dtls_enabled ? "true" : "false");

	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}

	auto& context             = g_contexts[ctx_id];
	context.remote_port       = remote_port;
	context.max_packet_size   = max_packet_size;
	context.dtls_enabled      = dtls_enabled;

	// There is no real peer; report the connection ready immediately so games
	// that require an established session keep running.
	Notify(ctx_id, RUDP_EVENT_SEND_READY, OK);

	return OK;
}

static KYTY_SYSV_ABI int RudpSend(int ctx_id, void* packet, int packet_size) {
	PRINT_NAME();

	LOGF("\t ctx_id      = %d\n"
	     "\t packet      = 0x%016" PRIx64 "\n"
	     "\t packet_size = %d\n",
	     ctx_id, reinterpret_cast<uint64_t>(packet), packet_size);

	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}
	if (packet == nullptr || packet_size <= 0) {
		return RUDP_ERROR_INVALID_ARGUMENT;
	}

	// Loop the packet back through the registered send handler, mirroring the
	// module's real behavior of handing ownership back to the caller.
	if (g_send_handler != nullptr) {
		g_send_handler(ctx_id, RUDP_EVENT_SEND_READY, packet, packet_size, g_event_arg);
	}

	return OK;
}

static KYTY_SYSV_ABI int RudpGetLocalPort(int ctx_id, uint32_t* local_port) {
	PRINT_NAME();

	if (local_port == nullptr) {
		return RUDP_ERROR_INVALID_ARGUMENT;
	}
	if (!IsValidContext(ctx_id)) {
		return RUDP_ERROR_INVALID_CTX_ID;
	}

	*local_port = g_contexts[ctx_id].local_port;

	return OK;
}

} // namespace Rudp

LIB_DEFINE(InitRudp_1) {
	LIB_FUNC("amuBfI-AQc4", Rudp::RudpInit);
	LIB_FUNC("6PBNpsgyaxw", Rudp::RudpEnableInternalIOThread);
	LIB_FUNC("SUEVes8gvmw", Rudp::RudpSetEventHandler);
	LIB_FUNC("WvPQ8s0qXCM", Rudp::RudpSetSendHandler);
	LIB_FUNC("Z1Tj8PqOwLE", Rudp::RudpCreateContext);
	LIB_FUNC("bDsZ2VdNkMg", Rudp::RudpDeleteContext);
	LIB_FUNC("eXzT4y5XcQM", Rudp::RudpSetOption);
	LIB_FUNC("hRaN3W9vOJE", Rudp::RudpBind);
	LIB_FUNC("kTqF7m2nZVU", Rudp::RudpConnect);
	LIB_FUNC("pLwE6r8sQHY", Rudp::RudpSend);
	LIB_FUNC("uNmC4x7tGDA", Rudp::RudpGetLocalPort);
}

} // namespace Libs
