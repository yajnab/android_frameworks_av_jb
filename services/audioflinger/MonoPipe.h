/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_AUDIO_MONO_PIPE_H
#define ANDROID_AUDIO_MONO_PIPE_H

#include <time.h>
#include "NBAIO.h"

namespace android {

// MonoPipe is similar to Pipe except:
//  - supports only a single reader, called MonoPipeReader
//  - write() cannot overrun; instead it will return a short actual count if insufficient space
//  - write() can optionally block if the pipe is full
// Like Pipe, it is not multi-thread safe for either writer or reader
// but writer and reader can be different threads.
class MonoPipe : public NBAIO_Sink {

    friend class MonoPipeReader;

public:
    // reqFrames will be rounded up to a power of 2, and all slots are available. Must be >= 2.
    // Note: whatever shares this object with another thread needs to do so in an SMP-safe way (like
    // creating it the object before creating the other thread, or storing the object with a
    // release_store). Otherwise the other thread could see a partially-constructed object.
    MonoPipe(size_t reqFrames, NBAIO_Format format, bool writeCanBlock = false);
    virtual ~MonoPipe();

    // NBAIO_Port interface

    //virtual ssize_t negotiate(const NBAIO_Format offers[], size_t numOffers,
    //                          NBAIO_Format counterOffers[], size_t& numCounterOffers);
    //virtual NBAIO_Format format() const;

    // NBAIO_Sink interface

    //virtual size_t framesWritten() const;
    //virtual size_t framesUnderrun() const;
    //virtual size_t underruns() const;

    virtual ssize_t availableToWrite() const;
    virtual ssize_t write(const void *buffer, size_t count);
    //virtual ssize_t writeVia(writeVia_t via, size_t total, void *user, size_t block);

            // average number of frames present in the pipe under normal conditions.
            // See throttling mechanism in MonoPipe::write()
            size_t  getAvgFrames() const { return mSetpoint; }
            void    setAvgFrames(size_t setpoint);
            size_t  maxFrames() const { return mMaxFrames; }

private:
    const size_t    mReqFrames;     // as requested in constructor, unrounded
    const size_t    mMaxFrames;     // always a power of 2
    void * const    mBuffer;
    // mFront and mRear will never be separated by more than mMaxFrames.
    // 32-bit overflow is possible if the pipe is active for a long time, but if that happens it's
    // safe because we "&" with (mMaxFrames-1) at end of computations to calculate a buffer index.
    volatile int32_t mFront;        // written by reader with android_atomic_release_store,
                                    // read by writer with android_atomic_acquire_load
    volatile int32_t mRear;         // written by writer with android_atomic_release_store,
                                    // read by reader with android_atomic_acquire_load
    bool            mWriteTsValid;  // whether mWriteTs is valid
    struct timespec mWriteTs;       // time that the previous write() completed
    size_t          mSetpoint;      // target value for pipe fill depth
    const bool      mWriteCanBlock; // whether write() should block if the pipe is full
};

}   // namespace android

#endif  // ANDROID_AUDIO_MONO_PIPE_H
