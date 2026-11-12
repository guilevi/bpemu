/* clip_sdl.c --- clipboard files elsewhere: SDL's text/uri-list.  */

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "clip.h"

static int
hex (int c)
{
  return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

int
clip_files (char ***out)
{
  *out = NULL;
  size_t len = 0;
  char *list = SDL_GetClipboardData ("text/uri-list", &len);
  if (!list)
    return 0;
  char *s = malloc (len + 1);
  memcpy (s, list, len);
  s[len] = 0;
  SDL_free (list);
  int n = 0, cap = 8;
  char **v = calloc (cap, sizeof *v);
  for (char *line = strtok (s, "\r\n"); line; line = strtok (NULL, "\r\n"))
    {
      if (strncmp (line, "file://", 7))
	continue;
      char *p = strchr (line + 7, '/'), *o;
      if (!p)
	continue;
      char *path = o = malloc (strlen (p) + 1);
      for (; *p; p++)
	if (*p == '%' && p[1] && p[2])
	  *o++ = (char) (hex (p[1]) << 4 | hex (p[2])), p += 2;
	else
	  *o++ = *p;
      *o = 0;
      if (n == cap)
	v = realloc (v, (cap *= 2) * sizeof *v);
      v[n++] = path;
    }
  free (s);
  *out = v;
  return n;
}

void
clip_free (char **paths, int n)
{
  for (int i = 0; i < n; i++)
    free (paths[i]);
  free (paths);
}
