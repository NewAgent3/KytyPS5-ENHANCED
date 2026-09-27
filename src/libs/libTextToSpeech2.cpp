#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>

namespace Libs {

LIB_VERSION("TextToSpeech2", 1, "TextToSpeech2", 1, 1);

namespace TextToSpeech2 {

// The emulator has no speech synthesis backend, so the TTS2 module models a
// permanently idle speaker: speech never starts, and status calls report
// "stopped" through the SDK's status codes.
constexpr int TTS2_STATUS_STOPPED  = 0;
constexpr int TTS2_STATUS_SPEAKING = 1;

struct SpeakerState {
	bool open  = false;
	bool muted = false;
	int  volume = 100;
};

static SpeakerState g_speaker {};

static int KYTY_SYSV_ABI TextToSpeech2OpenSpeaker(int speaker_id) {
	PRINT_NAME();

	LOGF("\t speaker_id = %d\n", speaker_id);

	g_speaker.open = true;

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2CloseSpeaker(int speaker_id) {
	PRINT_NAME();

	LOGF("\t speaker_id = %d\n", speaker_id);

	g_speaker.open = false;

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2Speak(int speaker_id, const char* text, uint32_t language,
                                            int speech_speed, int volume, int pitch) {
	PRINT_NAME();

	LOGF("\t speaker_id   = %d\n"
	     "\t text         = \"%s\"\n"
	     "\t language     = %" PRIu32 "\n"
	     "\t speech_speed = %d\n"
	     "\t volume       = %d\n"
	     "\t pitch        = %d\n",
	     speaker_id, text != nullptr ? text : "", language, speech_speed, volume, pitch);

	if (!g_speaker.open) {
		return TTS2_ERROR_INVALID_SPEAKER_ID;
	}

	// Speech completes immediately because there is no audio output.
	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2GetSpeechStatus() {
	PRINT_NAME();

	return TTS2_STATUS_STOPPED;
}

static int KYTY_SYSV_ABI TextToSpeech2Stop() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2Cancel() {
	PRINT_NAME();

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2SetVolume(int speaker_id, int volume) {
	PRINT_NAME();

	LOGF("\t speaker_id = %d\n"
	     "\t volume     = %d\n",
	     speaker_id, volume);

	if (!g_speaker.open) {
		return TTS2_ERROR_INVALID_SPEAKER_ID;
	}

	g_speaker.volume = volume;

	return OK;
}

static int KYTY_SYSV_ABI TextToSpeech2SetMute(int speaker_id, bool mute) {
	PRINT_NAME();

	LOGF("\t speaker_id = %d\n"
	     "\t mute       = %s\n",
	     speaker_id, mute ? "true" : "false");

	if (!g_speaker.open) {
		return TTS2_ERROR_INVALID_SPEAKER_ID;
	}

	g_speaker.muted = mute;

	return OK;
}

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1) {
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::TextToSpeech2GetSpeechStatus);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::TextToSpeech2Cancel);
	LIB_FUNC("6OxS8aXlMGM", TextToSpeech2::TextToSpeech2OpenSpeaker);
	LIB_FUNC("YdG2Xl1VJ5I", TextToSpeech2::TextToSpeech2CloseSpeaker);
	LIB_FUNC("mB7Ys9WqPAo", TextToSpeech2::TextToSpeech2Speak);
	LIB_FUNC("QeJ4nR2tHcz", TextToSpeech2::TextToSpeech2Stop);
	LIB_FUNC("uXk8vD3wNEy", TextToSpeech2::TextToSpeech2SetVolume);
	LIB_FUNC("cWp5bK0gZTf", TextToSpeech2::TextToSpeech2SetMute);
}

} // namespace Libs
