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

#define TRACE_TAG SERVICES

#include "sysdeps.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <thread>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/parsenetaddress.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/android_reboot.h>
#include <cutils/sockets.h>
#include <log/log_properties.h>

#include "adb.h"
#include "adb_io.h"
#include "adb_unique_fd.h"
#include "adb_utils.h"
#include "services.h"
#include "socket_spec.h"
#include "sysdeps.h"
#include "transport.h"

#include "daemon/file_sync_service.h"
#include "daemon/framebuffer_service.h"
#include "daemon/jdwp_service.h"
#include "daemon/logging.h"
#include "daemon/restart_service.h"
#include "daemon/shell_service.h"

void reconnect_service(unique_fd fd, atransport* t) {
    WriteFdExactly(fd.get(), "done");
    kick_transport(t);
}

unique_fd reverse_service(std::string_view command, atransport* transport) {
    // TODO: Switch handle_forward_request to std::string_view.
    std::string str(command);

    int s[2];
    if (adb_socketpair(s)) {
        PLOG(ERROR) << "cannot create service socket pair.";
        return unique_fd{};
    }
    VLOG(SERVICES) << "service socketpair: " << s[0] << ", " << s[1];
    if (!handle_forward_request(str.c_str(), transport, s[1])) {
        SendFail(s[1], "not a reverse forwarding command");
    }
    adb_close(s[1]);
    return unique_fd{s[0]};
}

// Shell service string can look like:
//   shell[,arg1,arg2,...]:[command]
unique_fd ShellService(std::string_view args, const atransport* transport) {
    size_t delimiter_index = args.find(':');
    if (delimiter_index == std::string::npos) {
        LOG(ERROR) << "No ':' found in shell service arguments: " << args;
        return unique_fd{};
    }

    // TODO: android::base::Split(const std::string_view&, ...)
    std::string service_args(args.substr(0, delimiter_index));
    std::string command(args.substr(delimiter_index + 1));

    // Defaults:
    //   PTY for interactive, raw for non-interactive.
    //   No protocol.
    //   $TERM set to "dumb".
    SubprocessType type(command.empty() ? SubprocessType::kPty : SubprocessType::kRaw);
    SubprocessProtocol protocol = SubprocessProtocol::kNone;
    std::string terminal_type = "dumb";

    for (const std::string& arg : android::base::Split(service_args, ",")) {
        if (arg == kShellServiceArgRaw) {
            type = SubprocessType::kRaw;
        } else if (arg == kShellServiceArgPty) {
            type = SubprocessType::kPty;
        } else if (arg == kShellServiceArgShellProtocol) {
            protocol = SubprocessProtocol::kShell;
        } else if (arg.starts_with("TERM=")) {
            terminal_type = arg.substr(strlen("TERM="));
        } else if (!arg.empty()) {
            // This is not an error to allow for future expansion.
            LOG(WARNING) << "Ignoring unknown shell service argument: " << arg;
        }
    }

    return StartSubprocess(command, terminal_type.c_str(), type, protocol);
}

static void spin_service(unique_fd fd) {
    if (!__android_log_is_debuggable()) {
        WriteFdExactly(fd.get(), "refusing to spin on non-debuggable build\n");
        return;
    }

    // A service that creates an fdevent that's always pending, and then ignores it.
    unique_fd pipe_read, pipe_write;
    if (!Pipe(&pipe_read, &pipe_write)) {
        WriteFdExactly(fd.get(), "failed to create pipe\n");
        return;
    }

    fdevent_run_on_looper([fd = pipe_read.release()]() {
        fdevent* fde = fdevent_create(
                fd, [](int, unsigned, void*) {}, nullptr);
        fdevent_add(fde, FDE_READ);
    });

    WriteFdExactly(fd.get(), "spinning\n");
}

[[maybe_unused]] static unique_fd reboot_device(const std::string& name) {
#if defined(__ANDROID_RECOVERY__)
    if (!__android_log_is_debuggable()) {
        auto reboot_service = [name](unique_fd fd) {
            std::string reboot_string = android::base::StringPrintf("reboot,%s", name.c_str());
            if (!android::base::SetProperty(ANDROID_RB_PROPERTY, reboot_string)) {
                WriteFdFmt(fd.get(), "reboot (%s) failed\n", reboot_string.c_str());
                return;
            }
            while (true) pause();
        };
        return create_service_thread("reboot", reboot_service);
    }
#endif
    // Fall through
    std::string cmd = "/system/bin/reboot ";
    cmd += name;
    return StartSubprocess(cmd, nullptr, SubprocessType::kRaw, SubprocessProtocol::kNone);
}

