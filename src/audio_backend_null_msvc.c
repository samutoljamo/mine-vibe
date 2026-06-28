/*
 * audio_backend_null_msvc.c — MSVC-only weak-symbol fallback.
 *
 * audio.c references audio_miniaudio_backend(). On GCC/Clang that symbol is
 * declared __attribute__((weak)) (see audio_backend.h), so linking audio.c
 * WITHOUT a backend TU (e.g. the pure test_audio / test_client unit tests)
 * leaves it NULL and the engine falls back to the always-safe NULL backend.
 *
 * MSVC has no portable weak-function attribute, so this file provides the
 * fallback via the linker's /alternatename directive: if the strong
 * audio_miniaudio_backend() (from audio_miniaudio.c) is NOT linked, the
 * unresolved reference is redirected to default_audio_miniaudio_backend(),
 * which returns NULL. When audio_miniaudio.c IS linked, its strong definition
 * wins and this default is ignored.
 *
 * This TU is compiled into the backend-less test targets only; the real game
 * build links audio_miniaudio.c and never needs it.
 */

#ifdef _MSC_VER

#include "audio_backend.h"

const AudioBackend* default_audio_miniaudio_backend(void)
{
    return 0;
}

/* x64 MSVC C symbols are undecorated (no leading underscore). */
#pragma comment(linker, "/alternatename:audio_miniaudio_backend=default_audio_miniaudio_backend")

#endif /* _MSC_VER */
