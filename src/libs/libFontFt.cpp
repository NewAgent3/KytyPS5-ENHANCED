#include "common/abi.h"
#include "common/common.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

LIB_VERSION("FontFt", 1, "FontFt", 1, 1);

namespace FontFt {

// The Font library forwards all glyph rendering to the FreeType-based FontFt
// module. Selection functions return an opaque handle the library passes back
// on every subsequent font call; the emulator keeps a tiny tagged state so
// library/renderer selection mismatch can be detected.
constexpr int FONT_FT_ERROR_INVALID_PARAMETER = -2135779199; // 0x80b90001

struct SelectionState {
	int value;
	int kind;
};

void* KYTY_SYSV_ABI FontSelectLibraryFt(int value) {
	PRINT_NAME();

	LOGF("\t value = %d\n", value);

	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 0;

	return &s_selection;
}

void* KYTY_SYSV_ABI FontSelectRendererFt(int value) {
	PRINT_NAME();

	LOGF("\t value = %d\n", value);

	static SelectionState s_selection {};
	s_selection.value = value;
	s_selection.kind  = 1;

	return &s_selection;
}

int KYTY_SYSV_ABI FontAllocMemoryFt(void* (*allocate)(uint64_t size, void* user_data),
                                    void (*deallocate)(void* pointer, void* user_data),
                                    void (*reallocate)(void** pointer, uint64_t size,
                                                       void* user_data),
                                    void* user_data) {
	PRINT_NAME();

	LOGF("\t allocate   = 0x%016" PRIx64 "\n"
	     "\t deallocate = 0x%016" PRIx64 "\n"
	     "\t reallocate = 0x%016" PRIx64 "\n"
	     "\t user_data  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(allocate), reinterpret_cast<uint64_t>(deallocate),
	     reinterpret_cast<uint64_t>(reallocate), reinterpret_cast<uint64_t>(user_data));

	if (allocate == nullptr || deallocate == nullptr || reallocate == nullptr) {
		return FONT_FT_ERROR_INVALID_PARAMETER;
	}

	// The emulator uses the system allocator; the guest hooks are recorded and
	// otherwise unused.
	return OK;
}

int KYTY_SYSV_ABI FontGetLibraryRevisionFt(uint32_t* revision) {
	PRINT_NAME();

	if (revision == nullptr) {
		return FONT_FT_ERROR_INVALID_PARAMETER;
	}

	// FreeType 2.13 was the revision shipped in PS5 system software builds
	// observed so far.
	*revision = 0x020d0000u;

	return OK;
}

} // namespace FontFt

LIB_DEFINE(InitFontFt_1) {
	LIB_FUNC("oM+XCzVG3oM", FontFt::FontSelectLibraryFt);
	LIB_FUNC("Xx974EW-QFY", FontFt::FontSelectRendererFt);
	LIB_FUNC("GmY9RlqTzVb", FontFt::FontAllocMemoryFt);
	LIB_FUNC("vJn7QsKfWco", FontFt::FontGetLibraryRevisionFt);
}

} // namespace Libs
