#include <cstdlib>
#include <fstream>
#include <ostream>
#define DISCORDPP_IMPLEMENTATION
#include "include/discordpp.h"
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <thread>
#include <atomic>
#include <string>
#include <functional>
#include <csignal>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <map>
#include <vector>
#include <set>
#include <ctime>
#include <chrono>
#include <memory>
#include <sstream>

#include <drogon/drogon.h>

#include <array>
#include <nlohmann/json.hpp>

#include <zip.h>

#include <filesystem>

namespace fs = std::filesystem;

uint64_t applicationId = 1028340906740420711;

// 15 minutes in seconds
const int INACTIVITY_TIMEOUT = 900;

// Create a flag to stop the application
std::atomic<bool> running = true;

// Event data structure
struct EventData {
    std::string type;
    std::map<std::string, std::string> metadata;
    std::vector<std::string> mappers;
};

// Global state
std::queue<EventData> eventQueue;
std::mutex queueMutex;
std::condition_variable queueCv;
std::string partyId = "";
EventData storedSongData;
time_t lastDataTime = std::time(nullptr);
bool rpcCleared = false;
bool inBeatmap = false;
time_t pauseStartTime = 0;
time_t totalPausedDuration = 0;
// Track last time we saw a HeartbeatReceiver so we can clear presence when it stops
time_t lastHeartbeatTime = 0;

nlohmann::json currentSongData;

int httpPort = 8080;
bool selfTest = false;
bool selfTestServer = false;
fs::path selfExePath;

bool downloadFile(const std::string& host,
                  const std::string& path,
                  const fs::path& outputFile)
{
    try {
        auto client = drogon::HttpClient::newHttpClient(host);
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPath(path);

        std::promise<drogon::HttpResponsePtr> responsePromise;
        auto future = responsePromise.get_future();

        client->sendRequest(request, [&responsePromise](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
            if (result == drogon::ReqResult::Ok) {
                responsePromise.set_value(response);
            } else {
                responsePromise.set_value(nullptr);
            }
        });

        auto response = future.get();
        if (!response || response->getStatusCode() != drogon::HttpStatusCode::k200OK) {
            std::cerr << "HTTP status: "
                      << (response ? static_cast<int>(response->getStatusCode()) : 0)
                      << '\n';
            return false;
        }

        std::ofstream out(outputFile, std::ios::binary);
        if (!out) {
            std::cerr << "Failed to open output file\n";
            return false;
        }

        out << response->getBody();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Request failed: " << e.what() << '\n';
        return false;
    }
}

void extractZip(const fs::path& archivePath, const fs::path& outputDir)
{
    int err = 0;

    zip* z = zip_open(archivePath.string().c_str(), ZIP_RDONLY, &err);
    if (!z) {
        return;
    }

    fs::create_directories(outputDir);

    zip_int64_t numEntries = zip_get_num_entries(z, 0);

    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(numEntries); ++i)
    {
        zip_stat_t st{};
        if (zip_stat_index(z, i, 0, &st) != 0) {
            continue;
        }

        fs::path entryPath = fs::path(st.name);

        // Prevent ZIP Slip attacks
        if (entryPath.is_absolute() || st.name[0] == '/' ||
            entryPath.string().find("..") != std::string::npos)
        {
            continue;
        }

        fs::path outPath = outputDir / entryPath;

        // Directory entry
        if (st.name[strlen(st.name) - 1] == '/')
        {
            fs::create_directories(outPath);
            continue;
        }

        // Create parent directories
        fs::create_directories(outPath.parent_path());

        zip_file* zf = zip_fopen_index(z, i, 0);
        if (!zf) {
            continue;
        }

        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            zip_fclose(zf);
            continue;
        }

        constexpr size_t BUFFER_SIZE = 8192;
        std::vector<char> buffer(BUFFER_SIZE);

        zip_int64_t bytesRead = 0;

        while ((bytesRead = zip_fread(zf, buffer.data(), buffer.size())) > 0)
        {
            out.write(buffer.data(), bytesRead);
        }

        zip_fclose(zf);
    }

    zip_close(z);
}

// Signal handler to stop the application
void signalHandler(int signum) {
    running.store(false);
}



