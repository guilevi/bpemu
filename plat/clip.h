/* clip.h --- files on the system clipboard (copied in Finder or Explorer).  */

#ifndef CLIP_H
#define CLIP_H

/* Paths (UTF-8) of the files on the clipboard: returns how many, and a
   malloc'd array of malloc'd strings in *out (free with clip_free).  */
int clip_files (char ***out);
void clip_free (char **paths, int n);

#endif
