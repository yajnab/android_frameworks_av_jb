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

#include <include/avc_utils.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/Utils.h>

#include "aah_rx_player.h"
#include "aah_tx_packet.h"

inline uint32_t min(uint32_t a, uint32_t b) {
    return (a < b ? a : b);
}

namespace android {

int64_t AAH_RXPlayer::Substream::kAboutToUnderflowThreshold =
    50ull * 1000;

AAH_RXPlayer::Substream::Substream(uint32_t ssrc, OMXClient& omx) {
    ssrc_ = ssrc;
    substream_details_known_ = false;
    buffer_in_progress_ = NULL;
    status_ = OK;
    codec_mime_type_ = "";

    decoder_ = new AAH_DecoderPump(omx);
    if (decoder_ == NULL) {
        ALOGE("%s failed to allocate decoder pump!", __PRETTY_FUNCTION__);
    }
    if (OK != decoder_->initCheck()) {
        ALOGE("%s failed to initialize decoder pump!", __PRETTY_FUNCTION__);
    }

    // cleanupBufferInProgress will reset most of the internal state variables.
    // Just need to make sure that buffer_in_progress_ is NULL before calling.
    cleanupBufferInProgress();
}

AAH_RXPlayer::Substream::~Substream() {
    shutdown();
}

void AAH_RXPlayer::Substream::shutdown() {
    substream_meta_ = NULL;
    status_ = OK;
    cleanupBufferInProgress();
    cleanupDecoder();
}

void AAH_RXPlayer::Substream::cleanupBufferInProgress() {
    if (NULL != buffer_in_progress_) {
        buffer_in_progress_->release();
        buffer_in_progress_ = NULL;
    }

    expected_buffer_size_ = 0;
    buffer_filled_ = 0;
    waiting_for_rap_ = true;

    aux_data_in_progress_.clear();
    aux_data_expected_size_ = 0;
}

void AAH_RXPlayer::Substream::cleanupDecoder() {
    if (decoder_ != NULL) {
        decoder_->shutdown();
    }
}

bool AAH_RXPlayer::Substream::shouldAbort(const char* log_tag) {
    // If we have already encountered a fatal error, do nothing.  We are just
    // waiting for our owner to shut us down now.
    if (OK != status_) {
        ALOGV("Skipping %s, substream has encountered fatal error (%d).",
                log_tag, status_);
        return true;
    }

    return false;
}

void AAH_RXPlayer::Substream::processPayloadStart(uint8_t* buf,
                                                  uint32_t amt,
                                                  int32_t ts_lower) {
    uint32_t min_length = 6;

    if (shouldAbort(__PRETTY_FUNCTION__)) {
        return;
    }

    // Do we have a buffer in progress already?  If so, abort the buffer.  In
    // theory, this should never happen.  If there were a discontinutity in the
    // stream, the discon in the seq_nos at the RTP level should have already
    // triggered a cleanup of the buffer in progress.  To see a problem at this
    // level is an indication either of a bug in the transmitter, or some form
    // of terrible corruption/tampering on the wire.
    if (NULL != buffer_in_progress_) {
        ALOGE("processPayloadStart is aborting payload already in progress.");
        cleanupBufferInProgress();
    }

    // Parse enough of the header to know where we stand.  Since this is a
    // payload start, it should begin with a TRTP header which has to be at
    // least 6 bytes long.
    if (amt < min_length) {
        ALOGV("Discarding payload too short to contain TRTP header (len = %u)",
                amt);
        return;
    }

    // Check the TRTP version number.
    if (0x01 != buf[0]) {
        ALOGV("Unexpected TRTP version (%u) in header.  Expected %u.",
                buf[0], 1);
        return;
    }

    // Extract the substream type field and make sure its one we understand (and
    // one that does not conflict with any previously received substream type.
    uint8_t header_type = (buf[1] >> 4) & 0xF;
    switch (header_type) {
        case TRTPPacket::kHeaderTypeAudio:
            // Audio, yay!  Just break.  We understand audio payloads.
            break;
        case TRTPPacket::kHeaderTypeVideo:
            ALOGV("RXed packet with unhandled TRTP header type (Video).");
            return;
        case TRTPPacket::kHeaderTypeSubpicture:
            ALOGV("RXed packet with unhandled TRTP header type (Subpicture).");
            return;
        case TRTPPacket::kHeaderTypeControl:
            ALOGV("RXed packet with unhandled TRTP header type (Control).");
            return;
        default:
            ALOGV("RXed packet with unhandled TRTP header type (%u).",
                    header_type);
            return;
    }

    if (substream_details_known_ && (header_type != substream_type_)) {
        ALOGV("RXed TRTP Payload for SSRC=0x%08x where header type (%u) does"
              " not match previously received header type (%u)",
              ssrc_, header_type, substream_type_);
        return;
    }

    // Check the flags to see if there is another 32 bits of timestamp present.
    uint32_t trtp_header_len = 6;
    bool ts_valid = buf[1] & TRTPPacket::kFlag_TSValid;
    if (ts_valid) {
        min_length += 4;
        trtp_header_len += 4;
        if (amt < min_length) {
            ALOGV("Discarding payload too short to contain TRTP timestamp"
                  " (len = %u)", amt);
            return;
        }
    }

    // Extract the TRTP length field and sanity check it.
    uint32_t trtp_len = U32_AT(buf + 2);
    if (trtp_len < min_length) {
        ALOGV("TRTP length (%u) is too short to be valid.  Must be at least %u"
              " bytes.", trtp_len, min_length);
        return;
    }

    // Extract the rest of the timestamp field if valid.
    int64_t ts = 0;
    uint32_t parse_offset = 6;
    if (ts_valid) {
        uint32_t ts_upper = U32_AT(buf + parse_offset);
        parse_offset += 4;
        ts = (static_cast<int64_t>(ts_upper) << 32) | ts_lower;
    }

    // Check the flags to see if there is another 24 bytes of timestamp
    // transformation present.
    if (buf[1] & TRTPPacket::kFlag_TSTransformPresent) {
        min_length += 24;
        parse_offset += 24;
        trtp_header_len += 24;
        if (amt < min_length) {
            ALOGV("Discarding payload too short to contain TRTP timestamp"
                  " transformation (len = %u)", amt);
            return;
        }
    }

    // TODO : break the parsing into individual parsers for the different
    // payload types (audio, video, etc).
    //
    // At this point in time, we know that this is audio.  Go ahead and parse
    // the basic header, check the codec type, and find the payload portion of
    // the packet.
    min_length += 3;
    if (trtp_len < min_length) {
        ALOGV("TRTP length (%u) is too short to be a valid audio payload.  Must"
              " be at least %u bytes.", trtp_len, min_length);
        return;
    }

    if (amt < min_length) {
        ALOGV("TRTP porttion of RTP payload (%u bytes) too small to contain"
              " entire TRTP header.  TRTP does not currently support"
              " fragmenting TRTP headers across RTP payloads", amt);
        return;
    }

    uint8_t codec_type = buf[parse_offset    ];
    uint8_t flags      = buf[parse_offset + 1];
    uint8_t volume     = buf[parse_offset + 2];
    parse_offset += 3;
    trtp_header_len += 3;

    if (!setupSubstreamType(header_type, codec_type)) {
        return;
    }

    if (decoder_ != NULL) {
        decoder_->setRenderVolume(volume);
    }

    if (waiting_for_rap_ && !(flags & TRTPAudioPacket::kFlag_RandomAccessPoint)) {
        ALOGV("Dropping non-RAP TRTP Audio Payload while waiting for RAP.");
        return;
    }

    // Check for the presence of codec aux data.
    if (flags & TRTPAudioPacket::kFlag_AuxLengthPresent) {
        min_length += 4;
        trtp_header_len += 4;

        if (trtp_len < min_length) {
            ALOGV("TRTP length (%u) is too short to be a valid audio payload.  "
                  "Must be at least %u bytes.", trtp_len, min_length);
            return;
        }

        if (amt < min_length) {
            ALOGV("TRTP porttion of RTP payload (%u bytes) too small to contain"
                  " entire TRTP header.  TRTP does not currently support"
                  " fragmenting TRTP headers across RTP payloads", amt);
            return;
        }

        aux_data_expected_size_ = U32_AT(buf + parse_offset);
        aux_data_in_progress_.clear();
        if (aux_data_in_progress_.capacity() < aux_data_expected_size_) {
            aux_data_in_progress_.setCapacity(aux_data_expected_size_);
        }
    } else {
        aux_data_expected_size_ = 0;
    }

    if ((aux_data_expected_size_ + trtp_header_len) > trtp_len) {
        ALOGV("Expected codec aux data length (%u) and TRTP header overhead"
              " (%u) too large for total TRTP payload length (%u).",
             aux_data_expected_size_, trtp_header_len, trtp_len);
        return;
    }

    // OK - everything left is just payload.  Compute the payload size, start
    // the buffer in progress and pack as much payload as we can into it.  If
    // the payload is finished once we are done, go ahead and send the payload
    // to the decoder.
    expected_buffer_size_ = trtp_len
                          - trtp_header_len
                          - aux_data_expected_size_;
    if (!expected_buffer_size_) {
        ALOGV("Dropping TRTP Audio Payload with 0 Access Unit length");
        return;
    }

    CHECK(amt >= trtp_header_len);
    uint32_t todo = amt - trtp_header_len;
    if ((expected_buffer_size_ + aux_data_expected_size_) < todo) {
        ALOGV("Extra data (%u > %u) present in initial TRTP Audio Payload;"
              " dropping payload.", todo,
              expected_buffer_size_ + aux_data_expected_size_);
        return;
    }

    buffer_filled_ = 0;
    buffer_in_progress_ = new MediaBuffer(expected_buffer_size_);
    if ((NULL == buffer_in_progress_) ||
            (NULL == buffer_in_progress_->data())) {
        ALOGV("Failed to allocate MediaBuffer of length %u",
                expected_buffer_size_);
        cleanupBufferInProgress();
        return;
    }

    sp<MetaData> meta = buffer_in_progress_->meta_data();
    if (meta == NULL) {
        ALOGV("Missing metadata structure in allocated MediaBuffer; dropping"
              " payload");
        cleanupBufferInProgress();
        return;
    }

    meta->setCString(kKeyMIMEType, codec_mime_type_);
    if (ts_valid) {
        meta->setInt64(kKeyTime, ts);
    }

    // Skip over the header we have already extracted.
    amt -= trtp_header_len;
    buf += trtp_header_len;

    // Extract as much of the expected aux data as we can.
    todo = min(aux_data_expected_size_, amt);
    if (todo) {
        aux_data_in_progress_.appendArray(buf, todo);
        buf += todo;
        amt -= todo;
    }

    // Extract as much of the expected payload as we can.
    todo = min(expected_buffer_size_, amt);
    if (todo > 0) {
        uint8_t* tgt =
            reinterpret_cast<uint8_t*>(buffer_in_progress_->data());
        memcpy(tgt, buf, todo);
        buffer_filled_ = amt;
        buf += todo;
        amt -= todo;
    }

    if (buffer_filled_ >= expected_buffer_size_) {
        processCompletedBuffer();
    }
}

void AAH_RXPlayer::Substream::processPayloadCont(uint8_t* buf,
                                                 uint32_t amt) {
    if (shouldAbort(__PRETTY_FUNCTION__)) {
        return;
    }

    if (NULL == buffer_in_progress_) {
        ALOGV("TRTP Receiver skipping payload continuation; no buffer currently"
              " in progress.");
        return;
    }

    CHECK(aux_data_in_progress_.size() <= aux_data_expected_size_);
    uint32_t aux_left = aux_data_expected_size_ - aux_data_in_progress_.size();
    if (aux_left) {
        uint32_t todo = min(aux_left, amt);
        aux_data_in_progress_.appendArray(buf, todo);
        amt -= todo;
        buf += todo;

        if (!amt)
            return;
    }

    CHECK(buffer_filled_ < expected_buffer_size_);
    uint32_t buffer_left = expected_buffer_size_ - buffer_filled_;
    if (amt > buffer_left) {
        ALOGV("Extra data (%u > %u) present in continued TRTP Audio Payload;"
              " dropping payload.", amt, buffer_left);
        cleanupBufferInProgress();
        return;
    }

    if (amt > 0) {
        uint8_t* tgt =
            reinterpret_cast<uint8_t*>(buffer_in_progress_->data());
        memcpy(tgt + buffer_filled_, buf, amt);
        buffer_filled_ += amt;
    }

    if (buffer_filled_ >= expected_buffer_size_) {
        processCompletedBuffer();
    }
}

void AAH_RXPlayer::Substream::processCompletedBuffer() {
    status_t res;

    CHECK(NULL != buffer_in_progress_);

    if (decoder_ == NULL) {
        ALOGV("Dropping complete buffer, no decoder pump allocated");
        goto bailout;
    }

    // Make sure our metadata used to initialize the decoder has been properly
    // set up.
    if (!setupSubstreamMeta())
        goto bailout;

    // If our decoder has not be set up, do so now.
    res = decoder_->init(substream_meta_);
    if (OK != res) {
        ALOGE("Failed to init decoder (res = %d)", res);
        cleanupDecoder();
        substream_meta_ = NULL;
        goto bailout;
    }

    // Queue the payload for decode.
    res = decoder_->queueForDecode(buffer_in_progress_);

    if (res != OK) {
        ALOGD("Failed to queue payload for decode, resetting decoder pump!"
             " (res = %d)", res);
        status_ = res;
        cleanupDecoder();
        cleanupBufferInProgress();
    }

    // NULL out buffer_in_progress before calling the cleanup helper.
    //
    // MediaBuffers use something of a hybrid ref-counting pattern which prevent
    // the AAH_DecoderPump's input queue from adding their own reference to the
    // MediaBuffer.  MediaBuffers start life with a reference count of 0, as
    // well as an observer which starts as NULL.  Before being given an
    // observer, the ref count cannot be allowed to become non-zero as it will
    // cause calls to release() to assert.  Basically, before a MediaBuffer has
    // an observer, they behave like non-ref counted obects where release()
    // serves the roll of delete.  After a MediaBuffer has an observer, they
    // become more like ref counted objects where add ref and release can be
    // used, and when the ref count hits zero, the MediaBuffer is handed off to
    // the observer.
    //
    // Given all of this, when we give the buffer to the decoder pump to wait in
    // the to-be-processed queue, the decoder cannot add a ref to the buffer as
    // it would in a traditional ref counting system.  Instead it needs to
    // "steal" the non-existent ref.  In the case of queue failure, we need to
    // make certain to release this non-existent reference so that the buffer is
    // cleaned up during the cleanupBufferInProgress helper.  In the case of a
    // successful queue operation, we need to make certain that the
    // cleanupBufferInProgress helper does not release the buffer since it needs
    // to remain alive in the queue.  We acomplish this by NULLing out the
    // buffer pointer before calling the cleanup helper.
    buffer_in_progress_ = NULL;

bailout:
    cleanupBufferInProgress();
}

bool AAH_RXPlayer::Substream::setupSubstreamMeta() {
    switch (codec_type_) {
        case TRTPAudioPacket::kCodecMPEG1Audio:
            codec_mime_type_ = MEDIA_MIMETYPE_AUDIO_MPEG;
            return setupMP3SubstreamMeta();

        case TRTPAudioPacket::kCodecAACAudio:
            codec_mime_type_ = MEDIA_MIMETYPE_AUDIO_AAC;
            return setupAACSubstreamMeta();

        default:
            ALOGV("Failed to setup substream metadata for unsupported codec"
                  " type (%u)", codec_type_);
            break;
    }

    return false;
}

bool AAH_RXPlayer::Substream::setupMP3SubstreamMeta() {
    const uint8_t* buffer_data = NULL;
    int sample_rate;
    int channel_count;
    size_t frame_size;
    status_t res;

    buffer_data = reinterpret_cast<const uint8_t*>(buffer_in_progress_->data());
    if (buffer_in_progress_->size() < 4) {
        ALOGV("MP3 payload too short to contain header, dropping payload.");
        return false;
    }

    // Extract the channel count and the sample rate from the MP3 header.  The
    // stagefright MP3 requires that these be delivered before decoing can
    // begin.
    if (!GetMPEGAudioFrameSize(U32_AT(buffer_data),
                               &frame_size,
                               &sample_rate,
                               &channel_count,
                               NULL,
                               NULL)) {
        ALOGV("Failed to parse MP3 header in payload, droping payload.");
        return false;
    }


    // Make sure that our substream metadata is set up properly.  If there has
    // been a format change, be sure to reset the underlying decoder.  In
    // stagefright, it seems like the only way to do this is to destroy and
    // recreate the decoder.
    if (substream_meta_ == NULL) {
        substream_meta_ = new MetaData();

        if (substream_meta_ == NULL) {
            ALOGE("Failed to allocate MetaData structure for MP3 substream");
            return false;
        }

        substream_meta_->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
        substream_meta_->setInt32  (kKeyChannelCount, channel_count);
        substream_meta_->setInt32  (kKeySampleRate,   sample_rate);
    } else {
        int32_t prev_sample_rate;
        int32_t prev_channel_count;
        substream_meta_->findInt32(kKeySampleRate,   &prev_sample_rate);
        substream_meta_->findInt32(kKeyChannelCount, &prev_channel_count);

        if ((prev_channel_count != channel_count) ||
            (prev_sample_rate   != sample_rate)) {
            ALOGW("MP3 format change detected, forcing decoder reset.");
            cleanupDecoder();

            substream_meta_->setInt32(kKeyChannelCount, channel_count);
            substream_meta_->setInt32(kKeySampleRate,   sample_rate);
        }
    }

    return true;
}

bool AAH_RXPlayer::Substream::setupAACSubstreamMeta() {
    int32_t sample_rate, channel_cnt;
    static const size_t overhead = sizeof(sample_rate)
                                 + sizeof(channel_cnt);

    if (aux_data_in_progress_.size() < overhead) {
        ALOGE("Not enough aux data (%u) to initialize AAC substream decoder",
                aux_data_in_progress_.size());
        return false;
    }

    const uint8_t* aux_data = aux_data_in_progress_.array();
    size_t aux_data_size = aux_data_in_progress_.size();
    sample_rate = U32_AT(aux_data);
    channel_cnt = U32_AT(aux_data + sizeof(sample_rate));

    const uint8_t* esds_data = NULL;
    size_t esds_data_size = 0;
    if (aux_data_size > overhead) {
        esds_data = aux_data + overhead;
        esds_data_size = aux_data_size - overhead;
    }

    // Do we already have metadata?  If so, has it changed at all?  If not, then
    // there should be nothing else to do.  Otherwise, release our old stream
    // metadata and make new metadata.
    if (substream_meta_ != NULL) {
        uint32_t type;
        const void* data;
        size_t size;
        int32_t prev_sample_rate;
        int32_t prev_channel_count;
        bool res;

        res = substream_meta_->findInt32(kKeySampleRate,   &prev_sample_rate);
        CHECK(res);
        res = substream_meta_->findInt32(kKeyChannelCount, &prev_channel_count);
        CHECK(res);

        // If nothing has changed about the codec aux data (esds, sample rate,
        // channel count), then we can just do nothing and get out.  Otherwise,
        // we will need to reset the decoder and make a new metadata object to
        // deal with the format change.
        bool hasData = (esds_data != NULL);
        bool hadData = substream_meta_->findData(kKeyESDS, &type, &data, &size);
        bool esds_change = (hadData != hasData);

        if (!esds_change && hasData)
            esds_change = ((size != esds_data_size) ||
                           memcmp(data, esds_data, size));

        if (!esds_change &&
            (prev_sample_rate   == sample_rate) &&
            (prev_channel_count == channel_cnt)) {
            return true;  // no change, just get out.
        }

        ALOGW("AAC format change detected, forcing decoder reset.");
        cleanupDecoder();
        substream_meta_ = NULL;
    }

    CHECK(substream_meta_ == NULL);

    substream_meta_ = new MetaData();
    if (substream_meta_ == NULL) {
        ALOGE("Failed to allocate MetaData structure for AAC substream");
        return false;
    }

    substream_meta_->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
    substream_meta_->setInt32  (kKeySampleRate,   sample_rate);
    substream_meta_->setInt32  (kKeyChannelCount, channel_cnt);

    if (esds_data) {
        substream_meta_->setData(kKeyESDS, kTypeESDS,
                                 esds_data, esds_data_size);
    }

    return true;
}

void AAH_RXPlayer::Substream::processTSTransform(const LinearTransform& trans) {
    if (decoder_ != NULL) {
        decoder_->setRenderTSTransform(trans);
    }
}

bool AAH_RXPlayer::Substream::isAboutToUnderflow() {
    if (decoder_ == NULL) {
        return false;
    }

    return decoder_->isAboutToUnderflow(kAboutToUnderflowThreshold);
}

bool AAH_RXPlayer::Substream::setupSubstreamType(uint8_t substream_type,
                                                 uint8_t codec_type) {
    // Sanity check the codec type.  Right now we only support MP3 and AAC.
    // Also check for conflicts with previously delivered codec types.
    if (substream_details_known_) {
        if (codec_type != codec_type_) {
            ALOGV("RXed TRTP Payload for SSRC=0x%08x where codec type (%u) does"
                  " not match previously received codec type (%u)",
                 ssrc_, codec_type, codec_type_);
            return false;
        }

        return true;
    }

    switch (codec_type) {
        // MP3 and AAC are all we support right now.
        case TRTPAudioPacket::kCodecMPEG1Audio:
        case TRTPAudioPacket::kCodecAACAudio:
            break;

        default:
            ALOGV("RXed TRTP Audio Payload for SSRC=0x%08x with unsupported"
                  " codec type (%u)", ssrc_, codec_type);
            return false;
    }

    substream_type_ = substream_type;
    codec_type_ = codec_type;
    substream_details_known_ = true;

    return true;
}

}  // namespace android
