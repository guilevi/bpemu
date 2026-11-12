/* bpcard --- make Book Port card images and put books and audio on them.

   bpcard new IMAGE [MB] [FILE...]       a fresh card (default 128 MB)
   bpcard add IMAGE [-d FOLDER] FILE...  add files, into FOLDER if given
   bpcard index FILE.txt [OUTDIR]        write FILE.txt._DD and ._IX

   Text files (.txt) become indexed books; anything else is copied as it
   is.  See card.h.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"

static int
usage (void)
{
  fprintf (stderr,
	   "usage: bpcard new IMAGE [MB] [FILE...]\n"
	   "       bpcard add IMAGE [-d FOLDER] FILE...\n"
	   "       bpcard index FILE.txt [OUTDIR]\n");
  return 2;
}

static int
write_out (const char *path, const unsigned char *b, size_t n)
{
  FILE *f = fopen (path, "wb");
  if (!f || fwrite (b, 1, n, f) != n || fclose (f))
    {
      perror (path);
      return -1;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc < 3)
    return usage ();
  const char *cmd = argv[1], *img = argv[2];

  if (!strcmp (cmd, "new"))
    {
      int i = 3;
      unsigned mb = 128;
      if (i < argc && strspn (argv[i], "0123456789") == strlen (argv[i]))
	mb = (unsigned) atoi (argv[i++]);
      if (card_create (img, mb))
	return 1;
      if (i < argc && card_add (img, NULL, argc - i, (const char *const *) argv + i))
	return 1;
      return 0;
    }
  if (!strcmp (cmd, "add"))
    {
      int i = 3;
      const char *folder = NULL;
      if (i + 1 < argc && !strcmp (argv[i], "-d"))
	{
	  folder = argv[i + 1];
	  i += 2;
	}
      if (i >= argc)
	return usage ();
      return card_add (img, folder, argc - i, (const char *const *) argv + i) ? 1 : 0;
    }
  if (!strcmp (cmd, "index"))
    {
      const char *in = argv[2], *dir = argc > 3 ? argv[3] : NULL;
      FILE *f = fopen (in, "rb");
      if (!f)
	{
	  perror (in);
	  return 1;
	}
      fseek (f, 0, SEEK_END);
      long len = ftell (f);
      fseek (f, 0, SEEK_SET);
      unsigned char *data = malloc (len > 0 ? len : 1);
      if (fread (data, 1, len, f) != (size_t) len)
	{
	  perror (in);
	  return 1;
	}
      fclose (f);
      const char *name = in;
      for (const char *p = in; *p; p++)
	if (*p == '/' || *p == '\\')
	  name = p + 1;
      unsigned char *dd, *ix;
      size_t ddlen, ixlen;
      card_index_text (data, len, name, &dd, &ddlen, &ix, &ixlen);
      char p1[4096], p2[4096];
      snprintf (p1, sizeof p1, "%s%s%s._DD", dir ? dir : "", dir ? "/" : "", dir ? name : in);
      snprintf (p2, sizeof p2, "%s%s%s._IX", dir ? dir : "", dir ? "/" : "", dir ? name : in);
      return write_out (p1, dd, ddlen) || write_out (p2, ix, ixlen) ? 1 : 0;
    }
  return usage ();
}
