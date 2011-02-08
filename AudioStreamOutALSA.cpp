/* AudioStreamOutALSA.cpp
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
#include <sys/time.h>

#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

namespace android
{

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    ALSAStreamOps(parent, handle)
{
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    close();
}

uint32_t AudioStreamOutALSA::channels() const
{
    int c = ALSAStreamOps::channels();
    return c;
}

status_t AudioStreamOutALSA::setVolume(float left, float right)
{
    return mixer()->setVolume (mHandle->curDev, left, right);
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    AutoMutex lock(mLock);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

	/* check if handle is still valid, otherwise we are coming out of standby */
	if(mHandle->handle == NULL) {
         nsecs_t previously = systemTime();
	     mHandle->module->open(mHandle, mHandle->curDev, mHandle->curMode);
         nsecs_t delta = systemTime() - previously;
         LOGE("RE-OPEN AFTER STANDBY:: took %llu msecs\n", ns2ms(delta));
	}

    acoustic_device_t *aDev = acoustics();

    // For output, we will pass the data on to the acoustics module, but the actual
    // data is expected to be sent to the audio device directly as well.
    if (aDev && aDev->write)
        aDev->write(aDev, buffer, bytes);

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    do {
        n = snd_pcm_writei(mHandle->handle,
                           (char *)buffer + sent,
                           snd_pcm_bytes_to_frames(mHandle->handle, bytes - sent));
        if (n < 0) {
            if (n == -EBADFD) {
                /* if there is such a problem, re-open the device to recover,
                then return immediately. we should not try to re-send again */
                LOGE("ERROR EBADFD\n");
                mHandle->module->open(mHandle, mHandle->curDev, mHandle->curMode);
                if (aDev && aDev->recover) aDev->recover(aDev, n);
                if (n) return static_cast<ssize_t>(n);
            }
            else if (mHandle->handle) {
                // snd_pcm_recover() will return 0 if successful in recovering from
                // an error, or -errno if the error was unrecoverable.
                if (n == -EPIPE) {
                    /* EPIPE is usually seen while we wait for the standby timer to expire
                    on the last active track, standby timer is currently 3 seconds, so you
                    should only see this during the specific case
                    where we are waiting for standby*/
                    LOGD("INFO: EPIPE\n");
                }
                n = snd_pcm_recover(mHandle->handle, n, 1);


		if (aDev && aDev->recover) aDev->recover(aDev, n);

                if (n) return static_cast<ssize_t>(n);
            }
        }
        else {
            sent += static_cast<ssize_t>(snd_pcm_frames_to_bytes(mHandle->handle, n));
            framesRendered += sent;
        }

    } while (mHandle->handle && sent < bytes);

    return sent;
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::open(int mode)
{
    AutoMutex lock(mLock);
    return ALSAStreamOps::open(mode);
}

status_t AudioStreamOutALSA::close()
{
    AutoMutex lock(mLock);

    snd_pcm_drain (mHandle->handle);
    framesRendered = 0;
    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

status_t AudioStreamOutALSA::standby()
{
    AutoMutex lock(mLock);

    snd_pcm_drain (mHandle->handle);

    /* save state of mHandle->handle so we can re-use it
    after coming out of standby */

    /* no need to save state yet, keep it simple for now
    all state info we need is maintained in mHandle,
    otherwise we will reuse defaults. */

    /* now close it so we can reach off while idle */
    LOGE("CALLING STANDBY\n");
    mHandle->module->close(mHandle);
    framesRendered = 0;
    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
//    return USEC_TO_MSEC (mHandle->latency);

    /* ugly hack, add to the teams technical debt */
    return 20;
}

status_t AudioStreamOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    *dspFrames = framesRendered;
    return NO_ERROR;
}

}       // namespace android
