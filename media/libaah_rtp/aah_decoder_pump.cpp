/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LibAAH_RTP"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <poll.h>
#include <pthread.h>

#include <common_time/cc_helper.h>
#include <media/AudioSystem.h>
#include <media/AudioTrack.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/Utils.h>
#include <utils/Timers.h>
#include <utils/threads.h>

#include "aah_decoder_pump.h"

namespace android {

static const long long kLongDecodeErrorThreshold = 1000000ll;
static const uint32_t kMaxLongErrorsBeforeFatal = 3;
static const uint32_t kMaxErrorsBeforeFatal = 60;

AAH_DecoderPump::AAH_DecoderPump(OMXClient& omx)
    : omx_(omx)
    , thread_status_(OK)
    , renderer_(NULL)
    , last_queued_pts_valid_(false)
    , last_queued_pts_(0)
    , last_ts_transform_valid_(false)
    , last_volume_(0xFF) {
    thread_ = new ThreadWrapper(this);
}

AAH_DecoderPump::~AAH_DecoderPump() {
    shutdown();
}

status_t AAH_DecoderPump::initCheck() {
    if (thread_ == NULL) {
        ALOGE("Failed to allocate thread");
        return NO_MEMORY;
    }

    return OK;
}

status_t AAH_DecoderPump::queueForDecode(MediaBuffer* buf) {
    if (NULL == buf) {
        return BAD_VALUE;
    }

    if (OK != thread_status_) {
        return thread_status_;
    }

    {   // Explicit scope for AutoMutex pattern.
        AutoMutex lock(&thread_lock_);
        in_queue_.push_back(buf);
    }

    thread_cond_.signal();

    return OK;
}

void AAH_DecoderPump::queueToRenderer(MediaBuffer* decoded_sample) {
    Mutex::Autolock lock(&render_lock_);
    sp<MetaData> meta;
    int64_t ts;
    status_t res;

    // Fetch the metadata and make sure the sample has a timestamp.  We
    // cannot render samples which are missing PTSs.
    meta = decoded_sample->meta_data();
    if ((meta == NULL) || (!meta->findInt64(kKeyTime, &ts))) {
        ALOGV("Decoded sample missing timestamp, cannot render.");
        CHECK(false);
    } else {
        // If we currently are not holding on to a renderer, go ahead and
        // make one now.
        if (NULL == renderer_) {
            renderer_ = new TimedAudioTrack();
            if (NULL != renderer_) {
                int frameCount;
                AudioTrack::getMinFrameCount(&frameCount,
                        AUDIO_STREAM_DEFAULT,
                        static_cast<int>(format_sample_rate_));
                audio_channel_mask_t ch_format =
                        audio_channel_out_mask_from_count(format_channels_);

                res = renderer_->set(AUDIO_STREAM_DEFAULT,
                        format_sample_rate_,
                        AUDIO_FORMAT_PCM_16_BIT,
                        ch_format,
                        frameCount);
                if (res != OK) {
                    ALOGE("Failed to setup audio renderer. (res = %d)", res);
                    delete renderer_;
                    renderer_ = NULL;
                } else {
                    CHECK(last_ts_transform_valid_);

                    res = renderer_->setMediaTimeTransform(
                            last_ts_transform_, TimedAudioTrack::COMMON_TIME);
                    if (res != NO_ERROR) {
                        ALOGE("Failed to set media time transform on AudioTrack"
                              " (res = %d)", res);
                        delete renderer_;
                        renderer_ = NULL;
                    } else {
                        float volume = static_cast<float>(last_volume_)
                                     / 255.0f;
                        if (renderer_->setVolume(volume, volume) != OK) {
                            ALOGW("%s: setVolume failed", __FUNCTION__);
                        }

                        renderer_->start();
                    }
                }
            } else {
                ALOGE("Failed to allocate AudioTrack to use as a renderer.");
            }
        }

        if (NULL != renderer_) {
            uint8_t* decoded_data =
                reinterpret_cast<uint8_t*>(decoded_sample->data());
            uint32_t decoded_amt  = decoded_sample->range_length();
            decoded_data += decoded_sample->range_offset();

            sp<IMemory> pcm_payload;
            res = renderer_->allocateTimedBuffer(decoded_amt, &pcm_payload);
            if (res != OK) {
                ALOGE("Failed to allocate %d byte audio track buffer."
                      " (res = %d)", decoded_amt, res);
            } else {
                memcpy(pcm_payload->pointer(), decoded_data, decoded_amt);

                res = renderer_->queueTimedBuffer(pcm_payload, ts);
                if (res != OK) {
                    ALOGE("Failed to queue %d byte audio track buffer with"
                          " media PTS %lld. (res = %d)", decoded_amt, ts, res);
                } else {
                    last_queued_pts_valid_ = true;
                    last_queued_pts_ = ts;
                }
            }

        } else {
            ALOGE("No renderer, dropping audio payload.");
        }
    }
}

void AAH_DecoderPump::stopAndCleanupRenderer() {
    if (NULL == renderer_) {
        return;
    }

    renderer_->stop();
    delete renderer_;
    renderer_ = NULL;
}

void AAH_DecoderPump::setRenderTSTransform(const LinearTransform& trans) {
    Mutex::Autolock lock(&render_lock_);

    if (last_ts_transform_valid_ && !memcmp(&trans,
                                            &last_ts_transform_,
                                            sizeof(trans))) {
        return;
    }

    last_ts_transform_       = trans;
    last_ts_transform_valid_ = true;

    if (NULL != renderer_) {
        status_t res = renderer_->setMediaTimeTransform(
                last_ts_transform_, TimedAudioTrack::COMMON_TIME);
        if (res != NO_ERROR) {
            ALOGE("Failed to set media time transform on AudioTrack"
                  " (res = %d)", res);
        }
    }
}

void AAH_DecoderPump::setRenderVolume(uint8_t volume) {
    Mutex::Autolock lock(&render_lock_);

    if (volume == last_volume_) {
        return;
    }

    last_volume_ = volume;
    if (renderer_ != NULL) {
        float volume = static_cast<float>(last_volume_) / 255.0f;
        if (renderer_->setVolume(volume, volume) != OK) {
            ALOGW("%s: setVolume failed", __FUNCTION__);
        }
    }
}

// isAboutToUnderflow is something of a hack used to figure out when it might be
// time to give up on trying to fill in a gap in the RTP sequence and simply
// move on with a discontinuity.  If we had perfect knowledge of when we were
// going to underflow, it would not be a hack, but unfortunately we do not.
// Right now, we just take the PTS of the last sample queued, and check to see
// if its presentation time is within kAboutToUnderflowThreshold from now.  If
// it is, then we say that we are about to underflow.  This decision is based on
// two (possibly invalid) assumptions.
//
// 1) The transmitter is leading the clock by more than
//    kAboutToUnderflowThreshold.
// 2) The delta between the PTS of the last sample queued and the next sample
//    is less than the transmitter's clock lead amount.
//
// Right now, the default transmitter lead time is 1 second, which is a pretty
// large number and greater than the 50mSec that kAboutToUnderflowThreshold is
// currently set to.  This should satisfy assumption #1 for now, but changes to
// the transmitter clock lead time could effect this.
//
// For non-sparse streams with a homogeneous sample rate (the vast majority of
// streams in the world), the delta between any two adjacent PTSs will always be
// the homogeneous sample period.  It is very uncommon to see a sample period
// greater than the 1 second clock lead we are currently using, and you
// certainly will not see it in an MP3 file which should satisfy assumption #2.
// Sparse audio streams (where no audio is transmitted for long periods of
// silence) and extremely low framerate video stream (like an MPEG-2 slideshow
// or the video stream for a pay TV audio channel) are examples of streams which
// might violate assumption #2.
bool AAH_DecoderPump::isAboutToUnderflow(int64_t threshold) {
    Mutex::Autolock lock(&render_lock_);

    // If we have never queued anything to the decoder, we really don't know if
    // we are going to underflow or not.
    if (!last_queued_pts_valid_ || !last_ts_transform_valid_) {
        return false;
    }

    // Don't have access to Common Time?  If so, then things are Very Bad
    // elsewhere in the system; it pretty much does not matter what we do here.
    // Since we cannot really tell if we are about to underflow or not, its
    // probably best to assume that we are not and proceed accordingly.
    int64_t tt_now;
    if (OK != cc_helper_.getCommonTime(&tt_now)) {
        return false;
    }

    // Transform from media time to common time.
    int64_t last_queued_pts_tt;
    if (!last_ts_transform_.doForwardTransform(last_queued_pts_,
                &last_queued_pts_tt)) {
        return false;
    }

    // Check to see if we are underflowing.
    return ((tt_now + threshold - last_queued_pts_tt) > 0);
}

void* AAH_DecoderPump::workThread() {
    // No need to lock when accessing decoder_ from the thread.  The
    // implementation of init and shutdown ensure that other threads never touch
    // decoder_ while the work thread is running.
    CHECK(decoder_ != NULL);
    CHECK(format_  != NULL);

    // Start the decoder and note its result code.  If something goes horribly
    // wrong, callers of queueForDecode and getOutput will be able to detect
    // that the thread encountered a fatal error and shut down by examining
    // thread_status_.
    thread_status_ = decoder_->start(format_.get());
    if (OK != thread_status_) {
        ALOGE("AAH_DecoderPump's work thread failed to start decoder"
              " (res = %d)", thread_status_);
        return NULL;
    }

    DurationTimer decode_timer;
    uint32_t consecutive_long_errors = 0;
    uint32_t consecutive_errors = 0;

    while (!thread_->exitPending()) {
        status_t res;
        MediaBuffer* bufOut = NULL;

        decode_timer.start();
        res = decoder_->read(&bufOut);
        decode_timer.stop();

        if (res == INFO_FORMAT_CHANGED) {
            // Format has changed.  Destroy our current renderer so that a new
            // one can be created during queueToRenderer with the proper format.
            //
            // TODO : In order to transition seamlessly, we should change this
            // to put the old renderer in a queue to play out completely before
            // we destroy it.  We can still create a new renderer, the timed
            // nature of the renderer should ensure a seamless splice.
            stopAndCleanupRenderer();
            res = OK;
        }

        // Try to be a little nuanced in our handling of actual decode errors.
        // Errors could happen because of minor stream corruption or because of
        // transient resource limitations.  In these cases, we would rather drop
        // a little bit of output and ride out the unpleasantness then throw up
        // our hands and abort everything.
        //
        // OTOH - When things are really bad (like we have a non-transient
        // resource or bookkeeping issue, or the stream being fed to us is just
        // complete and total garbage) we really want to terminate playback and
        // raise an error condition all the way up to the application level so
        // they can deal with it.
        //
        // Unfortunately, the error codes returned by the decoder can be a
        // little non-specific.  For example, if an OMXCodec times out
        // attempting to obtain an output buffer, the error we get back is a
        // generic -1.  Try to distinguish between this resource timeout error
        // and ES corruption error by timing how long the decode operation
        // takes.  Maintain accounting for both errors and "long errors".  If we
        // get more than a certain number consecutive errors of either type,
        // consider it fatal and shutdown (which will cause the error to
        // propagate all of the way up to the application level).  The threshold
        // for "long errors" is deliberately much lower than that of normal
        // decode errors, both because of how long they take to happen and
        // because they generally indicate resource limitation errors which are
        // unlikely to go away in pathologically bad cases (in contrast to
        // stream corruption errors which might happen 20 times in a row and
        // then be suddenly OK again)
        if (res != OK) {
            consecutive_errors++;
            if (decode_timer.durationUsecs() >= kLongDecodeErrorThreshold)
                consecutive_long_errors++;

            CHECK(NULL == bufOut);

            ALOGW("%s: Failed to decode data (res = %d)",
                    __PRETTY_FUNCTION__, res);

            if ((consecutive_errors      >= kMaxErrorsBeforeFatal) ||
                (consecutive_long_errors >= kMaxLongErrorsBeforeFatal)) {
                ALOGE("%s: Maximum decode error threshold has been reached."
                      " There have been %d consecutive decode errors, and %d"
                      " consecutive decode operations which resulted in errors"
                      " and took more than %lld uSec to process.  The last"
                      " decode operation took %lld uSec.",
                      __PRETTY_FUNCTION__,
                      consecutive_errors, consecutive_long_errors,
                      kLongDecodeErrorThreshold, decode_timer.durationUsecs());
                thread_status_ = res;
                break;
            }

            continue;
        }

        if (NULL == bufOut) {
            ALOGW("%s: Successful decode, but no buffer produced",
                    __PRETTY_FUNCTION__);
            continue;
        }

        // Successful decode (with actual output produced).  Clear the error
        // counters.
        consecutive_errors = 0;
        consecutive_long_errors = 0;

        queueToRenderer(bufOut);
        bufOut->release();
    }

    decoder_->stop();
    stopAndCleanupRenderer();

    return NULL;
}

status_t AAH_DecoderPump::init(const sp<MetaData>& params) {
    Mutex::Autolock lock(&init_lock_);

    if (decoder_ != NULL) {
        // already inited
        return OK;
    }

    if (params == NULL) {
        return BAD_VALUE;
    }

    if (!params->findInt32(kKeyChannelCount, &format_channels_)) {
        return BAD_VALUE;
    }

    if (!params->findInt32(kKeySampleRate, &format_sample_rate_)) {
        return BAD_VALUE;
    }

    CHECK(OK == thread_status_);
    CHECK(decoder_ == NULL);

    status_t ret_val = UNKNOWN_ERROR;

    // Cache the format and attempt to create the decoder.
    format_  = params;
    decoder_ = OMXCodec::Create(
            omx_.interface(),       // IOMX Handle
            format_,                // Metadata for substream (indicates codec)
            false,                  // Make a decoder, not an encoder
            sp<MediaSource>(this)); // We will be the source for this codec.

    if (decoder_ == NULL) {
      ALOGE("Failed to allocate decoder in %s", __PRETTY_FUNCTION__);
      goto bailout;
    }

    // Fire up the pump thread.  It will take care of starting and stopping the
    // decoder.
    ret_val = thread_->run("aah_decode_pump", ANDROID_PRIORITY_AUDIO);
    if (OK != ret_val) {
        ALOGE("Failed to start work thread in %s (res = %d)",
                __PRETTY_FUNCTION__, ret_val);
        goto bailout;
    }

bailout:
    if (OK != ret_val) {
        decoder_ = NULL;
        format_  = NULL;
    }

    return OK;
}

status_t AAH_DecoderPump::shutdown() {
    Mutex::Autolock lock(&init_lock_);
    return shutdown_l();
}

status_t AAH_DecoderPump::shutdown_l() {
    thread_->requestExit();
    thread_cond_.signal();
    thread_->requestExitAndWait();

    for (MBQueue::iterator iter = in_queue_.begin();
         iter != in_queue_.end();
         ++iter) {
        (*iter)->release();
    }
    in_queue_.clear();

    last_queued_pts_valid_   = false;
    last_ts_transform_valid_ = false;
    last_volume_             = 0xFF;
    thread_status_           = OK;

    decoder_ = NULL;
    format_  = NULL;

    return OK;
}

status_t AAH_DecoderPump::read(MediaBuffer **buffer,
                               const ReadOptions *options) {
    if (!buffer) {
        return BAD_VALUE;
    }

    *buffer = NULL;

    // While its not time to shut down, and we have no data to process, wait.
    AutoMutex lock(&thread_lock_);
    while (!thread_->exitPending() && in_queue_.empty())
        thread_cond_.wait(thread_lock_);

    // At this point, if its not time to shutdown then we must have something to
    // process.  Go ahead and pop the front of the queue for processing.
    if (!thread_->exitPending()) {
        CHECK(!in_queue_.empty());

        *buffer = *(in_queue_.begin());
        in_queue_.erase(in_queue_.begin());
    }

    // If we managed to get a buffer, then everything must be OK.  If not, then
    // we must be shutting down.
    return (NULL == *buffer) ? INVALID_OPERATION : OK;
}

AAH_DecoderPump::ThreadWrapper::ThreadWrapper(AAH_DecoderPump* owner)
    : Thread(false /* canCallJava*/ )
    , owner_(owner) {
}

bool AAH_DecoderPump::ThreadWrapper::threadLoop() {
    CHECK(NULL != owner_);
    owner_->workThread();
    return false;
}

}  // namespace android
