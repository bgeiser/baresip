/**
 * @file audiounit/player.c  AudioUnit output player
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "audiounit.h"


/* TODO: convert from app sample format to float */


struct auplay_st {
	const struct auplay *ap;      /* inheritance */
	struct audiosess_st *sess;
	AudioUnit au;
	pthread_mutex_t mutex;
	auplay_write_h *wh;
	void *arg;
	uint32_t sampsz;
	AudioConverterRef conv;

	uint8_t ch;
	void *tmp;
	size_t tmp_sampc;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	pthread_mutex_lock(&st->mutex);
	st->wh = NULL;
	pthread_mutex_unlock(&st->mutex);

	AudioOutputUnitStop(st->au);
	AudioUnitUninitialize(st->au);
	AudioComponentInstanceDispose(st->au);

	AudioConverterDispose(st->conv);

	mem_deref(st->sess);
	mem_deref(st->tmp);

	pthread_mutex_destroy(&st->mutex);
}


static OSStatus input_proc(
		  AudioConverterRef inAudioConverter,
		  UInt32 *ioNumberDataPackets,
		  AudioBufferList *ioData,
		  AudioStreamPacketDescription **outDataPacketDescription,
		  void *inUserData)
{
	struct auplay_st *st = inUserData;
	auplay_write_h *wh;
	void *arg;
	size_t sampc;
	size_t bytes;

	sampc = *ioNumberDataPackets * st->ch;
	bytes = *ioNumberDataPackets * st->ch * st->sampsz;

	wh  = st->wh;
	arg = st->arg;

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = st->ch;
	ioData->mBuffers[0].mData = st->tmp;
	ioData->mBuffers[0].mDataByteSize = (uint32_t)bytes;

	wh(st->tmp, sampc, arg);

	return noErr;
}


static OSStatus output_callback(void *inRefCon,
				AudioUnitRenderActionFlags *ioActionFlags,
				const AudioTimeStamp *inTimeStamp,
				UInt32 inBusNumber,
				UInt32 inNumberFrames,
				AudioBufferList *ioData)
{
	struct auplay_st *st = inRefCon;
	auplay_write_h *wh;
	void *arg;
	int ret;
	size_t sampc;

	(void)ioActionFlags;
	(void)inTimeStamp;
	(void)inBusNumber;
	(void)inNumberFrames;

	pthread_mutex_lock(&st->mutex);
	wh  = st->wh;
	arg = st->arg;
	pthread_mutex_unlock(&st->mutex);

	if (!wh)
		return 0;

	AudioBuffer *ab = &ioData->mBuffers[0];
	sampc = ab->mDataByteSize / st->sampsz;

	UInt32 packet_size = (uint32_t)sampc / st->ch;

	ret = AudioConverterFillComplexBuffer(st->conv,
					      input_proc,
					      st,
					      &packet_size,
					      ioData,
					      0);
	if (ret) {
		warning("audiounit: player:"
			" AudioConverterFillComplexBuffer (%d)\n", ret);
	}

#if 0
	if (packet_size != sampc) {
		warning("packet size changed: %zu -> %u\n",
			sampc, packet_size);
	}
#endif

	return 0;
}


static void interrupt_handler(bool interrupted, void *arg)
{
	struct auplay_st *st = arg;

	if (interrupted)
		AudioOutputUnitStop(st->au);
	else
		AudioOutputUnitStart(st->au);
}


static uint32_t aufmt_to_formatflags(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE:  return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_FLOAT:  return kLinearPCMFormatFlagIsFloat;
	default: return 0;
	}
}


int audiounit_player_alloc(struct auplay_st **stp, const struct auplay *ap,
			   struct auplay_prm *prm, const char *device,
			   auplay_write_h *wh, void *arg)
{
	AudioStreamBasicDescription fmt;
	AudioUnitElement outputBus = 0;
	AURenderCallbackStruct cb;
	struct auplay_st *st;
	OSStatus ret = 0;
	Float64 hw_srate = 0.0;
	UInt32 hw_size = sizeof(hw_srate);
	int err;

	(void)device;

	if (!stp || !ap || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->ap  = ap;
	st->wh  = wh;
	st->arg = arg;
	st->ch  = prm->ch;

	err = pthread_mutex_init(&st->mutex, NULL);
	if (err)
		goto out;

	err = audiosess_alloc(&st->sess, interrupt_handler, st);
	if (err)
		goto out;

	ret = AudioComponentInstanceNew(audiounit_comp, &st->au);
	if (ret)
		goto out;

	st->sampsz = (uint32_t)aufmt_sample_size(prm->fmt);

	st->tmp_sampc = prm->srate * prm->ch * prm->ptime / 1000;

	st->tmp = mem_zalloc(st->tmp_sampc * st->sampsz, NULL);

	fmt.mSampleRate       = 44100;
	fmt.mFormatID         = kAudioFormatLinearPCM;
#if TARGET_OS_IPHONE
	fmt.mFormatFlags      = aufmt_to_formatflags(prm->fmt)
		| kAudioFormatFlagsNativeEndian
		| kAudioFormatFlagIsPacked;
#else
	fmt.mFormatFlags      = aufmt_to_formatflags(prm->fmt)
		| kAudioFormatFlagsNativeEndian
		| kAudioFormatFlagIsPacked;
#endif
	fmt.mBitsPerChannel   = 8 * st->sampsz;
	fmt.mChannelsPerFrame = prm->ch;
	fmt.mBytesPerFrame    = st->sampsz * prm->ch;
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerPacket   = st->sampsz * prm->ch;

	ret = AudioUnitSetProperty(st->au, kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Input, outputBus,
				   &fmt, sizeof(fmt));
	if (ret) {
		warning("audiounit: player: format failed (%d)\n", ret);
		goto out;
	}

	/* after setting format */
	ret = AudioUnitInitialize(st->au);
	if (ret) {
		warning("audiounit: player: Initialize failed (%d)\n", ret);
		goto out;
	}

	cb.inputProc = output_callback;
	cb.inputProcRefCon = st;
	ret = AudioUnitSetProperty(st->au,
				   kAudioUnitProperty_SetRenderCallback,
				   kAudioUnitScope_Input, outputBus,
				   &cb, sizeof(cb));
	if (ret)
		goto out;

	ret = AudioOutputUnitStart(st->au);
	if (ret) {
		warning("audiounit: player: Start failed (%d)\n", ret);
		goto out;
	}

	ret = AudioUnitGetProperty(st->au,
				   kAudioUnitProperty_SampleRate,
				   kAudioUnitScope_Output,
				   0,
				   &hw_srate,
				   &hw_size);
	if (ret)
		goto out;

	debug("audiounit: player hardware sample rate is now at %f Hz\n",
	      hw_srate);

	/* resample: 48000 -> 44100 */

	AudioStreamBasicDescription fmt_src;
	AudioStreamBasicDescription fmt_dst;

	fmt_src = fmt;
	fmt_dst = fmt;

	fmt_src.mSampleRate = prm->srate;
	fmt_dst.mSampleRate = 44100;

	ret = AudioConverterNew(&fmt_src,
				&fmt_dst,
				&st->conv);
	if (ret) {
		warning("audiounit: player: AudioConverter failed (%d)\n",
			ret);
		goto out;
	}

 out:
	if (ret) {
		warning("audiounit: player failed: %d (%c%c%c%c)\n", ret,
			ret>>24, ret>>16, ret>>8, ret);
		err = ENODEV;
	}

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
