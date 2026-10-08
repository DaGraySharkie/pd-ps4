#include <PR/ultratypes.h>
#include <stdio.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"

#ifdef PLATFORM_PS4

// PS4: SDL has no audio driver here, so samples go to sceAudioOut directly. The main port only
// accepts 48 kHz; the game mixes at 22020 Hz. A feeder thread resamples (linear interpolation)
// and hands 256-frame blocks to the system, which blocks until the previous block has played.

#include <pthread.h>
#include <string.h>
#include <orbis/AudioOut.h>

#define PS4_AUDIO_SRC_RATE 22020
#define PS4_AUDIO_OUT_RATE 48000
#define PS4_AUDIO_GRANULARITY 256
#define PS4_AUDIO_RING_FRAMES 32768 // must be a power of two
#define PS4_AUDIO_SYSTEM_USER 0xFF

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

static const s16 *nextBuf;
static u32 nextSize = 0;

static s16 ring[PS4_AUDIO_RING_FRAMES * 2];
static u32 ringRead = 0;  // in frames, wraps naturally
static u32 ringWrite = 0;
static pthread_mutex_t ringLock = PTHREAD_MUTEX_INITIALIZER;
static s32 audioPort = -1;
static volatile s32 audioRunning = 0;

static inline u32 ringCount(void)
{
	return ringWrite - ringRead;
}

static void *audioFeeder(void *arg)
{
	(void)arg;
	static s16 block[PS4_AUDIO_GRANULARITY * 2];
	const f64 step = (f64)PS4_AUDIO_SRC_RATE / (f64)PS4_AUDIO_OUT_RATE;
	f64 pos = 0.0; // fractional read position relative to ringRead

	while (audioRunning) {
		pthread_mutex_lock(&ringLock);
		const u32 avail = ringCount();
		for (u32 i = 0; i < PS4_AUDIO_GRANULARITY; ++i) {
			const u32 idx = (u32)pos;
			if (idx + 1 < avail) {
				const f64 frac = pos - (f64)idx;
				const s16 *a = &ring[((ringRead + idx) & (PS4_AUDIO_RING_FRAMES - 1)) * 2];
				const s16 *b = &ring[((ringRead + idx + 1) & (PS4_AUDIO_RING_FRAMES - 1)) * 2];
				block[i * 2 + 0] = (s16)(a[0] + (b[0] - a[0]) * frac);
				block[i * 2 + 1] = (s16)(a[1] + (b[1] - a[1]) * frac);
				pos += step;
			} else {
				// underrun: silence until the game catches up
				block[i * 2 + 0] = 0;
				block[i * 2 + 1] = 0;
			}
		}
		u32 consumed = (u32)pos;
		if (consumed > avail) {
			consumed = avail;
		}
		ringRead += consumed;
		pos -= (f64)consumed;
		if (pos >= 1.0) {
			pos = 0.0;
		}
		pthread_mutex_unlock(&ringLock);

		sceAudioOutOutput(audioPort, block);
	}

	return NULL;
}

s32 audioInit(void)
{
	sceAudioOutInit(); // "already initialized" is fine

	audioPort = sceAudioOutOpen(PS4_AUDIO_SYSTEM_USER, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
		PS4_AUDIO_GRANULARITY, PS4_AUDIO_OUT_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
	if (audioPort < 0) {
		sysLogPrintf(LOG_ERROR, "PS4: sceAudioOutOpen failed: 0x%08x", (u32)audioPort);
		audioPort = -1;
		return -1;
	}

	nextBuf = NULL;
	ringRead = ringWrite = 0;
	audioRunning = 1;

	pthread_t thr;
	if (pthread_create(&thr, NULL, audioFeeder, NULL) != 0) {
		sysLogPrintf(LOG_ERROR, "PS4: could not start the audio thread");
		audioRunning = 0;
		sceAudioOutClose(audioPort);
		audioPort = -1;
		return -1;
	}
	pthread_detach(thr);

	sysLogPrintf(LOG_NOTE, "PS4: audio: %d Hz stereo resampled to %d Hz", PS4_AUDIO_SRC_RATE, PS4_AUDIO_OUT_RATE);
	return 0;
}

s32 audioGetBytesBuffered(void)
{
	pthread_mutex_lock(&ringLock);
	const u32 n = ringCount();
	pthread_mutex_unlock(&ringLock);
	return (s32)(n * 4);
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize && audioPort >= 0) {
		if (audioGetSamplesBuffered() < queueLimit) {
			const u32 frames = nextSize / 4;
			pthread_mutex_lock(&ringLock);
			const u32 space = PS4_AUDIO_RING_FRAMES - ringCount();
			const u32 n = frames < space ? frames : space;
			for (u32 i = 0; i < n; ++i) {
				const u32 w = ((ringWrite + i) & (PS4_AUDIO_RING_FRAMES - 1)) * 2;
				ring[w + 0] = nextBuf[i * 2 + 0];
				ring[w + 1] = nextBuf[i * 2 + 1];
			}
			ringWrite += n;
			pthread_mutex_unlock(&ringLock);
		}
	}
	nextBuf = NULL;
	nextSize = 0;
}

#else // PLATFORM_PS4

#include <SDL.h>


static SDL_AudioDeviceID dev;
static const s16 *nextBuf;
static u32 nextSize = 0;

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

s32 audioInit(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = bufferSize;
	want.callback = NULL;

	nextBuf = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	SDL_PauseAudioDevice(dev, 0);

	return 0;
}

s32 audioGetBytesBuffered(void)
{
	return SDL_GetQueuedAudioSize(dev);
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			SDL_QueueAudio(dev, nextBuf, nextSize);
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

#endif // PLATFORM_PS4

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
}
