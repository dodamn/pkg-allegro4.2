/*         ______   ___    ___ 
 *        /\  _  \ /\_ \  /\_ \ 
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___ 
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      Digitized sample driver for the MIDI player.
 *
 *      By Shawn Hargreaves, based on code by Tom Novelli.
 *      Chris Robinson added some optimizations and the digmid_set_pan method.
 *
 *      See readme.txt for copyright information.
 */


#include <string.h>
#include <limits.h>
#include <math.h>

#include "allegro.h"
#include "allegro/internal/aintern.h"


/* external interface to the Digmid driver */
static int digmid_detect(int input);
static int digmid_init(int input, int voices);
static void digmid_exit(int input);
static int digmid_load_patches(AL_CONST char *patches, AL_CONST char *drums);
static void digmid_key_on(int inst, int note, int bend, int vol, int pan);
static void digmid_key_off(int voice);
static void digmid_set_volume(int voice, int vol);
static void digmid_set_pitch(int voice, int note, int bend);
static void digmid_set_pan(int voice, int pan);


MIDI_DRIVER midi_digmid =
{
   MIDI_DIGMID,
   empty_string,
   empty_string,
   "DIGMID",
   0, 0, -1, 24, -1, -1,
   digmid_detect,
   digmid_init,
   digmid_exit,
   NULL,
   NULL,
   NULL,
   digmid_load_patches,
   _dummy_adjust_patches,
   digmid_key_on,
   digmid_key_off,
   digmid_set_volume,
   digmid_set_pitch,
   digmid_set_pan,
   _dummy_noop2
};


#define MAX_LAYERS   64


typedef struct PATCH_EXTRA          /* additional data for a Gravis PATCH */
{
   int low_note; 
   int high_note;
   int base_note;
   int play_mode; 
   int decay_time;
   int release_time;
   int sustain_level;
   int scale_freq;
   int scale_factor; 
   int pan;
} PATCH_EXTRA;


typedef struct PATCH                /* Gravis PATCH structure */
{
   int samples;                     /* number of samples in this instrument */
   SAMPLE *sample[MAX_LAYERS];      /* the waveform data */
   PATCH_EXTRA *extra[MAX_LAYERS];  /* additional waveform information */
   int master_vol;                  /* overall volume level */
} PATCH;


/* our instruments */
static PATCH *patch[256];


/* frequency table (generated by digmid_init) */
static long ftbl[130]; 


/* stored information about active voices */
typedef struct DIGMID_VOICE
{
   SAMPLE *s;
   PATCH_EXTRA *e;
   int inst;
   int vol;
} DIGMID_VOICE;


static DIGMID_VOICE digmid_voice[MIDI_VOICES];



/* destroy_patch:
 *  Frees a PATCH struct and all samples it contains.
 */
static void destroy_patch(PATCH *pat)
{
   int i;

   if (pat) {
      for (i=0; i < pat->samples; i++) {
	 destroy_sample(pat->sample[i]);

	 UNLOCK_DATA(pat->extra[i], sizeof(PATCH_EXTRA));
	 _AL_FREE(pat->extra[i]);
      }

      UNLOCK_DATA(pat, sizeof(PATCH));
      _AL_FREE(pat);
   }
}



/* scale64:
 *  Evalulates a*b/c using 64 bit arithmetic. This is used in an interrupt
 *  context, so we have to do it ourselves rather than relying on compiler
 *  support or floating point (it is impossible to reliably lock the
 *  compiler helpers that implement a 64 bit divide, and it isn't safe to
 *  use the i386 FPU stack in an interrupt context). Multithreaded systems
 *  are not bound by this restriction however, so we use the FPU there.
 */
#if !(defined ALLEGRO_MULTITHREADED)
static INLINE unsigned long scale64(unsigned long a, unsigned long b, unsigned long c)
{
   unsigned long al = a & 0xFFFF;
   unsigned long ah = a >> 16;
   unsigned long bl = b & 0xFFFF;
   unsigned long bh = b >> 16;
   unsigned long rl, rh, r, s, t, o;

   /* 64 bit multiply */
   rl = al*bl;
   rh = ah*bh;

   t = al*bh;
   o = rl;
   rh += (t >> 16);
   rl += (t << 16);
   if (rl < o)
      rh++;

   t = ah*bl;
   o = rl;
   rh += (t >> 16);
   rl += (t << 16);
   if (rl < o)
      rh++;

   /* 64 bit divide */
   s = 0x80000000;
   t = rh;
   r = 0;

   while (s) {
      o = t;
      t -= c;

      if (t > o)
	 t = o;
      else
	 r |= s;

      t <<= 1;
      if (rl & s)
	 t |= 1;

      s >>= 1;
   }

   return r << 1;
}
#else
static INLINE unsigned long scale64(unsigned long a, unsigned long b, unsigned long c)
{
   double scale = b / (double)c;
   return (unsigned long)a * scale;
}
#endif


