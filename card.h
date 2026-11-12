/* card.h --- CompactFlash card images: making them and putting books on
   them, with no outside tools (a small FAT16/FAT32 writer, card.c).  */

#ifndef CARD_H
#define CARD_H

#include <stddef.h>

/* A new card of mb megabytes the way a PC leaves one: an MBR with one FAT
   partition at sector 63, FAT16 up to 2000 MB, FAT32 above, labelled
   BOOKPORT.  Returns 0, or -1 with a message on stderr.  */
int card_create (const char *path, unsigned mb);

/* Put files on the card image, into folder (NULL or "" for the root;
   created if missing).  Text files (.txt) become indexed books, NAME._DD
   and NAME._IX (see card_index_text); anything else is copied as it is.
   An existing file of the same name is replaced.  */
int card_add (const char *img, const char *folder, int n,
	      const char *const *files);

/* The same for data in memory, under name.  */
int card_add_data (const char *img, const char *folder, const char *name,
		   const void *data, size_t len);

/* Index a text as tools/bpindex.py does by default (UBT's rules with its
   paragraph handling fixed): *dd and *ix are malloc'd ._DD and ._IX
   contents.  name is the file's name on the card.  */
int card_index_text (const void *text, size_t len, const char *name,
		     unsigned char **dd, size_t *ddlen,
		     unsigned char **ix, size_t *ixlen);

#endif
