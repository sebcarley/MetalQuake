/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "snd_main.h"

extern cvar_t snd_softclip;

static portable_sampleframe_t paintbuffer[PAINTBUFFER_SIZE];

extern speakerlayout_t snd_speakerlayout; // for querying the listeners

#ifdef CONFIG_VIDEO_CAPTURE
static portable_sampleframe_t paintbuffer_unswapped[PAINTBUFFER_SIZE];

static void S_CaptureAVISound(const portable_sampleframe_t *sampleframes, size_t length)
{
	size_t i;
	unsigned int j;

	if (!cls.capturevideo.active)
		return;

	// undo whatever swapping the channel layout (swapstereo, ALSA) did
	for(j = 0; j < snd_speakerlayout.channels; ++j)
	{
		unsigned int j0 = snd_speakerlayout.listeners[j].channel_unswapped;
		for(i = 0; i < length; ++i)
			paintbuffer_unswapped[i].sample[j0] = sampleframes[i].sample[j];
	}

	SCR_CaptureVideo_SoundFrame(paintbuffer_unswapped, length);
}
#endif

extern cvar_t snd_softclip;

static void S_SoftClipPaintBuffer(portable_sampleframe_t *painted_ptr, int nbframes, int width, int nchannels)
{
	int i;

	if((snd_softclip.integer == 1 && width <= 2) || snd_softclip.integer > 1)
	{
		portable_sampleframe_t *p = painted_ptr;

#if 0
/* Soft clipping, the sound of a dream, thanks to Jon Wattes
   post to Musicdsp.org */
#define SOFTCLIP(x) (x) = sin(bound(-M_PI/2, (x), M_PI/2)) * 0.25
#endif

		// let's do a simple limiter instead, seems to sound better
		static float maxvol = 0;
		maxvol = max(1.0f, maxvol * (1.0f - nbframes / (0.4f * snd_renderbuffer->format.speed)));
#define SOFTCLIP(x) if(fabs(x)>maxvol) maxvol=fabs(x); (x) /= maxvol;

		if (nchannels == 8)  // 7.1 surround
		{
			for (i = 0;i < nbframes;i++, p++)
			{
				SOFTCLIP(p->sample[0]);
				SOFTCLIP(p->sample[1]);
				SOFTCLIP(p->sample[2]);
				SOFTCLIP(p->sample[3]);
				SOFTCLIP(p->sample[4]);
				SOFTCLIP(p->sample[5]);
				SOFTCLIP(p->sample[6]);
				SOFTCLIP(p->sample[7]);
			}
		}
		else if (nchannels == 6)  // 5.1 surround
		{
			for (i = 0; i < nbframes; i++, p++)
			{
				SOFTCLIP(p->sample[0]);
				SOFTCLIP(p->sample[1]);
				SOFTCLIP(p->sample[2]);
				SOFTCLIP(p->sample[3]);
				SOFTCLIP(p->sample[4]);
				SOFTCLIP(p->sample[5]);
			}
		}
		else if (nchannels == 4)  // 4.0 surround
		{
			for (i = 0; i < nbframes; i++, p++)
			{
				SOFTCLIP(p->sample[0]);
				SOFTCLIP(p->sample[1]);
				SOFTCLIP(p->sample[2]);
				SOFTCLIP(p->sample[3]);
			}
		}
		else if (nchannels == 2)  // 2.0 stereo
		{
			for (i = 0; i < nbframes; i++, p++)
			{
				SOFTCLIP(p->sample[0]);
				SOFTCLIP(p->sample[1]);
			}
		}
		else if (nchannels == 1)  // 1.0 mono
		{
			for (i = 0; i < nbframes; i++, p++)
			{
				SOFTCLIP(p->sample[0]);
			}
		}
#undef SOFTCLIP
	}
}