/* load_patch:
 *  Reads a GUS format patch file from disk.
 */
static PATCH *load_patch(PACKFILE *f, int drum)
{
   PATCH *p = NULL;
   char buf[256];
   char mode;
   int env_rate[6];
   int env_offset[6];
   int i, j;
   int diff;
   int odd_len;

   pack_fread(buf, 22, f);                         /* ID tag */
   if (memcmp(buf, "GF1PATCH110\0", 12) || memcmp(buf+12, "ID#000002\0", 10)) {
      *allegro_errno = EINVAL;
      goto getout;
   }

   p = _AL_MALLOC(sizeof(PATCH));
   if (!p) {
      *allegro_errno = ENOMEM;
      goto getout;
   }

   pack_fread(buf, 65, f);                         /* description */
   p->master_vol = pack_igetw(f);                  /* volume */

   pack_fread(buf, 109, f);                        /* skip */

   p->samples = pack_getc(f);                      /* number of samples */
   pack_fread(buf, 40, f);                         /* skip */

   if (p->samples > MAX_LAYERS)
      p->samples = MAX_LAYERS;

   for (i=0; i<p->samples; i++) {                  /* for each sample... */
      p->sample[i] = _AL_MALLOC(sizeof(SAMPLE));
      if (!p->sample[i]) {
	 p->samples = i;
	 destroy_patch(p);
	 p = NULL;
	 *allegro_errno = ENOMEM;
	 goto getout;
      }

      p->extra[i] = _AL_MALLOC_ATOMIC(sizeof(PATCH_EXTRA));
      if (!p->extra[i])  {
	 _AL_FREE(p->sample[i]);
	 p->samples = i;
	 destroy_patch(p);
	 p = NULL;
	 *allegro_errno = ENOMEM;
	 goto getout;
      }

      pack_fread(buf, 8, f);                       /* layer name */

      p->sample[i]->len = pack_igetl(f);           /* sample length */
      p->sample[i]->loop_start = pack_igetl(f);    /* loop start */
      p->sample[i]->loop_end = pack_igetl(f);      /* loop end */
      p->sample[i]->freq = pack_igetw(f);          /* sample frequency */

      p->extra[i]->low_note = pack_igetl(f);       /* key min */
      p->extra[i]->high_note = pack_igetl(f);      /* key max */
      p->extra[i]->base_note = pack_igetl(f);      /* base key */
      pack_igetw(f);                               /* skip finetune */

      p->extra[i]->pan = pack_getc(f) * 255/15;    /* pan position */

      for (j=0; j<6; j++)                          /* envelope rate */
	 env_rate[j] = pack_getc(f);

      for (j=0; j<6; j++)                          /* envelope value */
	 env_offset[j] = pack_getc(f);

      pack_fread(buf, 6, f);                       /* skip trem and vib */

      mode = pack_getc(f);                         /* sample flags */

      p->sample[i]->bits = (mode & 1) ? 16 : 8;    /* how many bits? */

      p->sample[i]->stereo = FALSE;

      p->extra[i]->play_mode = 0;                  /* sort out loop flags */

      if (mode & 4)
	 p->extra[i]->play_mode |= PLAYMODE_LOOP;

      if (mode & 8) 
	 p->extra[i]->play_mode |= (PLAYMODE_BIDIR | PLAYMODE_LOOP);

      if (mode & 16) 
	 p->extra[i]->play_mode |= (PLAYMODE_BACKWARD | PLAYMODE_LOOP);

      /* convert envelope rates (GUS uses a 2.6 floating point format) */
      for (j=0; j<6; j++) {
	 static int vexp[4] = { 1, 8, 64, 512 };
	 int e = (env_rate[j] >> 6);
	 int m = (env_rate[j] & 0x3F);
	 env_rate[j] = ((65536 * vexp[e] / ((m) ? m : 1)) >> 12);
      }

      if ((mode & 32) && (!drum)) {
	 /* sustained volume envelope */
	 p->extra[i]->sustain_level = env_offset[2];
	 p->extra[i]->decay_time = 0;

	 diff = env_offset[0];
	 p->extra[i]->decay_time += env_rate[0] * diff / 256;

	 diff = ABS(env_offset[1] - env_offset[0]);
	 p->extra[i]->decay_time += env_rate[1] * diff / 256;

	 diff = ABS(env_offset[2] - env_offset[1]);
	 p->extra[i]->decay_time += env_rate[2] * diff / 256;

	 j = 3;
      }
      else {
	 /* one-shot volume envelope */
	 p->extra[i]->decay_time = 0;
	 p->extra[i]->sustain_level = 0;

	 for (j=0; j<6; j++) {
	    diff = ABS(env_offset[j] - ((j) ? env_offset[j-1] : 0));
	    p->extra[i]->decay_time += env_rate[j] * diff / 256;
	    if (env_offset[j] < 16) {
	       j++;
	       break;
	    }
	 }
      }

      /* measure release time */
      p->extra[i]->release_time = 0;

      while (j < 6) {
	 diff = ABS(env_offset[j] - env_offset[j-1]);
	 p->extra[i]->release_time += env_rate[j] * diff / 256;
	 if (env_offset[j] < 16)
	    break;
	 j++;
      }

      /* clamp very large/small sustain levels to zero or maximum */
      if (p->extra[i]->sustain_level < 16)
	 p->extra[i]->sustain_level = 0;
      else if (p->extra[i]->sustain_level > 192)
	 p->extra[i]->sustain_level = 255;

      if (p->extra[i]->release_time < 10)
	 p->extra[i]->release_time = 0;

      if ((p->extra[i]->sustain_level == 0) && 
	  (p->extra[i]->decay_time == 0)) {
	 p->extra[i]->sustain_level = 255;
	 p->extra[i]->play_mode &= ~PLAYMODE_LOOP;
      }

      p->extra[i]->scale_freq = pack_igetw(f);     /* scale values */
      p->extra[i]->scale_factor = pack_igetw(f);

      /* drums use a fixed frequency */
      if(drum) {
	 unsigned long freq = scale64(ftbl[drum-1], p->sample[i]->freq, p->extra[i]->base_note);

	 /* frequency scaling */
	 if (p->extra[i]->scale_factor != 1024) {
	    unsigned long f1 = scale64(p->sample[i]->freq, p->extra[i]->scale_freq, 60);
	    freq -= f1;
	    freq = scale64(freq, p->extra[i]->scale_factor, 1024);
	    freq += f1;
	 }

	 /* lower by an octave if we are going to overflow */
	 while (freq >= (1<<19)-1)
	    freq /= 2;

	 p->sample[i]->freq = freq;
      }

      pack_fread(buf, 36, f);                      /* skip reserved */

      if (p->sample[i]->bits == 16) {              /* adjust 16 bit loops */
	 odd_len = (p->sample[i]->len & 1);
	 p->sample[i]->len /= 2;
	 p->sample[i]->loop_start /= 2;
	 p->sample[i]->loop_end /= 2;
      }
      else
	 odd_len = FALSE;

      p->sample[i]->priority = 128;                /* set some defaults */
      p->sample[i]->param = 0;

      p->sample[i]->data = _AL_MALLOC_ATOMIC(p->sample[i]->len*((p->sample[i]->bits==8) ? 1 : sizeof(short)));
      if (!p->sample[i]->data) {
	 _AL_FREE(p->sample[i]);
	 _AL_FREE(p->extra[i]);
	 p->samples = i;
	 destroy_patch(p);
	 p = NULL;
	 *allegro_errno = ENOMEM;
	 goto getout;
      }

      if (p->sample[i]->bits == 8) {
	 /* read 8 bit sample data */
	 pack_fread(p->sample[i]->data, p->sample[i]->len, f);
	 if (!(mode & 2)) {
	    /* signed data - convert to unsigned */
	    for (j=0; j<(int)p->sample[i]->len; j++)
	       ((unsigned char *)p->sample[i]->data)[j] ^= 0x80;
	 }
      }
      else {
	 /* read 16 bit sample data */
	 for (j=0; j<(int)p->sample[i]->len; j++) {
	    ((unsigned short *)p->sample[i]->data)[j] = pack_igetw(f);
	 }
	 if (!(mode & 2)) {
	    /* signed data - convert to unsigned */
	    for (j=0; j<(int)p->sample[i]->len; j++)
	       ((unsigned short *)p->sample[i]->data)[j] ^= 0x8000;
	 }
	 if (odd_len)
	    pack_getc(f);
      }
   }

   getout:

   /* lock the data into physical memory */
   if (p) {
      LOCK_DATA(p, sizeof(PATCH));

      for (i=0; i<p->samples; i++) {
	 lock_sample(p->sample[i]);
	 LOCK_DATA(p->extra[i], sizeof(PATCH_EXTRA));
      }
   }

   return p;
}