struct ServiceSocket : public asocket {
    ServiceSocket() = delete;
    explicit ServiceSocket(atransport* transport) {
        CHECK(transport);
        install_local_socket(this);
        this->transport = transport;
        this->enqueue = [](asocket* self, apacket::payload_type data) {
            // TODO: This interface currently can't give any backpressure.
            send_ready(self->id, self->peer->id, self->transport, data.size());
            return static_cast<ServiceSocket*>(self)->Enqueue(std::move(data));
        };
        this->ready = [](asocket* self) { return static_cast<ServiceSocket*>(self)->Ready(); };
        this->close = [](asocket* self) { return static_cast<ServiceSocket*>(self)->Close(); };
    }
    virtual ~ServiceSocket() = default;

    ServiceSocket(const ServiceSocket& copy) = delete;
    ServiceSocket(ServiceSocket&& move) = delete;
    ServiceSocket& operator=(const ServiceSocket& copy) = delete;
    ServiceSocket& operator=(ServiceSocket&& move) = delete;

    virtual int Enqueue(apacket::payload_type data) { return -1; }
    virtual void Ready() {}
    virtual void Close() {
        if (peer) {
            peer->peer = nullptr;
            if (peer->shutdown) {
                peer->shutdown(peer);
            }
            peer->close(peer);
        }

        remove_socket(this);
        delete this;
    }
};

struct SinkSocket : public ServiceSocket {
    explicit SinkSocket(atransport* transport, size_t byte_count)
        : ServiceSocket(transport), bytes_left_(byte_count) {
        LOG(INFO) << "Creating new SinkSocket with capacity " << byte_count;
    }

    virtual ~SinkSocket() { LOG(INFO) << "SinkSocket destroyed"; }

    virtual int Enqueue(apacket::payload_type data) override final {
        if (bytes_left_ <= data.size()) {
            // Done reading.
            Close();
            return -1;
        }

        bytes_left_ -= data.size();
        return 0;
    }

    size_t bytes_left_;
};

struct SourceSocket : public ServiceSocket {
    explicit SourceSocket(atransport* transport, size_t byte_count)
        : ServiceSocket(transport), bytes_left_(byte_count) {
        LOG(INFO) << "Creating new SourceSocket with capacity " << byte_count;
    }

    virtual ~SourceSocket() { LOG(INFO) << "SourceSocket destroyed"; }

    void Ready() {
        size_t len = std::min(bytes_left_, get_max_payload());
        if (len == 0) {
            Close();
            return;
        }

        Block block(len);
        memset(block.data(), 0, block.size());
        peer->enqueue(peer, std::move(block));
        bytes_left_ -= len;
    }

    int Enqueue(apacket::payload_type data) { return -1; }

    size_t bytes_left_;
};

asocket* daemon_service_to_socket(std::string_view name, atransport* transport) {
    if (name == "jdwp") {
        return create_jdwp_service_socket();
    } else if (name == "track-jdwp") {
        return create_jdwp_tracker_service_socket();
    } else if (name == "track-app") {
        return create_app_tracker_service_socket();
    } else if (android::base::ConsumePrefix(&name, "sink:")) {
        uint64_t byte_count = 0;
        if (!ParseUint(&byte_count, name)) {
            return nullptr;
        }
        return new SinkSocket(transport, byte_count);
    } else if (android::base::ConsumePrefix(&name, "source:")) {
        uint64_t byte_count = 0;
        if (!ParseUint(&byte_count, name)) {
            return nullptr;
        }
        return new SourceSocket(transport, byte_count);
    }

    return nullptr;
}

// SteamOS host is Linux, not Android. Answer Android-only adb requests with help msg.


// install / install-create / install-write / install-commit, but not uninstall.
static bool IsInstallVerb( std::string_view svBody )
{
    size_t nPos = 0;
    while ( ( nPos = svBody.find( "install", nPos ) ) != std::string_view::npos )
    {
        bool bPrevUn = nPos >= 2 && svBody[nPos - 1] == 'n' && svBody[nPos - 2] == 'u';
        if ( !bPrevUn )
        {
            return true;
        }
        nPos += 7;
    }
    return false;
}

// Splits "svc:body" or "svc,opts:body" into svc and body. False if there's no ':'.
static bool SplitService( std::string_view svName, std::string_view* psvSvc, std::string_view* psvBody )
{
    size_t nColon = svName.find( ':' );
    if ( nColon == std::string_view::npos )
    {
        return false;
    }
    *psvSvc = svName.substr( 0, nColon );
    *psvBody = svName.substr( nColon + 1 );
    return true;
}

static bool IsShellService( std::string_view svSvc )
{
    return svSvc == "shell" || svSvc.starts_with( "shell," );
}

