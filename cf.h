/* cf.h --- a CompactFlash card on the Book Port's card slot.  */

#ifndef CF_H
#define CF_H

#include <stdint.h>

/* Open a raw disk image as the card.  */
int cf_insert (const char *path);
int cf_present (void);
/* Take the card out (closing the image, so other programs may change it).  */
void cf_eject (void);

/* The 16-byte register window, by offset.  */
uint8_t cf_read (unsigned off);
void cf_write (unsigned off, uint8_t v);

extern int cf_log;

#endif
