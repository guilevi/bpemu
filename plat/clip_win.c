/* clip_win.c --- clipboard files on Windows: CF_HDROP.  */

#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

#include "clip.h"

int
clip_files (char ***out)
{
  *out = NULL;
  if (!OpenClipboard (NULL))
    return 0;
  int n = 0;
  HANDLE h = GetClipboardData (CF_HDROP);
  HDROP drop = h ? (HDROP) GlobalLock (h) : NULL;
  if (drop)
    {
      UINT count = DragQueryFileW (drop, 0xFFFFFFFF, NULL, 0);
      char **v = calloc (count ? count : 1, sizeof *v);
      for (UINT i = 0; i < count; i++)
	{
	  UINT len = DragQueryFileW (drop, i, NULL, 0);
	  wchar_t *w = calloc (len + 1, sizeof *w);
	  DragQueryFileW (drop, i, w, len + 1);
	  int u = WideCharToMultiByte (CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	  v[n] = malloc (u);
	  WideCharToMultiByte (CP_UTF8, 0, w, -1, v[n], u, NULL, NULL);
	  n++;
	  free (w);
	}
      GlobalUnlock (h);
      *out = v;
    }
  CloseClipboard ();
  return n;
}

void
clip_free (char **paths, int n)
{
  for (int i = 0; i < n; i++)
    free (paths[i]);
  free (paths);
}
