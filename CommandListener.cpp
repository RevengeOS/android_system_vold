/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#define LOG_TAG "CommandListener"
#include <cutils/log.h>

#include <sysutils/SocketClient.h>

#include "CommandListener.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "Process.h"
#include "Xwarp.h"

CommandListener::CommandListener() :
                 FrameworkListener("vold") {
    registerCmd(new VolumeCmd());
    registerCmd(new AsecCmd());
    registerCmd(new ShareCmd());
    registerCmd(new StorageCmd());
    registerCmd(new XwarpCmd());
}

CommandListener::VolumeCmd::VolumeCmd() :
                 VoldCommand("volume") {
}

int CommandListener::VolumeCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    VolumeManager *vm = VolumeManager::Instance();
    int rc = 0;

    if (!strcmp(argv[1], "list")) {
        return vm->listVolumes(cli);
    } else if (!strcmp(argv[1], "mount")) {
        rc = vm->mountVolume(argv[2]);
    } else if (!strcmp(argv[1], "unmount")) {
        bool force = false;
        if (argc >= 4 && !strcmp(argv[3], "force")) {
            force = true;
        }
        rc = vm->unmountVolume(argv[2], force);
    } else if (!strcmp(argv[1], "format")) {
        rc = vm->formatVolume(argv[2]);
    } else if (!strcmp(argv[1], "share")) {
        rc = vm->shareVolume(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "unshare")) {
        rc = vm->unshareVolume(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "shared")) {
        bool enabled = false;

        if (vm->shareEnabled(argv[2], argv[3], &enabled)) {
            cli->sendMsg(
                    ResponseCode::OperationFailed, "Failed to determine share enable state", true);
        } else {
            cli->sendMsg(ResponseCode::ShareEnabledResult,
                    (enabled ? "Share enabled" : "Share disabled"), false);
        }
        return 0;
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown volume cmd", false);
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "volume operation succeeded", false);
    } else {
        int erno = errno;
        rc = ResponseCode::convertFromErrno();
        cli->sendMsg(rc, "volume operation failed", true);
    }

    return 0;
}

CommandListener::ShareCmd::ShareCmd() :
                 VoldCommand("share") {
}

int CommandListener::ShareCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    VolumeManager *vm = VolumeManager::Instance();
    int rc = 0;

    if (!strcmp(argv[1], "status")) {
        bool avail = false;

        if (vm->shareAvailable(argv[2], &avail)) {
            cli->sendMsg(
                    ResponseCode::OperationFailed, "Failed to determine share availability", true);
        } else {
            cli->sendMsg(ResponseCode::ShareStatusResult,
                    (avail ? "Share available" : "Share unavailable"), false);
        }
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown share cmd", false);
    }

    return 0;
}

CommandListener::StorageCmd::StorageCmd() :
                 VoldCommand("storage") {
}

int CommandListener::StorageCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "users")) {
        DIR *dir;
        struct dirent *de;

        if (!(dir = opendir("/proc"))) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to open /proc", true);
            return 0;
        }

        while ((de = readdir(dir))) {
            int pid = Process::getPid(de->d_name);

            if (pid < 0) {
                continue;
            }

            char processName[255];
            Process::getProcessName(pid, processName, sizeof(processName));

            if (Process::checkFileDescriptorSymLinks(pid, argv[2]) ||
                Process::checkFileMaps(pid, argv[2]) ||
                Process::checkSymLink(pid, argv[2], "cwd") ||
                Process::checkSymLink(pid, argv[2], "root") ||
                Process::checkSymLink(pid, argv[2], "exe")) {

                char msg[1024];
                snprintf(msg, sizeof(msg), "%d %s", pid, processName);
                cli->sendMsg(ResponseCode::StorageUsersListResult, msg, false);
            }
        }
        closedir(dir);
        cli->sendMsg(ResponseCode::CommandOkay, "Storage user list complete", false);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown storage cmd", false);
    }
    return 0;
}

CommandListener::AsecCmd::AsecCmd() :
                 VoldCommand("asec") {
}

