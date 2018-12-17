/**
 * @file audiounit/recorder.c  AudioUnit input recorder
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "audiounit.h"


/* todo: convert from "float" to application format */
/* todo: convert from 44100 Hz to application srate */


struct ausrc_st {
	const struct ausrc *as;      /* inheritance */
	struct audiosess_st *sess;
	AudioUnit au;
	pthread_mutex_t mutex;
	int ch;
	ausrc_read_h *rh;
	void *arg;
	uint32_t sampsz;
	AudioConverterRef conv;
	size_t sampc;            /* application sample count */
	void *tmp;
	void *buf;
};


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	pthread_mutex_lock(&st->mutex);
	st->rh = NULL;
	pthread_mutex_unlock(&st->mutex);

	AudioOutputUnitStop(st->au);
	AudioUnitUninitialize(st->au);
	AudioComponentInstanceDispose(st->au);

	AudioConverterDispose(st->conv);

	mem_deref(st->sess);
	mem_deref(st->buf);

	pthread_mutex_destroy(&st->mutex);
}


static OSStatus input_proc(
		  AudioConverterRef inAudioConverter,
		  UInt32 *ioNumberDataPackets,
		  AudioBufferList *ioData,
		  AudioStreamPacketDescription **outDataPacketDescription,
		  void *inUserData)
{
	struct ausrc_st *st = inUserData;
	size_t sampc;
	size_t bytes;

	sampc = *ioNumberDataPackets * st->ch;
	bytes = *ioNumberDataPackets * st->ch * st->sampsz;

	ioData->mNumberBuffers = 1;
	ioData->mBuffers[0].mNumberChannels = st->ch;
	ioData->mBuffers[0].mData = st->tmp;
	ioData->mBuffers[0].mDataByteSize = (uint32_t)bytes;

	return noErr;
}


static OSStatus input_callback(void *inRefCon,
			       AudioUnitRenderActionFlags *ioActionFlags,
			       const AudioTimeStamp *inTimeStamp,
			       UInt32 inBusNumber,
			       UInt32 inNumberFrames,
			       AudioBufferList *ioData)
{
	struct ausrc_st *st = inRefCon;
	AudioBufferList abl;
	AudioBufferList abl_out;
	OSStatus ret;
	ausrc_read_h *rh;
	void *arg;

	(void)ioData;

	pthread_mutex_lock(&st->mutex);
	rh  = st->rh;
	arg = st->arg;
	pthread_mutex_unlock(&st->mutex);

	if (!rh)
		return 0;

	abl.mNumberBuffers = 1;
	abl.mBuffers[0].mNumberChannels = st->ch;
	abl.mBuffers[0].mData = NULL;
	abl.mBuffers[0].mDataByteSize = inNumberFrames * st->sampsz;

	ret = AudioUnitRender(st->au,
			      ioActionFlags,
			      inTimeStamp,
			      inBusNumber,
			      inNumberFrames,
			      &abl);
	if (ret) {
		warning("audiounit: record: AudioUnitRender error (%d)\n",
			ret);
		return ret;
	}

	/* todo: calculate output sample count */
	size_t sampc_app = inNumberFrames;

	/* for the callback */
	st->tmp = abl.mBuffers[0].mData;

	abl_out.mNumberBuffers = 1;
	abl_out.mBuffers[0].mNumberChannels = st->ch;
	abl_out.mBuffers[0].mData = st->buf;
	abl_out.mBuffers[0].mDataByteSize = (uint32_t)(sampc_app * st->sampsz);

	UInt32 packet_size = (uint32_t)sampc_app / st->ch;

	ret = AudioConverterFillComplexBuffer(st->conv,
					      input_proc,
					      st,
					      &packet_size,
					      &abl_out,
					      0);
	if (ret) {
		warning("audiounit: record:"
			" AudioConverterFillComplexBuffer (%d)\n", ret);
	}

	rh(abl_out.mBuffers[0].mData, sampc_app, arg);

	return 0;
}


static void interrupt_handler(bool interrupted, void *arg)
{
	struct ausrc_st *st = arg;

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


int audiounit_recorder_alloc(struct ausrc_st **stp, const struct ausrc *as,
			     struct media_ctx **ctx,
			     struct ausrc_prm *prm, const char *device,
			     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	AudioStreamBasicDescription fmt;
	AudioUnitElement inputBus = 1;
	AURenderCallbackStruct cb;
	struct ausrc_st *st;
	Float64 hw_srate = 0.0;
	UInt32 hw_size = sizeof(hw_srate);
	OSStatus ret = 0;
	int err;
	uint32_t srate_hw = audiounit_hardware_srate();

	(void)ctx;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm)
		return EINVAL;

	info("audiounit: record: hardware srate %u Hz\n", srate_hw);

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->buf = mem_zalloc(8192, NULL);

	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;
	st->ch = prm->ch;

	st->as  = as;
	st->rh  = rh;
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

	fmt.mSampleRate       = srate_hw;
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
	fmt.mReserved         = 0;

	ret = AudioUnitSetProperty(st->au, kAudioUnitProperty_StreamFormat,
				   kAudioUnitScope_Output, inputBus,
				   &fmt, sizeof(fmt));
	if (ret) {
		warning("set prop: format\n");
		goto out;
	}

	cb.inputProc = input_callback;
	cb.inputProcRefCon = st;
	ret = AudioUnitSetProperty(st->au,
				   kAudioOutputUnitProperty_SetInputCallback,
				   kAudioUnitScope_Global, inputBus,
				   &cb, sizeof(cb));
	if (ret)
		goto out;

	/* NOTE: done after desc */
	ret = AudioUnitInitialize(st->au);
	if (ret) {
		warning("init error (%d)\n", ret);
		goto out;
	}

	/* resample: 44100 -> 48000 */

	AudioStreamBasicDescription fmt_src;
	AudioStreamBasicDescription fmt_dst;

	fmt_src = fmt;
	fmt_dst = fmt;

	fmt_src.mSampleRate = srate_hw;
	fmt_dst.mSampleRate = prm->srate;

	ret = AudioConverterNew(&fmt_src,
				&fmt_dst,
				&st->conv);
	if (ret) {
		warning("audiounit: record: AudioConverter failed (%d)\n",
			ret);
		goto out;
	}

	info("audiounit: record: enable resampler %u -> %u Hz\n",
	     srate_hw, prm->srate);

	ret = AudioOutputUnitStart(st->au);
	if (ret)
		goto out;

	ret = AudioUnitGetProperty(st->au,
				   kAudioUnitProperty_SampleRate,
				   kAudioUnitScope_Input,
				   0,
				   &hw_srate,
				   &hw_size);
	if (ret)
		goto out;

	debug("audiounit: record hardware sample rate is now at %f Hz\n",
	      hw_srate);

 out:
	if (ret) {
		warning("audiounit: record failed: %d (%c%c%c%c)\n", ret,
			ret>>24, ret>>16, ret>>8, ret);
		err = ENODEV;
	}

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