// APK install over a raw transport (exec: / abb_exec:), or a manual shell command
// (adb shell pm install ..., adb shell cmd package install ...). Skip install-incremental.
// Bails client-side for normal APKs and then falls back to the streamed path.
static bool IsApkInstall( std::string_view svName )
{
    std::string_view svBody;
    if ( !android::base::ConsumePrefix( &svName, "abb_exec:" ) &&
         !android::base::ConsumePrefix( &svName, "exec:" ) )
    {
        std::string_view svSvc;
        if ( !SplitService( svName, &svSvc, &svBody ) || !IsShellService( svSvc ) )
        {
            return false;
        }
    }
    else
    {
        svBody = svName;
    }
    if ( svBody.find( "incremental" ) != std::string_view::npos )
    {
        return false;
    }
    if ( !IsInstallVerb( svBody ) )
    {
        return false;
    }
    return svBody.find( "pm" ) != std::string_view::npos ||
           svBody.find( "package" ) != std::string_view::npos;
}

// Displayed when the user tries to adb install an apk.
static const std::string k_sGuidanceMultiline =
    "\n===========================================================\n"
    "SteamOS is running on this connection.\n"
    "APKs CAN NOT be installed over adb here.\n"
    "\n"
    "To install APKs launch Lepton Development on the device.\n"
    "Visit https://s.team/lepton for more information.\n"
    "===========================================================\n";

// Displayed on Unity's "no compatible device" dialog and the device selection dropdown in build settings.
// Changed slashes to a unicode equivalent so it will render properly in Unity.
static const std::string k_sGuidanceSingleline =
    "DOES NOT ACCEPT ADB INSTALL! To install APKs launch Lepton Development. "
    "https: ∕ ∕ s.team ∕ lepton for more information.";

// A friendly getprop dump Unity can parse.
// ro.build.version.sdk - a valid int below any real min-SDK. Allows Unity's device check to fail cleanly
// ro.product.model - the value displayed for the device name in some apps (our help msg)
static std::string GetpropDump()
{
    return "[ro.product.model]: [" + k_sGuidanceSingleline + "]\n"
           "[ro.build.version.sdk]: [1]\n"
           "[ro.build.version.release]: [0]\n"
           "[ro.product.brand]: [SteamOS]\n"
           "[ro.product.manufacturer]: [Valve]\n"
           "[ro.product.device]: [steamframe]\n"
           "[ro.product.name]: [steamframe]\n"
           "[ro.product.cpu.abi]: [arm64-v8a]\n"
           "[ro.product.cpu.abilist]: [arm64-v8a]\n"
           "[ro.product.cpu.abilist64]: [arm64-v8a]\n"
           "[ro.product.cpu.abilist32]: []\n"
           "[ro.build.characteristics]: [default]\n"
           "[ro.build.fingerprint]: [steamos/steamframe/steamframe:0/0/0:user/release-keys]\n";
}

// ---- adb interceptor ---------------------------------------------
static unique_fd ReplyAndClose( std::string sReply )
{
    return create_service_thread( "steamos_reply", [sReply = std::move( sReply )]( unique_fd fd )
    {
        WriteFdExactly( fd.get(), sReply.data(), sReply.size() );
    } );
}

// adb queries this before installing. Answering "false" skips incremental mode,
// which keeps a couple of confusing error lines off the user's console.
static bool IsIncrementalProbe( std::string_view svName )
{
    return ( svName.starts_with( "abb_exec:" ) || svName.starts_with( "exec:" ) ) &&
           svName.find( "enable_adb_incremental_install_default" ) != std::string_view::npos;
}

// Does the shell command start with `svWord` as its own word?
static bool CommandIs( std::string_view svCmd, std::string_view svWord )
{
    if ( !svCmd.starts_with( svWord ) )
    {
        return false;
    }
    if ( svCmd.size() == svWord.size() )
    {
        return true;
    }
    char ch = svCmd[svWord.size()];
    return ch == ' ' || ch == '\t' || ch == ';' || ch == '\n';
}

// Unity runs a few Android-only commands (getprop, dumpsys) while enumerating the device and treats any non-zero exit as fatal.
// Rewrite the ones we know about to print something useful and exit 0.
// Real commands (ls, cat, etc) fall through untouched.
static bool RewriteAndroidProbe( std::string_view svName, std::string* psOut )
{
    std::string_view svSvc, svCmd;
    if ( !SplitService( svName, &svSvc, &svCmd ) )
    {
        return false;
    }
    if ( !( svSvc == "exec" || IsShellService( svSvc ) ) )
    {
        return false;
    }

    size_t nLead = svCmd.find_first_not_of( " \t" );
    if ( nLead == std::string_view::npos )
    {
        return false;
    }
    std::string_view svRest = svCmd.substr( nLead );

    std::string sPayload;
    if ( CommandIs( svRest, "getprop" ) )
    {
        sPayload = GetpropDump();  // Unity parses this back into properties
    }
    else if ( CommandIs( svRest, "dumpsys" ) )
    {
        sPayload = k_sGuidanceMultiline;  // just shown, not parsed
    }
    else
    {
        return false;
    }

    *psOut = std::string( svSvc ) + ":printf '%s' " + escape_arg( sPayload ) + "; exit 0";
    return true;
}

