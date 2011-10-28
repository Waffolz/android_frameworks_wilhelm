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

//#define USE_LOG SLAndroidLogLevel_Verbose

#include "sles_allinclusive.h"
#include "android_GenericMediaPlayer.h"

#include <media/IMediaPlayerService.h>
#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/SurfaceComposerClient.h>
#include <media/stagefright/foundation/ADebug.h>

// default delay in Us used when reposting an event when the player is not ready to accept
// the command yet. This is for instance used when seeking on a MediaPlayer that's still preparing
#define DEFAULT_COMMAND_DELAY_FOR_REPOST_US (100*1000) // 100ms

// table of prefixes for known distant protocols; these are immediately dispatched to mediaserver
static const char* const kDistantProtocolPrefix[] = { "http://", "https://", "rtsp://"};
#define NB_DISTANT_PROTOCOLS (sizeof(kDistantProtocolPrefix)/sizeof(kDistantProtocolPrefix[0]))

// is the specified URI a known distant protocol?
bool isDistantProtocol(const char *uri)
{
    for (unsigned int i = 0; i < NB_DISTANT_PROTOCOLS; i++) {
        if (!strncasecmp(uri, kDistantProtocolPrefix[i], strlen(kDistantProtocolPrefix[i]))) {
            return true;
        }
    }
    return false;
}

