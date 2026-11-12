/* cardadd.c --- run tools/cardadd.py on the card image.  */

#include <libgen.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "cardadd.h"

extern char **environ;
static char script[PATH_MAX] = "tools/cardadd.py";

void
card_tools_init (const char *argv0)
{
  char real[PATH_MAX], dir[PATH_MAX];
  if (!realpath (argv0, real))
    return;
  snprintf (dir, sizeof dir, "%s", dirname (real));
  snprintf (script, sizeof script, "%s/tools/cardadd.py", dir);
}

int
card_add (const char *img, const char *folder, int n,
	  const char *const *files)
{
  const char **av = calloc (n + 8, sizeof *av);
  int k = 0;
  av[k++] = "python3";
  av[k++] = script;
  av[k++] = img;
  if (folder && *folder)
    {
      av[k++] = "-d";
      av[k++] = folder;
    }
  for (int i = 0; i < n; i++)
    av[k++] = files[i];
  av[k] = NULL;

  pid_t pid;
  int st = 0;
  int err = posix_spawnp (&pid, "python3", NULL, NULL, (char **) av, environ);
  free (av);
  if (err)
    {
      fprintf (stderr, "cardadd: cannot run python3: %s\n", strerror (err));
      return -1;
    }
  waitpid (pid, &st, 0);
  return WIFEXITED (st) && WEXITSTATUS (st) == 0 ? 0 : -1;
}