/* _digmid_find_patches:
 *  Tries to locate the GUS patch set directory and index file (default.cfg).
 */
int _digmid_find_patches(char *dir, int dir_size, char *file, int file_size)
{
   char filename[1024], tmp1[64], tmp2[64], tmp3[64], tmp4[64];
   AL_CONST char *name;
   char *datafile, *objectname, *envvar, *subdir, *s;

   name = get_config_string(uconvert_ascii("sound", tmp1), uconvert_ascii("patches", tmp2), NULL);
   datafile = uconvert_ascii("patches.dat", tmp1);
   objectname = uconvert_ascii("default.cfg", tmp2);
   envvar = uconvert_ascii("ULTRADIR", tmp3);
   subdir = uconvert_ascii("midi", tmp4);

   if (find_allegro_resource(filename, name, NULL, datafile, objectname, envvar, subdir, sizeof(filename)) != 0)
      return FALSE;

   if (dir && file) {
      s = ustrrchr(filename, '#');
      if(s == NULL)
         s = get_filename(filename);
      else
         s += ustrlen("#");
      ustrzcpy(file, file_size, s);

      usetc(s, 0);
      ustrzcpy(dir, dir_size, filename);
   }

   return TRUE;
}



/* parse_string:
 *  Splits a string into component parts, storing them in the argv pointers.
 *  Returns the number of components.
 */
