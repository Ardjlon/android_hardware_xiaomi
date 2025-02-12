/*
 * Copyright (C) 2020 The Android Open Source Project
 * Copyright (C) 2020 The PixelExperience Project
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

#define LOG_TAG "powerhal-libperfmgr"

#include "Power.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <perfmgr/HintManager.h>
#include <utils/Log.h>

#include <mutex>

#include "PowerHintSession.h"
#include "PowerSessionManager.h"

#include <linux/input.h>

constexpr int kWakeupModeOff = 4;
constexpr int kWakeupModeOn = 5;

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

using ::aidl::google::hardware::power::impl::pixel::PowerHintSession;
using ::android::perfmgr::HintManager;

#ifdef MODE_EXT
extern bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return);
extern bool setDeviceSpecificMode(Mode type, bool enabled);
#endif

constexpr char kPowerHalStateProp[] = "vendor.powerhal.state";
constexpr char kPowerHalAudioProp[] = "vendor.powerhal.audio";
constexpr char kPowerHalRenderingProp[] = "vendor.powerhal.rendering";

Power::Power()
    : mInteractionHandler(nullptr),
      mSustainedPerfModeOn(false),
      mPathCached(false) {
    mInteractionHandler = std::make_unique<InteractionHandler>();
    mInteractionHandler->Init();

    std::string state = ::android::base::GetProperty(kPowerHalStateProp, "");
    if (state == "SUSTAINED_PERFORMANCE") {
        LOG(INFO) << "Initialize with SUSTAINED_PERFORMANCE on";
        HintManager::GetInstance()->DoHint("SUSTAINED_PERFORMANCE");
        mSustainedPerfModeOn = true;
    } else {
        LOG(INFO) << "Initialize PowerHAL";
    }

    state = ::android::base::GetProperty(kPowerHalAudioProp, "");
    if (state == "AUDIO_STREAMING_LOW_LATENCY") {
        LOG(INFO) << "Initialize with AUDIO_LOW_LATENCY on";
        HintManager::GetInstance()->DoHint(state);
    }

    state = ::android::base::GetProperty(kPowerHalRenderingProp, "");
    if (state == "EXPENSIVE_RENDERING") {
        LOG(INFO) << "Initialize with EXPENSIVE_RENDERING on";
        HintManager::GetInstance()->DoHint("EXPENSIVE_RENDERING");
    }
}


int Power::open_ts_input() {
    if (mPathCached) {
        LOG(DEBUG) << "Using cached DT2W path.";
        return (open(mDt2wPath, O_RDWR));
    }

    int fd = -1;
    DIR *dir = opendir("/dev/input");

    if (dir != NULL) {
        struct dirent *ent;

        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_CHR) {
                char absolute_path[PATH_MAX] = {0};
                char name[80] = {0};

                strcpy(absolute_path, "/dev/input/");
                strcat(absolute_path, ent->d_name);

                fd = open(absolute_path, O_RDWR);
                if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) > 0) {
                    if (strcmp(name, "atmel_mxt_ts") == 0 || strcmp(name, "fts_ts") == 0 ||
                            strcmp(name, "fts") == 0 || strcmp(name, "ft5x46") == 0 ||
                            strcmp(name, "synaptics_dsx") == 0 ||
                            strcmp(name, "NVTCapacitiveTouchScreen") == 0) {
                        // cache the dt2w node after finding a match
                        LOG(INFO) << "Found and cached a valid DT2W node: " << absolute_path;
                        strncpy(mDt2wPath, absolute_path, PATH_MAX);
                        mPathCached = true;
                        break;
                    }
                }

                close(fd);
                fd = -1;
            }
        }

        closedir(dir);
    }

    return fd;
}

void Power::handle_dt2w(bool enabled) {
    char buf[80];
    int len;

    int fd = open_ts_input();
    if (fd == -1) {
        ALOGW("DT2W won't work because no supported touchscreen input devices were found");
        return;
    }
    struct input_event ev;
    ev.type = EV_SYN;
    ev.code = SYN_CONFIG;
    ev.value = enabled ? kWakeupModeOn : kWakeupModeOff;

    len = write(fd, &ev, sizeof(ev));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to fd %d: %s\n", fd, buf);
        // invalidate the dt2w path cache
        LOG(INFO) << "Invaliding the DT2W node cache.";
        mPathCached = false;
    }
    close(fd);
}

ndk::ScopedAStatus Power::setMode(Mode type, bool enabled) {
    LOG(DEBUG) << "Power setMode: " << toString(type) << " to: " << enabled;
    if (HintManager::GetInstance()->GetAdpfProfile() &&
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs > 0) {
        PowerSessionManager::getInstance()->updateHintMode(toString(type), enabled);
    }
#ifdef MODE_EXT
    if (setDeviceSpecificMode(type, enabled)) {
        return ndk::ScopedAStatus::ok();
    }
#endif
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
            handle_dt2w(enabled);
            break;
        case Mode::SUSTAINED_PERFORMANCE:
            if (enabled) {
                HintManager::GetInstance()->DoHint("SUSTAINED_PERFORMANCE");
            }
            mSustainedPerfModeOn = true;
            break;
        case Mode::LAUNCH:
            if (mSustainedPerfModeOn) {
                break;
            }
            [[fallthrough]];
        case Mode::FIXED_PERFORMANCE:
            [[fallthrough]];
        case Mode::EXPENSIVE_RENDERING:
            [[fallthrough]];
        case Mode::INTERACTIVE:
            [[fallthrough]];
        case Mode::DEVICE_IDLE:
            [[fallthrough]];
        case Mode::DISPLAY_INACTIVE:
            [[fallthrough]];
        case Mode::AUDIO_STREAMING_LOW_LATENCY:
            [[fallthrough]];
        default:
            if (enabled) {
                HintManager::GetInstance()->DoHint(toString(type));
            } else {
                HintManager::GetInstance()->EndHint(toString(type));
            }
            break;
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isModeSupported(Mode type, bool *_aidl_return) {
#ifdef MODE_EXT
    if (isDeviceSpecificModeSupported(type, _aidl_return)) {
        return ndk::ScopedAStatus::ok();
    }
#endif

    bool supported = HintManager::GetInstance()->IsHintSupported(toString(type));
    // DOUBLE_TAP_TO_WAKE handled insides PowerHAL specifically
    if (type == Mode::DOUBLE_TAP_TO_WAKE) {
        supported = true;
    }
    LOG(INFO) << "Power mode " << toString(type) << " isModeSupported: " << supported;
    *_aidl_return = supported;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::setBoost(Boost type, int32_t durationMs) {
    LOG(DEBUG) << "Power setBoost: " << toString(type) << " duration: " << durationMs;
    if (HintManager::GetInstance()->GetAdpfProfile() &&
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs > 0) {
        PowerSessionManager::getInstance()->updateHintBoost(toString(type), durationMs);
    }
    switch (type) {
        case Boost::INTERACTION:
            if (mSustainedPerfModeOn) {
                break;
            }
            mInteractionHandler->Acquire(durationMs);
            break;
        case Boost::DISPLAY_UPDATE_IMMINENT:
            [[fallthrough]];
        case Boost::ML_ACC:
            [[fallthrough]];
        case Boost::AUDIO_LAUNCH:
            [[fallthrough]];
        default:
            if (mSustainedPerfModeOn) {
                break;
            }
            if (durationMs > 0) {
                HintManager::GetInstance()->DoHint(toString(type),
                                                   std::chrono::milliseconds(durationMs));
            } else if (durationMs == 0) {
                HintManager::GetInstance()->DoHint(toString(type));
            } else {
                HintManager::GetInstance()->EndHint(toString(type));
            }
            break;
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isBoostSupported(Boost type, bool *_aidl_return) {
    bool supported = HintManager::GetInstance()->IsHintSupported(toString(type));
    LOG(INFO) << "Power boost " << toString(type) << " isBoostSupported: " << supported;
    *_aidl_return = supported;
    return ndk::ScopedAStatus::ok();
}

constexpr const char *boolToString(bool b) {
    return b ? "true" : "false";
}

binder_status_t Power::dump(int fd, const char **, uint32_t) {
    std::string buf(::android::base::StringPrintf(
            "HintManager Running: %s\n"
            "SustainedPerformanceMode: %s\n",
            boolToString(HintManager::GetInstance()->IsRunning()),
            boolToString(mSustainedPerfModeOn)));
    // Dump nodes through libperfmgr
    HintManager::GetInstance()->DumpToFd(fd);
    PowerSessionManager::getInstance()->dumpToFd(fd);
    if (!::android::base::WriteStringToFd(buf, fd)) {
        PLOG(ERROR) << "Failed to dump state to fd";
    }
    fsync(fd);
    return STATUS_OK;
}

ndk::ScopedAStatus Power::createHintSession(int32_t tgid, int32_t uid,
                                            const std::vector<int32_t> &threadIds,
                                            int64_t durationNanos,
                                            std::shared_ptr<IPowerHintSession> *_aidl_return) {
    if (!HintManager::GetInstance()->GetAdpfProfile() ||
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs <= 0) {
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (threadIds.size() == 0) {
        LOG(ERROR) << "Error: threadIds.size() shouldn't be " << threadIds.size();
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::shared_ptr<IPowerHintSession> session = ndk::SharedRefBase::make<PowerHintSession>(
            tgid, uid, threadIds, durationNanos);
    *_aidl_return = session;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getHintSessionPreferredRate(int64_t *outNanoseconds) {
    *outNanoseconds = HintManager::GetInstance()->GetAdpfProfile()
                              ? HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs
                              : 0;
    if (*outNanoseconds <= 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl
