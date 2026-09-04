/* machine.h -- internal helpers machine.c exports to the other core modules
 * (the public machine API lives in astro_internal.h with the struct). */

#ifndef ASTRO_MACHINE_H
#define ASTRO_MACHINE_H

#include "astro_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Writes to 0x0D/0x0E/0x0F acknowledge: force the INT line low. */
void astro_machine_irq_clear(astro_machine_t *m);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_MACHINE_H */
