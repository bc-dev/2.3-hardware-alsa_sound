/* ALSAMixer.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#define SND_MIXER_VOL_RANGE_MIN  (0)
#define SND_MIXER_VOL_RANGE_MAX  (100)

#define ALSA_NAME_MAX 128

#define ALSA_STRCAT(x,y) \
    if (strlen(x) + strlen(y) < ALSA_NAME_MAX) \
        strcat(x, y);

namespace android
{

// ----------------------------------------------------------------------------

struct mixer_info_t;

struct alsa_properties_t
{
    const AudioSystem::audio_devices device;
    const char         *propName;
    const char         *propDefault;
    mixer_info_t       *mInfo;
};

#define ALSA_PROP(dev, name, out, in) \
    {\
        {dev, "alsa.mixer.playback." name, out, NULL},\
        {dev, "alsa.mixer.capture." name, in, NULL}\
    }

static alsa_properties_t
mixerMasterProp[SND_PCM_STREAM_LAST+1] =
        ALSA_PROP(AudioSystem::DEVICE_OUT_ALL, "master", "PCM", "Capture");

static alsa_properties_t
mixerProp[][SND_PCM_STREAM_LAST+1] = {
    ALSA_PROP(AudioSystem::DEVICE_OUT_EARPIECE, "earpiece", "Earpiece", "Analog"),
    ALSA_PROP(AudioSystem::DEVICE_OUT_SPEAKER, "speaker", "Speaker",  ""),
    ALSA_PROP(AudioSystem::DEVICE_OUT_WIRED_HEADSET, "headset", "Headset", "Analog"),
    ALSA_PROP(AudioSystem::DEVICE_OUT_BLUETOOTH_SCO, "bluetooth.sco", "Bluetooth", "Bluetooth Capture"),
    ALSA_PROP(AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP, "bluetooth.a2dp", "Bluetooth A2DP", "Bluetooth A2DP Capture"),
//    ALSA_PROP(AudioSystem::DEVICE_OUT_FM_HEADPHONE, "fm", "FM", ""),
    ALSA_PROP(static_cast<AudioSystem::audio_devices>(0), "", NULL, NULL)
};

struct mixer_info_t
{
    mixer_info_t() :
        elem(0),
        min(SND_MIXER_VOL_RANGE_MIN),
        max(SND_MIXER_VOL_RANGE_MAX),
        mute(false)
    {
    }

    snd_mixer_elem_t *elem;
    long              min;
    long              max;
    long              volume;
    bool              mute;
    char              name[ALSA_NAME_MAX];
};

#ifdef AUDIO_MODEM_TI
struct mixer_incall_vol_info_t
{
    unsigned int      min;
    unsigned int      max;
    unsigned int      volume;
    char              name[ALSA_NAME_MAX];
};

struct alsa_incall_vol_properties_t
{
    const AudioSystem::audio_devices device;
    const char                       *propName;
    const char                       *propDefault;
    mixer_incall_vol_info_t          *mInfo;
};


#define ALSA_INCALL_VOLUME_PROP(dev, name, default) \
    {\
        dev, "alsa.incallvolume." name, default, NULL\
    }

static alsa_incall_vol_properties_t
inCallVolumeProp[] = {
    ALSA_INCALL_VOLUME_PROP(AudioSystem::DEVICE_OUT_EARPIECE,
                            "earpiece", "DAC Voice Digital Downlink Volume"),
    ALSA_INCALL_VOLUME_PROP(AudioSystem::DEVICE_OUT_SPEAKER,
                            "speaker", "DAC Voice Digital Downlink Volume"),
    ALSA_INCALL_VOLUME_PROP(AudioSystem::DEVICE_OUT_WIRED_HEADSET,
                            "headset", "DAC Voice Digital Downlink Volume"),
    ALSA_INCALL_VOLUME_PROP(AudioSystem::DEVICE_OUT_BLUETOOTH_SCO,
                            "bluetooth.sco", "BT Digital Playback Volume"),
    ALSA_INCALL_VOLUME_PROP(static_cast<AudioSystem::audio_devices>(0),
                            "", "")
};
#endif

static int initMixer (snd_mixer_t **mixer, const char *name)
{
    int err;

    if ((err = snd_mixer_open(mixer, 0)) < 0) {
        LOGE("Unable to open mixer: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(*mixer, name)) < 0) {
        LOGW("Unable to attach mixer to device %s: %s",
            name, snd_strerror(err));

        if ((err = snd_mixer_attach(*mixer, "hw:00")) < 0) {
            LOGE("Unable to attach mixer to device default: %s",
                snd_strerror(err));

            snd_mixer_close (*mixer);
            *mixer = NULL;
            return err;
        }
    }

    if ((err = snd_mixer_selem_register(*mixer, NULL, NULL)) < 0) {
        LOGE("Unable to register mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    // Get the mixer controls from the kernel
    if ((err = snd_mixer_load(*mixer)) < 0) {
        LOGE("Unable to load mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    return 0;
}

typedef int (*hasVolume_t)(snd_mixer_elem_t*);

static const hasVolume_t hasVolume[] = {
    snd_mixer_selem_has_playback_volume,
    snd_mixer_selem_has_capture_volume
};

typedef int (*getVolumeRange_t)(snd_mixer_elem_t*, long int*, long int*);

static const getVolumeRange_t getVolumeRange[] = {
    snd_mixer_selem_get_playback_volume_range,
    snd_mixer_selem_get_capture_volume_range
};

typedef int (*setVolume_t)(snd_mixer_elem_t*, long int);

static const setVolume_t setVol[] = {
    snd_mixer_selem_set_playback_volume_all,
    snd_mixer_selem_set_capture_volume_all
};

ALSAMixer::ALSAMixer()
{
    int err;

    initMixer (&mMixer[SND_PCM_STREAM_PLAYBACK], "AndroidPlayback");
    initMixer (&mMixer[SND_PCM_STREAM_CAPTURE], "AndroidCapture");

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {

        mixer_info_t *info = mixerMasterProp[i].mInfo = new mixer_info_t;

        property_get (mixerMasterProp[i].propName,
                      info->name,
                      mixerMasterProp[i].propDefault);

        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
             elem;
             elem = snd_mixer_elem_next(elem)) {

            if (!snd_mixer_selem_is_active(elem))
                continue;

            snd_mixer_selem_get_id(elem, sid);

            // Find PCM playback volume control element.
            const char *elementName = snd_mixer_selem_id_get_name(sid);

            if (info->elem == NULL &&
                strcmp(elementName, info->name) == 0 &&
                hasVolume[i] (elem)) {

                info->elem = elem;
                getVolumeRange[i] (elem, &info->min, &info->max);
                info->volume = info->max;
                setVol[i] (elem, info->volume);
                if (i == SND_PCM_STREAM_PLAYBACK &&
                    snd_mixer_selem_has_playback_switch (elem))
                    snd_mixer_selem_set_playback_switch_all (elem, 1);
                break;
            }
        }

        LOGV("Mixer: master '%s' %s.", info->name, info->elem ? "found" : "not found");

        for (int j = 0; mixerProp[j][i].device; j++) {

            mixer_info_t *info = mixerProp[j][i].mInfo = new mixer_info_t;

            property_get (mixerProp[j][i].propName,
                          info->name,
                          mixerProp[j][i].propDefault);

            for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
                 elem;
                 elem = snd_mixer_elem_next(elem)) {

                if (!snd_mixer_selem_is_active(elem))
                    continue;

                snd_mixer_selem_get_id(elem, sid);

                // Find PCM playback volume control element.
                const char *elementName = snd_mixer_selem_id_get_name(sid);

               if (info->elem == NULL &&
                    strcmp(elementName, info->name) == 0 &&
                    hasVolume[i] (elem)) {

                    info->elem = elem;
                    getVolumeRange[i] (elem, &info->min, &info->max);
                    info->volume = info->max;
                    setVol[i] (elem, info->volume);
                    if (i == SND_PCM_STREAM_PLAYBACK &&
                        snd_mixer_selem_has_playback_switch (elem))
                        snd_mixer_selem_set_playback_switch_all (elem, 1);
                    break;
                }
            }
            LOGV("Mixer: route '%s' %s.", info->name, info->elem ? "found" : "not found");
        }
    }
#ifdef AUDIO_MODEM_TI
    ALSAControl control("hw:00");
    status_t error;
    for (int i = 0; inCallVolumeProp[i].device; i++) {
        mixer_incall_vol_info_t *info = inCallVolumeProp[i].mInfo = new mixer_incall_vol_info_t;

        property_get (inCallVolumeProp[i].propName,
                      info->name,
                      inCallVolumeProp[i].propDefault);

        error = control.get(info->name, info->volume, 0);
        error = control.getmin(info->name, info->min);
        error = control.getmax(info->name, info->max);

        LOGV("Mixer: In Call Volume '%s' %s vol. %d min. %d max. %d",
             info->name, (error < 0) ? "not found" : "found %s vol. %d min. %d max. %d",
             info->volume, info->min, info->max);
    }
#endif
    LOGV("mixer initialized.");
}

ALSAMixer::~ALSAMixer()
{
    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {
        if (mMixer[i]) snd_mixer_close (mMixer[i]);
        if (mixerMasterProp[i].mInfo) {
            delete mixerMasterProp[i].mInfo;
            mixerMasterProp[i].mInfo = NULL;
        }
        for (int j = 0; mixerProp[j][i].device; j++) {
            if (mixerProp[j][i].mInfo) {
                delete mixerProp[j][i].mInfo;
                mixerProp[j][i].mInfo = NULL;
            }
        }
    }
    LOGV("mixer destroyed.");
}

status_t ALSAMixer::setMasterVolume(float volume)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_PLAYBACK].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + volume * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_playback_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setMasterGain(float gain)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_CAPTURE].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + gain * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_capture_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setVolume(uint32_t device, float left, float right)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + left * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_playback_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setGain(uint32_t device, float gain)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + gain * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_capture_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setCaptureMuteState(uint32_t device, bool state)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_capture_switch (info->elem)) {

                int err = snd_mixer_selem_set_capture_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s capture mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            } else {
                //is capture element has volume else return failure
                if(!snd_mixer_selem_has_capture_volume(info->elem)) {
                    LOGE("Element: %s doesn't have volume control",info->name);
                    return INVALID_OPERATION;
                } else {
                    //Is all the channels are joined
                    if(snd_mixer_selem_has_capture_volume_joined(info->elem)) {
                        //change all channels volume to 0
                        if (state) {
                            // muting
                            snd_mixer_selem_set_capture_volume_all(info->elem, 0);
                        } else {
                            //unmuting
                            snd_mixer_selem_set_capture_volume_all(info->elem, info->volume);
                        }
                    } else {
                        //snd_mixer_selem_channel_id_t chan_id;
                        for(int chan_id = SND_MIXER_SCHN_FRONT_LEFT;
                                chan_id <= SND_MIXER_SCHN_LAST;
                                chan_id++) {
                            // check channel is present
                            if(snd_mixer_selem_has_capture_channel(info->elem,
                                        (snd_mixer_selem_channel_id_t)chan_id)) {
                                if (state) {
                                    //make the volume to 0, incase of mute
                                    snd_mixer_selem_set_capture_volume(info->elem,
                                            (snd_mixer_selem_channel_id_t)chan_id, 0);
                                } else {
                                    //restore the volume incase of unmute
                                    snd_mixer_selem_set_capture_volume(info->elem,
                                            (snd_mixer_selem_channel_id_t)chan_id, info->volume);
                                }
                            }
                        }
                    }
                }
            }
            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getCaptureMuteState(uint32_t device, bool *state)
{
    if (!state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

status_t ALSAMixer::setPlaybackMuteState(uint32_t device, bool state)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_playback_switch (info->elem)) {

                int err = snd_mixer_selem_set_playback_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s playback mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            }

            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getPlaybackMuteState(uint32_t device, bool *state)
{
    if (!state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].device; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].device & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

#ifdef AUDIO_MODEM_TI
status_t ALSAMixer::setVoiceVolume(float volume)
{
    status_t error = NO_ERROR;
    mixer_incall_vol_info_t *info = NULL;

    ALSAControl control("hw:00");

    for (int j = 0; inCallVolumeProp[j].device; j++) {

        info = inCallVolumeProp[j].mInfo;

        // Make sure volume is between bounds.
        info->volume = info->min + volume * (info->max - info->min);
        if (info->volume > info->max) info->volume = info->max;
        if (info->volume < info->min) info->volume = info->min;

        LOGV("%s: in call volume level to apply: %d", info->name, info->volume);

        error = control.set(info->name, info->volume, 0);
        if (error < 0) {
            LOGE("%s: error applying in call volume: %d", info->name, info->volume);
            return error;
        }
    }

    return NO_ERROR;

}
#endif
};        // namespace android
