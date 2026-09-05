/* gamecontrollerdb_embedded.h -- the community SDL_GameControllerDB mapping
 * text compiled into the binary.
 *
 * Defined by a generated .c file (tools/gamepad/embed-gamecontrollerdb.py)
 * from third_party/gamecontrollerdb/gamecontrollerdb.txt (zlib license, see
 * third_party/gamecontrollerdb/LICENSE and COMPLIANCE.md).
 */

#ifndef ASTRO_GAMECONTROLLERDB_EMBEDDED_H
#define ASTRO_GAMECONTROLLERDB_EMBEDDED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NUL-terminated; astro_gamecontrollerdb_text_size is the file's actual byte
 * length (excluding that trailing byte) -- load with
 * SDL_IOFromConstMem(astro_gamecontrollerdb_text,
 * astro_gamecontrollerdb_text_size) + SDL_AddGamepadMappingsFromIO(). The
 * array's own size isn't usable via sizeof() from outside the generated .c
 * that defines it (an extern array is an incomplete type), hence this. */
extern const char astro_gamecontrollerdb_text[];
extern const size_t astro_gamecontrollerdb_text_size;

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_GAMECONTROLLERDB_EMBEDDED_H */