static int parse_string(char *buf, char *argv[])
{
   int c = 0;

   while (ugetc(buf) && (c<16)) {
      while ((ugetc(buf) == ' ') || (ugetc(buf) == '\t') || (ugetc(buf) == '='))
	 buf += uwidth(buf);

      if (ugetc(buf) == '#')
	 return c;

      if (ugetc(buf))
	 argv[c++] = buf;

      while (ugetc(buf) && (ugetc(buf) != ' ') && (ugetc(buf) != '\t') && (ugetc(buf) != '='))
	 buf += uwidth(buf);

      if (ugetc(buf))
	 buf += usetc(buf, 0);
   }

   return c;
}



/* digmid_load_patches:
 *  Reads the patches that are required by a particular song.
 */
static int digmid_load_patches(AL_CONST char *patches, AL_CONST char *drums)
{
   PACKFILE *f;
   char dir[1024], file[1024], buf[1024], filename[1024];
   char todo[256][1024];
   char *argv[16], *p;
   char tmp[128];
   int argc;
   int patchnum, flag_num;
   int drum_mode = FALSE;
   int override_mode = FALSE;
   int drum_start = 0;
   int type, size;
   int i, j, c;

   if (!_digmid_find_patches(dir, sizeof(dir), file, sizeof(file)))
      return -1;

   for (i=0; i<256; i++)
      usetc(todo[i], 0);

   ustrzcpy(buf, sizeof(buf), dir);
   ustrzcat(buf, sizeof(buf), file);

   f = pack_fopen(buf, F_READ);
   if (!f)
      return -1;

   while (pack_fgets(buf, sizeof(buf), f) != 0) {
      argc = parse_string(buf, argv);

      if (argc > 0) {
	 /* is first word all digits? */
	 flag_num = TRUE;
         p = argv[0];
         while ((c = ugetx(&p)) != 0) {
	    if ((!uisdigit(c)) && (c != '-')) {
	       flag_num = FALSE;
	       break;
	    }
	 }

	 if ((flag_num) && (argc >= 2)) {
	    if (ustricmp(argv[1], uconvert_ascii("begin_multipatch", tmp)) == 0) {
	       /* start the block of percussion instruments */
	       drum_start = ustrtol(argv[0], NULL, 10) - 1;
	       drum_mode = TRUE;
	    }
	    else if (ustricmp(argv[1], uconvert_ascii("override_patch", tmp)) == 0) {
	       /* ignore patch overrides */
	       override_mode = TRUE;
	    }
	    else if (!override_mode) {
	       /* must be a patch number */
	       patchnum = ustrtol(argv[0], NULL, 10);

	       if (!drum_mode)
		  patchnum--;

	       if ((patchnum >= 0) && (patchnum < 128) &&
		   (((drum_mode) && (drums[patchnum])) ||
		    ((!drum_mode) && (patches[patchnum])))) {

		  if (drum_mode)
		     patchnum += drum_start;

		  if (!patch[patchnum]) {
		     /* need to load this sample */
		     ustrzcpy(todo[patchnum], sizeof(todo[patchnum]), argv[1]);
		  }
	       }
	    }
	 }
	 else {
	    /* handle other keywords */
	    if (ustricmp(argv[0], uconvert_ascii("end_multipatch", tmp)) == 0) {
	       drum_mode = FALSE;
	       override_mode = FALSE;
	    }
	 }
      }
   }

   pack_fclose(f);

   if (ustrchr(dir, '#')) {
      /* read from a datafile */
      if ((ustrlen(dir) > 1) && (ugetat(dir, -1) == '#'))
	 usetat(dir, -1, 0);

      f = pack_fopen(dir, F_READ_PACKED);
      if (!f)
	 return -1;

      if (((ugetc(dir) == '#') && (ustrlen(dir) == 1)) || (!ustrchr(dir, '#'))) {
	 type = pack_mgetl(f);
	 if (type != DAT_MAGIC) {
	    pack_fclose(f);
	    return -1;
	 }
      }

      pack_mgetl(f);

      usetc(filename, 0);

      /* scan through the file */
      while (!pack_feof(f)) {
	 type = pack_mgetl(f);

	 if (type == DAT_PROPERTY) {
	    type = pack_mgetl(f);
	    size = pack_mgetl(f);

	    if (type == DAT_ID('N','A','M','E')) {
	       /* store name property */
	       pack_fread(buf, size, f);
	       buf[size] = 0;
	       do_uconvert(buf, U_ASCII, filename, U_CURRENT, sizeof(filename));
	    }
	    else {
	       /* skip other properties */
	       pack_fseek(f, size);
	    }
	 }
	 else if (type == DAT_PATCH) {
	    /* do we want this patch? */
	    for (i=0; i<256; i++)
	       if (ugetc(todo[i]) && (ustricmp(filename, todo[i]) == 0))
		  break;

	    if (i < 256) {
	       /* load this patch */
	       f = pack_fopen_chunk(f, FALSE);
	       patch[i] = load_patch(f, ((i > 127) ? (i - 127) : 0));
	       f = pack_fclose_chunk(f);

	       for (j=i+1; j<256; j++) {
		  /* share multiple copies of the instrument */
		  if (ustricmp(todo[i], todo[j]) == 0) {
		     patch[j] = patch[i];
		     usetc(todo[j], 0);
		  }
	       }

	       usetc(todo[i], 0);
	    }
	    else {
	       /* skip unwanted patch */
	       size = pack_mgetl(f);
	       pack_fseek(f, size+4);
	    }
	 }
	 else {
	    /* skip unwanted object */
	    size = pack_mgetl(f);
	    pack_fseek(f, size+4);
	 }
      }
   }
   else {
      /* read from regular disk files */
      for (i=0; i<256; i++) {
	 if (ugetc(todo[i])) {
	    if (is_relative_filename(todo[i])) {
	       ustrzcpy(filename, sizeof(filename), dir);
	       ustrzcat(filename, sizeof(filename), todo[i]);
            } else
	       ustrzcpy(filename, sizeof(filename), todo[i]);

	    if (ugetc(get_extension(filename)) == 0)
	       ustrzcat(filename, sizeof(filename), uconvert_ascii(".pat", tmp));

	    f = pack_fopen(filename, F_READ);
	    if (f) {
	       patch[i] = load_patch(f, ((i > 127) ? (i - 127) : 0));
	       pack_fclose(f);
	    }

	    for (j=i+1; j<256; j++) {
	       /* share multiple copies of the instrument */
	       if (ustricmp(todo[i], todo[j]) == 0) {
		  patch[j] = patch[i];
		  usetc(todo[j], 0);
	       }
	    }
	 }
      }
   }

   return 0;
}