static void S_ConvertPaintBuffer(portable_sampleframe_t *painted_ptr, void *rb_ptr, int nbframes, int width, int nchannels)
{
	int i;
	float val;
	if (width == 4)  // 32bit float
	{
		float *snd_out = (float*)rb_ptr;
		if (nchannels == 8)  // 7.1 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				*snd_out++ = painted_ptr->sample[0];
				*snd_out++ = painted_ptr->sample[1];
				*snd_out++ = painted_ptr->sample[2];
				*snd_out++ = painted_ptr->sample[3];
				*snd_out++ = painted_ptr->sample[4];
				*snd_out++ = painted_ptr->sample[5];
				*snd_out++ = painted_ptr->sample[6];
				*snd_out++ = painted_ptr->sample[7];
			}
		}
		else if (nchannels == 6)  // 5.1 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				*snd_out++ = painted_ptr->sample[0];
				*snd_out++ = painted_ptr->sample[1];
				*snd_out++ = painted_ptr->sample[2];
				*snd_out++ = painted_ptr->sample[3];
				*snd_out++ = painted_ptr->sample[4];
				*snd_out++ = painted_ptr->sample[5];
			}
		}
		else if (nchannels == 4)  // 4.0 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				*snd_out++ = painted_ptr->sample[0];
				*snd_out++ = painted_ptr->sample[1];
				*snd_out++ = painted_ptr->sample[2];
				*snd_out++ = painted_ptr->sample[3];
			}
		}
		else if (nchannels == 2)  // 2.0 stereo
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				*snd_out++ = painted_ptr->sample[0];
				*snd_out++ = painted_ptr->sample[1];
			}
		}
		else if (nchannels == 1)  // 1.0 mono
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				*snd_out++ = painted_ptr->sample[0];
			}
		}
	}
	else if (width == 2)  // 16bit
	{
		short *snd_out = (short*)rb_ptr;
		if (nchannels == 8)  // 7.1 surround
		{
			for (i = 0;i < nbframes;i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[1] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[2] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[3] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[4] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[5] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[6] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[7] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
			}
		}
		else if (nchannels == 6)  // 5.1 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[1] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[2] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[3] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[4] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[5] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
			}
		}
		else if (nchannels == 4)  // 4.0 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[1] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[2] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[3] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
			}
		}
		else if (nchannels == 2)  // 2.0 stereo
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
				val = (int)(painted_ptr->sample[1] * 32768.0f);*snd_out++ = bound(-32768, val, 32767);
			}
		}
		else if (nchannels == 1)  // 1.0 mono
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)((painted_ptr->sample[0] + painted_ptr->sample[1]) * 16384.0f);*snd_out++ = bound(-32768, val, 32767);
			}
		}
	}
	else  // 8bit
	{
		unsigned char *snd_out = (unsigned char*)rb_ptr;
		if (nchannels == 8)  // 7.1 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[1] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[2] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[3] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[4] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[5] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[6] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[7] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
			}
		}
		else if (nchannels == 6)  // 5.1 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[1] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[2] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[3] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[4] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[5] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
			}
		}
		else if (nchannels == 4)  // 4.0 surround
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[1] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[2] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[3] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
			}
		}
		else if (nchannels == 2)  // 2.0 stereo
		{
			for (i = 0; i < nbframes; i++, painted_ptr++)
			{
				val = (int)(painted_ptr->sample[0] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
				val = (int)(painted_ptr->sample[1] * 128.0f) + 128; *snd_out++ = bound(0, val, 255);
			}
		}
		else if (nchannels == 1)  // 1.0 mono
		{
			for (i = 0;i < nbframes;i++, painted_ptr++)
			{
				val = (int)((painted_ptr->sample[0] + painted_ptr->sample[1]) * 64.0f) + 128; *snd_out++ = bound(0, val, 255);
			}
		}
	}
}



/*
===============================================================================

UNDERWATER EFFECT

Muffles the intensity of sounds when the player is underwater

===============================================================================
*/

static struct
{
	float intensity;
	float alpha;
	float accum[SND_LISTENERS];
}
underwater = {0.f, 1.f, {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f}};

void S_SetUnderwaterIntensity(void)
{
	float target = cl.view_underwater ? bound(0.f, snd_waterfx.value, 2.f) : 0.f;

	if (underwater.intensity < target)
	{
		underwater.intensity += cl.realframetime * 4.f;
		underwater.intensity = min(underwater.intensity, target);
	}
	else if (underwater.intensity > target)
	{
		underwater.intensity -= cl.realframetime * 4.f;
		underwater.intensity = max(underwater.intensity, target);
	}

	underwater.alpha = underwater.intensity ? exp(-underwater.intensity * log(12.f)) : 1.f;
}

