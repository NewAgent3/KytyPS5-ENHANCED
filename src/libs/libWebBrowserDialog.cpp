#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>

namespace Libs {

LIB_VERSION("WebBrowserDialog", 1, "WebBrowserDialog", 1, 1);

namespace WebBrowserDialog {

// The web browser dialog is never opened by the emulator, so its status
// machine reports "none" and every terminal state call fails with the
// corresponding SDK error codes.
constexpr int WEB_BROWSER_DIALOG_STATUS_NONE     = 0;
constexpr int WEB_BROWSER_DIALOG_STATUS_RUNNING  = 1;
constexpr int WEB_BROWSER_DIALOG_STATUS_FINISHED = 2;

constexpr int WEB_BROWSER_DIALOG_ERROR_NOT_OPEN = -2146498559; // 0x801f0001

static int KYTY_SYSV_ABI WebBrowserDialogGetStatus() {
	PRINT_NAME();

	return WEB_BROWSER_DIALOG_STATUS_NONE;
}

static int KYTY_SYSV_ABI WebBrowserDialogOpen(void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	// Without a web view implementation there is no dialog to open.
	return WEB_BROWSER_DIALOG_ERROR_NOT_OPEN;
}

static int KYTY_SYSV_ABI WebBrowserDialogClose() {
	PRINT_NAME();

	return WEB_BROWSER_DIALOG_ERROR_NOT_OPEN;
}

static int KYTY_SYSV_ABI WebBrowserDialogGetResult(void* result) {
	PRINT_NAME();

	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	return WEB_BROWSER_DIALOG_ERROR_NOT_OPEN;
}

static int KYTY_SYSV_ABI WebBrowserDialogTerminate() {
	PRINT_NAME();

	return OK;
}

} // namespace WebBrowserDialog

LIB_DEFINE(InitWebBrowserDialog_1) {
	LIB_FUNC("CFTG6a8TjOU", WebBrowserDialog::WebBrowserDialogGetStatus);
	LIB_FUNC("d7g4SNp9xMM", WebBrowserDialog::WebBrowserDialogOpen);
	LIB_FUNC("HtP3e3KKGgY", WebBrowserDialog::WebBrowserDialogClose);
	LIB_FUNC("6NRVHmmyNMG", WebBrowserDialog::WebBrowserDialogGetResult);
	LIB_FUNC("b0hNyDEkMfY", WebBrowserDialog::WebBrowserDialogTerminate);
}

} // namespace Libs
