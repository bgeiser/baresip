/**
 * @file audiounit.c  AudioUnit sound driver
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <re.h>
#include <baresip.h>
#include "audiounit.h"


/**
 * @defgroup audiounit audiounit
 *
 * Audio driver module for OSX/iOS AudioUnit
 */


AudioComponent audiounit_comp = NULL;

static struct auplay *auplay;
static struct ausrc *ausrc;


#if TARGET_OS_IPHONE
static void interruptionListener(void *data, UInt32 inInterruptionState)
{
	(void)data;

	if (inInterruptionState == kAudioSessionBeginInterruption) {
		info("audiounit: interrupt Begin\n");
		audiosess_interrupt(true);
	}
	else if (inInterruptionState == kAudioSessionEndInterruption) {
		info("audiounit: interrupt End\n");
		audiosess_interrupt(false);
	}
}
#endif


uint32_t audiounit_hardware_srate(void)
{
	AudioDeviceID device_id = kAudioObjectUnknown;
	UInt32 info_size = sizeof(device_id);
	AudioObjectPropertyAddress default_input_device_address = {
		kAudioHardwarePropertyDefaultInputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	OSStatus result = AudioObjectGetPropertyData(kAudioObjectSystemObject,
					     &default_input_device_address, 0,
					     0, &info_size, &device_id);
	if (result != noErr)
		return 0;

	Float64 nominal_sample_rate;
	info_size = sizeof(nominal_sample_rate);

	AudioObjectPropertyAddress nominal_sample_rate_address = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	result = AudioObjectGetPropertyData(device_id,
					    &nominal_sample_rate_address,
					    0, 0,
					    &info_size, &nominal_sample_rate);
	if (result != noErr)
		return 0;

	return nominal_sample_rate;
}


static int module_init(void)
{
	AudioComponentDescription desc;
	CFStringRef name = NULL;
	int err;

#if TARGET_OS_IPHONE
	OSStatus ret;

	ret = AudioSessionInitialize(NULL, NULL, interruptionListener, 0);
	if (ret && ret != kAudioSessionAlreadyInitialized) {
		warning("audiounit: AudioSessionInitialize: %d\n", ret);
		return ENODEV;
	}
#endif

	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	audiounit_comp = AudioComponentFindNext(NULL, &desc);
	if (!audiounit_comp) {
		warning("audiounit: Voice Processing I/O not found\n");
		return ENOENT;
	}

	if (0 == AudioComponentCopyName(audiounit_comp, &name)) {
		info("audiounit: using component '%s'\n",
		     CFStringGetCStringPtr(name, kCFStringEncodingUTF8));
	}

	err  = auplay_register(&auplay, baresip_auplayl(),
			       "audiounit", audiounit_player_alloc);
	err |= ausrc_register(&ausrc, baresip_ausrcl(),
			      "audiounit", audiounit_recorder_alloc);

	return 0;
}


static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(audiounit) = {
	"audiounit",
	"audio",
	module_init,
	module_close,
};
