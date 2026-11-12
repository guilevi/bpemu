/* cardadd.h --- put files on the card image with tools/cardadd.py.  */

#ifndef CARDADD_H
#define CARDADD_H

/* Find tools/cardadd.py next to the program named argv0.  */
void card_tools_init (const char *argv0);

/* Add files (text files become indexed books) to the image, into folder
   (NULL or "" for the root).  The card must be out of the slot.  Returns
   0 on success.  */
int card_add (const char *img, const char *folder, int n,
	      const char *const *files);

#endif
