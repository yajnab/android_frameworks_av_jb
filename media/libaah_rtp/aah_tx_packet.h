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

#ifndef __AAH_TX_PACKET_H__
#define __AAH_TX_PACKET_H__

#include <media/stagefright/foundation/ABase.h>
#include <utils/LinearTransform.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>

namespace android {

class TRTPPacket : public RefBase {
  public:
    enum TRTPHeaderType {
        kHeaderTypeAudio = 1,
        kHeaderTypeVideo = 2,
        kHeaderTypeSubpicture = 3,
        kHeaderTypeControl = 4,
    };

    enum TRTPPayloadFlags {
        kFlag_TSTransformPresent = 0x02,
        kFlag_TSValid = 0x01,
    };

  protected:
    TRTPPacket(TRTPHeaderType headerType)
        : mIsPacked(false)
        , mVersion(2)
        , mPadding(false)
        , mExtension(false)
        , mCsrcCount(0)
        , mPayloadType(100)
        , mSeqNumber(0)
        , mPTSValid(false)
        , mPTS(0)
        , mEpoch(0)
        , mProgramID(0)
        , mSubstreamID(0)
        , mClockTranformValid(false)
        , mTRTPVersion(1)
        , mTRTPLength(0)
        , mTRTPHeaderType(headerType)
        , mPacket(NULL)
        , mPacketLen(0) { }

  public:
    virtual ~TRTPPacket();

    void setSeqNumber(uint16_t val);
    uint16_t getSeqNumber() const;

    void setPTS(int64_t val);
    int64_t getPTS() const;

    void setEpoch(uint32_t val);
    void setProgramID(uint16_t val);
    void setSubstreamID(uint16_t val);
    void setClockTransform(const LinearTransform& trans);

    uint8_t* getPacket() const;
    int getPacketLen() const;

    void setExpireTime(nsecs_t val);
    nsecs_t getExpireTime() const;

    virtual bool pack() = 0;

    // mask for the number of bits in a TRTP epoch
    static const uint32_t kTRTPEpochMask = (1 << 22) - 1;
    static const int kTRTPEpochShift = 10;

  protected:
    static const int kRTPHeaderLen = 12;
    virtual int TRTPHeaderLen() const;

    void writeTRTPHeader(uint8_t*& buf,
                         bool isFirstFragment,
                         int totalPacketLen);

    void writeU8(uint8_t*& buf, uint8_t val);
    void writeU16(uint8_t*& buf, uint16_t val);
    void writeU32(uint8_t*& buf, uint32_t val);
    void writeU64(uint8_t*& buf, uint64_t val);

    bool mIsPacked;

    uint8_t mVersion;
    bool mPadding;
    bool mExtension;
    uint8_t mCsrcCount;
    uint8_t mPayloadType;
    uint16_t mSeqNumber;
    bool mPTSValid;
    int64_t  mPTS;
    uint32_t mEpoch;
    uint16_t mProgramID;
    uint16_t mSubstreamID;
    LinearTransform mClockTranform;
    bool mClockTranformValid;
    uint8_t mTRTPVersion;
    uint32_t mTRTPLength;
    TRTPHeaderType mTRTPHeaderType;

    uint8_t* mPacket;
    int mPacketLen;

    nsecs_t mExpireTime;

    DISALLOW_EVIL_CONSTRUCTORS(TRTPPacket);
};

class TRTPAudioPacket : public TRTPPacket {
  public:
    enum AudioPayloadFlags {
        kFlag_AuxLengthPresent = 0x10,
        kFlag_RandomAccessPoint = 0x08,
        kFlag_Dropable = 0x04,
        kFlag_Discontinuity = 0x02,
        kFlag_EndOfStream = 0x01,
    };

    TRTPAudioPacket()
        : TRTPPacket(kHeaderTypeAudio)
        , mCodecType(kCodecInvalid)
        , mRandomAccessPoint(false)
        , mDropable(false)
        , mDiscontinuity(false)
        , mEndOfStream(false)
        , mVolume(0)
        , mAccessUnitData(NULL)
        , mAccessUnitLen(0)
        , mAuxData(NULL)
        , mAuxDataLen(0) { }

    enum TRTPAudioCodecType {
        kCodecInvalid = 0,
        kCodecPCMBigEndian = 1,
        kCodecPCMLittleEndian = 2,
        kCodecMPEG1Audio = 3,
        kCodecAACAudio = 4,
    };

    void setCodecType(TRTPAudioCodecType val);
    void setRandomAccessPoint(bool val);
    void setDropable(bool val);
    void setDiscontinuity(bool val);
    void setEndOfStream(bool val);
    void setVolume(uint8_t val);
    void setAccessUnitData(const void* data, size_t len);
    void setAuxData(const void* data, size_t len);

    virtual bool pack();

  protected:
    virtual int TRTPHeaderLen() const;

  private:
    TRTPAudioCodecType mCodecType;
    bool mRandomAccessPoint;
    bool mDropable;
    bool mDiscontinuity;
    bool mEndOfStream;
    uint8_t mVolume;

    const void* mAccessUnitData;
    size_t mAccessUnitLen;
    const void* mAuxData;
    size_t mAuxDataLen;

    DISALLOW_EVIL_CONSTRUCTORS(TRTPAudioPacket);
};

class TRTPControlPacket : public TRTPPacket {
  public:
    TRTPControlPacket()
        : TRTPPacket(kHeaderTypeControl)
        , mCommandID(kCommandNop) {}

    enum TRTPCommandID {
        kCommandNop   = 1,
        kCommandFlush = 2,
        kCommandEOS   = 3,
    };

    void setCommandID(TRTPCommandID val);

    virtual bool pack();

  private:
    TRTPCommandID mCommandID;

    DISALLOW_EVIL_CONSTRUCTORS(TRTPControlPacket);
};

}  // namespace android

#endif  // __AAH_TX_PLAYER_H__
