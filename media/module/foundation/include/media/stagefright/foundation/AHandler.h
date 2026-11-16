/*
 * Copyright (C) 2010 The Android Open Source Project
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

#ifndef A_HANDLER_H_

#define A_HANDLER_H_

#include <media/stagefright/foundation/ALooper.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>

#include <atomic>

namespace android {

struct AMessage;

struct AHandler : public RefBase {
    AHandler()
        : mID(0),
          mVerboseStats(false),
          mMessageCounter(0),
          mDeliveringMessage(false),
          mCurrentMessageWhat(0),
          mCurrentMessageStartTimeUs(0){
    }

    ALooper::handler_id id() const {
        return mID;
    }

    sp<ALooper> looper() const {
        return mLooper.promote();
    }

    wp<ALooper> getLooper() const {
        return mLooper;
    }

    wp<AHandler> getHandler() const {
        // allow getting a weak reference to a const handler
        return const_cast<AHandler *>(this);
    }

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg) = 0;

private:
    friend struct AMessage;      // deliverMessage()
    friend struct ALooperRoster; // setID()

    ALooper::handler_id mID;
    wp<ALooper> mLooper;

    inline void setID(ALooper::handler_id id, const wp<ALooper> &looper) {
        mID = id;
        mLooper = looper;
    }

    bool mVerboseStats;
    uint64_t mMessageCounter;
    KeyedVector<uint32_t, uint32_t> mMessages;

    Mutex mLock;
    // Delivery status is written on every message and read only by dumpsys.
    // Keeping it lock free avoids a second mutex acquisition per message for
    // handlers that are flooded (e.g. MTK's vtservice IMCB dispatcher).
    // mLock is kept so the object layout does not change for prebuilt blobs.
    std::atomic<bool> mDeliveringMessage;
    std::atomic<uint32_t> mCurrentMessageWhat;
    std::atomic<int64_t> mCurrentMessageStartTimeUs;

    void deliverMessage(const sp<AMessage> &msg);

    void setDeliveryStatus(bool, uint32_t, int64_t);
    void getDeliveryStatus(bool&, uint32_t&, int64_t&);


    DISALLOW_EVIL_CONSTRUCTORS(AHandler);
};

}  // namespace android

#endif  // A_HANDLER_H_