// Helper function to join mappers
std::string joinMappers(const std::vector<std::string>& mappers) {
    if (mappers.empty()) return "Unknown";

    std::set<std::string> uniqueMappers(mappers.begin(), mappers.end());
    std::string result;
    for (const auto& mapper : uniqueMappers) {
        if (!result.empty()) result += ", ";
        result += mapper;
    }
    return result;
}

// Update rich presence, optionally with a small image asset.
// If the asset is not found in the Discord application, logs a warning and
// retries without it so the presence still shows.
void updatePresence(std::shared_ptr<discordpp::Client> client,
                    discordpp::Activity activity,
                    const std::string& smallImage = "",
                    const std::string& smallText = "") {
    if (!smallImage.empty()) {
        discordpp::Activity withAssets = activity;
        discordpp::ActivityAssets assets;
        assets.SetSmallImage(smallImage);
        if (!smallText.empty()) assets.SetSmallText(smallText);
        withAssets.SetAssets(assets);

        // activity (no assets) is captured for the fallback retry
        client->UpdateRichPresence(withAssets, [client, activity, smallImage](auto result) {
            if (!result.Successful()) {
                std::string err = result.Error();
                if (err.find("asset") != std::string::npos) {
                    std::cerr << "⚠️  Asset '" << smallImage << "' not found in your Discord application "
                                 "(upload it under Rich Presence > Art Assets in the Developer Portal). "
                                 "Retrying without assets.\n";
                    client->UpdateRichPresence(activity, [](auto r) {
                        if (!r.Successful())
                            std::cerr << "❌ Failed to update rich presence: " << r.Error() << std::endl;
                    });
                } else {
                    std::cerr << "❌ Failed to update rich presence: " << err << std::endl;
                }
            }
        });
    } else {
        client->UpdateRichPresence(activity, [](auto result) {
            if (!result.Successful())
                std::cerr << "❌ Failed to update rich presence: " << result.Error() << std::endl;
        });
    }
}

// Token persistence helpers
static fs::path getTokenFilePath() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return fs::path(appdata) / "BeatSaberBridgeAPI_token.json";
    return fs::path("auth_token.json");
#else
    const char* home = std::getenv("HOME");
    if (home) return fs::path(home) / ".beatsaberbridge_token.json";
    return fs::path("auth_token.json");
#endif
}

static bool loadAuthToken(std::string& accessToken, std::string& refreshToken, int32_t& expiresIn) {
    try {
        fs::path p = getTokenFilePath();
        if (!fs::exists(p)) return false;
        std::ifstream in(p);
        if (!in) return false;
        nlohmann::json j;
        in >> j;
        if (j.contains("access_token")) accessToken = j["access_token"].get<std::string>();
        if (j.contains("refresh_token")) refreshToken = j["refresh_token"].get<std::string>();
        if (j.contains("expires_in")) expiresIn = j["expires_in"].get<int32_t>();
        return !accessToken.empty();
    } catch (...) {
        return false;
    }
}

static int findAvailableLocalPort() {
    constexpr int fallbackPort = 8080;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return fallbackPort;
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return fallbackPort;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int result = bind(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr));
    if (result != 0) {
        closesocket(sock);
        WSACleanup();
        return fallbackPort;
    }

    sockaddr_in boundAddr{};
    int addrLen = sizeof(boundAddr);
    result = getsockname(sock, reinterpret_cast<SOCKADDR*>(&boundAddr), &addrLen);
    int port = fallbackPort;
    if (result == 0) {
        port = ntohs(boundAddr.sin_port);
    }

    closesocket(sock);
    WSACleanup();
    return port;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return fallbackPort;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return fallbackPort;
    }

    sockaddr_in boundAddr{};
    socklen_t addrLen = sizeof(boundAddr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) == 0) {
        int port = ntohs(boundAddr.sin_port);
        close(sock);
        return port;
    }

    close(sock);
    return fallbackPort;
#endif
}

static void saveAuthToken(const std::string& accessToken, const std::string& refreshToken, int32_t expiresIn) {
    try {
        fs::path p = getTokenFilePath();
        nlohmann::json j;
        j["access_token"] = accessToken;
        j["refresh_token"] = refreshToken;
        j["expires_in"] = expiresIn;
        std::ofstream out(p, std::ios::trunc);
        if (out) out << j.dump(4);
    } catch (...) {
        // ignore failures to persist
    }
}

