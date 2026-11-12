/* bpemu --- the Book Port in a window.

   bpemu [-f fwdir] [-r rc8650.srec] [-c card.img] [-F flash.bin]
	 [-a file]... [-D folder]

   -a puts files on the card before power-on (text files become indexed
   books; see card.h), into folder -D if given.  Dropping files on the
   window, or pasting them (Cmd+V on a Mac, Ctrl+V elsewhere, after
   copying them in Finder or Explorer), does the same while running: the
   card comes out, the files go on, the card goes back and the Book Port
   restarts (it lists a card's files when it powers on; settings not saved
   with D+E+F are lost, as with a battery change).  Pasted text, with no
   files on the clipboard, becomes a book of its own.

   With no card image, one is made: holding the firmware's update files
   from fwdir if they are there, which a new Book Port installs.

   Keys, by position on a US layout (scancodes, so other layouts keep the
   same physical keys):

	1 2 3          1 2 3
	q w e          4 5 6
	a s d          7 8 9
	z x c          * 0 #

	4 5 6          A B C
	r t y          D E F

   Several keys may be held at once for the Book Port's chord commands.
   Escape or closing the window quits.  */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "card.h"
#include "bp.h"
#include "cf.h"
#include "norflash.h"
#include "plat/clip.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir (p)
#else
#define make_dir(p) mkdir (p, 0777)
#endif

static void release_all (void);

static int
exists (const char *path)
{
  struct stat st;
  return stat (path, &st) == 0;
}

/* Make the folder a path is in.  */
static void
make_parent (const char *path)
{
  char d[1024];
  snprintf (d, sizeof d, "%s", path);
  char *slash = strrchr (d, '/');
  if (slash)
    {
      *slash = 0;
      if (!exists (d))
	make_dir (d);
    }
}

/* A first card: the update files a new Book Port installs, if present.  */
static int
first_card (const char *card, const char *fwdir)
{
  static const char *const upd[] = { "BPMsgs._BB", "BPfwex._BB", "Spanish.dix" };
  char paths[3][1024];
  const char *have[3];
  int n = 0;
  make_parent (card);
  if (card_create (card, 128))
    return -1;
  for (int i = 0; i < 3; i++)
    {
      snprintf (paths[i], sizeof paths[i], "%s/%s", fwdir, upd[i]);
      if (exists (paths[i]))
	have[n++] = paths[i];
    }
  return n ? card_add (card, NULL, n, have) : 0;
}

static void
fatal (const char *msg)
{
  fprintf (stderr, "bpemu: %s\n", msg);
  SDL_ShowSimpleMessageBox (SDL_MESSAGEBOX_ERROR, "Book Port", msg, NULL);
}

/* Swap the card for one with these files (or this text, as a book) on it,
   and power-cycle the Book Port so it lists them.  */
static void
change_card (const char *card, const char *folder, int n,
	     const char *const *files, const char *text)
{
  release_all ();
  cf_eject ();
  int r = 0;
  if (!exists (card))
    {
      make_parent (card);
      r = card_create (card, 128);
    }
  if (!r && n)
    r = card_add (card, folder, n, files);
  if (!r && text)
    {
      char name[64];
      time_t t = time (NULL);
      strftime (name, sizeof name, "Pasted %Y-%m-%d %H%M%S.txt", localtime (&t));
      r = card_add_data (card, folder, name, text, strlen (text));
    }
  if (r)
    fatal ("Could not put the files on the card.");
  if (cf_insert (card) == 0)
    board_reset ();
}

static const struct
{
  SDL_Scancode sc;
  int key;
} keymap[] = {
  {SDL_SCANCODE_1, BP_K1}, {SDL_SCANCODE_2, BP_K2}, {SDL_SCANCODE_3, BP_K3},
  {SDL_SCANCODE_Q, BP_K4}, {SDL_SCANCODE_W, BP_K5}, {SDL_SCANCODE_E, BP_K6},
  {SDL_SCANCODE_A, BP_K7}, {SDL_SCANCODE_S, BP_K8}, {SDL_SCANCODE_D, BP_K9},
  {SDL_SCANCODE_Z, BP_KSTAR}, {SDL_SCANCODE_X, BP_K0}, {SDL_SCANCODE_C, BP_KHASH},
  {SDL_SCANCODE_4, BP_KA}, {SDL_SCANCODE_5, BP_KB}, {SDL_SCANCODE_6, BP_KC},
  {SDL_SCANCODE_R, BP_KD}, {SDL_SCANCODE_T, BP_KE}, {SDL_SCANCODE_Y, BP_KF},
};

static int
lookup (SDL_Scancode sc)
{
  for (size_t i = 0; i < SDL_arraysize (keymap); i++)
    if (keymap[i].sc == sc)
      return keymap[i].key;
  return -1;
}

static void
release_all (void)
{
  for (int k = 0; k < BP_NKEYS; k++)
    bp_set_key (k, 0);
}

