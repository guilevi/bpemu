/* bprun --- headless Book Port runner, for finding out what the board needs.

   bprun [-l logmask] [-s seconds] [-o out.wav] [-f fwdir] [-r rc8650.srec]
	 [-c card.img] [-a file]... [-D folder] [-A t:file]... [-k keys]

   -a adds a file to the card before power-on (text files become indexed
   books, see tools/cardadd.py), into -D's folder if given.  -A t:file
   takes the card out at t seconds, adds the file, puts it back a second
   later and power-cycles the Book Port, as dropping a file on bpemu's
   window does (the firmware lists a card's files when it mounts it, at
   power-on).

   Prints what the firmware sends the RC8650 on UART0, with control bytes
   shown as ^X and the chip's replies as {xx}, and optionally writes the
   speech to a WAV file.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "cardadd.h"
#include "bp.h"
#include "cf.h"
#include "norflash.h"

#define DEFAULT_SREC "fw/rc8650.srec"

static void
put32 (FILE *f, uint32_t v)
{
  fputc (v, f); fputc (v >> 8, f); fputc (v >> 16, f); fputc (v >> 24, f);
}

static void
put16 (FILE *f, uint16_t v)
{
  fputc (v, f); fputc (v >> 8, f);
}

int
main (int argc, char **argv)
{
  double secs = 5;
  const char *fwdir = "fw", *srec = getenv ("RC8650_FIRMWARE"), *wav = NULL;
  int c;

  if (!srec)
    srec = DEFAULT_SREC;
  const char *dump = NULL, *card = NULL, *keyscript = NULL;
  const char *flashfile = "state/flash.bin", *folder = NULL;
  const char *adds[64], *hot[64];
  int nadd = 0, nhot = 0;
  card_tools_init (argv[0]);
  while ((c = getopt (argc, argv, "l:s:o:f:r:d:c:Ck:F:Na:A:D:")) != -1)
    switch (c)
      {
      case 'l': bp_log = strtol (optarg, NULL, 0); break;
      case 's': secs = atof (optarg); break;
      case 'o': wav = optarg; break;
      case 'f': fwdir = optarg; break;
      case 'r': srec = optarg; break;
      case 'd': dump = optarg; break;
      case 'c': card = optarg; break;
      case 'C': cf_log = 1; break;
      case 'k': keyscript = optarg; break;
      case 'F': flashfile = optarg; break;
      case 'N': nor_log = 1; break;
      case 'a': if (nadd < 64) adds[nadd++] = optarg; break;
      case 'A': if (nhot < 64) hot[nhot++] = optarg; break;
      case 'D': folder = optarg; break;
      default:
	fprintf (stderr, "usage: bprun [-l logmask] [-s seconds] [-o out.wav]"
		 " [-f fwdir] [-r rc8650.srec] [-c card.img] [-C]\n"
		 "             [-a file]... [-D folder] [-A t:file]..."
		 " [-k keys] [-F flash.bin] [-N]\n");
	return 2;
      }

  board_echo_tx = 1;
  if (nor_open (flashfile) < 0)
    return 1;
  if (nadd && (!card || card_add (card, folder, nadd, adds) < 0))
    {
      fprintf (stderr, "bprun: could not add files%s\n",
	       card ? "" : " (no card: give -c)");
      return 1;
    }
  if (card && cf_insert (card) < 0)
    return 1;
  if (board_init (fwdir, srec) < 0)
    return 1;

  /* BP_PINS="8:08:00,..." : port:mask:value, hex - for experiments.  */
  for (const char *ps = getenv ("BP_PINS"); ps && *ps;)
    {
      unsigned p, m, v;
      if (sscanf (ps, "%x:%x:%x", &p, &m, &v) != 3)
	break;
      bp_set_pins (p, m, v);
      ps = strchr (ps, ',');
      ps = ps ? ps + 1 : NULL;
    }

  FILE *out = NULL;
  uint32_t samples = 0;
  if (wav && !(out = fopen (wav, "wb")))
    {
      perror (wav);
      return 1;
    }
  if (out)
    fseek (out, 44, SEEK_SET);

  /* -k t:pos[:ms],... : press matrix position pos at t seconds for ms
     (default 150); pos may be a+b+c for a chord.  */
  struct { uint64_t at, until; int pos[4], n, down; } ev[256];
  int nev = 0;
  for (const char *k = keyscript; k && *k && nev < 256;)
    {
      double t;
      char posbuf[32];
      int ms = 150, n = 0;
      if (sscanf (k, "%lf:%31[0-9+kA-F*#]:%d", &t, posbuf, &ms) < 2)
	break;
      for (char *q = strtok (posbuf, "+"); q && n < 4; q = strtok (NULL, "+"))
	{
	  /* A key name (1-9 0 * # A-F, prefixed k) or a raw matrix position.  */
	  static const char names[] = "123456789*0#ABCDEF";
	  const char *hit = q[0] == 'k' && q[1] ? strchr (names, q[1]) : NULL;
	  ev[nev].pos[n++] = hit ? bp_key_pos[hit - names] : atoi (q);
	}
      ev[nev].n = n;
      ev[nev].at = (uint64_t) (t * 1000);
      ev[nev].until = ev[nev].at + ms;
      ev[nev].down = 0;
      nev++;
      k = strchr (k, ',');
      k = k ? k + 1 : NULL;
    }

  uint64_t total_ms = (uint64_t) (secs * 1000);
  for (uint64_t ms = 0; ms < total_ms; ms++)
    {
      for (int i = 0; i < nev; i++)
	{
	  if (!ev[i].down && ms == ev[i].at)
	    {
	      printf ("\n[%.2f press", ms / 1000.0);
	      for (int j = 0; j < ev[i].n; j++)
		printf (" %d", ev[i].pos[j]), bp_set_matrix (ev[i].pos[j], 1);
	      printf ("]\n");
	      ev[i].down = 1;
	    }
	  else if (ev[i].down == 1 && ms == ev[i].until)
	    {
	      for (int j = 0; j < ev[i].n; j++)
		bp_set_matrix (ev[i].pos[j], 0);
	      ev[i].down = 2;
	    }
	}
      for (int i = 0; i < nhot && card; i++)
	{
	  uint64_t at = (uint64_t) (atof (hot[i]) * 1000);
	  const char *f = strchr (hot[i], ':');
	  if (ms == at)
	    {
	      printf ("\n[%.2f card out]\n", ms / 1000.0);
	      cf_eject ();
	      if (f && card_add (card, folder, 1, (const char *const[]) { f + 1 }) < 0)
		fprintf (stderr, "bprun: could not add %s\n", f + 1);
	    }
	  else if (ms == at + 1000)
	    {
	      printf ("\n[%.2f card in, power-cycled]\n", ms / 1000.0);
	      cf_insert (card);
	      board_reset ();
	    }
	}
      board_run_us (1000);
      int16_t pcm[512];
      size_t n;
      while ((n = board_audio (pcm, 512)) > 0)
	{
	  if (out)
	    fwrite (pcm, 2, n, out);
	  samples += n;
	}
      if ((bp_log & BP_LOG_PC) && ms % 10 == 0)
	fprintf (stderr, "t=%.2f PC %06x\n", ms / 1000.0, bp_pc ());
    }

  if (out)
    {
      unsigned rate = board_sample_rate ();
      fseek (out, 0, SEEK_SET);
      fwrite ("RIFF", 1, 4, out); put32 (out, 36 + samples * 2);
      fwrite ("WAVEfmt ", 1, 8, out); put32 (out, 16); put16 (out, 1);
      put16 (out, 1); put32 (out, rate); put32 (out, rate * 2);
      put16 (out, 2); put16 (out, 16);
      fwrite ("data", 1, 4, out); put32 (out, samples * 2);
      fclose (out);
    }
  if (dump)
    {
      /* -d addr[:len], hex */
      char *e;
      unsigned long a = strtoul (dump, &e, 16), len = *e == ':' ? strtoul (e + 1, NULL, 16) : 16;
      for (unsigned long i = 0; i < len; i++)
	printf ("%s%02x", i % 16 ? " " : i ? "\n" : "", bp_peek (a + i));
      printf ("\n");
    }
  printf ("\n[t=%.2fs, PC %06x, %u samples of speech]\n", secs, bp_pc (),
	  samples);
  return 0;
}