void httpServer() {
    auto& app = drogon::app();

    const std::string listenAddress = (selfTest || selfTestServer) ? "127.0.0.1" : "0.0.0.0";
    app.addListener(listenAddress, httpPort);
    app.setThreadNum(1);

    app.registerHandler("/version",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto res = drogon::HttpResponse::newHttpResponse();
            res->setStatusCode(drogon::HttpStatusCode::k200OK);
            res->setContentTypeString("application/json");
            res->setBody(nlohmann::json({{"version", "v0.1.8"}}).dump());
            callback(res);
        },
        {drogon::Get}
    );

    app.registerHandler("/update",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            try {
                if (selfTest) {
                    for (const auto& entry : fs::directory_iterator(selfExePath.parent_path())) {
                        auto path = entry.path();
                        auto ext = path.extension().string();

                        bool shouldUpdate = false;
                        #ifdef _WIN32
                            shouldUpdate = (ext == ".exe" || ext == ".dll");
                        #else
                            shouldUpdate = (ext == ".so" || ext == ".dylib" || path == selfExePath);
                        #endif

                        if (!shouldUpdate) continue;

                        fs::path oldPath = path;
                        oldPath.replace_extension(".old");

                        #ifdef _WIN32
                            if (fs::exists(oldPath)) fs::remove(oldPath);
                        #endif

                        fs::rename(path, oldPath);
                        fs::copy(oldPath, path);
                    }

                    for (const auto& cleanEntry : fs::directory_iterator(selfExePath.parent_path())) {
                        if (cleanEntry.path().extension() == ".old") {
                            std::error_code ec;
                            fs::remove(cleanEntry.path(), ec);
                        }
                    }

                    std::cout << "Self-test passed: update file operations completed successfully" << std::endl;
                    std::exit(EXIT_SUCCESS);
                }

                fs::path temp_path = fs::temp_directory_path();
                fs::path current_path = fs::current_path();

                std::cout << temp_path << std::endl;

                auto githubClient = drogon::HttpClient::newHttpClient("https://api.github.com");
                auto githubRequest = drogon::HttpRequest::newHttpRequest();
                githubRequest->setMethod(drogon::Get);
                githubRequest->setPath("/repos/RainzDev/BeatSaberBridgeAPI.CPP/releases/latest");

                std::promise<drogon::HttpResponsePtr> releasePromise;
                auto releaseFuture = releasePromise.get_future();
                githubClient->sendRequest(githubRequest, [&releasePromise](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
                    if (result == drogon::ReqResult::Ok) {
                        releasePromise.set_value(response);
                    } else {
                        releasePromise.set_value(nullptr);
                    }
                });

                auto clientResponse = releaseFuture.get();
                if (clientResponse && clientResponse->getStatusCode() == drogon::HttpStatusCode::k200OK) {
                    nlohmann::json jsonData = nlohmann::json::parse(clientResponse->getBody());

                    std::array assets = jsonData["assets"];

                    std::string downloadUrl;

                    #ifdef _WIN32
                        downloadUrl = assets[2]["browser_download_url"];
                    #elif __linux__
                        downloadUrl = assets[0]["browser_download_url"];
                    #elif __APPLE__
                        #include "TargetConditionals.h"
                        #if TARGET_OS_MAC
                            downloadUrl = assets[1]["browser_download_url"];
                        #endif
                    #endif

                    fs::path mainTempPath = "temp_BeatSaberBridgeAPI";
                    fs::path zipName = "download.zip";
                    fs::path mainOld = "BeatSaberBridgeAPI.old";
                    fs::path winDll = "discord_partner_sdk.old";

                    fs::create_directory(temp_path / mainTempPath);

                    downloadUrl.erase(0, 18);

                    downloadFile("https://github.com", downloadUrl, temp_path / mainTempPath / zipName);
                    extractZip(temp_path / mainTempPath / zipName, temp_path / mainTempPath);

                    for (const auto& entry : fs::directory_iterator(temp_path / mainTempPath)) {
                        if (entry != temp_path / mainTempPath / zipName) {
                            std::cout << entry << std::endl;

                            #ifdef _WIN32
                                for (const auto& filePath : fs::directory_iterator(current_path)) {
                                    if (filePath.path().filename() == current_path / mainOld || filePath.path().filename() == current_path / winDll) {
                                        fs::remove(filePath);
                                    }
                                }
                                fs::rename(current_path / entry.path().filename(), current_path / entry.path().filename().replace_extension(".old"));
                            #else
                                fs::remove(entry.path().filename());
                            #endif
                            fs::remove(entry.path().filename());
                            fs::copy(entry, current_path / entry.path().filename(), fs::copy_options::overwrite_existing);
                        }
                    }

                    std::cout << "Update completed! This program will automatically terminate. Please run the new file. (If on Linux or MacOS, please make sure to run \"chmod +x ./BeatSaberBridgeAPI\" again.)" << std::endl;
                    std::exit(EXIT_SUCCESS);
                } else {
                    std::cerr << "Failed to download latest release" << std::endl;
                }

                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::HttpStatusCode::k200OK);
                res->setContentTypeString("application/json");
                res->setBody(nlohmann::json({{"status", "success"}}).dump());
                callback(res);
            } catch (const std::exception& e) {
                std::cerr << "❌ Error processing request: " << e.what() << std::endl;
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
                res->setContentTypeString("application/json");
                res->setBody(nlohmann::json({{"status", "error"}, {"message", e.what()}}).dump());
                callback(res);
            }
        },
        {drogon::Post}
    );

    app.registerHandler("/sendData",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            try {
                std::cerr << "A\n";

                auto path = req->path();

                std::cerr << "B\n";

                auto body = req->body();

                std::cout << "Body: [" << body << "]" << std::endl;

                auto json = nlohmann::json::parse(body);

                std::cout << json << std::endl;

                EventData event;
                event.type = json["type"];

                for (auto& [key, value] : json.items()) {
                    if (key != "type" && key != "mappers") {
                        if (value.is_string()) {
                            event.metadata[key] = value.get<std::string>();
                        } else if (value.is_number()) {
                            event.metadata[key] = std::to_string(value.get<int>());
                        }
                    }
                }

                if (json.contains("mappers") && json["mappers"].is_array()) {
                    for (const auto& mapper : json["mappers"]) {
                        if (mapper.is_string()) {
                            event.mappers.push_back(mapper.get<std::string>());
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    eventQueue.push(event);
                }

                queueCv.notify_one();

                std::cout << "📨 Received event: " << event.type << std::endl;

                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::HttpStatusCode::k200OK);
                res->setContentTypeString("application/json");
                res->setBody(nlohmann::json({{"status", "success"}}).dump());
                callback(res);
            } catch (const std::exception& e) {
                std::cerr << "❌ Error processing request: " << e.what() << std::endl;
                auto res = drogon::HttpResponse::newHttpResponse();
                res->setStatusCode(drogon::HttpStatusCode::k400BadRequest);
                res->setContentTypeString("application/json");
                res->setBody(nlohmann::json({{"status", "error"}, {"message", e.what()}}).dump());
                callback(res);
            }
        },
        {drogon::Post}
    );

    app.run();
}

