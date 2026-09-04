/* roms_embedded.h -- the table of Bally system ROMs compiled into the binary.
 *
 * The array is defined by a generated .c file (tools/roms/embed-roms.py),
 * which is empty for a WITH_ASTROCADE_ROMS=OFF build -- the shipping
 * configuration, where the user imports the BIOS at run time instead. See
 * COMPLIANCE.md.
 */

#ifndef ASTRO_ROMS_EMBEDDED_H
#define ASTRO_ROMS_EMBEDDED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;           /* e.g. "astro.bin" */
    const unsigned char *data;
    size_t size;
} astro_embedded_rom;

extern const astro_embedded_rom astro_embedded_roms[];
extern const int astro_embedded_rom_count;

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_ROMS_EMBEDDED_H */