namespace android {

//--------------------------------------------------------------------------------------------------
MediaPlayerNotificationClient::MediaPlayerNotificationClient(GenericMediaPlayer* gmp) :
    mGenericMediaPlayer(gmp),
    mPlayerPrepared(PREPARE_NOT_STARTED)
{
    SL_LOGV("MediaPlayerNotificationClient::MediaPlayerNotificationClient()");
}

MediaPlayerNotificationClient::~MediaPlayerNotificationClient() {
    SL_LOGV("MediaPlayerNotificationClient::~MediaPlayerNotificationClient()");
}

// Map a MEDIA_* enum to a string
static const char *media_to_string(int msg)
{
    switch (msg) {
#define _(x) case MEDIA_##x: return "MEDIA_" #x;
      _(PREPARED)
      _(SET_VIDEO_SIZE)
      _(SEEK_COMPLETE)
      _(PLAYBACK_COMPLETE)
      _(BUFFERING_UPDATE)
      _(ERROR)
      _(NOP)
      _(TIMED_TEXT)
      _(INFO)
#undef _
    default:
        return NULL;
    }
}

//--------------------------------------------------
// IMediaPlayerClient implementation
void MediaPlayerNotificationClient::notify(int msg, int ext1, int ext2, const Parcel *obj) {
    SL_LOGV("MediaPlayerNotificationClient::notify(msg=%s (%d), ext1=%d, ext2=%d)",
            media_to_string(msg), msg, ext1, ext2);

    sp<GenericMediaPlayer> genericMediaPlayer(mGenericMediaPlayer.promote());
    if (genericMediaPlayer == NULL) {
        SL_LOGW("MediaPlayerNotificationClient::notify after GenericMediaPlayer destroyed");
        return;
    }

    switch (msg) {
      case MEDIA_PREPARED:
        {
        Mutex::Autolock _l(mLock);
        if (PREPARE_IN_PROGRESS == mPlayerPrepared) {
            mPlayerPrepared = PREPARE_COMPLETED_SUCCESSFULLY;
            mPlayerPreparedCondition.signal();
        } else {
            SL_LOGE("Unexpected MEDIA_PREPARED");
        }
        }
        break;

      case MEDIA_SET_VIDEO_SIZE:
        // only send video size updates if the player was flagged as having video, to avoid
        // sending video size updates of (0,0)
        // We're running on a different thread than genericMediaPlayer's ALooper thread,
        // so it would normally be racy to access fields within genericMediaPlayer.
        // But in this case mHasVideo is const, so it is safe to access.
        // Or alternatively, we could notify unconditionally and let it decide whether to handle.
        if (genericMediaPlayer->mHasVideo) {
            genericMediaPlayer->notify(PLAYEREVENT_VIDEO_SIZE_UPDATE,
                    (int32_t)ext1, (int32_t)ext2, true /*async*/);
        }
        break;

      case MEDIA_SEEK_COMPLETE:
        genericMediaPlayer->seekComplete();
        break;

      case MEDIA_PLAYBACK_COMPLETE:
        genericMediaPlayer->notify(PLAYEREVENT_ENDOFSTREAM, 1, true /*async*/);
        break;

      case MEDIA_BUFFERING_UPDATE:
        // values received from Android framework for buffer fill level use percent,
        //   while SL/XA use permille, so does GenericPlayer
        genericMediaPlayer->bufferingUpdate(ext1 * 10 /*fillLevelPerMille*/);
        break;

      case MEDIA_ERROR:
        {
        Mutex::Autolock _l(mLock);
        if (PREPARE_IN_PROGRESS == mPlayerPrepared) {
            mPlayerPrepared = PREPARE_COMPLETED_UNSUCCESSFULLY;
            mPlayerPreparedCondition.signal();
        } else {
            // inform client of errors after preparation
            genericMediaPlayer->notify(PLAYEREVENT_ERRORAFTERPREPARE, ext1, true /*async*/);
        }
        }
        break;

      case MEDIA_NOP:
      case MEDIA_TIMED_TEXT:
      case MEDIA_INFO:
        break;

      default: { }
    }

}

//--------------------------------------------------
void MediaPlayerNotificationClient::beforePrepare()
{
    Mutex::Autolock _l(mLock);
    assert(mPlayerPrepared == PREPARE_NOT_STARTED);
    mPlayerPrepared = PREPARE_IN_PROGRESS;
}

//--------------------------------------------------
bool MediaPlayerNotificationClient::blockUntilPlayerPrepared() {
    Mutex::Autolock _l(mLock);
    assert(mPlayerPrepared != PREPARE_NOT_STARTED);
    while (mPlayerPrepared == PREPARE_IN_PROGRESS) {
        mPlayerPreparedCondition.wait(mLock);
    }
    assert(mPlayerPrepared == PREPARE_COMPLETED_SUCCESSFULLY ||
            mPlayerPrepared == PREPARE_COMPLETED_UNSUCCESSFULLY);
    return mPlayerPrepared == PREPARE_COMPLETED_SUCCESSFULLY;
}

//--------------------------------------------------------------------------------------------------
GenericMediaPlayer::GenericMediaPlayer(const AudioPlayback_Parameters* params, bool hasVideo) :
    GenericPlayer(params),
    mHasVideo(hasVideo),
    mSeekTimeMsec(0),
    mVideoSurface(0),
    mVideoSurfaceTexture(0),
    mPlayer(0),
    mPlayerClient(new MediaPlayerNotificationClient(this))
{
    SL_LOGD("GenericMediaPlayer::GenericMediaPlayer()");

}

GenericMediaPlayer::~GenericMediaPlayer() {
    SL_LOGD("GenericMediaPlayer::~GenericMediaPlayer()");
}

void GenericMediaPlayer::preDestroy() {
    // FIXME can't access mPlayer from outside the looper (no mutex!) so using mPreparedPlayer
    sp<IMediaPlayer> player;
    getPreparedPlayer(player);
    if (player != NULL) {
        player->stop();
        // causes CHECK failure in Nuplayer, but commented out in the subclass preDestroy
        // randomly causes a NPE in StagefrightPlayer, heap corruption, or app hang
        //player->setDataSource(NULL);
        player->setVideoSurface(NULL);
        player->disconnect();
        // release all references to the IMediaPlayer
        // FIXME illegal if not on looper
        //mPlayer.clear();
        {
            Mutex::Autolock _l(mPreparedPlayerLock);
            mPreparedPlayer.clear();
        }
    }
    GenericPlayer::preDestroy();
}

//--------------------------------------------------
// overridden from GenericPlayer
// pre-condition:
//   msec != NULL
// post-condition
//   *msec ==
//                  ANDROID_UNKNOWN_TIME if position is unknown at time of query,
//               or the current MediaPlayer position
void GenericMediaPlayer::getPositionMsec(int* msec) {
    SL_LOGD("GenericMediaPlayer::getPositionMsec()");
    sp<IMediaPlayer> player;
    getPreparedPlayer(player);
    // To avoid deadlock, directly call the MediaPlayer object
    if (player == 0 || player->getCurrentPosition(msec) != NO_ERROR) {
        *msec = ANDROID_UNKNOWN_TIME;
    }
}

//--------------------------------------------------
void GenericMediaPlayer::setVideoSurface(const sp<Surface> &surface) {
    SL_LOGV("GenericMediaPlayer::setVideoSurface()");
    // FIXME bug - race condition, should do in looper
    if (mVideoSurface.get() == surface.get()) {
        return;
    }
    if ((mStateFlags & kFlagPrepared) && (mPlayer != 0)) {
        mPlayer->setVideoSurface(surface);
    }
    mVideoSurface = surface;
    mVideoSurfaceTexture = NULL;
}

void GenericMediaPlayer::setVideoSurfaceTexture(const sp<ISurfaceTexture> &surfaceTexture) {
    SL_LOGV("GenericMediaPlayer::setVideoSurfaceTexture()");
    // FIXME bug - race condition, should do in looper
    if (mVideoSurfaceTexture.get() == surfaceTexture.get()) {
        return;
    }
    if ((mStateFlags & kFlagPrepared) && (mPlayer != 0)) {
        mPlayer->setVideoSurfaceTexture(surfaceTexture);
    }
    mVideoSurfaceTexture = surfaceTexture;
    mVideoSurface = NULL;
}


//--------------------------------------------------
// Event handlers

// blocks until mPlayer is prepared
void GenericMediaPlayer::onPrepare() {
    SL_LOGD("GenericMediaPlayer::onPrepare()");
    // Attempt to prepare at most once, and only if there is a MediaPlayer
    if (!(mStateFlags & (kFlagPrepared | kFlagPreparedUnsuccessfully)) && (mPlayer != 0)) {
        if (mHasVideo) {
            if (mVideoSurface != 0) {
                mPlayer->setVideoSurface(mVideoSurface);
            } else if (mVideoSurfaceTexture != 0) {
                mPlayer->setVideoSurfaceTexture(mVideoSurfaceTexture);
            }
        }
        mPlayer->setAudioStreamType(mPlaybackParams.streamType);
        mPlayerClient->beforePrepare();
        mPlayer->prepareAsync();
        if (mPlayerClient->blockUntilPlayerPrepared()) {
            mStateFlags |= kFlagPrepared;
            afterMediaPlayerPreparedSuccessfully();
        } else {
            mStateFlags |= kFlagPreparedUnsuccessfully;
        }
    }
    GenericPlayer::onPrepare();
    SL_LOGD("GenericMediaPlayer::onPrepare() done, mStateFlags=0x%x", mStateFlags);
}


void GenericMediaPlayer::onPlay() {
    SL_LOGD("GenericMediaPlayer::onPlay()");
    if (((mStateFlags & (kFlagPrepared | kFlagPlaying)) == kFlagPrepared) && (mPlayer != 0)) {
        mPlayer->start();
    }
    GenericPlayer::onPlay();
}


void GenericMediaPlayer::onPause() {
    SL_LOGD("GenericMediaPlayer::onPause()");
    if (!(~mStateFlags & (kFlagPrepared | kFlagPlaying)) && (mPlayer != 0)) {
        mPlayer->pause();
    }
    GenericPlayer::onPause();
}


void GenericMediaPlayer::onSeekComplete() {
    SL_LOGV("GenericMediaPlayer::onSeekComplete()");
    // did we initiate the seek?
    if (!(mStateFlags & kFlagSeeking)) {
        // no, are we looping?
        if (mStateFlags & kFlagLooping) {
            // yes, per OpenSL ES 1.0.1 and 1.1 do NOT report it to client
            // notify(PLAYEREVENT_ENDOFSTREAM, 1, true /*async*/);
        // no, well that's surprising, but it's probably just a benign race condition
        } else {
            SL_LOGW("Unexpected seek complete event ignored");
        }
    }
    GenericPlayer::onSeekComplete();
}


/**
 * pre-condition: WHATPARAM_SEEK_SEEKTIME_MS parameter value >= 0
 */
void GenericMediaPlayer::onSeek(const sp<AMessage> &msg) {
    SL_LOGV("GenericMediaPlayer::onSeek");
    int64_t timeMsec = ANDROID_UNKNOWN_TIME;
    if (!msg->findInt64(WHATPARAM_SEEK_SEEKTIME_MS, &timeMsec)) {
        // invalid command, drop it
        return;
    }
    if ((mStateFlags & kFlagSeeking) && (timeMsec == mSeekTimeMsec) &&
            (timeMsec != ANDROID_UNKNOWN_TIME)) {
        // already seeking to the same non-unknown time, cancel this command
        return;
    } else if (mStateFlags & kFlagPreparedUnsuccessfully) {
        // discard seeks after unsuccessful prepare
    } else if (!(mStateFlags & kFlagPrepared)) {
        // we are not ready to accept a seek command at this time, retry later
        msg->post(DEFAULT_COMMAND_DELAY_FOR_REPOST_US);
    } else {
        if (mPlayer != 0) {
            mStateFlags |= kFlagSeeking;
            mSeekTimeMsec = (int32_t)timeMsec;
            // seek to unknown time is used by StreamPlayer after discontinuity
            if (timeMsec == ANDROID_UNKNOWN_TIME) {
                // FIXME simulate a MEDIA_SEEK_COMPLETE event in 250 ms;
                // this is a terrible hack to make up for mediaserver not sending one
                (new AMessage(kWhatSeekComplete, id()))->post(250000);
            } else if (OK != mPlayer->seekTo(timeMsec)) {
                mStateFlags &= ~kFlagSeeking;
                mSeekTimeMsec = ANDROID_UNKNOWN_TIME;
            }
        }
    }
}


void GenericMediaPlayer::onLoop(const sp<AMessage> &msg) {
    SL_LOGV("GenericMediaPlayer::onLoop");
    int32_t loop = 0;
    if (msg->findInt32(WHATPARAM_LOOP_LOOPING, &loop)) {
        if (loop) {
            mStateFlags |= kFlagLooping;
        } else {
            mStateFlags &= ~kFlagLooping;
        }
        // if we have a MediaPlayer then tell it now, otherwise we'll tell it after it's created
        if (mPlayer != 0) {
            (void) mPlayer->setLooping(loop);
        }
    }
}


void GenericMediaPlayer::onVolumeUpdate() {
    SL_LOGD("GenericMediaPlayer::onVolumeUpdate()");
    // use settings lock to read the volume settings
    Mutex::Autolock _l(mSettingsLock);
    if (mPlayer != 0) {
        mPlayer->setVolume(mAndroidAudioLevels.mFinalVolume[0],
                mAndroidAudioLevels.mFinalVolume[1]);
    }
}


void GenericMediaPlayer::onAttachAuxEffect(const sp<AMessage> &msg) {
    SL_LOGD("GenericMediaPlayer::onAttachAuxEffect()");
    int32_t effectId = 0;
    if (msg->findInt32(WHATPARAM_ATTACHAUXEFFECT, &effectId)) {
        if (mPlayer != 0) {
            status_t status;
            status = mPlayer->attachAuxEffect(effectId);
            // attachAuxEffect returns a status but we have no way to report it back to app
            (void) status;
        }
    }
}


void GenericMediaPlayer::onSetAuxEffectSendLevel(const sp<AMessage> &msg) {
    SL_LOGD("GenericMediaPlayer::onSetAuxEffectSendLevel()");
    float level = 0.0f;
    if (msg->findFloat(WHATPARAM_SETAUXEFFECTSENDLEVEL, &level)) {
        if (mPlayer != 0) {
            status_t status;
            status = mPlayer->setAuxEffectSendLevel(level);
            // setAuxEffectSendLevel returns a status but we have no way to report it back to app
            (void) status;
        }
    }
}


void GenericMediaPlayer::onBufferingUpdate(const sp<AMessage> &msg) {
    int32_t fillLevel = 0;
    if (msg->findInt32(WHATPARAM_BUFFERING_UPDATE, &fillLevel)) {
        SL_LOGD("GenericMediaPlayer::onBufferingUpdate(fillLevel=%d)", fillLevel);

        Mutex::Autolock _l(mSettingsLock);
        mCacheFill = fillLevel;
        // handle cache fill update
        if (mCacheFill - mLastNotifiedCacheFill >= mCacheFillNotifThreshold) {
            notifyCacheFill();
        }
        // handle prefetch status update
        //   compute how much time ahead of position is buffered
        int durationMsec, positionMsec = -1;
        if ((mStateFlags & kFlagPrepared) && (mPlayer != 0)
                && (OK == mPlayer->getDuration(&durationMsec))
                        && (OK == mPlayer->getCurrentPosition(&positionMsec))) {
            if ((-1 != durationMsec) && (-1 != positionMsec)) {
                // evaluate prefetch status based on buffer time thresholds
                int64_t bufferedDurationMsec = (durationMsec * fillLevel / 100) - positionMsec;
                CacheStatus_t newCacheStatus = mCacheStatus;
                if (bufferedDurationMsec > DURATION_CACHED_HIGH_MS) {
                    newCacheStatus = kStatusHigh;
                } else if (bufferedDurationMsec > DURATION_CACHED_MED_MS) {
                    newCacheStatus = kStatusEnough;
                } else if (bufferedDurationMsec > DURATION_CACHED_LOW_MS) {
                    newCacheStatus = kStatusIntermediate;
                } else if (bufferedDurationMsec == 0) {
                    newCacheStatus = kStatusEmpty;
                } else {
                    newCacheStatus = kStatusLow;
                }

                if (newCacheStatus != mCacheStatus) {
                    mCacheStatus = newCacheStatus;
                    notifyStatus();
                }
            }
        }
    } else {
        SL_LOGV("GenericMediaPlayer::onBufferingUpdate(fillLevel=unknown)");
    }
}


//--------------------------------------------------
/**
 * called from GenericMediaPlayer::onPrepare after the MediaPlayer mPlayer is prepared successfully
 * pre-conditions:
 *  mPlayer != 0
 *  mPlayer is prepared successfully
 */
void GenericMediaPlayer::afterMediaPlayerPreparedSuccessfully() {
    SL_LOGV("GenericMediaPlayer::afterMediaPlayerPrepared()");
    assert(mPlayer != 0);
    assert(mStateFlags & kFlagPrepared);
    // Mark this player as prepared successfully, so safe to directly call getCurrentPosition
    {
        Mutex::Autolock _l(mPreparedPlayerLock);
        assert(mPreparedPlayer == 0);
        mPreparedPlayer = mPlayer;
    }
    // retrieve channel count
    int32_t channelCount;
    Parcel *reply = new Parcel();
    status_t status = mPlayer->getParameter(KEY_PARAMETER_AUDIO_CHANNEL_COUNT, reply);
    if (status == NO_ERROR) {
        channelCount = reply->readInt32();
    } else {
        // FIXME MPEG-2 TS doesn't yet implement this key, so default to stereo
        channelCount = 2;
    }
    if (UNKNOWN_NUMCHANNELS != channelCount) {
        // now that we know the channel count, re-calculate the volumes
        notify(PLAYEREVENT_CHANNEL_COUNT, channelCount, true /*async*/);
    } else {
        LOGW("channel count is still unknown after prepare");
    }
    delete reply;
    // retrieve duration
    {
        int msec = 0;
        if (OK == mPlayer->getDuration(&msec)) {
            Mutex::Autolock _l(mSettingsLock);
            mDurationMsec = msec;
        }
    }
    // now that we have a MediaPlayer, set the looping flag
    if (mStateFlags & kFlagLooping) {
        (void) mPlayer->setLooping(1);
    }
    // when the MediaPlayer mPlayer is prepared, there is "sufficient data" in the playback buffers
    // if the data source was local, and the buffers are considered full so we need to notify that
    bool isLocalSource = true;
    if (kDataLocatorUri == mDataLocatorType) {
        isLocalSource = !isDistantProtocol(mDataLocator.uriRef);
    }
    if (isLocalSource) {
        SL_LOGD("media player prepared on local source");
        {
            Mutex::Autolock _l(mSettingsLock);
            mCacheStatus = kStatusHigh;
            mCacheFill = 1000;
            notifyStatus();
            notifyCacheFill();
        }
    } else {
        SL_LOGD("media player prepared on non-local source");
    }
}


//--------------------------------------------------
// If player is prepared successfully, set output parameter to that reference, otherwise NULL
void GenericMediaPlayer::getPreparedPlayer(sp<IMediaPlayer> &preparedPlayer)
{
    Mutex::Autolock _l(mPreparedPlayerLock);
    preparedPlayer = mPreparedPlayer;
}

} // namespace android