static void S_UnderwaterFilter(int endtime)
{
	int i;
	int sl;

	if (!underwater.intensity)
	{
		if (endtime > 0)
			for (sl = 0; sl < SND_LISTENERS; sl++)
				underwater.accum[sl] = paintbuffer[endtime-1].sample[sl];
		return;
	}

	for (i = 0; i < endtime; i++)
		for (sl = 0; sl < SND_LISTENERS; sl++)
		{
			underwater.accum[sl] += underwater.alpha * (paintbuffer[i].sample[sl] - underwater.accum[sl]);
			paintbuffer[i].sample[sl] = underwater.accum[sl];
		}
}



/*
===============================================================================

ROOM REVERB (SEPTEMBER2 Part B1, 2026-09-06)

A Freeverb-shaped algorithmic reverb on a MONO SEND BUS after the channel
paint: eight parallel feedback combs with a one-pole damping filter in each
loop, then four series allpasses, per output channel, the right channel's
delays offset by 23 samples so the tail decorrelates -- integer-sample delay
lines, no convolution, ~1% of a core. The send is filled from the channels'
own resampled samples (a second lerp pass over the fetch buffer, so the dry
paint loops are textually untouched), pre-delayed and low-cut, and the wet
result is ADDED to the front pair. THE TASTE RULE: at snd_reverb 0 not one
line of this runs and the dry path is bit-identical (the mix-dump gate); at 1
the dry path is the loudest thing in the mix -- the wet is what the ROOM does
to the sound, never what the sound is. Stereo only: the bus is skipped on any
other speaker layout. Music and local sounds never enter it (sendvol 0).
The room itself comes from S_Update (snd_main.c): ten tracelines from the
listener give a mean free path and a sky share; the parameters arrive here
as targets and are smoothed per mix block so nothing zips.

===============================================================================
*/

#define RV_COMBS 8
#define RV_ALLP 4
#define RV_MAXCOMB 4096      // 1617 * 96000/44100 + 23 = 3543: room for a 96 kHz device
#define RV_MAXALLP 1400
#define RV_MAXPRE 9600       // 100 ms at 96 kHz
#define RV_STEREOSPREAD 23
static const int rv_combtune[RV_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };   // Freeverb's tunings at 44.1 kHz
static const int rv_alltune[RV_ALLP] = { 556, 441, 341, 225 };
typedef struct { float buf[RV_MAXCOMB]; int len, idx; float store; } rv_comb_t;
typedef struct { float buf[RV_MAXALLP]; int len, idx; } rv_allp_t;
static struct
{
	rv_comb_t comb[2][RV_COMBS];
	rv_allp_t allp[2][RV_ALLP];
	float pre[RV_MAXPRE]; int prelen, preidx;
	float hpx1, hpy1, hpcoef;
	float feedback, damp, wet;              // live (smoothed per block)
	float tfeedback, tdamp, twet, tpre_ms, tlowcut;   // targets from S_Update
	int rate;
	int inited;
} rv;
int snd_reverb_active;                      // read by the mixer; written by S_ReverbSetParams (main thread)
static float snd_sendbuf[PAINTBUFFER_SIZE]; // the mono send, one mix block

void S_ReverbSetParams(int enable, float feedback, float damp, float wet, float predelay_ms, float lowcut_hz)
{
	rv.tfeedback = bound(0.0f, feedback, 0.985f);
	rv.tdamp = bound(0.0f, damp, 1.0f);
	rv.twet = bound(0.0f, wet, 4.0f);
	rv.tpre_ms = bound(0.0f, predelay_ms, 100.0f);
	rv.tlowcut = bound(0.0f, lowcut_hz, 2000.0f);
	if (enable && !snd_reverb_active)
		rv.inited = 0;   // a fresh enable starts from silence, not from a stale tail
	snd_reverb_active = enable ? 1 : 0;
}

