/*
 * Copyright (C) 2018 The Android Open Source Project
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

#pragma once

#include "adb_unique_fd.h"

// ==========================================================================
// The generated protobuf header below includes sysdeps.h, which in turn does
//
// #undef foo
// #define foo ___xxx_foo
//
// I believe this is to guard against the unintentional use of native
// open/write/etc calls instead of the <system>_open etc that are defined
// on a per platform basis.
//
// However it also includes /usr/include/google/protobuf/io/coded_stream.h
// which in recent versions includes some headers from abseil which contain
// calls to basic_ostream::write - which these #defines mask out, breaking
// the build.
//
// We should remove the leaky guard defines and reinstate them after the
// affected header(s) have been included.
#undef open
#undef shutdown
#undef close
#undef read
#undef pread
#undef write
#undef pwrite
#undef lseek
#undef unlink
#undef creat
#undef isatty
#undef accept
#undef setsockopt
#undef socketpair
#undef poll
#undef mkdir
#include "fastdeploy/proto/ApkEntry.pb.h"
#define open       ___xxx_open
#define shutdown   ___xxx_shutdown
#define close      ___xxx_close
#define read       ___xxx_read
#define pread      ___xxx_pread
#define write      ___xxx_write
#define pwrite     ___xxx_pwrite
#define lseek      ___xxx_lseek
#define unlink     ___xxx_unlink
#define creat      ___xxx_creat
#define isatty     ___xxx_isatty
#define accept     ___xxx_accept
#define setsockopt ___xxx_setsockopt
#define socketpair ___xxx_socketpair
#define poll       ___xxx_poll
#define mkdir      ___xxx_mkdir
// ==========================================================================

#include <optional>
#include <string>

enum FastDeploy_AgentUpdateStrategy {
    FastDeploy_AgentUpdateAlways,
    FastDeploy_AgentUpdateNewerTimeStamp,
    FastDeploy_AgentUpdateDifferentVersion
};

void fastdeploy_set_agent_update_strategy(FastDeploy_AgentUpdateStrategy agent_update_strategy);
int get_device_api_level();

std::optional<com::android::fastdeploy::APKMetaData> extract_metadata(const char* apk_path);
unique_fd install_patch(int argc, const char** argv);
unique_fd apply_patch_on_device(const char* output_path);
int stream_patch(const char* apk_path, com::android::fastdeploy::APKMetaData metadata,
                 unique_fd patch_fd);