/* digmid_freq:
 *  Helper for converting note numbers to sample frequencies.
 */ 
static int digmid_freq(int inst, SAMPLE *s, PATCH_EXTRA *e, int note, int bend)
{
   unsigned long freq, f1, f2, sfreq, base_note;

   sfreq = s->freq;
   base_note = e->base_note;

   /* calculate frequency */
   if(bend) {
      f1 = scale64(ftbl[note], sfreq, base_note);
      f2 = scale64(ftbl[note+1], sfreq, base_note);

      /* quick pitch bend method - ~.035% error - acceptable? */
      freq = ((f1*(4096-bend)) + (f2*bend)) / 4096;
   }
   else
      freq = scale64(ftbl[note], sfreq, base_note);

   /* frequency scaling */
   if (e->scale_factor != 1024) {
      f1 = scale64(sfreq, e->scale_freq, 60);
      freq -= f1;
      freq = scale64(freq, e->scale_factor, 1024);
      freq += f1;
   }

   /* lower by an octave if we are going to overflow */
   while (freq >= (1<<19)-1)
      freq /= 2;

   return freq;
}

END_OF_STATIC_FUNCTION(digmid_freq);



/* digmid_trigger:
 *  Helper for activating a specific sample layer.
 */
static void digmid_trigger(int inst, int snum, int note, int bend, int vol, int pan)
{
   int freq, voice;
   DIGMID_VOICE *info;
   PATCH_EXTRA *e;
   SAMPLE *s;

   voice = _midi_allocate_voice(-1, -1);
   if (voice < 0)
      return;

   s = patch[inst]->sample[snum];
   e = patch[inst]->extra[snum];

   if (inst > 127) {
      pan = e->pan;
      freq = s->freq;
   }
   else
      freq = digmid_freq(inst, s, e, note, bend);

   /* store note information for later use */
   info = &digmid_voice[voice - midi_digmid.basevoice];
   info->s = s;
   info->e = e;
   info->inst = inst;
   info->vol = vol;

   /* play the note */
   reallocate_voice(voice, s);
   voice_set_playmode(voice, e->play_mode);
   voice_set_volume(voice, vol);
   voice_set_frequency(voice, freq);
   voice_set_pan(voice, pan);

   if (e->sustain_level < 255)
      voice_ramp_volume(voice, e->decay_time, e->sustain_level*vol/255);

   voice_start(voice);
}