static void S_ReverbInit(int rate)
{
	int c, k;
	memset(&rv.comb, 0, sizeof(rv.comb));
	memset(&rv.allp, 0, sizeof(rv.allp));
	memset(rv.pre, 0, sizeof(rv.pre));
	rv.preidx = 0; rv.hpx1 = rv.hpy1 = 0.0f;
	for (c = 0; c < 2; c++)
	{
		for (k = 0; k < RV_COMBS; k++)
		{
			int len = (int)((double)rv_combtune[k] * rate / 44100.0) + (c ? RV_STEREOSPREAD : 0);
			rv.comb[c][k].len = bound(8, len, RV_MAXCOMB);
			rv.comb[c][k].idx = 0; rv.comb[c][k].store = 0.0f;
		}
		for (k = 0; k < RV_ALLP; k++)
		{
			int len = (int)((double)rv_alltune[k] * rate / 44100.0) + (c ? RV_STEREOSPREAD : 0);
			rv.allp[c][k].len = bound(8, len, RV_MAXALLP);
			rv.allp[c][k].idx = 0;
		}
	}
	rv.rate = rate;
	rv.feedback = rv.tfeedback; rv.damp = rv.tdamp; rv.wet = 0.0f;   // the wet fades in from silence
	rv.inited = 1;
}

static void S_ReverbProcess(int nframes)
{
	int i, c, k;
	int rate = snd_renderbuffer->format.speed;
	if (!rv.inited || rv.rate != rate)
	{
		S_ReverbInit(rate);
		Con_DPrintf("reverb: bus running at %d Hz (wet target %.2f)\n", rate, rv.twet);   // first-event liveness (developer)
	}
	// smoothed parameters: one step per mix block (a few ms), no zipper noise
	rv.feedback += (rv.tfeedback - rv.feedback) * 0.1f;
	rv.damp     += (rv.tdamp     - rv.damp)     * 0.1f;
	rv.wet      += (rv.twet      - rv.wet)      * 0.1f;
	rv.prelen = bound(1, (int)(rv.tpre_ms * 0.001f * rate), RV_MAXPRE - 1);
	rv.hpcoef = (rv.tlowcut > 1.0f) ? (float)exp(-2.0 * M_PI * rv.tlowcut / rate) : 0.0f;
	for (i = 0; i < nframes; i++)
	{
		float in = snd_sendbuf[i];
		float x, out[2];
		// pre-delay: the first reflection arrives after the direct sound
		x = rv.pre[rv.preidx];
		rv.pre[rv.preidx] = in;
		if (++rv.preidx >= rv.prelen) rv.preidx = 0;
		// low cut: a one-pole high-pass keeps the mud out of the tail
		if (rv.hpcoef > 0.0f)
		{
			float y = rv.hpcoef * (rv.hpy1 + x - rv.hpx1);
			rv.hpx1 = x; rv.hpy1 = y; x = y;
		}
		x *= 0.015f;   // Freeverb's fixed input gain
		for (c = 0; c < 2; c++)
		{
			float acc = 0.0f;
			for (k = 0; k < RV_COMBS; k++)
			{
				rv_comb_t *cb = &rv.comb[c][k];
				float o = cb->buf[cb->idx];
				cb->store = o * (1.0f - rv.damp) + cb->store * rv.damp;
				cb->buf[cb->idx] = x + cb->store * rv.feedback;
				if (++cb->idx >= cb->len) cb->idx = 0;
				acc += o;
			}
			for (k = 0; k < RV_ALLP; k++)
			{
				rv_allp_t *ap = &rv.allp[c][k];
				float bo = ap->buf[ap->idx];
				float o = -acc + bo;
				ap->buf[ap->idx] = acc + bo * 0.5f;
				if (++ap->idx >= ap->len) ap->idx = 0;
				acc = o;
			}
			out[c] = acc;
		}
		paintbuffer[i].sample[0] += out[0] * rv.wet * 3.0f;   // Freeverb's scalewet: wet 0.33 is its nominal unity
		paintbuffer[i].sample[1] += out[1] * rv.wet * 3.0f;
	}
}

/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