time_t songStartTime;
time_t songEndTime;
int songLength;

void rpcWorker(std::shared_ptr<discordpp::Client> client) {
    while (running) {
        EventData data;
        bool hasEvent = false;

        // Wait for an event or timeout (for inactivity checks)
        {
            std::unique_lock<std::mutex> lk(queueMutex);
            queueCv.wait_for(lk, std::chrono::milliseconds(100), [&]() { return !eventQueue.empty() || !running.load(); });
            if (!eventQueue.empty()) {
                data = eventQueue.front();
                eventQueue.pop();
                hasEvent = true;
            }
        }

        if (hasEvent) {
            try {
                lastDataTime = std::time(nullptr);
                rpcCleared = false;

                discordpp::Activity activity;
                
                // If this is a heartbeat event, record the time and skip other handling
                if (data.type == "HeartbeatReceiver") {
                    lastHeartbeatTime = std::time(nullptr);
                    continue;
                }

                if (data.type == "BeatmapInitialized") {
                    inBeatmap = true;

                    songStartTime = std::time(nullptr);
                    songLength = std::stoi(data.metadata["duration"]);
                    songEndTime = songStartTime + songLength;

                    storedSongData = data;
                    totalPausedDuration = 0;
                    pauseStartTime = 0;

                    currentSongData["title"] = data.metadata["title"];
                    currentSongData["author"] = data.metadata["author"];
                    currentSongData["difficulty"] = data.metadata["difficulty"];
                    currentSongData["mappers"] = joinMappers(data.mappers);
                    currentSongData["duration"] = data.metadata["duration"];

                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState(currentSongData["difficulty"].get<std::string>() + " | " + "🎯 0" + " | " + "❌ 0" + " | " + "💥 0" +  " | " + "💣 0");
                    activity.SetDetails(data.metadata["author"] + " - " + data.metadata["title"] + " | " + "Mapped by " + joinMappers(data.mappers));

                    discordpp::ActivityTimestamps timestamps;
                    timestamps.SetStart(songStartTime);
                    timestamps.SetEnd(songEndTime);
                    activity.SetTimestamps(timestamps);

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "MainMenuInitialized") {
                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Status: Main Menu");

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "LevelSelectionMenuInitialized") {
                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Status: Level Selection Menu");

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "BeatmapCleared") {
                    inBeatmap = false;
                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Status: Cleared | " + data.metadata["difficulty"]);
                    activity.SetDetails(data.metadata["author"] + " - " + data.metadata["title"] + " | " + joinMappers(data.mappers));

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "BeatmapFailed") {
                    inBeatmap = false;
                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Status: Failed | " + storedSongData.metadata["difficulty"]);
                    activity.SetDetails(storedSongData.metadata["author"] + " - " + storedSongData.metadata["title"] + " | " + joinMappers(storedSongData.mappers));

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "BeatmapPaused") {
                    inBeatmap = false;
                    pauseStartTime = std::time(nullptr);

                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Level Paused");

                    updatePresence(client, activity);
                }
                else if (data.type == "BeatmapResumed") {
                    inBeatmap = true;
                    time_t currentTime = std::time(nullptr);
                    songStartTime += currentTime - pauseStartTime; // Add the amount of time paused onto the start time of the song, in theory will even out the timer
                    songEndTime = songStartTime + songLength;

                    if (pauseStartTime != 0) {
                        totalPausedDuration += (currentTime - pauseStartTime);
                        pauseStartTime = 0;
                    }

                    int adjustedStart = 0; // In real implementation, store start time with song
                    int adjustedEnd = adjustedStart + std::stoi(storedSongData.metadata["duration"]);

                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState(storedSongData.metadata["author"] + " - " + storedSongData.metadata["title"]);
                    activity.SetDetails("Mapped by " + joinMappers(storedSongData.mappers) + " | " + storedSongData.metadata["difficulty"]);

                    discordpp::ActivityTimestamps timestamps;
                    timestamps.SetStart(songStartTime);
                    timestamps.SetEnd(songEndTime);
                    activity.SetTimestamps(timestamps);

                    updatePresence(client, activity);
                }
                else if (data.type == "BeatmapRestarted") {
                    inBeatmap = true;

                    songStartTime = std::time(nullptr);
                    songLength = std::stoi(data.metadata["duration"]);
                    songEndTime = songStartTime + songLength;

                    storedSongData = data;
                    totalPausedDuration = 0;
                    pauseStartTime = 0;

                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState(currentSongData["difficulty"].get<std::string>() + " | " + "🎯 0" + " | " + "❌ 0" + " | " + "💥 0" +  " | " + "💣 0");
                    activity.SetDetails(currentSongData["author"].get<std::string>() + " - " + currentSongData["title"].get<std::string>() + " | " + "Mapped by " + currentSongData["mappers"].get<std::string>());

                    discordpp::ActivityTimestamps timestamps;
                    timestamps.SetStart(songStartTime);
                    timestamps.SetEnd(songEndTime);
                    activity.SetTimestamps(timestamps);

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "LobbyPlayerOnDisconnect" || data.type == "LobbyPlayerOnConnect") {
                    if (partyId.empty()) {
                        // Generate a simple UUID (simplified)
                        partyId = "party_" + std::to_string(std::time(nullptr));
                    }

                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetDetails("Status: Multiplayer Lobby");
                    activity.SetState(data.metadata["playerCount"] + " players waiting...");

                    // Set party info
                    discordpp::ActivityParty party;
                    party.SetId(partyId);
                    party.SetCurrentSize(std::stoi(data.metadata["playerCount"]));
                    party.SetMaxSize(std::stoi(data.metadata["maxPlayerCount"]));

                    discordpp::ActivitySecrets secrets;
                    secrets.SetJoin("bsrpc://BeatTogether/" + data.metadata["lobbyCode"]);

                    activity.SetSecrets(secrets);
                    activity.SetParty(party);

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "BeatmapStatUpdate") {
                    if (!inBeatmap) continue;

                    activity.SetType(discordpp::ActivityTypes::Playing);

                    activity.SetDetails(currentSongData["author"].get<std::string>() + " - " + currentSongData["title"].get<std::string>() + " | " + "Mapped by " + currentSongData["mappers"].get<std::string>());
                    activity.SetState(currentSongData["difficulty"].get<std::string>() + " | " + "🎯 " + data.metadata["score"] + " | " + "❌" + data.metadata["notesMissed"] + " | " + "💥 " + data.metadata["notesBadCut"] +  " | " + "💣 " + data.metadata["bombsHit"]);

                    // If the client provided a currentTime field (seconds into the song),
                    // compute remaining time and set timestamps so Discord shows remaining time.
                    try {
                        if (!data.metadata["currentTime"].empty()) {
                            double currentTime = std::stod(data.metadata["currentTime"]);

                            songStartTime = std::time(nullptr) - currentTime;
                            songEndTime = songStartTime + songLength;

                            discordpp::ActivityTimestamps timestamps;
                            timestamps.SetStart(songStartTime);
                            timestamps.SetEnd(songEndTime);
                            //timestamps.SetEnd(static_cast<long long>((now + static_cast<long long>(std::round(remaining)))));
                            activity.SetTimestamps(timestamps);
                        }
                    } catch (...) {
                        // If parsing fails, just skip timestamps update.
                    }

                    updatePresence(client, activity, "quest", "Meta Quest");
                }
                else if (data.type == "MultiplayerBeatmapInitialized") {
                    partyId = "";
                    songStartTime = std::time(nullptr);
                    songLength = std::stoi(data.metadata["duration"]);
                    songEndTime = songStartTime + songLength;

                    // Prepare the activity now, but perform the actual update after a short delay
                    activity.SetType(discordpp::ActivityTypes::Playing);
                    activity.SetState("Status: Playing | " + data.metadata["difficulty"] + " | Multiplayer");
                    activity.SetDetails(data.metadata["author"] + " - " + data.metadata["title"] + " | " + joinMappers(data.mappers));

                    discordpp::ActivityTimestamps timestamps;
                    timestamps.SetStart(songStartTime);
                    timestamps.SetEnd(songEndTime);
                    activity.SetTimestamps(timestamps);

                    // Schedule the presence update asynchronously to avoid blocking the worker
                    std::thread([client, activity]() mutable {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        updatePresence(client, activity, "quest", "Meta Quest");
                    }).detach();
                }
                }
            catch (const std::exception& e) {
                std::cerr << "❌ Error processing event '" << data.type << "': " << e.what() << std::endl;
            }
        }

        // Clear presence if we haven't seen a HeartbeatReceiver recently
        const int HEARTBEAT_TIMEOUT = 60; // seconds
        if (!rpcCleared) {
            time_t now = std::time(nullptr);
            if (lastHeartbeatTime != 0 && (now - lastHeartbeatTime) > HEARTBEAT_TIMEOUT) {
                client->ClearRichPresence();
                rpcCleared = true;
            }
        }
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    selfExePath = fs::canonical(fs::path(argv[0]));

    // Parse runtime arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            httpPort = std::stoi(argv[++i]);
        } else if (arg == "--app-id" && i + 1 < argc) {
            applicationId = std::stoull(argv[++i]);
        } else if (arg == "--self-test") {
            selfTest = true;
        } else if (arg == "--self-test-server") {
            selfTestServer = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--port <port>] [--app-id <id>] [--self-test] [--self-test-server]" << std::endl;
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
        }
    }

    if (selfTestServer) {
        httpServer();
        return 0;
    }

    if (selfTest) {
        std::atomic<int> selfTestResult{1};
        std::string selfTestFailureReason;

        std::thread tester([&]() {
            auto appClient = drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(httpPort));
            auto request = drogon::HttpRequest::newHttpRequest();
            request->setMethod(drogon::Get);
            request->setPath("/version");

            while (true) {
                std::promise<drogon::HttpResponsePtr> p;
                auto f = p.get_future();
                appClient->sendRequest(request, [&p](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
                    if (result == drogon::ReqResult::Ok) p.set_value(response);
                    else p.set_value(nullptr);
                });

                if (f.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready) {
                    auto resp = f.get();
                    if (resp && resp->getStatusCode() == drogon::HttpStatusCode::k200OK) {
                        selfTestResult.store(0);
                        break;
                    }

                    if (resp) {
                        selfTestFailureReason = "HTTP status " + std::to_string(static_cast<int>(resp->getStatusCode()));
                        try {
                            std::string body = std::string(resp->getBody());
                            if (!body.empty()) {
                                if (body.size() > 512) body = body.substr(0, 512) + "...";
                                selfTestFailureReason += "; body: " + body;
                            }
                        } catch (...) {}
                    } else {
                        selfTestFailureReason = "no response (request failed)";
                    }
                } else {
                    selfTestFailureReason = "waiting for server to become reachable";
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            drogon::app().quit();
        });

        httpServer();

        if (tester.joinable()) tester.join();

        if (selfTestResult.load() == 0) {
            std::cout << "Self-test passed: HTTP server responded to /version" << std::endl;
            return 0;
        }

        std::cerr << "Self-test failed: /version did not respond OK" << std::endl;
        if (!selfTestFailureReason.empty()) {
            std::cerr << selfTestFailureReason << std::endl;
        }
        return 1;
    }

    std::signal(SIGINT, signalHandler);
    std::cout << "🚀 Initializing Beat Saber Bridge API..." << std::endl;

    // Create our Discord Client
    auto client = std::make_shared<discordpp::Client>();

    // Set up logging callback
    client->AddLogCallback([](auto message, auto severity) {
        std::cout << "[" << EnumToString(severity) << "] " << message << std::endl;
    }, discordpp::LoggingSeverity::Verbose);

    std::function<void()> doAuthorize;

    // Set up status callback to monitor client connection
    client->SetStatusChangedCallback([client, &doAuthorize](discordpp::Client::Status status, discordpp::Client::Error error, int32_t errorDetail) {
        std::cout << "🔄 Status changed: " << discordpp::Client::StatusToString(status) << std::endl;

        if (status == discordpp::Client::Status::Ready) {
            std::cout << "✅ Discord client is ready!" << std::endl;
        } else if (error != discordpp::Client::Error::None) {
            std::cerr << "❌ Connection error: " << discordpp::Client::ErrorToString(error) << " (detail: " << errorDetail << ")" << std::endl;
        }

        if (status == discordpp::Client::Status::Disconnected &&
            error == discordpp::Client::Error::UnexpectedClose) {
            try {
                fs::path p = getTokenFilePath();
                if (fs::exists(p)) {
                    fs::remove(p);
                    std::cout << "🧹 Removed stale Discord auth token after invalid-session disconnect." << std::endl;
                }
            } catch (...) {}

            if (doAuthorize) {
                doAuthorize();
            }
        }
    });

    // Generate OAuth2 code verifier for authentication
    auto codeVerifier = client->CreateAuthorizationCodeVerifier();

    // Set up authentication arguments
    discordpp::AuthorizationArgs args{};
    args.SetClientId(applicationId);
    args.SetScopes(discordpp::Client::GetDefaultPresenceScopes());
    args.SetCodeChallenge(codeVerifier.Challenge());

    std::cout << "🔐 Starting authorization process..." << std::endl;
    // Prepare helpers for token removal and authorization so we can retry if stored token is invalid
    auto removeSavedToken = []() {
        try {
            fs::path p = getTokenFilePath();
            if (fs::exists(p)) fs::remove(p);
        } catch (...) {}
    };

    doAuthorize = [&]() {
        client->Authorize(args, [client, codeVerifier, &doAuthorize](auto result, auto code, auto redirectUri) {
            if (!result.Successful()) {
                std::cerr << "❌ Authorization failed: " << result.Error() << std::endl;
                return;
            }
            std::cout << "✅ Authorization successful! Getting access token..." << std::endl;

            // Exchange auth code for access token
            client->GetToken(applicationId, code, codeVerifier.Verifier(), redirectUri,
                [client, &doAuthorize](discordpp::ClientResult result,
                                       std::string accessToken,
                                       std::string refreshToken,
                                       discordpp::AuthorizationTokenType tokenType,
                                       int32_t expiresIn,
                                       std::string scope) {
                    if (!result.Successful()) {
                        std::cerr << "❌ GetToken failed: " << result.Error() << std::endl;
                        return;
                    }

                    // Save token to disk for future runs
                    saveAuthToken(accessToken, refreshToken, expiresIn);

                    std::cout << "🔓 Access token received! Establishing connection..." << std::endl;
                    // Next Step: Update the token and connect
                    client->UpdateToken(discordpp::AuthorizationTokenType::Bearer, accessToken, [client, &doAuthorize](discordpp::ClientResult updateResult) {
                        if (updateResult.Successful()) {
                            std::cout << "🔑 Token updated, connecting to Discord..." << std::endl;
                            client->Connect();
                        } else {
                            std::cerr << "❌ Failed to update token after GetToken: " << updateResult.Error() << std::endl;
                            // If we can't update, remove saved token and try authorizing again
                            try { fs::path p = getTokenFilePath(); if (fs::exists(p)) fs::remove(p); } catch(...) {}
                            doAuthorize();
                        }
                    });
                });
        });
    };

    // Try to load a saved token first so the user doesn't need to re-authorize
    {
        std::string savedAccess, savedRefresh;
        int32_t savedExpires = 0;
        if (loadAuthToken(savedAccess, savedRefresh, savedExpires)) {
            std::cout << "🔐 Loaded saved access token, attempting to use it..." << std::endl;
            client->UpdateToken(discordpp::AuthorizationTokenType::Bearer, savedAccess, [client, &doAuthorize](discordpp::ClientResult result) {
                if (result.Successful()) {
                    std::cout << "🔑 Token updated from disk, connecting to Discord..." << std::endl;
                    client->Connect();
                } else {
                    std::cerr << "❌ Failed to update token from disk: " << result.Error() << std::endl;
                    // Remove stored token and re-run authorization
                    try { fs::path p = getTokenFilePath(); if (fs::exists(p)) fs::remove(p); } catch(...) {}
                    doAuthorize();
                }
            });
        } else {
            doAuthorize();
        }
    }

    // Start RPC worker thread
    // Ensure drogon's internal signal handlers also stop our main loop.
    drogon::app().setIntSignalHandler([]() {
        running.store(false);
        drogon::app().quit();
    });
    drogon::app().setTermSignalHandler([]() {
        running.store(false);
        drogon::app().quit();
    });
    std::thread rpcWorkerThread(rpcWorker, client);
    rpcWorkerThread.detach();

    // Run discord callbacks in a separate thread so the main thread can host the
    // drogon HTTP server (which blocks in app.run()).
    std::thread discordThread([client]() {
        while (running.load()) {
            discordpp::RunCallbacks();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    discordThread.detach();

    std::cout << "HTTP Server listening on http://" << (selfTest ? "127.0.0.1" : "0.0.0.0") << ":" << httpPort << std::endl;

    // Run HTTP server in the main thread (blocking). This replaces the previous
    // threaded server approach and leverages drogon's async IO internally.
    httpServer();

    // When the HTTP server exits (e.g. due to SIGINT/SIGTERM), ensure the
    // background loops stop.
    running.store(false);

    return 0;
}