int
main (int argc, char **argv)
{
  const char *fwdir = "fw", *card = "cards/card.img", *flash = "state/flash.bin";
  const char *srec = getenv ("RC8650_FIRMWARE"), *folder = NULL;
  const char *adds[64];
  int c, nadd = 0;

  while ((c = getopt (argc, argv, "f:r:c:F:a:D:")) != -1)
    switch (c)
      {
      case 'f': fwdir = optarg; break;
      case 'r': srec = optarg; break;
      case 'c': card = optarg; break;
      case 'F': flash = optarg; break;
      case 'a': if (nadd < 64) adds[nadd++] = optarg; break;
      case 'D': folder = optarg; break;
      default:
	fprintf (stderr, "usage: bpemu [-f fwdir] [-r rc8650.srec]"
		 " [-c card.img] [-F flash.bin] [-a file]... [-D folder]\n");
	return 2;
      }

  if (!srec)
    srec = board_find_srec (fwdir);
  make_parent (flash);
  if (nor_open (flash) < 0)
    {
      fatal ("Cannot open the flash image.");
      return 1;
    }
  if (!exists (card) && first_card (card, fwdir) < 0)
    fatal ("Cannot make a card image.");
  if (nadd && card_add (card, folder, nadd, adds) < 0)
    {
      fprintf (stderr, "bpemu: could not add files to %s\n", card);
      return 1;
    }
  if (exists (card) && cf_insert (card) < 0)
    return 1;
  if (board_init (fwdir, srec) < 0)
    {
      fatal ("Cannot start: the Book Port firmware (bpfw._BB in the fw"
	     " folder) or the RC8650 image (rc8650.srec, or set"
	     " RC8650_FIRMWARE) is missing.");
      return 1;
    }

  if (!SDL_Init (SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
      fprintf (stderr, "SDL: %s\n", SDL_GetError ());
      return 1;
    }
  SDL_Window *win;
  SDL_Renderer *ren;
  if (!SDL_CreateWindowAndRenderer ("Book Port", 480, 320, 0, &win, &ren))
    {
      fprintf (stderr, "SDL: %s\n", SDL_GetError ());
      return 1;
    }

  SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, (int) board_sample_rate () };
  SDL_AudioStream *audio =
    SDL_OpenAudioDeviceStream (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
			       NULL, NULL);
  if (!audio)
    fprintf (stderr, "SDL audio: %s (running silent)\n", SDL_GetError ());
  else
    SDL_ResumeAudioStreamDevice (audio);

  /* Emulated time follows the wall clock.  Should the audio device run a
     little slower than the clock, speech queues up; past this much, the
     machine waits for it.  */
  const int max_queued = (int) board_sample_rate () / 2 * sizeof (int16_t);
  Uint64 last = SDL_GetTicksNS ();
  int running = 1;

  /* Files dropped on the window, added in one go when the drop ends.  */
  char *dropped[64];
  int ndrop = 0;

  while (running)
    {
      SDL_Event ev;
      while (SDL_PollEvent (&ev))
	switch (ev.type)
	  {
	  case SDL_EVENT_QUIT:
	    running = 0;
	    break;
	  case SDL_EVENT_KEY_DOWN:
	  case SDL_EVENT_KEY_UP:
	    {
	      if (ev.key.scancode == SDL_SCANCODE_ESCAPE)
		{
		  running = 0;
		  break;
		}
	      if (ev.key.repeat)
		break;
	      /* Paste: files copied in Finder or Explorer, or else text.  */
#ifdef __APPLE__
	      const SDL_Keymod paste_mod = SDL_KMOD_GUI;
#else
	      const SDL_Keymod paste_mod = SDL_KMOD_CTRL;
#endif
	      if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_V
		  && (ev.key.mod & paste_mod))
		{
		  char **files;
		  int n = clip_files (&files);
		  if (n)
		    change_card (card, folder, n, (const char *const *) files, NULL);
		  else if (SDL_HasClipboardText ())
		    {
		      char *t = SDL_GetClipboardText ();
		      if (t && *t)
			change_card (card, folder, 0, NULL, t);
		      SDL_free (t);
		    }
		  clip_free (files, n);
		  last = SDL_GetTicksNS ();
		  break;
		}
	      int k = lookup (ev.key.scancode);
	      if (k >= 0)
		bp_set_key (k, ev.type == SDL_EVENT_KEY_DOWN);
	      break;
	    }
	  case SDL_EVENT_WINDOW_FOCUS_LOST:
	    release_all ();
	    break;
	  case SDL_EVENT_DROP_FILE:
	    if (ndrop < 64 && ev.drop.data)
	      dropped[ndrop++] = SDL_strdup (ev.drop.data);
	    break;
	  case SDL_EVENT_DROP_COMPLETE:
	    if (!ndrop)
	      break;
	    change_card (card, folder, ndrop, (const char *const *) dropped, NULL);
	    for (int i = 0; i < ndrop; i++)
	      SDL_free (dropped[i]);
	    ndrop = 0;
	    last = SDL_GetTicksNS ();
	    break;
	  }

      Uint64 now = SDL_GetTicksNS ();
      Uint64 us = (now - last) / 1000;
      last = now;
      if (us > 50000)		/* after a stall, do not try to catch up */
	us = 50000;
      if (!audio || SDL_GetAudioStreamQueued (audio) < max_queued)
	board_run_us (us);

      int16_t pcm[1024];
      size_t n;
      while ((n = board_audio (pcm, SDL_arraysize (pcm))) > 0)
	if (audio)
	  SDL_PutAudioStreamData (audio, pcm, (int) (n * sizeof pcm[0]));

      SDL_SetRenderDrawColor (ren, 24, 24, 28, 255);
      SDL_RenderClear (ren);
      SDL_RenderPresent (ren);
      SDL_Delay (2);
    }

  if (audio)
    SDL_DestroyAudioStream (audio);
  SDL_DestroyRenderer (ren);
  SDL_DestroyWindow (win);
  SDL_Quit ();
  return 0;
}
