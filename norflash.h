/* norflash.h --- the Book Port's external AMD flash.  */

#ifndef NORFLASH_H
#define NORFLASH_H

#include <stdint.h>

/* Load the flash contents from path, creating an erased image if absent.  */
int nor_open (const char *path);

/* Bus accesses, by chip byte address.  */
uint8_t nor_read (uint32_t a);
void nor_write (uint32_t a, uint8_t v);

extern int nor_log;

#endif