int CommandListener::AsecCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    VolumeManager *vm = VolumeManager::Instance();
    int rc = 0;

    if (!strcmp(argv[1], "list")) {
        DIR *d = opendir(Volume::SEC_ASECDIR);

        if (!d) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to open asec dir", true);
            return 0;
        }

        struct dirent *dent;
        while ((dent = readdir(d))) {
            if (dent->d_name[0] == '.')
                continue;
            if (!strcmp(&dent->d_name[strlen(dent->d_name)-5], ".asec")) {
                char id[255];
                memset(id, 0, sizeof(id));
                strncpy(id, dent->d_name, strlen(dent->d_name) -5);
                cli->sendMsg(ResponseCode::AsecListResult, id, false);
            }
        }
        closedir(d);
    } else if (!strcmp(argv[1], "create")) {
        if (argc != 7) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: asec create <container-id> <size_mb> <fstype> <key> <ownerUid>", false);
            return 0;
        }

        unsigned int numSectors = (atoi(argv[3]) * (1024 * 1024)) / 512;
        rc = vm->createAsec(argv[2], numSectors, argv[4], argv[5], atoi(argv[6]));
    } else if (!strcmp(argv[1], "finalize")) {
        if (argc != 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: asec finalize <container-id>", false);
            return 0;
        }
        rc = vm->finalizeAsec(argv[2]);
    } else if (!strcmp(argv[1], "destroy")) {
        if (argc < 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: asec destroy <container-id> [force]", false);
            return 0;
        }
        bool force = false;
        if (argc > 3 && !strcmp(argv[3], "force")) {
            force = true;
        }
        rc = vm->destroyAsec(argv[2], force);
    } else if (!strcmp(argv[1], "mount")) {
        if (argc != 5) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: asec mount <namespace-id> <key> <ownerUid>", false);
            return 0;
        }
        rc = vm->mountAsec(argv[2], argv[3], atoi(argv[4]));
    } else if (!strcmp(argv[1], "unmount")) {
        if (argc < 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: asec unmount <container-id> [force]", false);
            return 0;
        }
        bool force = false;
        if (argc > 3 && !strcmp(argv[3], "force")) {
            force = true;
        }
        rc = vm->unmountAsec(argv[2], force);
    } else if (!strcmp(argv[1], "rename")) {
        if (argc != 4) {
            cli->sendMsg(ResponseCode::CommandSyntaxError,
                    "Usage: asec rename <old_id> <new_id>", false);
            return 0;
        }
        rc = vm->renameAsec(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "path")) {
        if (argc != 3) {
            cli->sendMsg(ResponseCode::CommandSyntaxError, "Usage: asec path <container-id>", false);
            return 0;
        }
        char path[255];

        if (vm->getAsecMountPath(argv[2], path, sizeof(path))) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to get path", true);
        } else {
            cli->sendMsg(ResponseCode::AsecPathResult, path, false);
        }
        return 0;
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown asec cmd", false);
    }

    if (!rc) {
        cli->sendMsg(ResponseCode::CommandOkay, "asec operation succeeded", false);
    } else {
        rc = ResponseCode::convertFromErrno();
        cli->sendMsg(rc, "asec operation failed", true);
    }

    return 0;
}

CommandListener::XwarpCmd::XwarpCmd() :
                 VoldCommand("xwarp") {
}

int CommandListener::XwarpCmd::runCommand(SocketClient *cli,
                                                      int argc, char **argv) {
    if (argc < 2) {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Missing Argument", false);
        return 0;
    }

    if (!strcmp(argv[1], "enable")) {
        if (Xwarp::enable()) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to enable xwarp", true);
            return 0;
        }

        cli->sendMsg(ResponseCode::CommandOkay, "Xwarp mirroring started", false);
    } else if (!strcmp(argv[1], "disable")) {
        if (Xwarp::disable()) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to disable xwarp", true);
            return 0;
        }

        cli->sendMsg(ResponseCode::CommandOkay, "Xwarp disabled", false);
    } else if (!strcmp(argv[1], "status")) {
        char msg[255];
        bool r;
        unsigned mirrorPos, maxSize;

        if (Xwarp::status(&r, &mirrorPos, &maxSize)) {
            cli->sendMsg(ResponseCode::OperationFailed, "Failed to get xwarp status", true);
            return 0;
        }
        snprintf(msg, sizeof(msg), "%s %u %u", (r ? "ready" : "not-ready"), mirrorPos, maxSize);
        cli->sendMsg(ResponseCode::XwarpStatusResult, msg, false);
    } else {
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Unknown storage cmd", false);
    }

    return 0;
}

