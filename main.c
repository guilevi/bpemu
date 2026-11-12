/* bpemu --- the Book Port in a window.

   bpemu [-f fwdir] [-r rc8650.srec] [-c card.img] [-F flash.bin]
	 [-a file]... [-D folder]

   -a puts files on the card before power-on (text files become indexed
   books; see tools/cardadd.py), into folder -D if given.  Dropping files
   on the window does the same while running: the card comes out, the
   files go on, the card goes back and the Book Port restarts (it lists a
   card's files when it powers on; settings not saved with D+E+F are lost,
   as with a battery change).

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
#include <unistd.h>

#include "board.h"
#include "cardadd.h"
#include "bp.h"
#include "cf.h"
#include "norflash.h"

#define DEFAULT_SREC "fw/rc8650.srec"

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

  if (!srec)
    srec = DEFAULT_SREC;
  card_tools_init (argv[0]);
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

  if (nor_open (flash) < 0)
    return 1;
  if (nadd && card_add (card, folder, nadd, adds) < 0)
    {
      fprintf (stderr, "bpemu: could not add files to %s\n", card);
      return 1;
    }
  if (access (card, F_OK) == 0 && cf_insert (card) < 0)
    return 1;
  if (board_init (fwdir, srec) < 0)
    return 1;

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
	    release_all ();
	    cf_eject ();
	    if (card_add (card, folder, ndrop, (const char *const *) dropped) < 0)
	      fprintf (stderr, "bpemu: could not add the dropped files\n");
	    for (int i = 0; i < ndrop; i++)
	      SDL_free (dropped[i]);
	    ndrop = 0;
	    if (cf_insert (card) == 0)
	      board_reset ();
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