void S_MixToBuffer(void *stream, unsigned int bufferframes)
{
	int channelindex;
	channel_t *ch;
	int totalmixframes;
	unsigned char *outbytes = (unsigned char *) stream;
	sfx_t *sfx;
	portable_sampleframe_t *paint;
	int wantframes;
	int i;
	int count;
	int fetched;
	int fetch;
	int istartframe;
	int iendframe;
	int ilengthframes;
	int totallength;
	int loopstart;
	int indexfrac;
	int indexfracstep;
#define S_FETCHBUFFERSIZE 4096
	float fetchsampleframes[S_FETCHBUFFERSIZE*2];
	const float *fetchsampleframe;
	float vol[SND_LISTENERS];
	float lerp[2];
	float sample[3];
	double posd;
	double speedd;
	float maxvol;
	float sendv;
	int sendbase, sendfrac0;
	qbool looping;
	qbool silent;

	// mix as many times as needed to fill the requested buffer
	while (bufferframes)
	{
		// limit to the size of the paint buffer
		totalmixframes = min(bufferframes, PAINTBUFFER_SIZE);

		// clear the paint buffer
		memset(paintbuffer, 0, totalmixframes * sizeof(paintbuffer[0]));
		if (snd_reverb_active)
			memset(snd_sendbuf, 0, totalmixframes * sizeof(snd_sendbuf[0]));

		// paint in the channels.
		// channels with zero volumes still advance in time but don't paint.
		ch = channels; // cppcheck complains here but it is wrong, channels is a channel_t[MAX_CHANNELS] and not an int
		for (channelindex = 0;channelindex < (int)total_channels;channelindex++, ch++)
		{
			sfx = ch->sfx;
			if (sfx == NULL)
				continue;
			if (!S_LoadSound (sfx, true))
				continue;
			if (ch->flags & CHANNELFLAG_PAUSED)
				continue;
			if (!sfx->total_length)
				continue;

			// copy the channel information to the stack for reference, otherwise the
			// values might change during a mix if the spatializer is updating them
			// (note: this still may get some old and some new values!)
			posd = ch->position;
			speedd = ch->mixspeed * sfx->format.speed / snd_renderbuffer->format.speed;
			for (i = 0;i < SND_LISTENERS;i++)
				vol[i] = ch->volume[i];
			sendv = snd_reverb_active ? ch->sendvol : 0.0f;   // Part B1: this channel's reverb send

			// check total volume level, because we can skip some code on silent sounds but other code must still run (position updates mainly)
			maxvol = 0;
			for (i = 0;i < SND_LISTENERS;i++)
				if(vol[i] > maxvol)
					maxvol = vol[i];
			switch(snd_renderbuffer->format.width)
			{
				case 1: // 8bpp
					silent = maxvol < (1.0f / (256.0f));
					// so silent it has zero effect
					break;
				case 2: // 16bpp
					silent = maxvol < (1.0f / (65536.0f));
					// so silent it has zero effect
					break;
				default: // floating point
					silent = maxvol < 1.0e-13f;
					// 130 dB is difference between hearing
					// threshold and a jackhammer from
					// working distance.
					// therefore, anyone who turns up
					// volume so much they notice this
					// cutoff, likely already has their
					// ear-drums blown out anyway.
					break;
			}

			// when doing prologic mixing, some channels invert one side
			if (ch->prologic_invert == -1)
				vol[1] *= -1.0f;

			// get some sfx info in a consistent form
			totallength = sfx->total_length;
			loopstart = (int)sfx->loopstart < totallength ? (int)sfx->loopstart : ((ch->flags & CHANNELFLAG_FORCELOOP) ? 0 : totallength);
			looping = loopstart < totallength;

			// do the actual paint now (may skip work if silent)
			paint = paintbuffer;
			istartframe = 0;
			for (wantframes = totalmixframes;wantframes > 0;posd += count * speedd, wantframes -= count)
			{
				// check if this is a delayed sound
				if (posd < 0)
				{
					// for a delayed sound we have to eat into the delay first
					count = (int)floor(-posd / speedd) + 1;
					count = bound(1, count, wantframes);
					// let the for loop iterator apply the skip
					continue;
				}

				// compute a fetch size that won't overflow our buffer
				count = wantframes;
				for (;;)
				{
					istartframe = (int)floor(posd);
					iendframe = (int)floor(posd + (count-1) * speedd);
					ilengthframes = count > 1 ? (iendframe - istartframe + 2) : 2;
					if (ilengthframes <= S_FETCHBUFFERSIZE)
						break;
					// reduce count by 25% and try again
					count -= count >> 2;
				}

				// zero whole fetch buffer for safety
				// (floating point noise from uninitialized memory = HORRIBLE)
				// otherwise we would only need to clear the excess
				if (!silent)
					memset(fetchsampleframes, 0, ilengthframes*sfx->format.channels*sizeof(fetchsampleframes[0]));

				// if looping, do multiple fetches
				fetched = 0;
				for (;;)
				{
					fetch = min(ilengthframes - fetched, totallength - istartframe);
					if (fetch > 0)
					{
						if (!silent)
							sfx->fetcher->getsamplesfloat(ch, sfx, istartframe, fetch, fetchsampleframes + fetched*sfx->format.channels);
						istartframe += fetch;
						fetched += fetch;
					}
					if (istartframe == totallength && looping && fetched < ilengthframes)
					{
						// loop and fetch some more
						posd += loopstart - totallength;
						istartframe = loopstart;
					}
					else
					{
						break;
					}
				}

				// SEPTEMBER2 Part B2: OCCLUSION AS A LOW-PASS. A blocked sound is
				// DULLED before it is quiet: one pole per channel over the fetched
				// samples, at the sfx's own rate (the coefficient is computed there
				// by the spatializer), before the resampling paint below -- so the
				// dry paint loops are untouched and lpalpha 0 is the old bytes.
				// The one frame a fetch block re-fetches from the previous block is
				// filtered twice against an advanced state: a one-pole's memory of
				// one sample, inaudible. Mono sounds only; music is stereo and local.
				if (!silent && sfx->format.channels == 1 && ch->lpalpha > 0.0f && ch->lpalpha < 1.0f)
				{
					float a = ch->lpalpha, z = ch->lpstate;
					int k;
					for (k = 0; k < ilengthframes; k++)
					{
						z += a * (fetchsampleframes[k] - z);
						fetchsampleframes[k] = z;
					}
					ch->lpstate = z;
				}
				// set up our fixedpoint resampling variables (float to int conversions are expensive so do not do one per sampleframe)
				fetchsampleframe = fetchsampleframes;
				indexfrac = (int)floor((posd - floor(posd)) * 65536.0);
				indexfracstep = (int)floor(speedd * 65536.0);
				sendbase = (int)(paint - paintbuffer);   // Part B1: where this fetch's frames land in the block
				sendfrac0 = indexfrac;
				if (!silent)
				{
					if (sfx->format.channels == 2)
					{
						// music is stereo
#if SND_LISTENERS != 8
#error the following code only supports up to 8 channels, update it
#endif
						if (snd_speakerlayout.channels > 2)
						{
							// surround mixing
							for (i = 0;i < count;i++, paint++)
							{
								lerp[1] = indexfrac * (1.0f / 65536.0f);
								lerp[0] = 1.0f - lerp[1];
								sample[0] = fetchsampleframe[0] * lerp[0] + fetchsampleframe[2] * lerp[1];
								sample[1] = fetchsampleframe[1] * lerp[0] + fetchsampleframe[3] * lerp[1];
								sample[2] = (sample[0] + sample[1]) * 0.5f;
								paint->sample[0] += sample[0] * vol[0];
								paint->sample[1] += sample[1] * vol[1];
								paint->sample[2] += sample[0] * vol[2];
								paint->sample[3] += sample[1] * vol[3];
								paint->sample[4] += sample[2] * vol[4];
								paint->sample[5] += sample[2] * vol[5];
								paint->sample[6] += sample[0] * vol[6];
								paint->sample[7] += sample[1] * vol[7];
								indexfrac += indexfracstep;
								fetchsampleframe += 2 * (indexfrac >> 16);
								indexfrac &= 0xFFFF;
							}
						}
						else
						{
							// stereo mixing
							for (i = 0;i < count;i++, paint++)
							{
								lerp[1] = indexfrac * (1.0f / 65536.0f);
								lerp[0] = 1.0f - lerp[1];
								sample[0] = fetchsampleframe[0] * lerp[0] + fetchsampleframe[2] * lerp[1];
								sample[1] = fetchsampleframe[1] * lerp[0] + fetchsampleframe[3] * lerp[1];
								paint->sample[0] += sample[0] * vol[0];
								paint->sample[1] += sample[1] * vol[1];
								indexfrac += indexfracstep;
								fetchsampleframe += 2 * (indexfrac >> 16);
								indexfrac &= 0xFFFF;
							}
						}
					}
					else if (sfx->format.channels == 1)
					{
						// most sounds are mono
#if SND_LISTENERS != 8
#error the following code only supports up to 8 channels, update it
#endif
						if (snd_speakerlayout.channels > 2)
						{
							// surround mixing
							for (i = 0;i < count;i++, paint++)
							{
								lerp[1] = indexfrac * (1.0f / 65536.0f);
								lerp[0] = 1.0f - lerp[1];
								sample[0] = fetchsampleframe[0] * lerp[0] + fetchsampleframe[1] * lerp[1];
								paint->sample[0] += sample[0] * vol[0];
								paint->sample[1] += sample[0] * vol[1];
								paint->sample[2] += sample[0] * vol[2];
								paint->sample[3] += sample[0] * vol[3];
								paint->sample[4] += sample[0] * vol[4];
								paint->sample[5] += sample[0] * vol[5];
								paint->sample[6] += sample[0] * vol[6];
								paint->sample[7] += sample[0] * vol[7];
								indexfrac += indexfracstep;
								fetchsampleframe += (indexfrac >> 16);
								indexfrac &= 0xFFFF;
							}
						}
						else
						{
							// stereo mixing
							for (i = 0;i < count;i++, paint++)
							{
								lerp[1] = indexfrac * (1.0f / 65536.0f);
								lerp[0] = 1.0f - lerp[1];
								sample[0] = fetchsampleframe[0] * lerp[0] + fetchsampleframe[1] * lerp[1];
								paint->sample[0] += sample[0] * vol[0];
								paint->sample[1] += sample[0] * vol[1];
								indexfrac += indexfracstep;
								fetchsampleframe += (indexfrac >> 16);
								indexfrac &= 0xFFFF;
							}
						}
					}
				}
				// SEPTEMBER2 Part B1: THE SEND. The same lerp the mono paint loops
				// just did, run again from the block's start into the mono send
				// bus at this channel's send level -- a second pass rather than a
				// line inside those loops, so the dry path's text does not move.
				if (!silent && sendv > 0.0f && sfx->format.channels == 1)
				{
					static int sendreported;
					const float *sf = fetchsampleframes;
					if (!sendreported) { sendreported = 1; Con_DPrintf("reverb: first send (%s at %.3f)\n", sfx->name, sendv); }
					int sfrac = sendfrac0;
					int k;
					for (k = 0; k < count; k++)
					{
						float l1 = sfrac * (1.0f / 65536.0f);
						snd_sendbuf[sendbase + k] += (sf[0] * (1.0f - l1) + sf[1] * l1) * sendv;
						sfrac += indexfracstep;
						sf += (sfrac >> 16);
						sfrac &= 0xFFFF;
					}
				}
			}
			ch->position = posd;
			if (!looping && istartframe == totallength)
				S_StopChannel(ch - channels, false, false);
		}

		// Part B1: the room's answer, added to the front pair BEFORE the soft clip
		// (so a loud room cannot clip harder than the dry mix would); stereo only
		if (snd_reverb_active && snd_renderbuffer->format.channels == 2)
			S_ReverbProcess(totalmixframes);
		S_SoftClipPaintBuffer(paintbuffer, totalmixframes, snd_renderbuffer->format.width, snd_renderbuffer->format.channels);

		S_UnderwaterFilter(totalmixframes);


#ifdef CONFIG_VIDEO_CAPTURE
		if (!snd_usethreadedmixing)
			S_CaptureAVISound(paintbuffer, totalmixframes);
#endif

		S_ConvertPaintBuffer(paintbuffer, outbytes, totalmixframes, snd_renderbuffer->format.width, snd_renderbuffer->format.channels);
		S_DumpMixBytes(outbytes, (size_t)totalmixframes * snd_renderbuffer->format.width * snd_renderbuffer->format.channels);   // SND_DUMPMIX: the mix bed

		// advance the output pointer
		outbytes += totalmixframes * snd_renderbuffer->format.width * snd_renderbuffer->format.channels;
		bufferframes -= totalmixframes;
	}
}
