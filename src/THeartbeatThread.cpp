// BeamMP, the BeamNG.drive multiplayer mod.
// Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
//
// BeamMP Ltd. can be contacted by electronic mail via contact@beammp.com.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "THeartbeatThread.h"

#include "Client.h"
#include "Http.h"
#include "ChronoWrapper.h"
//#include "SocketIO.h"
#include <rapidjson/document.h>
#include <rapidjson/rapidjson.h>
#include <sstream>

namespace json = rapidjson;

void THeartbeatThread::operator()() {
    RegisterThread("Heartbeat");
    std::string Body;
    std::string T;

    // these are "hot-change" related variables
    static std::string Last;

    static std::chrono::high_resolution_clock::time_point LastNormalUpdateTime = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point LastUpdateReminderTime = std::chrono::high_resolution_clock::now();
    bool isAuth = false;
    std::chrono::high_resolution_clock::duration UpdateReminderTimePassed;
    auto UpdateReminderTimeout = ChronoWrapper::TimeFromStringWithLiteral(Application::Settings.UpdateReminderTime);
    while (!Application::IsShuttingDown()) {
        Body = GenerateCall();
        // a hot-change occurs when a setting has changed, to update the backend of that change.
        auto Now = std::chrono::high_resolution_clock::now();
        bool Unchanged = Last == Body;
        auto TimePassed = (Now - LastNormalUpdateTime);
        UpdateReminderTimePassed = (Now - LastUpdateReminderTime);
        auto Threshold = Unchanged ? 30 : 5;
        if (TimePassed < std::chrono::seconds(Threshold)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        beammp_debug("heartbeat (after " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(TimePassed).count()) + "s)");

        Last = Body;
        LastNormalUpdateTime = Now;
        if (!Application::Settings.CustomIP.empty()) {
            Body += "&ip=" + Application::Settings.CustomIP;
        }

        auto Target = "/heartbeat";
        unsigned int ResponseCode = 0;

        json::Document Doc;
        bool Ok = false;
        for (const auto& Url : Application::GetBackendUrlsInOrder()) {
            T = Http::POST(Url, 443, Target, Body, "application/x-www-form-urlencoded", &ResponseCode, { { "api-v", "2" } });
            Doc.Parse(T.data(), T.size());
            if (Doc.HasParseError() || !Doc.IsObject()) {
                if (!Application::Settings.Private) {
                    beammp_trace("Backend response failed to parse as valid json");
                    beammp_trace("Response was: `" + T + "`");
                }
            } else if (ResponseCode != 200) {
                beammp_errorf("Response code from the heartbeat: {}", ResponseCode);
            } else {
                // all ok
                Ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::string Status {};
        std::string Code {};
        std::string Message {};
        const auto StatusKey = "status";
        const auto CodeKey = "code";
        const auto MessageKey = "msg";

        if (Ok) {
            if (Doc.HasMember(StatusKey) && Doc[StatusKey].IsString()) {
                Status = Doc[StatusKey].GetString();
            } else {
                Ok = false;
            }
            if (Doc.HasMember(CodeKey) && Doc[CodeKey].IsString()) {
                Code = Doc[CodeKey].GetString();
            } else {
                Ok = false;
            }
            if (Doc.HasMember(MessageKey) && Doc[MessageKey].IsString()) {
                Message = Doc[MessageKey].GetString();
            } else {
                Ok = false;
            }
            if (!Ok) {
                beammp_error("Missing/invalid json members in backend response");
            }
        } else {
            if (!Application::Settings.Private) {
                beammp_warn("Backend failed to respond to a heartbeat. Your server may temporarily disappear from the server list. This is not an error, and will likely resolve itself soon. Direct connect will still work.");
            }
        }

        if (Ok && !isAuth && !Application::Settings.Private) {
            if (Status == "2000") {
                beammp_info(("Authenticated! " + Message));
                isAuth = true;
            } else if (Status == "200") {
                beammp_info(("Resumed authenticated session! " + Message));
                isAuth = true;
            } else {
                if (Message.empty()) {
                    Message = "Backend didn't provide a reason.";
                }
                beammp_error("Backend REFUSED the auth key. Reason: " + Message);
            }
        }
        if (isAuth || Application::Settings.Private) {
            Application::SetSubsystemStatus("Heartbeat", Application::Status::Good);
        }
        // beammp_debugf("Update reminder time passed: {}, Update reminder time: {}", UpdateReminderTimePassed.count(), UpdateReminderTimeout.count());
        if (!Application::Settings.HideUpdateMessages && UpdateReminderTimePassed.count() > UpdateReminderTimeout.count()) {
            LastUpdateReminderTime = std::chrono::high_resolution_clock::now();
            Application::CheckForUpdates();
        }
    }
}

std::string THeartbeatThread::GenerateCall() {
    std::stringstream Ret;

    Ret << "uuid=" << Application::Settings.Key
        << "&players=" << mServer.ClientCount()
        << "&maxplayers=" << Application::Settings.MaxPlayers
        << "&port=" << Application::Settings.Port
        << "&map=" << Application::Settings.MapName
        << "&private=" << (Application::Settings.Private ? "true" : "false")
        << "&version=" << Application::ServerVersionString()
        << "&clientversion=" << std::to_string(Application::ClientMajorVersion()) + ".0" // FIXME: Wtf.
        << "&name=" << Application::Settings.ServerName
        << "&tags=" << Application::Settings.ServerTags
        << "&modlist=" << mResourceManager.TrimmedList()
        << "&modstotalsize=" << mResourceManager.MaxModSize()
        << "&modstotal=" << mResourceManager.ModsLoaded()
        << "&playerslist=" << GetPlayers()
        << "&desc=" << Application::Settings.ServerDesc
        << "&pass=" << (Application::Settings.Password.empty() ? "false" : "true");
    return Ret.str();
}
THeartbeatThread::THeartbeatThread(TResourceManager& ResourceManager, TServer& Server)
    : mResourceManager(ResourceManager)
    , mServer(Server) {
    Application::SetSubsystemStatus("Heartbeat", Application::Status::Starting);
    Application::RegisterShutdownHandler([&] {
        Application::SetSubsystemStatus("Heartbeat", Application::Status::ShuttingDown);
        if (mThread.joinable()) {
            mThread.join();
        }
        Application::SetSubsystemStatus("Heartbeat", Application::Status::Shutdown);
    });
    Start();
}
std::string THeartbeatThread::GetPlayers() {
    std::string Return;
    mServer.ForEachClient([&](const std::weak_ptr<TClient>& ClientPtr) -> bool {
        ReadLock Lock(mServer.GetClientMutex());
        if (!ClientPtr.expired()) {
            Return += ClientPtr.lock()->GetName() + ";";
        }
        return true;
    });
    return Return;
}
/*THeartbeatThread::~THeartbeatThread() {
}*/
