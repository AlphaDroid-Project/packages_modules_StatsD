/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "StatsService.h"

#include <android-base/file.h>
#include <android-base/strings.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/multiuser.h>
#include <private/android_filesystem_config.h>
#include <src/statsd_config.pb.h>
#include <src/uid_data.pb.h>
#include <statslog_statsd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <utils/String16.h>

#include "android-base/stringprintf.h"
#include "config/ConfigKey.h"
#include "config/ConfigManager.h"
#include "flags/FlagProvider.h"
#include "guardrail/StatsdStats.h"
#include "stats_log_util.h"
#include "storage/StorageManager.h"
#include "subscriber/SubscriberReporter.h"
#include "utils/DbUtils.h"

using namespace android;

using android::base::StringPrintf;
using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_MESSAGE;

using Status = ::ndk::ScopedAStatus;

namespace android {
namespace os {
namespace statsd {

constexpr const char* kPermissionDump = "android.permission.DUMP";

constexpr const char* kTracedProbesSid = "u:r:traced_probes:s0";

constexpr const char* kPermissionRegisterPullAtom = "android.permission.REGISTER_STATS_PULL_ATOM";

#define STATS_SERVICE_DIR "/data/misc/stats-service"

// for StatsDataDumpProto
const int FIELD_ID_REPORTS_LIST = 1;

static Status exception(int32_t code, const std::string& msg) {
    ALOGE("%s (%d)", msg.c_str(), code);
    return Status::fromExceptionCodeWithMessage(code, msg.c_str());
}

static bool checkPermission(const char* permission) {
    pid_t pid = AIBinder_getCallingPid();
    uid_t uid = AIBinder_getCallingUid();
    return checkPermissionForIds(permission, pid, uid);
}

Status checkUid(uid_t expectedUid) {
    uid_t uid = AIBinder_getCallingUid();
    if (uid == expectedUid || uid == AID_ROOT) {
        return Status::ok();
    } else {
        return exception(EX_SECURITY,
                         StringPrintf("UID %d is not expected UID %d", uid, expectedUid));
    }
}

#define ENFORCE_UID(uid) {                                        \
    Status status = checkUid((uid));                              \
    if (!status.isOk()) {                                         \
        return status;                                            \
    }                                                             \
}

Status checkSid(const char* expectedSid) {
    const char* sid = nullptr;
    if (__builtin_available(android __ANDROID_API_U__, *)) {
        sid = AIBinder_getCallingSid();
    }

    // root (which is the uid in tests for example) has all permissions.
    uid_t uid = AIBinder_getCallingUid();
    if (uid == AID_ROOT) {
        return Status::ok();
    }

    if (sid != nullptr && strcmp(expectedSid, sid) == 0) {
        return Status::ok();
    } else {
        return exception(EX_SECURITY,
                         StringPrintf("SID '%s' is not expected SID '%s'", sid, expectedSid));
    }
}

#define ENFORCE_SID(sid)                 \
    {                                    \
        Status status = checkSid((sid)); \
        if (!status.isOk()) {            \
            return status;               \
        }                                \
    }

StatsService::StatsService(const sp<UidMap>& uidMap, shared_ptr<LogEventQueue> queue,
                           const std::shared_ptr<LogEventFilter>& logEventFilter,
                           int initEventDelaySecs)
    : mUidMap(uidMap),
      mAnomalyAlarmMonitor(new AlarmMonitor(
              MIN_DIFF_TO_UPDATE_REGISTERED_ALARM_SECS,
              [this](const shared_ptr<IStatsCompanionService>& /*sc*/, int64_t timeMillis) {
                  mProcessor->setAnomalyAlarm(timeMillis);
                  StatsdStats::getInstance().noteRegisteredAnomalyAlarmChanged();
              },
              [this](const shared_ptr<IStatsCompanionService>& /*sc*/) {
                  mProcessor->cancelAnomalyAlarm();
                  StatsdStats::getInstance().noteRegisteredAnomalyAlarmChanged();
              })),
      mPeriodicAlarmMonitor(new AlarmMonitor(
              MIN_DIFF_TO_UPDATE_REGISTERED_ALARM_SECS,
              [](const shared_ptr<IStatsCompanionService>& sc, int64_t timeMillis) {
                  if (sc != nullptr) {
                      sc->setAlarmForSubscriberTriggering(timeMillis);
                      StatsdStats::getInstance().noteRegisteredPeriodicAlarmChanged();
                  }
              },
              [](const shared_ptr<IStatsCompanionService>& sc) {
                  if (sc != nullptr) {
                      sc->cancelAlarmForSubscriberTriggering();
                      StatsdStats::getInstance().noteRegisteredPeriodicAlarmChanged();
                  }
              })),
      mEventQueue(std::move(queue)),
      mLogEventFilter(logEventFilter),
      mBootCompleteTrigger({kBootCompleteTag, kUidMapReceivedTag, kAllPullersRegisteredTag},
                           [this]() { onStatsdInitCompleted(); }),
      mStatsCompanionServiceDeathRecipient(
              AIBinder_DeathRecipient_new(StatsService::statsCompanionServiceDied)),
      mInitEventDelaySecs(initEventDelaySecs) {
    mPullerManager = new StatsPullerManager();
    StatsPuller::SetUidMap(mUidMap);
    mConfigManager = new ConfigManager();
    mProcessor = new StatsLogProcessor(
            mUidMap, mPullerManager, mAnomalyAlarmMonitor, mPeriodicAlarmMonitor,
            getElapsedRealtimeNs(),
            [this](const ConfigKey& key) {
                shared_ptr<IPendingIntentRef> receiver = mConfigManager->GetConfigReceiver(key);
                if (receiver == nullptr) {
                    VLOG("Could not find a broadcast receiver for %s", key.ToString().c_str());
                    return false;
                }
                Status status = receiver->sendDataBroadcast(mProcessor->getLastReportTimeNs(key));
                if (status.isOk()) {
                    return true;
                }
                if (status.getExceptionCode() == EX_TRANSACTION_FAILED &&
                    status.getStatus() == STATUS_DEAD_OBJECT) {
                    mConfigManager->RemoveConfigReceiver(key, receiver);
                }
                VLOG("Failed to send a broadcast for receiver %s", key.ToString().c_str());
                return false;
            },
            [this](const int& uid, const vector<int64_t>& activeConfigs) {
                shared_ptr<IPendingIntentRef> receiver =
                    mConfigManager->GetActiveConfigsChangedReceiver(uid);
                if (receiver == nullptr) {
                    VLOG("Could not find receiver for uid %d", uid);
                    return false;
                }
                Status status = receiver->sendActiveConfigsChangedBroadcast(activeConfigs);
                if (status.isOk()) {
                    VLOG("StatsService::active configs broadcast succeeded for uid %d" , uid);
                    return true;
                }
                if (status.getExceptionCode() == EX_TRANSACTION_FAILED &&
                    status.getStatus() == STATUS_DEAD_OBJECT) {
                    mConfigManager->RemoveActiveConfigsChangedReceiver(uid, receiver);
                }
                VLOG("StatsService::active configs broadcast failed for uid %d", uid);
                return false;
            },
            [this](const ConfigKey& key, const string& delegatePackage,
                   const vector<int64_t>& restrictedMetrics) {
                set<string> configPackages;
                set<int32_t> delegateUids;
                for (const auto& kv : UidMap::sAidToUidMapping) {
                    if (kv.second == static_cast<uint32_t>(key.GetUid())) {
                        configPackages.insert(kv.first);
                    }
                    if (kv.first == delegatePackage) {
                        delegateUids.insert(kv.second);
                    }
                }
                if (configPackages.empty()) {
                    configPackages = mUidMap->getAppNamesFromUid(key.GetUid(), true);
                }
                if (delegateUids.empty()) {
                    delegateUids = mUidMap->getAppUid(delegatePackage);
                }
                mConfigManager->SendRestrictedMetricsBroadcast(configPackages, key.GetId(),
                                                               delegateUids, restrictedMetrics);
            },
            logEventFilter);

    mUidMap->setListener(mProcessor);
    mConfigManager->AddListener(mProcessor);

    init_system_properties();

    if (mEventQueue != nullptr) {
        mLogsReaderThread = std::make_unique<std::thread>([this] { readLogs(); });
    }
}

StatsService::~StatsService() {
    onStatsdInitCompletedHandlerTermination();
    if (mEventQueue != nullptr) {
        stopReadingLogs();
        mLogsReaderThread->join();
    }
}

/* Runs on a dedicated thread to process pushed events. */
void StatsService::readLogs() {
    // Read forever..... long live statsd
    while (1) {
        // Block until an event is available.
        auto event = mEventQueue->waitPop();

        // Below flag will be set when statsd is exiting and log event will be pushed to break
        // out of waitPop.
        if (mIsStopRequested) {
            break;
        }

        // Pass it to StatsLogProcess to all configs/metrics
        // At this point, the LogEventQueue is not blocked, so that the socketListener
        // can read events from the socket and write to buffer to avoid data drop.
        mProcessor->OnLogEvent(event.get());
        // The ShellSubscriber is only used by shell for local debugging.
        if (mShellSubscriber != nullptr) {
            mShellSubscriber->onLogEvent(*event);
        }
    }
}

void StatsService::init_system_properties() {
    mEngBuild = false;
    const prop_info* buildType = __system_property_find("ro.build.type");
    if (buildType != NULL) {
        __system_property_read_callback(buildType, init_build_type_callback, this);
    }
}

void StatsService::init_build_type_callback(void* cookie, const char* /*name*/, const char* value,
                                            uint32_t serial) {
    if (0 == strcmp("eng", value) || 0 == strcmp("userdebug", value)) {
        reinterpret_cast<StatsService*>(cookie)->mEngBuild = true;
    }
}

/**
 * Write data from statsd.
 * Format for statsdStats:  adb shell dumpsys stats --metadata [-v] [--proto]
 * Format for data report:  adb shell dumpsys stats [anything other than --metadata] [--proto]
 * Anything ending in --proto will be in proto format.
 * Anything without --metadata as the first argument will be report information.
 *     (bugreports call "adb shell dumpsys stats --dump-priority NORMAL -a --proto")
 * TODO: Come up with a more robust method of enacting <serviceutils/PriorityDumper.h>.
 */
status_t StatsService::dump(int fd, const char** args, uint32_t numArgs) {
    if (!checkPermission(kPermissionDump)) {
        return PERMISSION_DENIED;
    }

    int lastArg = numArgs - 1;
    bool asProto = false;
    if (lastArg >= 0 && string(args[lastArg]) == "--proto") { // last argument
        asProto = true;
        lastArg--;
    }
    if (numArgs > 0 && string(args[0]) == "--metadata") { // first argument
        // Request is to dump statsd stats.
        bool verbose = false;
        if (lastArg >= 0 && string(args[lastArg]) == "-v") {
            verbose = true;
            lastArg--;
        }
        dumpStatsdStats(fd, verbose, asProto);
    } else {
        // Request is to dump statsd report data.
        if (asProto) {
            dumpIncidentSection(fd);
        } else {
            dprintf(fd, "Non-proto format of stats data dump not available; see proto version.\n");
        }
    }

    return NO_ERROR;
}

/**
 * Write debugging data about statsd in text or proto format.
 */
void StatsService::dumpStatsdStats(int out, bool verbose, bool proto) {
    if (proto) {
        vector<uint8_t> data;
        StatsdStats::getInstance().dumpStats(&data, false); // does not reset statsdStats.
        for (size_t i = 0; i < data.size(); i ++) {
            dprintf(out, "%c", data[i]);
        }
    } else {
        StatsdStats::getInstance().dumpStats(out);
        mProcessor->dumpStates(out, verbose);
    }
}

/**
 * Write stats report data in StatsDataDumpProto incident section format.
 */
void StatsService::dumpIncidentSection(int out) {
    ProtoOutputStream proto;
    for (const ConfigKey& configKey : mConfigManager->GetAllConfigKeys()) {
        uint64_t reportsListToken =
                proto.start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_REPORTS_LIST);
        // Don't include the current bucket to avoid skipping buckets.
        // If we need to include the current bucket later, consider changing to NO_TIME_CONSTRAINTS
        // or other alternatives to avoid skipping buckets for pulled metrics.
        mProcessor->onDumpReport(configKey, getElapsedRealtimeNs(), getWallClockNs(),
                                 false /* includeCurrentBucket */, false /* erase_data */, ADB_DUMP,
                                 FAST, &proto);
        proto.end(reportsListToken);
        proto.flush(out);
        proto.clear();
    }
}

/**
 * Implementation of the adb shell cmd stats command.
 */
status_t StatsService::handleShellCommand(int in, int out, int err, const char** argv,
                                          uint32_t argc) {
    uid_t uid = AIBinder_getCallingUid();
    if (uid != AID_ROOT && uid != AID_SHELL) {
        return PERMISSION_DENIED;
    }

    Vector<String8> utf8Args;
    utf8Args.setCapacity(argc);
    for (uint32_t i = 0; i < argc; i++) {
        utf8Args.push(String8(argv[i]));
    }

    if (argc >= 1) {
        // adb shell cmd stats config ...
        if (!utf8Args[0].compare(String8("config"))) {
            return cmd_config(in, out, err, utf8Args);
        }

        if (!utf8Args[0].compare(String8("print-uid-map"))) {
            return cmd_print_uid_map(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("dump-report"))) {
            return cmd_dump_report(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("pull-source")) && argc > 1) {
            return cmd_print_pulled_metrics(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("send-broadcast"))) {
            return cmd_trigger_broadcast(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("print-stats"))) {
            return cmd_print_stats(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("meminfo"))) {
            return cmd_dump_memory_info(out);
        }

        if (!utf8Args[0].compare(String8("write-to-disk"))) {
            return cmd_write_data_to_disk(out);
        }

        if (!utf8Args[0].compare(String8("log-app-breadcrumb"))) {
            return cmd_log_app_breadcrumb(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("log-binary-push"))) {
            return cmd_log_binary_push(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("clear-puller-cache"))) {
            return cmd_clear_puller_cache(out);
        }

        if (!utf8Args[0].compare(String8("print-logs"))) {
            return cmd_print_logs(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("send-active-configs"))) {
            return cmd_trigger_active_config_broadcast(out, utf8Args);
        }

        if (!utf8Args[0].compare(String8("data-subscribe"))) {
            initShellSubscriber();
            int timeoutSec = -1;
            if (argc >= 2) {
                timeoutSec = atoi(utf8Args[1].c_str());
            }
            mShellSubscriber->startNewSubscription(in, out, timeoutSec);
            return NO_ERROR;
        }
    }

    print_cmd_help(out);
    return NO_ERROR;
}

void StatsService::print_cmd_help(int out) {
    dprintf(out,
            "usage: adb shell cmd stats print-stats-log [tag_required] "
            "[timestamp_nsec_optional]\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats meminfo\n");
    dprintf(out, "\n");
    dprintf(out, "  Prints the malloc debug information. You need to run the following first: \n");
    dprintf(out, "   # adb shell stop\n");
    dprintf(out, "   # adb shell setprop libc.debug.malloc.program statsd \n");
    dprintf(out, "   # adb shell setprop libc.debug.malloc.options backtrace \n");
    dprintf(out, "   # adb shell start\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats print-uid-map [PKG]\n");
    dprintf(out, "usage: adb shell cmd stats print-uid-map --with_certificate_hash\n");
    dprintf(out, "\n");
    dprintf(out, "  Prints the UID, app name, version mapping.\n");
    dprintf(out,
            "  PKG                         Optional package name to print the uids of the "
            "package\n");
    dprintf(out, "  --with_certificate_hash     Print package certificate hash in hex\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats pull-source ATOM_TAG [PACKAGE] \n");
    dprintf(out, "\n");
    dprintf(out, "  Prints the output of a pulled atom\n");
    dprintf(out, "  UID           The atom to pull\n");
    dprintf(out, "  PACKAGE       The package to pull from. Default is AID_SYSTEM\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats write-to-disk \n");
    dprintf(out, "\n");
    dprintf(out, "  Flushes all data on memory to disk.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats log-app-breadcrumb [UID] LABEL STATE\n");
    dprintf(out, "  Writes an AppBreadcrumbReported event to the statslog buffer.\n");
    dprintf(out, "  UID           The uid to use. It is only possible to pass a UID\n");
    dprintf(out, "                parameter on eng builds. If UID is omitted the calling\n");
    dprintf(out, "                uid is used.\n");
    dprintf(out, "  LABEL         Integer in [0, 15], as per atoms.proto.\n");
    dprintf(out, "  STATE         Integer in [0, 3], as per atoms.proto.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out,
            "usage: adb shell cmd stats log-binary-push NAME VERSION STAGING ROLLBACK_ENABLED "
            "LOW_LATENCY STATE EXPERIMENT_IDS\n");
    dprintf(out, "  Log a binary push state changed event.\n");
    dprintf(out, "  NAME                The train name.\n");
    dprintf(out, "  VERSION             The train version code.\n");
    dprintf(out, "  STAGING             If this train requires a restart.\n");
    dprintf(out, "  ROLLBACK_ENABLED    If rollback should be enabled for this install.\n");
    dprintf(out, "  LOW_LATENCY         If the train requires low latency monitoring.\n");
    dprintf(out, "  STATE               The status of the train push.\n");
    dprintf(out, "                      Integer value of the enum in atoms.proto.\n");
    dprintf(out, "  EXPERIMENT_IDS      Comma separated list of experiment ids.\n");
    dprintf(out, "                      Leave blank for none.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats config remove [UID] [NAME]\n");
    dprintf(out, "usage: adb shell cmd stats config update [UID] NAME\n");
    dprintf(out, "\n");
    dprintf(out, "  Adds, updates or removes a configuration. The proto should be in\n");
    dprintf(out, "  wire-encoded protobuf format and passed via stdin. If no UID and name is\n");
    dprintf(out, "  provided, then all configs will be removed from memory and disk.\n");
    dprintf(out, "\n");
    dprintf(out, "  UID           The uid to use. It is only possible to pass the UID\n");
    dprintf(out, "                parameter on eng builds. If UID is omitted the calling\n");
    dprintf(out, "                uid is used.\n");
    dprintf(out, "  NAME          The per-uid name to use\n");
    dprintf(out, "\n");
    dprintf(out, "\n              *Note: If both UID and NAME are omitted then all configs will\n");
    dprintf(out, "\n                     be removed from memory and disk!\n");
    dprintf(out, "\n");
    dprintf(out,
            "usage: adb shell cmd stats dump-report [UID] NAME [--keep_data] "
            "[--include_current_bucket] [--proto]\n");
    dprintf(out, "  Dump all metric data for a configuration.\n");
    dprintf(out, "  UID           The uid of the configuration. It is only possible to pass\n");
    dprintf(out, "                the UID parameter on eng builds. If UID is omitted the\n");
    dprintf(out, "                calling uid is used.\n");
    dprintf(out, "  NAME          The name of the configuration\n");
    dprintf(out, "  --keep_data   Do NOT erase the data upon dumping it.\n");
    dprintf(out, "  --proto       Print proto binary.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats send-broadcast [UID] NAME\n");
    dprintf(out, "  Send a broadcast that triggers the subscriber to fetch metrics.\n");
    dprintf(out, "  UID           The uid of the configuration. It is only possible to pass\n");
    dprintf(out, "                the UID parameter on eng builds. If UID is omitted the\n");
    dprintf(out, "                calling uid is used.\n");
    dprintf(out, "  NAME          The name of the configuration\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out,
            "usage: adb shell cmd stats send-active-configs [--uid=UID] [--configs] "
            "[NAME1] [NAME2] [NAME3..]\n");
    dprintf(out, "  Send a broadcast that informs the subscriber of the current active configs.\n");
    dprintf(out, "  --uid=UID     The uid of the configurations. It is only possible to pass\n");
    dprintf(out, "                the UID parameter on eng builds. If UID is omitted the\n");
    dprintf(out, "                calling uid is used.\n");
    dprintf(out, "  --configs     Send the list of configs in the name list instead of\n");
    dprintf(out, "                the currently active configs\n");
    dprintf(out, "  NAME LIST     List of configuration names to be included in the broadcast.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats print-stats\n");
    dprintf(out, "  Prints some basic stats.\n");
    dprintf(out, "  --proto       Print proto binary instead of string format.\n");
    dprintf(out, "\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats clear-puller-cache\n");
    dprintf(out, "  Clear cached puller data.\n");
    dprintf(out, "\n");
    dprintf(out, "usage: adb shell cmd stats print-logs\n");
    dprintf(out, "  Requires root privileges.\n");
    dprintf(out, "  Can be disabled by calling adb shell cmd stats print-logs 0\n");
}

status_t StatsService::cmd_trigger_broadcast(int out, Vector<String8>& args) {
    string name;
    bool good = false;
    int uid;
    const int argCount = args.size();
    if (argCount == 2) {
        // Automatically pick the UID
        uid = AIBinder_getCallingUid();
        name.assign(args[1].c_str(), args[1].size());
        good = true;
    } else if (argCount == 3) {
        good = getUidFromArgs(args, 1, uid);
        if (!good) {
            dprintf(out, "Invalid UID. Note that the metrics can only be dumped for "
                         "other UIDs on eng or userdebug builds.\n");
        }
        name.assign(args[2].c_str(), args[2].size());
    }
    if (!good) {
        print_cmd_help(out);
        return UNKNOWN_ERROR;
    }
    ConfigKey key(uid, StrToInt64(name));
    shared_ptr<IPendingIntentRef> receiver = mConfigManager->GetConfigReceiver(key);
    if (receiver == nullptr) {
        VLOG("Could not find receiver for %s, %s", args[1].c_str(), args[2].c_str());
        return UNKNOWN_ERROR;
    } else if (receiver->sendDataBroadcast(mProcessor->getLastReportTimeNs(key)).isOk()) {
        VLOG("StatsService::trigger broadcast succeeded to %s, %s", args[1].c_str(),
             args[2].c_str());
    } else {
        VLOG("StatsService::trigger broadcast failed to %s, %s", args[1].c_str(), args[2].c_str());
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t StatsService::cmd_trigger_active_config_broadcast(int out, Vector<String8>& args) {
    const int argCount = args.size();
    int uid;
    vector<int64_t> configIds;
    if (argCount == 1) {
        // Automatically pick the uid and send a broadcast that has no active configs.
        uid = AIBinder_getCallingUid();
        mProcessor->GetActiveConfigs(uid, configIds);
    } else {
        int curArg = 1;
        if(args[curArg].find("--uid=") == 0) {
            string uidArgStr(args[curArg].c_str());
            string uidStr = uidArgStr.substr(6);
            if (!getUidFromString(uidStr.c_str(), uid)) {
                dprintf(out, "Invalid UID. Note that the config can only be set for "
                             "other UIDs on eng or userdebug builds.\n");
                return UNKNOWN_ERROR;
            }
            curArg++;
        } else {
            uid = AIBinder_getCallingUid();
        }
        if (curArg == argCount || args[curArg] != "--configs") {
            VLOG("Reached end of args, or specify configs not set. Sending actual active configs,");
            mProcessor->GetActiveConfigs(uid, configIds);
        } else {
            // Flag specified, use the given list of configs.
            curArg++;
            for (int i = curArg; i < argCount; i++) {
                char* endp;
                int64_t configID = strtoll(args[i].c_str(), &endp, 10);
                if (endp == args[i].c_str() || *endp != '\0') {
                    dprintf(out, "Error parsing config ID.\n");
                    return UNKNOWN_ERROR;
                }
                VLOG("Adding config id %ld", static_cast<long>(configID));
                configIds.push_back(configID);
            }
        }
    }
    shared_ptr<IPendingIntentRef> receiver = mConfigManager->GetActiveConfigsChangedReceiver(uid);
    if (receiver == nullptr) {
        VLOG("Could not find receiver for uid %d", uid);
        return UNKNOWN_ERROR;
    } else if (receiver->sendActiveConfigsChangedBroadcast(configIds).isOk()) {
        VLOG("StatsService::trigger active configs changed broadcast succeeded for uid %d" , uid);
    } else {
        VLOG("StatsService::trigger active configs changed broadcast failed for uid %d", uid);
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t StatsService::cmd_config(int in, int out, int err, Vector<String8>& args) {
    const int argCount = args.size();
    if (argCount >= 2) {
        if (args[1] == "update" || args[1] == "remove") {
            bool good = false;
            int uid = -1;
            string name;

            if (argCount == 3) {
                // Automatically pick the UID
                uid = AIBinder_getCallingUid();
                name.assign(args[2].c_str(), args[2].size());
                good = true;
            } else if (argCount == 4) {
                good = getUidFromArgs(args, 2, uid);
                if (!good) {
                    dprintf(err, "Invalid UID. Note that the config can only be set for "
                                 "other UIDs on eng or userdebug builds.\n");
                }
                name.assign(args[3].c_str(), args[3].size());
            } else if (argCount == 2 && args[1] == "remove") {
                good = true;
            }

            if (!good) {
                // If arg parsing failed, print the help text and return an error.
                print_cmd_help(out);
                return UNKNOWN_ERROR;
            }

            if (args[1] == "update") {
                char* endp;
                int64_t configID = strtoll(name.c_str(), &endp, 10);
                if (endp == name.c_str() || *endp != '\0') {
                    dprintf(err, "Error parsing config ID.\n");
                    return UNKNOWN_ERROR;
                }

                // Read stream into buffer.
                string buffer;
                if (!android::base::ReadFdToString(in, &buffer)) {
                    dprintf(err, "Error reading stream for StatsConfig.\n");
                    return UNKNOWN_ERROR;
                }

                // Parse buffer.
                StatsdConfig config;
                if (!config.ParseFromString(buffer)) {
                    dprintf(err, "Error parsing proto stream for StatsConfig.\n");
                    return UNKNOWN_ERROR;
                }

                // Add / update the config.
                mConfigManager->UpdateConfig(ConfigKey(uid, configID), config);
            } else {
                if (argCount == 2) {
                    cmd_remove_all_configs(out);
                } else {
                    // Remove the config.
                    mConfigManager->RemoveConfig(ConfigKey(uid, StrToInt64(name)));
                }
            }

            return NO_ERROR;
        }
    }
    print_cmd_help(out);
    return UNKNOWN_ERROR;
}

status_t StatsService::cmd_dump_report(int out, const Vector<String8>& args) {
    if (mProcessor != nullptr) {
        int argCount = args.size();
        bool good = false;
        bool proto = false;
        bool includeCurrentBucket = false;
        bool eraseData = true;
        int uid;
        string name;
        if (!std::strcmp("--proto", args[argCount-1].c_str())) {
            proto = true;
            argCount -= 1;
        }
        if (!std::strcmp("--include_current_bucket", args[argCount-1].c_str())) {
            includeCurrentBucket = true;
            argCount -= 1;
        }
        if (!std::strcmp("--keep_data", args[argCount-1].c_str())) {
            eraseData = false;
            argCount -= 1;
        }
        if (argCount == 2) {
            // Automatically pick the UID
            uid = AIBinder_getCallingUid();
            name.assign(args[1].c_str(), args[1].size());
            good = true;
        } else if (argCount == 3) {
            good = getUidFromArgs(args, 1, uid);
            if (!good) {
                dprintf(out, "Invalid UID. Note that the metrics can only be dumped for "
                             "other UIDs on eng or userdebug builds.\n");
            }
            name.assign(args[2].c_str(), args[2].size());
        }
        if (good) {
            vector<uint8_t> data;
            mProcessor->onDumpReport(ConfigKey(uid, StrToInt64(name)), getElapsedRealtimeNs(),
                                     getWallClockNs(), includeCurrentBucket, eraseData, ADB_DUMP,
                                     NO_TIME_CONSTRAINTS, &data);
            if (proto) {
                for (size_t i = 0; i < data.size(); i ++) {
                    dprintf(out, "%c", data[i]);
                }
            } else {
                dprintf(out, "Non-proto stats data dump not currently supported.\n");
            }
            return android::OK;
        } else {
            // If arg parsing failed, print the help text and return an error.
            print_cmd_help(out);
            return UNKNOWN_ERROR;
        }
    } else {
        dprintf(out, "Log processor does not exist...\n");
        return UNKNOWN_ERROR;
    }
}

status_t StatsService::cmd_print_stats(int out, const Vector<String8>& args) {
    int argCount = args.size();
    bool proto = false;
    if (!std::strcmp("--proto", args[argCount-1].c_str())) {
        proto = true;
        argCount -= 1;
    }
    StatsdStats& statsdStats = StatsdStats::getInstance();
    if (proto) {
        vector<uint8_t> data;
        statsdStats.dumpStats(&data, false); // does not reset statsdStats.
        for (size_t i = 0; i < data.size(); i ++) {
            dprintf(out, "%c", data[i]);
        }

    } else {
        vector<ConfigKey> configs = mConfigManager->GetAllConfigKeys();
        for (const ConfigKey& key : configs) {
            dprintf(out, "Config %s uses %zu bytes\n", key.ToString().c_str(),
                    mProcessor->GetMetricsSize(key));
        }
        statsdStats.dumpStats(out);
    }
    return NO_ERROR;
}

status_t StatsService::cmd_print_uid_map(int out, const Vector<String8>& args) {
    if (args.size() > 1) {
        if (!std::strcmp("--with_certificate_hash", args[1].c_str())) {
            mUidMap->printUidMap(out, /* includeCertificateHash */ true);
        } else {
            string pkg;
            pkg.assign(args[1].c_str(), args[1].size());
            auto uids = mUidMap->getAppUid(pkg);
            dprintf(out, "%s -> [ ", pkg.c_str());
            for (const auto& uid : uids) {
                dprintf(out, "%d ", uid);
            }
            dprintf(out, "]\n");
        }
    } else {
        mUidMap->printUidMap(out, /* includeCertificateHash */ false);
    }
    return NO_ERROR;
}

status_t StatsService::cmd_write_data_to_disk(int out) {
    dprintf(out, "Writing data to disk\n");
    mProcessor->WriteDataToDisk(ADB_DUMP, NO_TIME_CONSTRAINTS, getElapsedRealtimeNs(),
                                getWallClockNs());
    return NO_ERROR;
}

status_t StatsService::cmd_log_app_breadcrumb(int out, const Vector<String8>& args) {
    bool good = false;
    int32_t uid;
    int32_t label;
    int32_t state;
    const int argCount = args.size();
    if (argCount == 3) {
        // Automatically pick the UID
        uid = AIBinder_getCallingUid();
        label = atoi(args[1].c_str());
        state = atoi(args[2].c_str());
        good = true;
    } else if (argCount == 4) {
        good = getUidFromArgs(args, 1, uid);
        if (!good) {
            dprintf(out,
                    "Invalid UID. Note that selecting a UID for writing AppBreadcrumb can only be "
                    "done for other UIDs on eng or userdebug builds.\n");
        }
        label = atoi(args[2].c_str());
        state = atoi(args[3].c_str());
    }
    if (good) {
        dprintf(out, "Logging AppBreadcrumbReported(%d, %d, %d) to statslog.\n", uid, label, state);
        util::stats_write(util::APP_BREADCRUMB_REPORTED, uid, label, state);
    } else {
        print_cmd_help(out);
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t StatsService::cmd_log_binary_push(int out, const Vector<String8>& args) {
    // Security checks are done in the sendBinaryPushStateChanged atom.
    const int argCount = args.size();
    if (argCount != 7 && argCount != 8) {
        dprintf(out, "Incorrect number of argument supplied\n");
        return UNKNOWN_ERROR;
    }
    string trainName = string(args[1].c_str());
    int64_t trainVersion = strtoll(args[2].c_str(), nullptr, 10);
    int32_t state = atoi(args[6].c_str());
    vector<int64_t> experimentIds;
    if (argCount == 8) {
        vector<string> experimentIdsString = android::base::Split(string(args[7].c_str()), ",");
        for (string experimentIdString : experimentIdsString) {
            int64_t experimentId = strtoll(experimentIdString.c_str(), nullptr, 10);
            experimentIds.push_back(experimentId);
        }
    }
    dprintf(out, "Logging BinaryPushStateChanged\n");
    vector<uint8_t> experimentIdBytes;
    writeExperimentIdsToProto(experimentIds, &experimentIdBytes);
    LogEvent event(trainName, trainVersion, args[3].c_str(), args[4].c_str(), args[5].c_str(),
                   state, experimentIdBytes, 0);
    mProcessor->OnLogEvent(&event);
    return NO_ERROR;
}

status_t StatsService::cmd_print_pulled_metrics(int out, const Vector<String8>& args) {
    int s = atoi(args[1].c_str());
    vector<int32_t> uids;
    if (args.size() > 2) {
        string package = string(args[2].c_str());
        auto it = UidMap::sAidToUidMapping.find(package);
        if (it != UidMap::sAidToUidMapping.end()) {
            uids.push_back(it->second);
        } else {
            set<int32_t> uids_set = mUidMap->getAppUid(package);
            uids.insert(uids.end(), uids_set.begin(), uids_set.end());
        }
    } else {
        uids.push_back(AID_SYSTEM);
    }
    vector<shared_ptr<LogEvent>> stats;
    if (mPullerManager->Pull(s, uids, getElapsedRealtimeNs(), &stats)) {
        for (const auto& it : stats) {
            dprintf(out, "Pull from %d: %s\n", s, it->ToString().c_str());
        }
        dprintf(out, "Pull from %d: Received %zu elements\n", s, stats.size());
        return NO_ERROR;
    }
    return UNKNOWN_ERROR;
}

status_t StatsService::cmd_remove_all_configs(int out) {
    dprintf(out, "Removing all configs...\n");
    VLOG("StatsService::cmd_remove_all_configs was called");
    mConfigManager->RemoveAllConfigs();
    StorageManager::deleteAllFiles(STATS_SERVICE_DIR);
    return NO_ERROR;
}

status_t StatsService::cmd_dump_memory_info(int out) {
    dprintf(out, "meminfo not available.\n");
    return NO_ERROR;
}

status_t StatsService::cmd_clear_puller_cache(int out) {
    VLOG("StatsService::cmd_clear_puller_cache with Pid %i, Uid %i",
            AIBinder_getCallingPid(), AIBinder_getCallingUid());
    if (checkPermission(kPermissionDump)) {
        int cleared = mPullerManager->ForceClearPullerCache();
        dprintf(out, "Puller removed %d cached data!\n", cleared);
        return NO_ERROR;
    } else {
        return PERMISSION_DENIED;
    }
}

status_t StatsService::cmd_print_logs(int out, const Vector<String8>& args) {
    Status status = checkUid(AID_ROOT);
    if (!status.isOk()) {
        return PERMISSION_DENIED;
    }

    VLOG("StatsService::cmd_print_logs with pid %i, uid %i", AIBinder_getCallingPid(),
         AIBinder_getCallingUid());
    bool enabled = true;
    if (args.size() >= 2) {
        enabled = atoi(args[1].c_str()) != 0;
    }
    mProcessor->setPrintLogs(enabled);
    return NO_ERROR;
}

bool StatsService::getUidFromArgs(const Vector<String8>& args, size_t uidArgIndex, int32_t& uid) {
    return getUidFromString(args[uidArgIndex].c_str(), uid);
}

bool StatsService::getUidFromString(const char* s, int32_t& uid) {
    if (*s == '\0') {
        return false;
    }
    char* endc = NULL;
    int64_t longUid = strtol(s, &endc, 0);
    if (*endc != '\0') {
        return false;
    }
    int32_t goodUid = static_cast<int32_t>(longUid);
    if (longUid < 0 || static_cast<uint64_t>(longUid) != static_cast<uid_t>(goodUid)) {
        return false;  // It was not of uid_t type.
    }
    uid = goodUid;

    int32_t callingUid = AIBinder_getCallingUid();
    return mEngBuild // UserDebug/EngBuild are allowed to impersonate uids.
            || (callingUid == goodUid) // Anyone can 'impersonate' themselves.
            || (callingUid == AID_ROOT && goodUid == AID_SHELL); // ROOT can impersonate SHELL.
}

Status StatsService::informAllUidData(const ScopedFileDescriptor& fd) {
    ENFORCE_UID(AID_SYSTEM);

    // Parse fd into proto.
    UidData uidData;
    if (!uidData.ParseFromFileDescriptor(fd.get())) {
        return exception(EX_ILLEGAL_ARGUMENT, "Error parsing proto stream for UidData.");
    }

    mUidMap->updateMap(getElapsedRealtimeNs(), uidData);
    mBootCompleteTrigger.markComplete(kUidMapReceivedTag);
    VLOG("StatsService::informAllUidData UidData proto parsed successfully.");
    return Status::ok();
}

Status StatsService::informOnePackage(const string& app, int32_t uid, int64_t version,
                                      const string& versionString, const string& installer,
                                      const vector<uint8_t>& certificateHash) {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::informOnePackage was called");

    mUidMap->updateApp(getElapsedRealtimeNs(), app, uid, version, versionString, installer,
                       certificateHash);
    return Status::ok();
}

Status StatsService::informOnePackageRemoved(const string& app, int32_t uid) {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::informOnePackageRemoved was called");
    mUidMap->removeApp(getElapsedRealtimeNs(), app, uid);
    mConfigManager->RemoveConfigs(uid);
    return Status::ok();
}

Status StatsService::informAnomalyAlarmFired() {
    ENFORCE_UID(AID_SYSTEM);
    // Anomaly alarms are handled internally now. This code should be fully deleted.
    return Status::ok();
}

Status StatsService::informAlarmForSubscriberTriggeringFired() {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::informAlarmForSubscriberTriggeringFired was called");
    int64_t currentTimeSec = getElapsedRealtimeSec();
    std::unordered_set<sp<const InternalAlarm>, SpHash<InternalAlarm>> alarmSet =
            mPeriodicAlarmMonitor->popSoonerThan(static_cast<uint32_t>(currentTimeSec));
    if (alarmSet.size() > 0) {
        VLOG("Found periodic alarm fired.");
        mProcessor->onPeriodicAlarmFired(currentTimeSec * NS_PER_SEC, alarmSet);
    } else {
        ALOGW("Cannot find an periodic alarm that fired. Perhaps it was recently cancelled.");
    }
    return Status::ok();
}

Status StatsService::informPollAlarmFired() {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::informPollAlarmFired was called");
    mProcessor->informPullAlarmFired(getElapsedRealtimeNs());
    VLOG("StatsService::informPollAlarmFired succeeded");
    return Status::ok();
}

Status StatsService::systemRunning() {
    ENFORCE_UID(AID_SYSTEM);

    // When system_server is up and running, schedule the dropbox task to run.
    VLOG("StatsService::systemRunning");
    sayHiToStatsCompanion();
    return Status::ok();
}

Status StatsService::informDeviceShutdown() {
    ENFORCE_UID(AID_SYSTEM);
    VLOG("StatsService::informDeviceShutdown");
    onStatsdInitCompletedHandlerTermination();
    int64_t elapsedRealtimeNs = getElapsedRealtimeNs();
    int64_t wallClockNs = getWallClockNs();
    mProcessor->WriteDataToDisk(DEVICE_SHUTDOWN, FAST, elapsedRealtimeNs, wallClockNs);
    mProcessor->SaveActiveConfigsToDisk(elapsedRealtimeNs);
    mProcessor->SaveMetadataToDisk(wallClockNs, elapsedRealtimeNs);
    return Status::ok();
}

void StatsService::sayHiToStatsCompanion() {
    shared_ptr<IStatsCompanionService> statsCompanion = getStatsCompanionService();
    if (statsCompanion != nullptr) {
        VLOG("Telling statsCompanion that statsd is ready");
        statsCompanion->statsdReady();
    } else {
        VLOG("Could not access statsCompanion");
    }
}

Status StatsService::statsCompanionReady() {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::statsCompanionReady was called");
    shared_ptr<IStatsCompanionService> statsCompanion = getStatsCompanionService();
    if (statsCompanion == nullptr) {
        return exception(EX_NULL_POINTER,
                         "StatsCompanion unavailable despite it contacting statsd.");
    }
    VLOG("StatsService::statsCompanionReady linking to statsCompanion.");
    AIBinder_linkToDeath(statsCompanion->asBinder().get(),
                         mStatsCompanionServiceDeathRecipient.get(), this);
    mPullerManager->SetStatsCompanionService(statsCompanion);
    mAnomalyAlarmMonitor->setStatsCompanionService(statsCompanion);
    mPeriodicAlarmMonitor->setStatsCompanionService(statsCompanion);
    return Status::ok();
}

Status StatsService::bootCompleted() {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::bootCompleted was called");
    mBootCompleteTrigger.markComplete(kBootCompleteTag);
    return Status::ok();
}

void StatsService::onStatsdInitCompleted() {
    if (mInitEventDelaySecs > 0) {
        // The hard-coded delay is determined based on perfetto traces evaluation
        // for statsd during the boot.
        // The delay is required to properly process event storm which often has place
        // after device boot.
        // This function is called from a dedicated thread without holding locks, so sleeping is ok.
        // See MultiConditionTrigger::markComplete() executorThread for details
        // For more details see http://b/277958338

        std::unique_lock<std::mutex> lk(mStatsdInitCompletedHandlerTerminationFlagMutex);
        if (mStatsdInitCompletedHandlerTerminationFlag.wait_for(
                    lk, std::chrono::seconds(mInitEventDelaySecs),
                    [this] { return mStatsdInitCompletedHandlerTerminationRequested; })) {
            VLOG("StatsService::onStatsdInitCompleted() Early termination is requested");
            return;
        }
    }

    mProcessor->onStatsdInitCompleted(getElapsedRealtimeNs());
}

void StatsService::Startup() {
    mConfigManager->Startup();
    int64_t wallClockNs = getWallClockNs();
    int64_t elapsedRealtimeNs = getElapsedRealtimeNs();
    mProcessor->LoadActiveConfigsFromDisk();
    mProcessor->LoadMetadataFromDisk(wallClockNs, elapsedRealtimeNs);
    mProcessor->EnforceDataTtls(wallClockNs, elapsedRealtimeNs);
}

void StatsService::Terminate() {
    ALOGI("StatsService::Terminating");
    onStatsdInitCompletedHandlerTermination();
    if (mProcessor != nullptr) {
        int64_t elapsedRealtimeNs = getElapsedRealtimeNs();
        int64_t wallClockNs = getWallClockNs();
        mProcessor->WriteDataToDisk(TERMINATION_SIGNAL_RECEIVED, FAST, elapsedRealtimeNs,
                                    wallClockNs);
        mProcessor->SaveActiveConfigsToDisk(elapsedRealtimeNs);
        mProcessor->SaveMetadataToDisk(wallClockNs, elapsedRealtimeNs);
    }
}

void StatsService::onStatsdInitCompletedHandlerTermination() {
    {
        std::unique_lock<std::mutex> lk(mStatsdInitCompletedHandlerTerminationFlagMutex);
        mStatsdInitCompletedHandlerTerminationRequested = true;
    }
    mStatsdInitCompletedHandlerTerminationFlag.notify_all();
}

// Test only interface!!!
void StatsService::OnLogEvent(LogEvent* event) {
    mProcessor->OnLogEvent(event);
    if (mShellSubscriber != nullptr) {
        mShellSubscriber->onLogEvent(*event);
    }
}

Status StatsService::getData(int64_t key, const int32_t callingUid, vector<uint8_t>* output) {
    ENFORCE_UID(AID_SYSTEM);
    getDataChecked(key, callingUid, output);
    return Status::ok();
}

Status StatsService::getDataFd(int64_t key, const int32_t callingUid,
                               const ScopedFileDescriptor& fd) {
    ENFORCE_UID(AID_SYSTEM);
    vector<uint8_t> reportData;
    getDataChecked(key, callingUid, &reportData);

    if (reportData.size() >= std::numeric_limits<int32_t>::max()) {
        ALOGE("Report size is infeasible big and can not be returned");
        return exception(EX_ILLEGAL_STATE, "Report size is infeasible big.");
    }

    const uint32_t bytesToWrite = static_cast<uint32_t>(reportData.size());
    VLOG("StatsService::getDataFd report size %d", bytesToWrite);

    // write 4 bytes of report size for correct buffer allocation
    const uint32_t bytesToWriteBE = htonl(bytesToWrite);
    if (!android::base::WriteFully(fd.get(), &bytesToWriteBE, sizeof(uint32_t))) {
        return exception(EX_ILLEGAL_STATE, "Failed to write report data size to file descriptor");
    }
    if (!android::base::WriteFully(fd.get(), reportData.data(), reportData.size())) {
        return exception(EX_ILLEGAL_STATE, "Failed to write report data to file descriptor");
    }

    VLOG("StatsService::getDataFd written");
    return Status::ok();
}

void StatsService::getDataChecked(int64_t key, const int32_t callingUid, vector<uint8_t>* output) {
    VLOG("StatsService::getData with Uid %i", callingUid);
    ConfigKey configKey(callingUid, key);
    // The dump latency does not matter here since we do not include the current bucket, we do not
    // need to pull any new data anyhow.
    mProcessor->onDumpReport(configKey, getElapsedRealtimeNs(), getWallClockNs(),
                             false /* include_current_bucket*/, true /* erase_data */,
                             GET_DATA_CALLED, FAST, output);
}

Status StatsService::getMetadata(vector<uint8_t>* output) {
    ENFORCE_UID(AID_SYSTEM);

    StatsdStats::getInstance().dumpStats(output, false); // Don't reset the counters.
    return Status::ok();
}

Status StatsService::addConfiguration(int64_t key, const vector <uint8_t>& config,
                                      const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);

    if (addConfigurationChecked(callingUid, key, config)) {
        return Status::ok();
    } else {
        return exception(EX_ILLEGAL_ARGUMENT, "Could not parse malformatted StatsdConfig.");
    }
}

bool StatsService::addConfigurationChecked(int uid, int64_t key, const vector<uint8_t>& config) {
    ConfigKey configKey(uid, key);
    StatsdConfig cfg;
    if (config.size() > 0) {  // If the config is empty, skip parsing.
        if (!cfg.ParseFromArray(&config[0], config.size())) {
            return false;
        }
    }
    mConfigManager->UpdateConfig(configKey, cfg);
    return true;
}

Status StatsService::removeDataFetchOperation(int64_t key,
                                              const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);
    ConfigKey configKey(callingUid, key);
    mConfigManager->RemoveConfigReceiver(configKey);
    return Status::ok();
}

Status StatsService::setDataFetchOperation(int64_t key,
                                           const shared_ptr<IPendingIntentRef>& pir,
                                           const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);

    ConfigKey configKey(callingUid, key);
    mConfigManager->SetConfigReceiver(configKey, pir);
    if (StorageManager::hasConfigMetricsReport(configKey)) {
        VLOG("StatsService::setDataFetchOperation marking configKey %s to dump reports on disk",
             configKey.ToString().c_str());
        mProcessor->noteOnDiskData(configKey);
    }
    return Status::ok();
}

Status StatsService::setActiveConfigsChangedOperation(const shared_ptr<IPendingIntentRef>& pir,
                                                      const int32_t callingUid,
                                                      vector<int64_t>* output) {
    ENFORCE_UID(AID_SYSTEM);

    mConfigManager->SetActiveConfigsChangedReceiver(callingUid, pir);
    if (output != nullptr) {
        mProcessor->GetActiveConfigs(callingUid, *output);
    } else {
        ALOGW("StatsService::setActiveConfigsChanged output was nullptr");
    }
    return Status::ok();
}

Status StatsService::removeActiveConfigsChangedOperation(const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);

    mConfigManager->RemoveActiveConfigsChangedReceiver(callingUid);
    return Status::ok();
}

Status StatsService::removeConfiguration(int64_t key, const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);

    ConfigKey configKey(callingUid, key);
    mConfigManager->RemoveConfig(configKey);
    return Status::ok();
}

Status StatsService::setBroadcastSubscriber(int64_t configId,
                                            int64_t subscriberId,
                                            const shared_ptr<IPendingIntentRef>& pir,
                                            const int32_t callingUid) {
    VLOG("StatsService::setBroadcastSubscriber called.");
    ENFORCE_UID(AID_SYSTEM);

    if (pir == nullptr) {
        return exception(EX_NULL_POINTER,
                         "setBroadcastSubscriber provided with null PendingIntentRef");
    }

    ConfigKey configKey(callingUid, configId);
    SubscriberReporter::getInstance()
            .setBroadcastSubscriber(configKey, subscriberId, pir);
    return Status::ok();
}

Status StatsService::unsetBroadcastSubscriber(int64_t configId,
                                              int64_t subscriberId,
                                              const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::unsetBroadcastSubscriber called.");
    ConfigKey configKey(callingUid, configId);
    SubscriberReporter::getInstance()
            .unsetBroadcastSubscriber(configKey, subscriberId);
    return Status::ok();
}

Status StatsService::allPullersFromBootRegistered() {
    ENFORCE_UID(AID_SYSTEM);

    VLOG("StatsService::allPullersFromBootRegistered was called");
    mBootCompleteTrigger.markComplete(kAllPullersRegisteredTag);
    return Status::ok();
}

Status StatsService::registerPullAtomCallback(int32_t uid, int32_t atomTag, int64_t coolDownMillis,
                                              int64_t timeoutMillis,
                                              const std::vector<int32_t>& additiveFields,
                                              const shared_ptr<IPullAtomCallback>& pullerCallback) {
    ENFORCE_UID(AID_SYSTEM);
    VLOG("StatsService::registerPullAtomCallback called.");
    mPullerManager->RegisterPullAtomCallback(uid, atomTag, MillisToNano(coolDownMillis),
                                             MillisToNano(timeoutMillis), additiveFields,
                                             pullerCallback);
    return Status::ok();
}

Status StatsService::registerNativePullAtomCallback(
        int32_t atomTag, int64_t coolDownMillis, int64_t timeoutMillis,
        const std::vector<int32_t>& additiveFields,
        const shared_ptr<IPullAtomCallback>& pullerCallback) {
    if (!checkPermission(kPermissionRegisterPullAtom)) {
        return exception(
                EX_SECURITY,
                StringPrintf("Uid %d does not have the %s permission when registering atom %d",
                             AIBinder_getCallingUid(), kPermissionRegisterPullAtom, atomTag));
    }
    VLOG("StatsService::registerNativePullAtomCallback called.");
    int32_t uid = AIBinder_getCallingUid();
    mPullerManager->RegisterPullAtomCallback(uid, atomTag, MillisToNano(coolDownMillis),
                                             MillisToNano(timeoutMillis), additiveFields,
                                             pullerCallback);
    return Status::ok();
}

Status StatsService::unregisterPullAtomCallback(int32_t uid, int32_t atomTag) {
    ENFORCE_UID(AID_SYSTEM);
    VLOG("StatsService::unregisterPullAtomCallback called.");
    mPullerManager->UnregisterPullAtomCallback(uid, atomTag);
    return Status::ok();
}

Status StatsService::unregisterNativePullAtomCallback(int32_t atomTag) {
    if (!checkPermission(kPermissionRegisterPullAtom)) {
        return exception(
                EX_SECURITY,
                StringPrintf("Uid %d does not have the %s permission when unregistering atom %d",
                             AIBinder_getCallingUid(), kPermissionRegisterPullAtom, atomTag));
    }
    VLOG("StatsService::unregisterNativePullAtomCallback called.");
    int32_t uid = AIBinder_getCallingUid();
    mPullerManager->UnregisterPullAtomCallback(uid, atomTag);
    return Status::ok();
}

Status StatsService::getRegisteredExperimentIds(std::vector<int64_t>* experimentIdsOut) {
    ENFORCE_UID(AID_SYSTEM);
    // TODO: add verifier permission

    experimentIdsOut->clear();
    // Read the latest train info
    vector<InstallTrainInfo> trainInfoList = StorageManager::readAllTrainInfo();
    if (trainInfoList.empty()) {
        // No train info means no experiment IDs, return an empty list
        return Status::ok();
    }

    // Copy the experiment IDs to the out vector
    for (InstallTrainInfo& trainInfo : trainInfoList) {
        experimentIdsOut->insert(experimentIdsOut->end(),
                                 trainInfo.experimentIds.begin(),
                                 trainInfo.experimentIds.end());
    }
    return Status::ok();
}

Status StatsService::updateProperties(const vector<PropertyParcel>& properties) {
    ENFORCE_UID(AID_SYSTEM);

    // TODO(b/281765292): Forward statsd_java properties received here to FlagProvider.

    return Status::ok();
}

void StatsService::statsCompanionServiceDied(void* cookie) {
    auto thiz = static_cast<StatsService*>(cookie);
    thiz->statsCompanionServiceDiedImpl();
}

void StatsService::statsCompanionServiceDiedImpl() {
    ALOGW("statscompanion service died");
    StatsdStats::getInstance().noteSystemServerRestart(getWallClockSec());
    onStatsdInitCompletedHandlerTermination();
    if (mProcessor != nullptr) {
        ALOGW("Reset statsd upon system server restarts.");
        int64_t systemServerRestartNs = getElapsedRealtimeNs();
        int64_t wallClockNs = getWallClockNs();
        ProtoOutputStream activeConfigsProto;
        mProcessor->WriteActiveConfigsToProtoOutputStream(systemServerRestartNs,
                STATSCOMPANION_DIED, &activeConfigsProto);
        metadata::StatsMetadataList metadataList;
        mProcessor->WriteMetadataToProto(wallClockNs, systemServerRestartNs, &metadataList);
        mProcessor->WriteDataToDisk(STATSCOMPANION_DIED, FAST, systemServerRestartNs, wallClockNs);
        mProcessor->resetConfigs();

        std::string serializedActiveConfigs;
        if (activeConfigsProto.serializeToString(&serializedActiveConfigs)) {
            ActiveConfigList activeConfigs;
            if (activeConfigs.ParseFromString(serializedActiveConfigs)) {
                mProcessor->SetConfigsActiveState(activeConfigs, systemServerRestartNs);
            }
        }
        mProcessor->SetMetadataState(metadataList, wallClockNs, systemServerRestartNs);
    }
    mAnomalyAlarmMonitor->setStatsCompanionService(nullptr);
    mPeriodicAlarmMonitor->setStatsCompanionService(nullptr);
    mPullerManager->SetStatsCompanionService(nullptr);
}

Status StatsService::setRestrictedMetricsChangedOperation(const int64_t configId,
                                                          const string& configPackage,
                                                          const shared_ptr<IPendingIntentRef>& pir,
                                                          const int32_t callingUid,
                                                          vector<int64_t>* output) {
    ENFORCE_UID(AID_SYSTEM);
    if (!isAtLeastU()) {
        ALOGW("setRestrictedMetricsChangedOperation invoked on U- device");
        return Status::ok();
    }
    mConfigManager->SetRestrictedMetricsChangedReceiver(configPackage, configId, callingUid, pir);
    if (output != nullptr) {
        mProcessor->fillRestrictedMetrics(configId, configPackage, callingUid, output);
    } else {
        ALOGW("StatsService::setRestrictedMetricsChangedOperation output was nullptr");
    }
    return Status::ok();
}

Status StatsService::removeRestrictedMetricsChangedOperation(const int64_t configId,
                                                             const string& configPackage,
                                                             const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);
    if (!isAtLeastU()) {
        ALOGW("removeRestrictedMetricsChangedOperation invoked on U- device");
        return Status::ok();
    }
    mConfigManager->RemoveRestrictedMetricsChangedReceiver(configPackage, configId, callingUid);
    return Status::ok();
}

Status StatsService::querySql(const string& sqlQuery, const int32_t minSqlClientVersion,
                              const optional<vector<uint8_t>>& policyConfig,
                              const shared_ptr<IStatsQueryCallback>& callback,
                              const int64_t configKey, const string& configPackage,
                              const int32_t callingUid) {
    ENFORCE_UID(AID_SYSTEM);
    if (callback == nullptr) {
        ALOGW("querySql called with null callback.");
        StatsdStats::getInstance().noteQueryRestrictedMetricFailed(
                configKey, configPackage, std::nullopt, callingUid,
                InvalidQueryReason(NULL_CALLBACK));
        return Status::ok();
    }
    mProcessor->querySql(sqlQuery, minSqlClientVersion, policyConfig, callback, configKey,
                         configPackage, callingUid);
    return Status::ok();
}

Status StatsService::addSubscription(const vector<uint8_t>& subscriptionConfig,
                                     const shared_ptr<IStatsSubscriptionCallback>& callback) {
    ENFORCE_SID(kTracedProbesSid);

    initShellSubscriber();

    mShellSubscriber->startNewSubscription(subscriptionConfig, callback);
    return Status::ok();
}

Status StatsService::removeSubscription(const shared_ptr<IStatsSubscriptionCallback>& callback) {
    ENFORCE_SID(kTracedProbesSid);

    if (mShellSubscriber != nullptr) {
        mShellSubscriber->unsubscribe(callback);
    }
    return Status::ok();
}

Status StatsService::flushSubscription(const shared_ptr<IStatsSubscriptionCallback>& callback) {
    ENFORCE_SID(kTracedProbesSid);

    if (mShellSubscriber != nullptr) {
        mShellSubscriber->flushSubscription(callback);
    }
    return Status::ok();
}

void StatsService::initShellSubscriber() {
    std::lock_guard<std::mutex> lock(mShellSubscriberMutex);
    if (mShellSubscriber == nullptr) {
        mShellSubscriber = new ShellSubscriber(mUidMap, mPullerManager, mLogEventFilter);
    }
}

void StatsService::stopReadingLogs() {
    mIsStopRequested = true;
    // Push this event so that readLogs will process and break out of the loop
    // after the stop is requested.
    int64_t timeStamp;
    std::unique_ptr<LogEvent> logEvent = std::make_unique<LogEvent>(/*uid=*/0, /*pid=*/0);
    mEventQueue->push(std::move(logEvent), &timeStamp);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