END_OF_STATIC_FUNCTION(digmid_trigger);



/* digmid_key_on:
 *  Triggers the specified voice. The instrument is specified as a GM
 *  patch number, pitch as a midi note number, and volume from 0-127.
 *  The bend parameter is _not_ expressed as a midi pitch bend value.
 *  It ranges from 0 (no pitch change) to 0xFFF (almost a semitone sharp).
 *  Drum sounds are indicated by passing an instrument number greater than
 *  128, in which case the sound is GM percussion key #(inst-128).
 */
static void digmid_key_on(int inst, int note, int bend, int vol, int pan)
{
   PATCH_EXTRA *e;
   long freq;
   int best, best_diff;
   int diff;
   int i, c;

   /* quit if instrument is not available */
   if ((!patch[inst]) || (patch[inst]->samples < 1))
      return;

   /* adjust volume and pan ranges */
   vol *= 2;
   pan *= 2;

   if (patch[inst]->samples == 1) {
      /* only one sample to choose from */
      digmid_trigger(inst, 0, note, bend, vol, pan);
   }
   else {
      /* find the sample(s) with best frequency range */
      best = -1;
      best_diff = INT_MAX;
      c = 0;

      for (i=0; i<patch[inst]->samples; i++) {
	 freq = ftbl[note];
	 e = patch[inst]->extra[i];

	 if ((freq >= e->low_note) && (freq <= e->high_note)) {
	    digmid_trigger(inst, i, note, bend, vol, pan);
	    c++;
	    if (c > 4)
	       break;
	 }
	 else {
	    diff = MIN(ABS(freq - e->low_note), ABS(freq - e->high_note));
	    if (diff < best_diff) {
	       best_diff = diff;
	       best = i;
	    }
	 }
      }

      if ((c <= 0) && (best >= 0))
	 digmid_trigger(inst, best, note, bend, vol, pan);
   }
}

END_OF_STATIC_FUNCTION(digmid_key_on);



/* digmid_key_off:
 *  Hey, guess what this does :-)
 */
static void digmid_key_off(int voice)
{
   DIGMID_VOICE *info = &digmid_voice[voice - midi_digmid.basevoice];

   if (info->inst > 127) 
      return;

   if (info->e->release_time > 0)
      voice_ramp_volume(voice, info->e->release_time, 0);
   else
      voice_stop(voice);
}

END_OF_STATIC_FUNCTION(digmid_key_off);



/* digmid_set_volume:
 *  Sets the volume of the specified voice (vol range 0-127).
 */
static void digmid_set_volume(int voice, int vol)
{
   DIGMID_VOICE *info = &digmid_voice[voice - midi_digmid.basevoice];
   int v;

   if (info->inst > 127) 
      return;

   vol *= 2;

   if (info->e->sustain_level < 255) {
      /* adjust for volume ramping */
      int current = voice_get_volume(voice);
      int target = info->e->sustain_level*info->vol/255;
      int start = info->vol;

      if (ABS(current - target) < 8) {
	 /* ramp has finished */
	 voice_set_volume(voice, vol*info->e->sustain_level/255);
      }
      else {
	 /* in the middle of a ramp */
	 int mu;

	 if (start > target)
	    mu = MID(0, (current-target) * 256 / (start-target), 256);
	 else
	    mu = 0;

	 v = mu+info->e->sustain_level*(256-mu)/256;
	 v = MID(0, vol*v/255, 255);

	 voice_set_volume(voice, v);
	 voice_ramp_volume(voice, info->e->decay_time*mu/256, info->e->sustain_level*vol/255);
      }
   }
   else {
      /* no ramp */
      voice_set_volume(voice, vol);
   }

   info->vol = vol;
}

END_OF_STATIC_FUNCTION(digmid_set_volume);



/* digmid_set_pitch:
 *  Sets the pitch of the specified voice.
 */
static void digmid_set_pitch(int voice, int note, int bend)
{
   DIGMID_VOICE *info = &digmid_voice[voice - midi_digmid.basevoice];
   int freq;

   if (info->inst > 127)
      return;

   freq = digmid_freq(info->inst, info->s, info->e, note, bend);

   voice_set_frequency(voice, freq);
}

END_OF_STATIC_FUNCTION(digmid_set_pitch);



static void digmid_set_pan(int voice, int pan)
{
   DIGMID_VOICE *info = &digmid_voice[voice - midi_digmid.basevoice];

   if (info->inst > 127)
      return;

   pan *= 2;
   voice_set_pan(voice, pan);
}

END_OF_STATIC_FUNCTION(digmid_set_pan);



/* digmid_detect:
 *  Have we got a sensible looking patch set?
 */
static int digmid_detect(int input)
{
   if (input)
      return FALSE;

   if (!_digmid_find_patches(NULL, 0, NULL, 0)) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("DIGMID patch set not found"));
      return FALSE;
   }

   return TRUE;
}



/* digmid_init:
 *  Setup the digmid driver.
 */
static int digmid_init(int input, int voices)
{
   float f;
   int i;

   midi_digmid.desc = get_config_text("Software wavetable synth");

   for (i=0; i<256; i++)
      patch[i] = NULL;

   midi_digmid.voices = voices;

   /* A10 = 14080.000 hz */
   ftbl[129] = 14080000;
   f = ftbl[129]; 

   /* create frequency table */
   for (i=128; i>=0; i--) {
       f /= pow(2.0, 1.0/12.0);
       ftbl[i] = f;
   }

   LOCK_VARIABLE(midi_digmid);
   LOCK_VARIABLE(patch);
   LOCK_VARIABLE(ftbl);
   LOCK_VARIABLE(digmid_voice);
   LOCK_FUNCTION(digmid_freq);
   LOCK_FUNCTION(digmid_trigger);
   LOCK_FUNCTION(digmid_key_on);
   LOCK_FUNCTION(digmid_key_off);
   LOCK_FUNCTION(digmid_set_volume);
   LOCK_FUNCTION(digmid_set_pitch);
   LOCK_FUNCTION(digmid_set_pan);

   return 0;
}



/* digmid_exit:
 *  Cleanup when we are finished.
 */
static void digmid_exit(int input)
{
   int i, j;

   for (i=0; i<256; i++) {
      if (patch[i]) {
	 for (j=i+1; j<256; j++) {
	    if (patch[j] == patch[i])
	       patch[j] = NULL;
	 }
	 destroy_patch(patch[i]);
	 patch[i] = NULL;
      }
   }
}