unique_fd daemon_service_to_fd(std::string_view name, atransport* transport) {
    ADB_LOG(Service) << "transport " << transport->serial_name() << " opening service " << name;

#if !defined(__ANDROID__) && !defined(__ANDROID_RECOVERY__)
    // Handle the Android-only requests before adb's normal dispatch below.
    if ( IsApkInstall( name ) )
    {
        return ReplyAndClose( k_sGuidanceMultiline );
    }
    if ( IsIncrementalProbe( name ) )
    {
        return ReplyAndClose( "false\n" );
    }
    std::string sProbe;
    if ( RewriteAndroidProbe( name, &sProbe ) )
    {
        name = sProbe;
    }
#endif

#if defined(__ANDROID__) && !defined(__ANDROID_RECOVERY__)
    if (name.starts_with("abb:") || name.starts_with("abb_exec:")) {
        return execute_abb_command(name);
    }
#endif

#if defined(__ANDROID__)
    if (name.starts_with("framebuffer:")) {
        return create_service_thread("fb", framebuffer_service);
    } else if (android::base::ConsumePrefix(&name, "remount:")) {
        std::string cmd = "/system/bin/remount ";
        cmd += name;
        return StartSubprocess(cmd, nullptr, SubprocessType::kRaw, SubprocessProtocol::kNone);
    } else if (android::base::ConsumePrefix(&name, "reboot:")) {
        return reboot_device(std::string(name));
    } else if (android::base::ConsumePrefix(&name, "backup:")) {
        std::string cmd = "/system/bin/bu backup ";
        cmd += name;
        return StartSubprocess(cmd, nullptr, SubprocessType::kRaw, SubprocessProtocol::kNone);
    } else if (name.starts_with("restore:")) {
        return StartSubprocess("/system/bin/bu restore", nullptr, SubprocessType::kRaw,
                               SubprocessProtocol::kNone);
    } else if (name.starts_with("disable-verity:")) {
        return StartSubprocess("/system/bin/disable-verity", nullptr, SubprocessType::kRaw,
                               SubprocessProtocol::kNone);
    } else if (name.starts_with("enable-verity:")) {
        return StartSubprocess("/system/bin/enable-verity", nullptr, SubprocessType::kRaw,
                               SubprocessProtocol::kNone);
    } else if (android::base::ConsumePrefix(&name, "tcpip:")) {
        std::string str(name);

        int port;
        if (sscanf(str.c_str(), "%d", &port) != 1) {
            return unique_fd{};
        }
        return create_service_thread("tcp",
                                     std::bind(restart_tcp_service, std::placeholders::_1, port));
    } else if (name.starts_with("usb:")) {
        return create_service_thread("usb", restart_usb_service);
    }
#endif

    if (android::base::ConsumePrefix(&name, "dev:")) {
        return unique_fd{unix_open(name, O_RDWR | O_CLOEXEC)};
#if defined(__ANDROID__)
    } else if (android::base::ConsumePrefix(&name, "jdwp:")) {
        pid_t pid;
        if (!ParseUint(&pid, name)) {
            return unique_fd{};
        }
        return create_jdwp_connection_fd(pid);
#endif
    } else if (android::base::ConsumePrefix(&name, "root")) {
        return create_service_thread("root", restart_root_service);
    } else if (android::base::ConsumePrefix(&name, "unroot")) {
        return create_service_thread("unroot", restart_unroot_service);
    } else if (android::base::ConsumePrefix(&name, "shell")) {
        return ShellService(name, transport);
    } else if (android::base::ConsumePrefix(&name, "exec:")) {
        return StartSubprocess(std::string(name), nullptr, SubprocessType::kRaw,
                               SubprocessProtocol::kNone);
    } else if (name.starts_with("sync:")) {
        return create_service_thread("sync", file_sync_service);
    } else if (android::base::ConsumePrefix(&name, "reverse:")) {
        return reverse_service(name, transport);
    } else if (name == "reconnect") {
        return create_service_thread(
                "reconnect", std::bind(reconnect_service, std::placeholders::_1, transport));
    } else if (name == "spin") {
        return create_service_thread("spin", spin_service);
    }

    return unique_fd{};
}
