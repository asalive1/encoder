#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <lame.h>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <portaudio.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <curl/curl.h>
#include <fdk-aac/aacenc_lib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "encoders.h"
#include "httplib.h"
#include "json.hpp"
#include "version.h"

namespace fs = std::filesystem;
std::string getProgramDir() {
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count == -1) return ".";
    std::string fullPath(result, count);
    size_t pos = fullPath.find_last_of('/');
    return (pos == std::string::npos) ? "." : fullPath.substr(0, pos);
}
// Resolve network interface name to IP address
std::string getInterfaceIP(const std::string& ifname) {
    if (ifname.empty()) return "";

    // If it's already an IP address, return as-is
    if (ifname.find('.') != std::string::npos) return ifname;

    struct ifaddrs *ifaddrs_ptr, *ifa;
    if (getifaddrs(&ifaddrs_ptr) == -1) return "";

    for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            std::string(ifa->ifa_name) == ifname) {
            char ip[INET_ADDRSTRLEN];
            struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip, INET_ADDRSTRLEN);
            freeifaddrs(ifaddrs_ptr);
            return std::string(ip);
            }
    }
    freeifaddrs(ifaddrs_ptr);
    return "";
}

void clearHlsOutput(const std::string& hlsRoot) {
    namespace fs = std::filesystem;

    // Remove all files in segments directory
    std::string segmentsDir = hlsRoot + "/segments";
    if (fs::exists(segmentsDir)) {
        for (auto& entry : fs::directory_iterator(segmentsDir)) {
            if (fs::is_regular_file(entry.path())) {
                fs::remove(entry.path());
            }
        }
    }
}

using json = nlohmann::ordered_json;

// OpenSSL for TCP + HTTP (optional TLS in future). We use plain TCP here.
static std::ofstream g_logFile;
static std::string g_currentLogFile;
static int g_logRotationCount = 0;

// Helper: get current date string
std::string currentDate() {
    time_t now = time(nullptr);
    tm* lt = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", lt);
    return std::string(buf);
}

// Helper: get timestamp string
std::string timestamp() {
    time_t now = time(nullptr);
    tm* lt = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
    return std::string(buf);
}

// Helper: check file size
size_t fileSize(const std::string& filename) {
    struct stat st;
    if (stat(filename.c_str(), &st) == 0) return st.st_size;
    return 0;
}
// Numeric epoch timestamp with millisecond precision (e.g., 1758569019.475)
double nowStartEpochSeconds() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = time_point_cast<milliseconds>(now);
    const double secs = ms.time_since_epoch().count() / 1000.0;
    return secs;
}
// Return current UTC time in ISO-8601 with milliseconds and Z suffix
std::string currentUtcIso8601() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch());

    // Break into seconds and milliseconds
    auto secs = duration_cast<seconds>(ms);
    auto millis = ms - secs;

    std::time_t tt = secs.count();
    std::tm gmt{};
    gmtime_r(&tt, &gmt); // convert to UTC

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &gmt);

    std::ostringstream oss;
    oss << buf << "." << std::setw(3) << std::setfill('0') << millis.count() << "Z";
    return oss.str();
}

// Forward declare
static std::deque<std::string> g_logQueue;
static std::mutex g_logMutex;

bool startMp3EncoderWithConfig();
bool stopMp3EncoderWithConfig();
void stopAudioEngine();
void maybeStopEngineIfIdle();


static std::deque<std::string> g_metaHistory;

void broadcastLogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logQueue.push_back(line);
    // Keep queue bounded (e.g. last 500 lines)
    if (g_logQueue.size() > 500) {
        g_logQueue.pop_front();
    }
}

//Configuration
struct Config {
    int webPort = 8080;
    int controlPort = 9010;      // new control port
    bool controlEnabled = false; // enable/disable control listener
    std::string commandStart;   // Start Command Message
    std::string commandStop;    // Stop Command Message

    std::string icecastUrl;
    std::string user;
    std::string pass;
    std::string amperwaveStationId;
    std::string iceDataParse;  //json file for custom Icecast metadata
    std::string logDir;
    // NEW: MP3 Icecast settings
    bool        mp3Enabled = false;       // enable/disable MP3 stream
    bool        mp3MetaEnabled = true;    // send ICY metadata for MP3
    std::string mp3IcecastUrl;            // URL mountpoint for MP3 stream
    std::string mp3User;                  // username for MP3 stream
    std::string mp3Pass;                  // password for MP3 stream

    int sampleRate;
    int channels;
    int bitrate;
    int deviceIndex;
    int listenPort;
    int icyMetaInt;

    //input selection and RTP options
    std::string inputType;    // "portaudio" or "rtp"
    std::string rtpAddress;   // e.g., "239.192.0.10" (multicast) or "10.10.10.50" (unicast)
    int         rtpPort;      // e.g., 5004
    std::string rtpInterface; // local NIC name to bind/join multicast on, e.g., "eth0"

    // choose the local interface/IP for TCP sending to Icecast (optional)
    std::string icecastInterface; // local interface name to bind before connect(), e.g., "eth0"
    // gain multiplier for RTP audio
    float rtpGain;            // e.g., 1.0 = unity, 2.0 = double volume

    // HLS settings
    bool        hlsEnabled = false;           // enable/disable HLS engine
    int         hlsSegmentSeconds = 6;        // duration target per segment
    int         hlsWindow = 5;                // number of segments in the playlist window
    int         hlsStartTimeOffset = 0;       // EXT-X-START TIME-OFFSET in seconds (0 = oldest segment, negative = from live edge)
    std::string hlsPath;                      // optional override; default to getProgramDir()+"/HLS"
    bool        hlsMetaEnabled = true;     // enable/disable JSON on EXTINF
    std::string hlsMetaFile;               // path to JSON file for test runs (e.g., "<program>/HLS/metadata.json")
    bool        hlsMetaUseFile = true;     // true=file source; false=generate from live metadata

    bool        iceEnabled = true;      // start Icecast/AmperWave stream when headless
    bool        iceMetaEnabled = true;  // send ICY metadata blocks
    // NEW: base folder for config files; backend-only
    std::string configDir;  // empty -> default to g_appRoot + "/config"
};


// Global config object and mutex

static std::string g_configPath = "config.json";

static Config g_config;
static std::mutex g_configMutex;

static std::atomic<bool> g_running{false};

// Engine state
static std::atomic<bool> g_engineRunning{false};
static std::thread g_engineThread;

// Encoded frame queue for Icecast sink
static std::mutex g_encodedMutex;
static std::condition_variable g_encodedCv;
static std::deque<std::vector<uint8_t>> g_encodedFrames;

// Icecast sink state
static std::atomic<bool> g_iceRunning{false};
static std::thread g_iceThread;
static int g_iceSock = -1;
static int g_iceMetaIntEffective = 0;
std::atomic<bool> aacRunning{false};

static constexpr auto kIcecastTakeoverTimeout = std::chrono::seconds(60);
static constexpr auto kIcecastRetryInterval = std::chrono::seconds(2);

// --- MP3 sink globals ---
static std::atomic<bool> g_iceRunningMp3{false};
std::atomic<bool> g_mp3StopRequested{false};
static int               g_iceSockMp3 = -1;
static std::thread       g_iceThreadMp3;
static int               g_iceMetaIntEffectiveMp3 = 0;
std::atomic<bool> mp3Running{false};

// Separate encoded queue for MP3
static std::mutex                        g_encodedMutexMp3;
static std::condition_variable           g_encodedCvMp3;
static std::deque<std::vector<uint8_t>>  g_encodedFramesMp3;

// Control listener state
std::atomic<bool> controlShutdown{false};
std::atomic<bool> controlRunning{false};
std::thread controlThread;
int g_controlServerFd = -1;

// Config access (you likely already have this)
extern std::mutex g_configMutex;
extern Config g_config;

std::atomic<bool> hlsRunning{false};

static std::mutex g_metaMutex;
static std::string g_currentMeta;
//static std::chrono::steady_clock::time_point g_lastMetaChange;
std::thread metaThread;
std::atomic<bool> metaRunning{false};
static std::atomic<bool> metaShutdown{false};
std::mutex g_metaCtrlMutex;
static int g_metadataServerFd = -1;
bool ensureDirExists(const std::string& path);


void initAppLayout() {
    // Ensure config and logs directories exist
    ensureDirExists(getProgramDir() + "/config");
    ensureDirExists(getProgramDir() + "/logs");
    ensureDirExists(getProgramDir() + "/HLS");

    // Set default config path if not overridden
    g_configPath = getProgramDir() + "/config/config.json";
}
void pruneOldLogs(int keepDays = 14);

static std::string g_currentLogDate;
// Reopen a new dated log if the date changed; reset rotation counter
void rolloverIfDateChanged() {
    const std::string today = currentDate();
    if (today == g_currentLogDate) return;

    // The day changed -> move to a new base file
    g_currentLogDate = today;
    g_logRotationCount = 0;

    // Determine current logDir
    std::string logDir;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logDir = g_config.logDir.empty() ? (getProgramDir() + "/logs") : g_config.logDir;
    }
    ensureDirExists(logDir);

    // Close existing and open new day’s file
    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }

    g_currentLogFile = logDir + "/Encoder" + g_currentLogDate + ".log";
    g_logFile.open(g_currentLogFile, std::ios::app);
    if (g_logFile.is_open()) {
        g_logFile << "[" << timestamp() << "] [log] File opened: " << g_currentLogFile << std::endl;
        g_logFile.flush();
    }
    pruneOldLogs(14);

}

void logMessage(const std::string& msg);

void initLogFile() {
    // Determine logDir: prefer config value; otherwise default to <program_dir>/logs
    std::string logDir;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logDir = g_config.logDir;
    }
    if (logDir.empty()) {
        logDir = getProgramDir() + "/logs";
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config.logDir = logDir;
    }

    // Ensure directory exists
    if (!ensureDirExists(logDir)) {
        logDir = getProgramDir();
        logMessage("[log] Failed to create logs directory, falling back to program directory: " + logDir);
        ensureDirExists(logDir);
    }

    // Build dated filename inside the chosen logDir
    g_currentLogDate = currentDate();
    std::string baseName = logDir + "/Encoder" + g_currentLogDate + ".log";
    g_currentLogFile = baseName;

    g_logFile.open(baseName, std::ios::app);
    g_logRotationCount = 0;

    // Force an initial write so the file always appears
    if (g_logFile.is_open()) {
        g_logFile << "[" << timestamp() << "] [log] File opened: " << baseName << std::endl;
        g_logFile.flush();
    }
    // After opening the log file
    pruneOldLogs(14);
}
bool fileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ensureDirExists(const std::string& path) {
    try {
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
            logMessage("[filesystem] Created missing directory: " + path);
        } else if (!std::filesystem::is_directory(path)) {
            logMessage("[filesystem] Path exists but is not a directory: " + path);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        logMessage(std::string("[filesystem] Failed to ensure dir: ") + path + " (" + e.what() + ")");
        return false;
    }
}

// Ability to configure a Config Directory Location
std::string g_appRoot;     // e.g., "/home/andys/CLionProjects/AACencoderAudacy/cmake-build-debug"
std::string g_configDir;   // computed default: g_appRoot + "/config"

// Call this early in main()
static std::string dirname(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

void initAppRoot(const char* argv0) {
#ifdef __linux__
    // Resolve the absolute path to the running executable
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exePath;
    if (n > 0) {
        buf[n] = '\0';
        exePath = std::string(buf);
    } else {
        // Fallback to argv0 if /proc/self/exe unavailable
        exePath = argv0 ? std::string(argv0) : std::string(".");
    }
    g_appRoot = dirname(exePath);
#else
    // Non-Linux fallback: best effort from argv0
    g_appRoot = dirname(argv0 ? std::string(argv0) : std::string("."));
#endif

    g_configDir = g_appRoot + "/config";
    logMessage("[app] Root: " + g_appRoot + " | Default config dir: " + g_configDir);
}


// Returns absolute path for a file intended to live under the config directory.
static std::string resolveConfigPath(const std::string& baseDir, const std::string& name) {
    if (name.empty()) return "";
    // Absolute file name stays absolute
    if (!name.empty() && name[0] == '/') return name;

    // Prefer provided baseDir, else default app-root config dir
    std::string root = baseDir.empty() ? g_configDir : baseDir;

    // If root is relative, anchor to g_appRoot (handles "./config" and "config")
    if (!root.empty() && root[0] != '/') {
        if (root.rfind("./", 0) == 0) {
            root = root.substr(2);
        }
        root = g_appRoot + "/" + root;
    }

    return root + "/" + name;
}

// Parse "YYYY-MM-DD" from "EncoderYYYY-MM-DD.log" filename; returns empty string if not matching
std::string extractDateFromLogName(const std::string& name) {
    // Expected base: EncoderYYYY-MM-DD.log, possibly with ".N" suffix
    // Find "Encoder" and ".log"
    const std::string prefix = "Encoder";
    const std::string suffix = ".log";
    auto p = name.find(prefix);
    auto s = name.find(suffix);
    if (p != 0 || s == std::string::npos) return "";

    // Date portion between prefix and suffix, remove any ".N" suffix first
    std::string core = name.substr(0, s); // "EncoderYYYY-MM-DD"
    // Strip any numbered rotation: "EncoderYYYY-MM-DD.log.N" => core unaffected
    // We already trimmed at ".log", so no rotation part here.
    if (core.size() <= prefix.size()) return "";
    return core.substr(prefix.size()); // "YYYY-MM-DD"
}

// Convert "YYYY-MM-DD" to time_t (midnight local)
time_t toTimeT(const std::string& ymd) {
    if (ymd.size() != 10) return (time_t)0;
    struct tm t{};
    t.tm_isdst = -1;
    // Parse
    try {
        t.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
        t.tm_mon  = std::stoi(ymd.substr(5, 2)) - 1;
        t.tm_mday = std::stoi(ymd.substr(8, 2));
    } catch (...) { return (time_t)0; }
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
    return mktime(&t);
}
// Delete files older than 'keepDays' based on date in filename
void pruneOldLogs(int keepDays) {
    std::string logDir;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logDir = g_config.logDir.empty() ? (getProgramDir() + "/logs") : g_config.logDir;
    }
    DIR* d = opendir(logDir.c_str());
    if (!d) {
        // Avoid noisy logging here; optional:
        return;
    }

    // Threshold time
    time_t now = time(nullptr);
    const time_t threshold = now - (keepDays * 24 * 60 * 60);

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        // Match base and rotated logs:
        // - EncoderYYYY-MM-DD.log
        // - EncoderYYYY-MM-DD.log.N
        if (name.rfind("Encoder", 0) != 0) continue;
        if (name.find(".log") == std::string::npos) continue;

        // Extract date from the base name prior to any rotation suffix
        std::string dateStr = extractDateFromLogName(name);
        if (dateStr.empty()) continue;

        time_t logDay = toTimeT(dateStr);
        if (logDay == (time_t)0) continue;

        if (logDay < threshold) {
            std::string path = logDir + "/" + name;
            // Safety: don't delete the current active file
            if (path == g_currentLogFile) continue;

            if (std::remove(path.c_str()) == 0) {
                // Optionally log once per prune cycle from caller
            } else {
                // Optionally log failure
            }
        }
    }
    closedir(d);
}
// Rotate log if >10MB
void rotateLogIfNeeded() {
    g_logFile.flush();
    if (fileSize(g_currentLogFile) > 10 * 1024 * 1024) {
        // Roll to a numbered log in the same day
        g_logFile.close();
        g_logRotationCount++;
        std::string rotated = g_currentLogFile + "." + std::to_string(g_logRotationCount);
        g_logFile.open(rotated, std::ios::app);
        if (g_logFile.is_open()) {
            g_logFile << "[" << timestamp() << "] [log] Rotated due to size >10MB, now writing to: " << rotated << std::endl;
            g_logFile.flush();
        }
    }
}

// Log function
void logMessage(const std::string& msg) {
    if (!g_logFile.is_open()) initLogFile();
    const std::string line = "[" + timestamp() + "] " + msg;

    g_logFile << line << std::endl;
    g_logFile.flush();
    rotateLogIfNeeded();

    std::cout << line << std::endl;

    // New: broadcast to WebSocket clients
    broadcastLogLine(line);
}

// ---------------- HLS globals ----------------
struct HlsState {
    std::deque<uint8_t> buffer; // encoded AAC bytes accumulating
    std::atomic<bool>   running{false};
    std::thread         thread;
    std::mutex          mutex;

    int                 lastPurgedSeq = -1;
    int                 seq = 0;
    std::chrono::system_clock::time_point segStart;
    double              targetSeconds = 6.0;
    int                 window = 5;
    std::string         dir;      // absolute path to HLS folder
    std::string         playlist; // index.m3u8 path

    // Per-segment metadata snapshot: seq number -> JSON string captured when the segment was written.
    // This ensures each EXTINF tag carries the metadata that was actually current when that audio
    // window was encoded, so the Orban 5950 (and similar processors) display the correct "now playing"
    // rather than whatever happens to be in g_currentMetaJson at playlist-rebuild time.
    std::map<int, std::string> segmentMeta;
};

static HlsState g_hls;
static std::mutex g_hlsCtrlMutex; // to serialize start/stop requests
static std::string g_currentMetaJson;
static std::string g_currentMetaXml;

// Parse XML for Icecast using iceDataParse json file.
std::string parseMetadataXML(const std::string& xml, const std::string& stationId) {
    // Snapshot config
    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    // Resolve mapping file path using configDir with fallback
    const std::string mappingFile = resolveConfigPath(cfgCopy.configDir, cfgCopy.iceDataParse);
    logMessage("[metadata] Resolving iceDataParse: base=" + cfgCopy.configDir +
               " file=" + cfgCopy.iceDataParse +
               " -> " + mappingFile);

    // Load mapping JSON fresh on each call
    nlohmann::json mapping;
    try {
        std::ifstream f(mappingFile);
        if (f) {
            f >> mapping;
        } else {
            logMessage("[metadata] iceDataParse not found: " + mappingFile);
        }
    } catch (...) {
        logMessage("[metadata] Failed reading iceDataParse: " + mappingFile);
    }

    // Parse XML
    xmlDocPtr doc = xmlReadMemory(xml.c_str(), (int)xml.size(),
                                  "meta.xml", nullptr,
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return "";

    xmlNode* root = xmlDocGetRootElement(doc);
    if (!root) { xmlFreeDoc(doc); return ""; }

    auto getText = [](xmlNode* root, const char* name)->std::string {
        for (xmlNode* cur = root->children; cur; cur = cur->next) {
            if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST name)) {
                xmlChar* c = xmlNodeGetContent(cur);
                if (c) {
                    std::string val = reinterpret_cast<char*>(c);
                    xmlFree(c);
                    return val;
                }
            }
        }
        return "";
    };

    // Build output using mapping fields
    std::ostringstream oss;

    // --- Template mode: if "template" key is present, use {field} substitution ---
    if (mapping.contains("template") && mapping["template"].is_string()) {
        std::string tmpl = mapping["template"].get<std::string>();

        // Helper lambda: replace all occurrences of 'from' with 'to' in 'str'
        auto replaceAll = [](std::string str, const std::string& from, const std::string& to) -> std::string {
            size_t pos = 0;
            while ((pos = str.find(from, pos)) != std::string::npos) {
                str.replace(pos, from.size(), to);
                pos += to.size();
            }
            return str;
        };

        // Support {stationId} as a special placeholder
        tmpl = replaceAll(tmpl, "{stationId}", stationId);

        // Collect all XML field names referenced in the template and substitute them
        // We scan the template for {tagname} patterns and look each up in the XML
        size_t start = 0;
        while ((start = tmpl.find('{', start)) != std::string::npos) {
            size_t end = tmpl.find('}', start);
            if (end == std::string::npos) break;
            std::string tag = tmpl.substr(start + 1, end - start - 1);
            std::string val = getText(root, tag.c_str());
            tmpl = replaceAll(tmpl, "{" + tag + "}", val);
            start = 0; // restart scan after substitution
        }

        oss << tmpl;
        logMessage("[metadata] template result: " + oss.str());
    } else {
        // --- Legacy mode: optionally prepend stationId then append field values with separator ---
        // "includeStationId" defaults to true for backward compatibility
        bool includeStationId = true;
        if (mapping.contains("includeStationId") && mapping["includeStationId"].is_boolean()) {
            includeStationId = mapping["includeStationId"].get<bool>();
        }
        if (includeStationId) {
            oss << stationId;
        }

        // Default separator is " | " if not specified
        std::string sep = " | ";
        if (mapping.contains("separator") && mapping["separator"].is_string()) {
            sep = mapping["separator"].get<std::string>();
        }

        if (mapping.contains("fields") && mapping["fields"].is_array()) {
            bool first = !includeStationId; // skip leading separator when stationId is omitted
            for (const auto& field : mapping["fields"]) {
                std::string tag = field.get<std::string>();
                std::string val = getText(root, tag.c_str());
                if (!first) oss << sep;
                oss << val;
                first = false;
            }
        } else {
            // Fallback: no mapping -> produce just stationId (or empty string)
            logMessage("[metadata] iceDataParse missing 'fields' array, using stationId only");
        }
    }

    xmlFreeDoc(doc);
    return oss.str();
}

// Build HLS metadata JSON from a batch of <nowplaying> elements sent by the automation system.
// The automation sends the stack as concatenated <nowplaying> elements (no wrapping root):
//   - First element (empty/absent stack_pos, air_time set): the "now on air" trigger – same song as stack_pos=0
//   - stack_pos=0 : currently playing  ← becomes "current"
//   - stack_pos=1,2,3... : upcoming items ← becomes "upcoming[]"
// We wrap in a synthetic root so libxml2 can iterate all children at once.
nlohmann::ordered_json buildHlsMetaJson(const std::string& xml, const std::string& stationId) {
    nlohmann::ordered_json j;
    try {
        // Wrap concatenated elements so the parser sees a single-root document.
        std::string wrapped = "<_batch_>" + xml + "</_batch_>";
        xmlDocPtr doc = xmlReadMemory(wrapped.c_str(), (int)wrapped.size(),
                                      "meta.xml", nullptr,
                                      XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
        if (!doc) return j;
        xmlNode* batchRoot = xmlDocGetRootElement(doc);
        if (!batchRoot) { xmlFreeDoc(doc); return j; }

        // Helper: get text content of a named child element
        auto getText = [](xmlNode* node, const char* name) -> std::string {
            for (xmlNode* cur = node->children; cur; cur = cur->next) {
                if (cur->type == XML_ELEMENT_NODE && xmlStrEqual(cur->name, BAD_CAST name)) {
                    xmlChar* c = xmlNodeGetContent(cur);
                    if (c) {
                        std::string val = reinterpret_cast<char*>(c);
                        xmlFree(c);
                        return val;
                    }
                }
            }
            return "";
        };

        struct NowPlayingItem {
            std::string stackPos;
            std::string airTime;
            std::string type;
            std::string title;
            std::string artist;
            std::string album;    // automation sends album info in <trivia>
            std::string duration; // milliseconds as string
            std::string cart;
            std::string category;
        };

        // Collect all <nowplaying> children in document order
        std::vector<NowPlayingItem> items;
        for (xmlNode* child = batchRoot->children; child; child = child->next) {
            if (child->type == XML_ELEMENT_NODE &&
                xmlStrEqual(child->name, BAD_CAST "nowplaying")) {
                NowPlayingItem item;
                item.stackPos = getText(child, "stack_pos");
                item.airTime  = getText(child, "air_time");
                item.type     = getText(child, "media_type");
                item.title    = getText(child, "title");
                item.artist   = getText(child, "artist");
                item.album    = getText(child, "trivia");    // <trivia> carries the album/show name
                item.duration = getText(child, "duration");  // milliseconds
                item.cart     = getText(child, "cart");
                item.category = getText(child, "category");
                items.push_back(std::move(item));
            }
        }
        xmlFreeDoc(doc);

        if (items.empty()) return j;

        // Select current item: prefer stack_pos == "0"; fall back to first with air_time set;
        // last resort: first element in document order.
        const NowPlayingItem* current = nullptr;
        for (const auto& item : items) {
            if (item.stackPos == "0") { current = &item; break; }
        }
        if (!current) {
            for (const auto& item : items) {
                if (!item.airTime.empty()) { current = &item; break; }
            }
        }
        if (!current) current = &items[0];

        // Collect upcoming items: stack_pos >= 1, sorted numerically
        std::vector<const NowPlayingItem*> upcomingItems;
        for (const auto& item : items) {
            if (item.stackPos.empty()) continue;
            try {
                if (std::stoi(item.stackPos) >= 1)
                    upcomingItems.push_back(&item);
            } catch (...) {}
        }
        std::sort(upcomingItems.begin(), upcomingItems.end(),
                  [](const NowPlayingItem* a, const NowPlayingItem* b) {
                      return std::stoi(a->stackPos) < std::stoi(b->stackPos);
                  });

        // Build a JSON object for one item; startSecs is estimated epoch start time
        auto makeItem = [](const NowPlayingItem* item, double startSecs) -> nlohmann::ordered_json {
            double durSeconds = 0.0;
            if (!item->duration.empty()) {
                try {
                    durSeconds = std::stod(item->duration);
                    // Automation sends milliseconds; convert to seconds
                    if (durSeconds > 1000.0) durSeconds /= 1000.0;
                } catch (...) {}
            }
            nlohmann::ordered_json o;
            o["type"]     = item->type;
            o["title"]    = item->title;
            o["album"]    = item->album;
            o["artist"]   = item->artist;
            o["image"]    = "";
            o["duration"] = durSeconds;
            o["start"]    = startSecs;
            o["id"]       = item->cart;
            o["category"] = item->category;
            return o;
        };

        const double nowSecs = nowStartEpochSeconds();

        j["current"] = makeItem(current, nowSecs);

        // Estimate start times for upcoming items by chaining durations
        nlohmann::ordered_json upcomingArr = nlohmann::ordered_json::array();
        double nextStart = nowSecs;
        {
            // Advance past current item's duration
            double d = 0.0;
            try { d = std::stod(current->duration); if (d > 1000.0) d /= 1000.0; } catch (...) {}
            nextStart += d;
        }
        for (const auto* item : upcomingItems) {
            upcomingArr.push_back(makeItem(item, nextStart));
            double d = 0.0;
            try { d = std::stod(item->duration); if (d > 1000.0) d /= 1000.0; } catch (...) {}
            nextStart += d;
        }
        j["upcoming"] = upcomingArr;

        logMessage("[metadataServer] HLS JSON metadata: " + j.dump());

    } catch (...) {
        // leave empty if parsing fails
    }
    return j;
}

// Simple HTTP server starter for HLS (headless mode)
void startHlsServerFromConfig() {
    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    const int port = cfgCopy.webPort;
    static httplib::Server svr;

    // Resolve HLS root and segments dir
    std::string hlsRoot = cfgCopy.hlsPath.empty()
        ? (getProgramDir() + "/HLS")
        : cfgCopy.hlsPath;

    std::string segmentsDir = hlsRoot + "/segments";
    ensureDirExists(hlsRoot);
    ensureDirExists(segmentsDir);

    {
        std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
        ensureDirExists(hlsRoot);
        ensureDirExists(segmentsDir);
        g_hls.dir = segmentsDir;                 // segments directory
    }
    {
    // Remove index.m3u8 and master.m3u8 if they exist
    std::string indexFile = hlsRoot + "/index.m3u8";
    std::string masterFile = hlsRoot + "/master.m3u8";
    if (fs::exists(indexFile)) fs::remove(indexFile);
    if (fs::exists(masterFile)) fs::remove(masterFile);
    }

    // Explicit handler for master playlist
    svr.Get("/hls/master.m3u8", [hlsRoot](const httplib::Request&, httplib::Response& res) {
        std::string path = hlsRoot + "/master.m3u8";
        std::ifstream f(path);
        if (!f.is_open()) { res.status = 404; return; }
        std::ostringstream ss; ss << f.rdbuf();
        //res.set_content(ss.str(), "application/vnd.apple.mpegurl");
        res.set_content(ss.str(), "audio/mpegurl");

    });

    // Explicit handler for playlist
    svr.Get("/hls/index.m3u8", [](const httplib::Request&, httplib::Response& res) {
        std::string path;
        { std::lock_guard<std::mutex> lock(g_hlsCtrlMutex); path = g_hls.playlist; }

        std::ifstream f(path);
        if (!f.is_open()) { res.status = 404; return; }
        std::ostringstream ss; ss << f.rdbuf();
        //res.set_content(ss.str(), "application/vnd.apple.mpegurl");
        res.set_content(ss.str(), "audio/mpegurl");
    });

    // Explicit handler for AAC segments
    svr.Get(R"(/hls/segments/(.*\.aac))", [segmentsDir](const httplib::Request& req, httplib::Response& res) {
        std::string path = segmentsDir + "/" + req.matches[1].str();
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { res.status = 404; return; }

        // Get file size
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        f.seekg(0, std::ios::beg);

        // Provide content with correct MIME
        res.set_content_provider(
            static_cast<size_t>(size), "audio/aac",
            [path](size_t offset, size_t length, httplib::DataSink &sink) {
                std::ifstream f(path, std::ios::binary);
                f.seekg(offset);
                std::vector<char> buf(length);
                f.read(buf.data(), length);
                sink.write(buf.data(), f.gcount());
                return true;
            }
        );
    });


    // Mount only the segments directory under /hls/segments
    //svr.set_mount_point("/hls/segments", segmentsDir.c_str());  //Remove for NodePing Changes

    // Info route
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("HLS: /hls/index.m3u8 playlist, segments under /hls/segments/", "text/plain");
    });

    std::thread([port]() {
        logMessage("[HTTP] Headless HLS server on http://0.0.0.0:" + std::to_string(port));
        logMessage("[HTTP] HLS playlist at http://localhost:" + std::to_string(port) + "/hls/index.m3u8");
        svr.listen("0.0.0.0", port);
    }).detach();
}

bool sendNowPlayingToAmperWave(const std::string& stationId,
                               const std::string& artist,
                               const std::string& title) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        logMessage("[AmperWave] curl_easy_init failed");
        return false;
    }

    auto escape = [](const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '\"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };

    std::string payload = "{\"stationId\":\"" + escape(stationId) +
                          "\",\"artist\":\"" + escape(artist) +
                          "\",\"title\":\"" + escape(title) + "\"}";

    logMessage("[AmperWave] Sending request: " + payload);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AACIceEncoderWebUI/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        logMessage(std::string("[AmperWave] curl error: ") + curl_easy_strerror(res));
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        logMessage("[AmperWave] HTTP status: " + std::to_string(code));
        if (code < 200 || code >= 300) {
            res = CURLE_HTTP_RETURNED_ERROR;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}


// Save config to disk
bool saveConfigToFile(const Config& cfg, const std::string& filename = "") {
    std::string targetFile = filename.empty() ? g_configPath : filename;
    size_t slash = targetFile.find_last_of('/');
    if (slash != std::string::npos) ensureDirExists(targetFile.substr(0, slash));
    logMessage("[config] Saving to " + targetFile);

    json j = {
        {"webPort", cfg.webPort},
        {"controlPort", cfg.controlPort},
        {"controlEnabled", cfg.controlEnabled},
        {"commandStart", cfg.commandStart},
        {"commandStop", cfg.commandStop},
        // Icecast settings
        {"iceEnabled", cfg.iceEnabled},
        {"iceMetaEnabled", cfg.iceMetaEnabled},
        {"icecastUrl", cfg.icecastUrl},
        {"icyMetaInt", cfg.icyMetaInt},
        {"user", cfg.user},
        {"pass", cfg.pass},
        {"icecastInterface", cfg.icecastInterface},
        // MP3 fields
        {"mp3Enabled", cfg.mp3Enabled},
        {"mp3MetaEnabled", cfg.mp3MetaEnabled},
        {"mp3IcecastUrl", cfg.mp3IcecastUrl},
        {"mp3User", cfg.mp3User},
        {"mp3Pass", cfg.mp3Pass},

        // Station / logging
        {"amperwaveStationId", cfg.amperwaveStationId},
           {"iceDataParse", cfg.iceDataParse},
        {"logDir", cfg.logDir},

        // Audio
        {"bitrate", cfg.bitrate},
        {"channels", cfg.channels},
        {"sampleRate", cfg.sampleRate},
        {"deviceIndex", cfg.deviceIndex},
        {"listenPort", cfg.listenPort},

        // Input
        {"inputType", cfg.inputType},
        {"rtpAddress", cfg.rtpAddress},
        {"rtpPort", cfg.rtpPort},
        {"rtpInterface", cfg.rtpInterface},
        {"rtpGain", cfg.rtpGain},

        // HLS
        {"hlsEnabled", cfg.hlsEnabled},
        {"hlsSegmentSeconds", cfg.hlsSegmentSeconds},
        {"hlsWindow", cfg.hlsWindow},
        {"hlsStartTimeOffset", cfg.hlsStartTimeOffset},
        {"hlsPath", cfg.hlsPath},
        {"hlsMetaEnabled", cfg.hlsMetaEnabled},
        // NEW: backend-only field written to disk; you may omit from /getconfig if desired
        {"configDir", cfg.configDir},
    };

    std::ofstream out(targetFile);
    if (!out.is_open()) return false;
    out << j.dump(4);
    return true;
}


bool loadConfigFromFile(Config& cfg, const std::string& filename = "") {
    std::string targetFile = filename.empty() ? g_configPath : filename;
    std::ifstream in(targetFile);
    if (!in.is_open()) return false;
    json j; in >> j;

    cfg.webPort            = j.value("webPort", cfg.webPort);
    cfg.controlPort        = j.value("controlPort", cfg.controlPort);
    cfg.controlEnabled     = j.value("controlEnable", cfg.controlEnabled);
    cfg.commandStart       = j.value("commandStart", cfg.commandStart);
    cfg.commandStop        = j.value("commandStop", cfg.commandStop);
    // Icecast toggles
    cfg.iceEnabled         = j.value("iceEnabled", cfg.iceEnabled);
    cfg.iceMetaEnabled     = j.value("iceMetaEnabled", cfg.iceMetaEnabled);

    // Icecast connection settings
    cfg.icecastUrl         = j.value("icecastUrl", cfg.icecastUrl);
    cfg.user               = j.value("user", cfg.user);
    cfg.pass               = j.value("pass", cfg.pass);
    cfg.amperwaveStationId = j.value("amperwaveStationId", cfg.amperwaveStationId);
    cfg.iceDataParse       = j.value("iceDataParse", cfg.iceDataParse);
    cfg.mp3Enabled         = j.value("mp3Enabled", cfg.mp3Enabled);
    cfg.mp3MetaEnabled     = j.value("mp3MetaEnabled", cfg.mp3MetaEnabled);
    cfg.mp3IcecastUrl      = j.value("mp3IcecastUrl", cfg.mp3IcecastUrl);
    cfg.mp3User            = j.value("mp3User", cfg.mp3User);
    cfg.mp3Pass            = j.value("mp3Pass", cfg.mp3Pass);
    cfg.logDir             = j.value("logDir", cfg.logDir);
    cfg.sampleRate         = j.value("sampleRate", cfg.sampleRate);
    cfg.channels           = j.value("channels", cfg.channels);
    cfg.bitrate            = j.value("bitrate", cfg.bitrate);
    cfg.deviceIndex        = j.value("deviceIndex", cfg.deviceIndex);
    cfg.listenPort         = j.value("listenPort", cfg.listenPort);
    cfg.icyMetaInt         = j.value("icyMetaInt", cfg.icyMetaInt);

    // RTP + interface
    cfg.inputType       = j.value("inputType", cfg.inputType);
    cfg.rtpAddress      = j.value("rtpAddress", cfg.rtpAddress);
    cfg.rtpPort         = j.value("rtpPort", cfg.rtpPort);
    cfg.rtpInterface    = j.value("rtpInterface", cfg.rtpInterface);
    cfg.icecastInterface= j.value("icecastInterface", cfg.icecastInterface);
    cfg.rtpGain         = j.value("rtpGain", 1.0f);

    // HLS

    cfg.hlsEnabled          = j.value("hlsEnabled", cfg.hlsEnabled);
    cfg.hlsSegmentSeconds   = j.value("hlsSegmentSeconds", cfg.hlsSegmentSeconds);
    cfg.hlsWindow           = j.value("hlsWindow", cfg.hlsWindow);
    cfg.hlsStartTimeOffset  = j.value("hlsStartTimeOffset", cfg.hlsStartTimeOffset);
    cfg.hlsPath             = j.value("hlsPath", cfg.hlsPath);
    cfg.hlsMetaEnabled      = j.value("hlsMetaEnabled", cfg.hlsMetaEnabled);
    cfg.configDir = j.value("configDir", "");

    return true;
}
//------------------Control Server------------------

// Optional: serialize command execution to avoid overlapping start/stop operations
//static std::mutex g_commandMutex;

// Trim leading/trailing whitespace (spaces, tabs, CR/LF), keep case sensitivity
static inline std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Helper: process one command line (case-sensitive exact match)
static void processControlCommand(const std::string& line, int client_fd) {
    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    logMessage("[controlServer] Comparing line='" + line +
               "' to commandStart='" + cfgCopy.commandStart +
               "', commandStop='" + cfgCopy.commandStop + "'");

    if (line == cfgCopy.commandStart) {
        logMessage("[controlServer] Start command received");

        bool okAac = true, okMp3 = true, okHls = true;

        if (cfgCopy.iceEnabled) {
            okAac = startEncoderWithConfig();
        } else {
            logMessage("[controlServer] AAC Icecast disabled by config");
        }

        if (cfgCopy.mp3Enabled) {
            okMp3 = startMp3EncoderWithConfig();
        } else {
            logMessage("[controlServer] MP3 Icecast disabled by config");
        }

        if (cfgCopy.hlsEnabled) {
            okHls = startHlsWithConfig();
        } else {
            logMessage("[controlServer] HLS disabled by config");
        }

        maybeStopEngineIfIdle();

        std::string resp = "START: AAC(" + std::string(okAac ? "ok" : "skip") +
                           ") MP3(" + std::string(okMp3 ? "ok" : "skip") +
                           ") HLS(" + std::string(okHls ? "ok" : "skip") + ")\n";
        (void)write(client_fd, resp.c_str(), resp.size());
        return;
    }

    if (line == cfgCopy.commandStop) {
        logMessage("[controlServer] Stop command received");

        stopEncoder();
        stopMp3EncoderWithConfig();
        stopHls();
        stopAudioEngine();

        const char* resp = "STOP: all streams and engine stopped\n";
        (void)write(client_fd, resp, std::strlen(resp));
        return;
    }

    const char* resp = "UNKNOWN COMMAND\n";
    (void)write(client_fd, resp, std::strlen(resp));
}


void controlServer(int port) {
    logMessage("[controlServer] Starting on port " + std::to_string(port));

    g_controlServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_controlServerFd < 0) { perror("socket"); return; }

    int opt = 1;
    setsockopt(g_controlServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_controlServerFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(g_controlServerFd); g_controlServerFd = -1; return;
    }
    if (listen(g_controlServerFd, 8) < 0) {
        perror("listen"); close(g_controlServerFd); g_controlServerFd = -1; return;
    }

    controlRunning = true;

    while (!controlShutdown) {
        int client_fd = accept(g_controlServerFd, nullptr, nullptr);
        if (client_fd < 0) {
            if (controlShutdown) break;
            perror("accept");
            continue;
        }

        logMessage("[controlServer] Client connected");

        std::string pending;
        struct pollfd pfd{client_fd, POLLIN, 0};
        while (!controlShutdown) {
            int ret = poll(&pfd, 1, 200);
            if (ret <= 0) continue;

            if (pfd.revents & POLLIN) {
                char buf[4096];
                ssize_t n = read(client_fd, buf, sizeof(buf));
                if (n <= 0) break;

                // Log raw payload immediately
                std::string raw(buf, static_cast<size_t>(n));
                logMessage("[controlServer] Raw control payload: " + raw);

                pending.append(raw);

                // Split on '\n' and process each line
                size_t pos = 0;
                while (true) {
                    size_t nl = pending.find('\n', pos);
                    if (nl == std::string::npos) {
                        pending.erase(0, pos);
                        break;
                    }

                    std::string line = pending.substr(pos, nl - pos);
                    line = trimWs(line);

                    if (!line.empty()) {
                        logMessage("[controlServer] Parsed control command: '" + line + "'");
                        processControlCommand(line, client_fd);
                    }

                    pos = nl + 1;
                    if (pos >= pending.size()) {
                        pending.clear();
                        break;
                    }
                }
            }
        }

        // Flush any leftover data if client disconnects without newline
        if (!pending.empty()) {
            std::string line = trimWs(pending);
            if (!line.empty()) {
                logMessage("[controlServer] Parsed control command (EOF): '" + line + "'");
                processControlCommand(line, client_fd);
            }
            pending.clear();
        }

        close(client_fd);
        logMessage("[controlServer] Client disconnected");
    }

    if (g_controlServerFd >= 0) {
        close(g_controlServerFd);
        g_controlServerFd = -1;
    }

    controlRunning = false;
    logMessage("[controlServer] Listener stopped");
}


void startControlListener() {
    if (!g_config.controlEnabled) {
        logMessage("[control] Disabled in config");
        return;
    }
    if (controlRunning) {
        logMessage("[control] Already running");
        return;
    }
    controlShutdown = false;
    controlThread = std::thread(controlServer, g_config.controlPort);
    logMessage("[control] Listener start requested on port " + std::to_string(g_config.controlPort));
}

void stopControlListener() {
    if (!controlRunning) {
        logMessage("[control] Not running");
        return;
    }

    logMessage("[control] Stop requested");
    controlShutdown = true;

    // Self-connect to unblock accept() immediately
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(g_config.controlPort);
        connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        close(sock);
    }

    if (controlThread.joinable()) {
        controlThread.join();
    }

    controlRunning = false;
    logMessage("[control] Listener stopped successfully");
}



// ---------------- Metadata server ----------------
void metadataServer(int port) {
    logMessage("[metadataServer] Starting on port " + std::to_string(port));

    g_metadataServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_metadataServerFd < 0) {
        perror("[metadataServer] socket");
        metaRunning = false;
        return;
    }

    int opt = 1;
    setsockopt(g_metadataServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_metadataServerFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[metadataServer] bind");
        close(g_metadataServerFd);
        g_metadataServerFd = -1;
        metaRunning = false;
        return;
    }
    logMessage("[metadataServer] Bound on port " + std::to_string(port));

    if (listen(g_metadataServerFd, 8) < 0) {
        perror("[metadataServer] listen");
        close(g_metadataServerFd);
        g_metadataServerFd = -1;
        metaRunning = false;
        return;
    }
    metaRunning = true; // mark as running
    metaShutdown = false; // reset shutdown flag
    logMessage("[metadataServer] Listening on port " + std::to_string(port));

    // Set socket to non-blocking for accept()
    int flags = fcntl(g_metadataServerFd, F_GETFL, 0);
    fcntl(g_metadataServerFd, F_SETFL, flags | O_NONBLOCK);

    while (!metaShutdown) {
        // Check global shutdown flag
        if (!g_running) {
            break;
        }
        int client_fd = accept(g_metadataServerFd, nullptr, nullptr);
        if (client_fd < 0) {
            if (metaShutdown) {
                logMessage("[metadataServer] Shutdown requested, breaking accept loop");
                break;
            }
            // For non-blocking socket, EAGAIN/EWOULDBLOCK is normal
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            perror("[metadataServer] accept");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        logMessage("[metadataServer] Client connected on port " + std::to_string(port));

        // Snapshot stationId once per connection (avoids repeated lock)
        std::string stationId;
        { std::lock_guard<std::mutex> lock(g_configMutex);
          stationId = g_config.amperwaveStationId; }

        // Per-connection accumulation buffer.
        // The automation sends ALL stack items as back-to-back <nowplaying> elements in one burst.
        // TCP may deliver them in one read() or split across several reads (especially in testing
        // with nc). We accumulate everything seen within a short "settle" window (100 ms of silence)
        // before processing, so buildHlsMetaJson always receives the complete batch and can select
        // stack_pos=0 as current and build upcoming[] from the rest.
        std::string connBuffer;
        connBuffer.reserve(16384);
        bool hasPending = false;

        // Lambda: process whatever is accumulated in connBuffer, then clear it
        auto processBatch = [&]() {
            if (connBuffer.empty()) return;

            // Strip leading whitespace; skip if it's not XML
            size_t first = connBuffer.find_first_not_of(" \r\n\t");
            if (first == std::string::npos || connBuffer[first] != '<') {
                // Plain-text path (unchanged behaviour)
                std::string trimmed = connBuffer;
                trimmed.erase(0, first == std::string::npos ? trimmed.size() : first);
                trimmed.erase(trimmed.find_last_not_of(" \r\n\t") + 1);
                if (!trimmed.empty())
                    logMessage("[metadataServer] Plain text message received: " + trimmed);
                connBuffer.clear();
                hasPending = false;
                return;
            }

            // ---------- XML path ----------
            {
                std::lock_guard<std::mutex> lock(g_metaMutex);
                g_currentMetaXml = connBuffer;
            }

            // parseMetadataXML expects a single-root XML document (it was written when the
            // automation only sent one <nowplaying> at a time).  The batch from automation is
            // multiple back-to-back root elements, which libxml2 rejects without a wrapper.
            // Extract just the first <nowplaying>…</nowplaying> for ICY/Icecast output.
            std::string firstElement;
            {
                const std::string closeTag = "</nowplaying>";
                size_t endPos = connBuffer.find(closeTag);
                if (endPos != std::string::npos) {
                    firstElement = connBuffer.substr(0, endPos + closeTag.size());
                } else {
                    firstElement = connBuffer; // single element or malformed — use as-is
                }
            }

            std::string formatted = parseMetadataXML(firstElement, stationId);
            if (!formatted.empty()) {
                std::lock_guard<std::mutex> lock(g_metaMutex);
                g_currentMeta = formatted;
                g_metaHistory.push_front(formatted);
                if (g_metaHistory.size() > 5) g_metaHistory.pop_back();
                logMessage("[metadataServer] Icecast Metadata updated: " + formatted);
            }

            // HLS JSON metadata — always process the full batch regardless of ICY result.
            // buildHlsMetaJson wraps the batch in a synthetic root so libxml2 handles it,
            // selects stack_pos=0 as current, and builds upcoming[] from the rest.
            auto j = buildHlsMetaJson(connBuffer, stationId);
            if (!j.empty()) {
                std::lock_guard<std::mutex> lock(g_metaMutex);
                g_currentMetaJson = j.dump();
                logMessage("[metadataServer] HLS JSON metadata updated");
            } else {
                logMessage("[metadataServer] HLS JSON metadata build failed or empty");
            }

            connBuffer.clear();
            hasPending = false;
        };

struct pollfd pfd{client_fd, POLLIN, 0};
while (!metaShutdown) {
    // Use a short poll timeout when data has arrived recently (settle window).
    // Use the normal 200 ms otherwise (saves CPU while idle).
    int pollMs = hasPending ? 100 : 200;
    int ret = poll(&pfd, 1, pollMs);
    if (ret < 0) {
        perror("[metadataServer] poll");
        break;
    }

    if (ret == 0) {
        // Timeout: if we have accumulated data and nothing more arrived within the settle
        // window, declare the burst complete and process it.
        if (hasPending) processBatch();
        continue;
    }

    if (pfd.revents & POLLIN) {
        std::vector<char> buf(16384);
        ssize_t n = read(client_fd, buf.data(), buf.size());
        if (n <= 0) {
            // Connection closed or error — flush any pending data first
            if (hasPending) processBatch();
            break;
        }
        if (metaShutdown) {
            logMessage("[metadataServer] Stop requested, aborting client read");
            break;
        }

        try {
            connBuffer.append(buf.data(), n);
            hasPending = true;
        } catch (const std::exception& e) {
            logMessage(std::string("[metadataServer] Exception: ") + e.what());
        } catch (...) {
            logMessage("[metadataServer] Unknown exception");
        }
    }
}


        close(client_fd);
        logMessage("[metadataServer] Client disconnected on port " + std::to_string(port));
    }

    if (g_metadataServerFd >= 0) {
        close(g_metadataServerFd);
        g_metadataServerFd = -1;
    }
    metaRunning = false;
    logMessage("[metadataServer] Listener stopped on port " + std::to_string(port));
}

void stopMetadataListener() {
    std::lock_guard<std::mutex> guard(g_metaCtrlMutex);
    if (!metaRunning) {
        logMessage("[metadata] Listener not running");
        return;
    }

    logMessage("[metadata] Stop requested");
    metaShutdown = true;

    // Close the listening socket to unblock accept()
    if (g_metadataServerFd >= 0) {
        close(g_metadataServerFd);
        g_metadataServerFd = -1;
    }

    if (metaThread.joinable()) {
        metaThread.join();
    }

    metaRunning = false;
    logMessage("[metadata] Listener stopped successfully");
}



// Helper: parse icecast URL into host, port, and mount; supports http://host:port/mount
struct IcecastEndpoint {
    std::string host;
    std::string port = "8000";
    std::string mount; // e.g., /stream
};

bool parseIcecastUrl(const std::string& url, IcecastEndpoint& ep) {
    if (url.rfind("http://", 0) != 0) return false;
    std::string rest = url.substr(7);
    auto slash = rest.find('/');
    if (slash == std::string::npos) return false;
    std::string hostport = rest.substr(0, slash);
    ep.mount = rest.substr(slash); // includes leading '/'
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        ep.host = hostport.substr(0, colon);
        ep.port = hostport.substr(colon + 1);
    } else {
        ep.host = hostport;
    }
    return !ep.host.empty() && !ep.mount.empty();
}

// ---------------- Audio capture ----------------
struct AudioState {
    PaStream* stream = nullptr;
    int frameSize = 1024;       // Larger buffer to reduce overflow
    PaSampleFormat format = paInt16;
};

// Common interface for audio inputs
struct IAudioInput {
    virtual ~IAudioInput() {}
    virtual bool init() = 0;
    virtual bool readFrame(std::vector<int16_t>& pcm) = 0; // fill one frame of interleaved PCM
    virtual void shutdown() = 0;
    virtual int frameSize() const = 0;
};

// ---------------- PortAudio input ----------------
class PortAudioInput : public IAudioInput {
public:
    PortAudioInput(int deviceIndex, int channels, int sampleRate, int frameSize = 1024)
    : deviceIndex_(deviceIndex), channels_(channels), sampleRate_(sampleRate), frameSize_(frameSize) {}

    bool init() override {
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            logMessage(std::string("[PortAudio] init error: ") + Pa_GetErrorText(err));
            return false;
        }

        int numDevices = Pa_GetDeviceCount();
        logMessage("[PortAudio] Devices:");
        for (int i = 0; i < numDevices; i++) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (info) logMessage("  " + std::to_string(i) + ": " + std::string(info->name));
        }

        PaStreamParameters inParams{};
        inParams.device = (deviceIndex_ >= 0) ? deviceIndex_ : Pa_GetDefaultInputDevice();
        if (inParams.device == paNoDevice) {
            logMessage("[PortAudio] No input device");
            return false;
        }
        const PaDeviceInfo* di = Pa_GetDeviceInfo(inParams.device);
        logMessage(std::string("[PortAudio] Using: ") + di->name);

        inParams.channelCount = channels_;
        inParams.sampleFormat = paInt16; // use int16 directly
        inParams.suggestedLatency = di->defaultHighInputLatency;

        err = Pa_OpenStream(&stream_, &inParams, nullptr, sampleRate_, frameSize_, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            logMessage(std::string("[PortAudio] OpenStream failed: ") + Pa_GetErrorText(err));
            return false;
        }
        err = Pa_StartStream(stream_);
        if (err != paNoError) {
            logMessage(std::string("[PortAudio] StartStream failed: ") + Pa_GetErrorText(err));
            return false;
        }
        return true;
    }

    bool readFrame(std::vector<int16_t>& pcm) override {
        pcm.resize(frameSize_ * channels_);
        PaError err = Pa_ReadStream(stream_, pcm.data(), frameSize_);
        if (err == paNoError) return true;
        if (err == paInputOverflowed) {
            logMessage("[PortAudio] Input overflow");
            return true;
        }
        logMessage(std::string("[PortAudio] ReadStream error: ") + Pa_GetErrorText(err));
        return false;
    }

    void shutdown() override {
        if (stream_) {
            Pa_StopStream(stream_);
            Pa_CloseStream(stream_);
            stream_ = nullptr;
        }
        PaError err = Pa_Terminate();
        if (err != paNoError) {
            logMessage(std::string("[PortAudio] Terminate error: ") + Pa_GetErrorText(err));
        } else {
            logMessage("[PortAudio] Shutdown complete");
        }
    }



    int frameSize() const override { return frameSize_; }

private:
    PaStream* stream_ = nullptr;
    int deviceIndex_;
    int channels_;
    int sampleRate_;
    int frameSize_;
};

// ---------------- RTP input ----------------

// ---------------- RTP global socket + multicast init ----------------
int g_rtpSock = -1; // shared RTP socket

bool initRtpMulticast(const Config& cfg) {
    if (cfg.inputType != "rtp") return true; // nothing to do

    g_rtpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rtpSock < 0) {
        perror("[RTP] socket");
        return false;
    }

    int reuse = 1;
    setsockopt(g_rtpSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port   = htons(cfg.rtpPort);
    // CRITICAL FIX: Bind to the specific multicast group address, not INADDR_ANY
    // This prevents receiving packets from other multicast groups on the same port
    local.sin_addr.s_addr = inet_addr(cfg.rtpAddress.c_str());

    if (bind(g_rtpSock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("[RTP] bind");
        close(g_rtpSock);
        g_rtpSock = -1;
        return false;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(cfg.rtpAddress.c_str());

    std::string rtpIP = getInterfaceIP(cfg.rtpInterface);
    mreq.imr_interface.s_addr = rtpIP.empty() ? INADDR_ANY : inet_addr(rtpIP.c_str());

    logMessage("[RTP] Attempting multicast " + cfg.rtpAddress + ":" +
               std::to_string(cfg.rtpPort) + " on interface " +
               (cfg.rtpInterface.empty() ? "ANY" : cfg.rtpInterface +
                (rtpIP.empty() ? "" : " (" + rtpIP + ")")));

    if (setsockopt(g_rtpSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("[RTP] IP_ADD_MEMBERSHIP");
        close(g_rtpSock);
        g_rtpSock = -1;
        return false;
    }

    logMessage("[RTP] Subscribed to multicast " + cfg.rtpAddress + ":" +
               std::to_string(cfg.rtpPort) + " successfully");
    return true;
}


class RtpInput : public IAudioInput {
public:
    RtpInput(const std::string& addr, int port, const std::string& iface,
             int channels, int sampleRate,
             int frameSize = 240,
             float gain = 1.0f)
    : addr_(addr), port_(port), iface_(iface), channels_(channels),
      sampleRate_(sampleRate), frameSize_(frameSize), gain_(gain) {}

    bool init() override {
        if (g_rtpSock < 0) {
            logMessage("[RTP] Global socket not initialized");
            return false;
        }

        sock_ = g_rtpSock; // reuse global socket

        // Buffer + timeout (per instance)
        configureSocket();

        jbCap_ = (size_t)frameSize_ * (size_t)channels_ * 20;
        lastPacketTime_ = std::chrono::steady_clock::now();
        lastReconnectAttempt_ = lastPacketTime_;

        logMessage("[RTP] Initialized (reusing global socket). Gain=" + std::to_string(gain_));
        return true;
    }

bool readFrame(std::vector<int16_t>& pcm) override {
    pcm.clear();
    pcm.reserve((size_t)frameSize_ * (size_t)channels_);
    uint8_t pkt[1500];

    // RTP header parser (handles CSRC and extension)
    auto rtpHeaderLen = [](const uint8_t* buf, size_t len) -> size_t {
        if (len < 12) return 0;
        const uint8_t vpxcc = buf[0];
        const uint8_t cc = vpxcc & 0x0F;        // CSRC count
        const bool x = (buf[0] & 0x10) != 0;    // extension flag
        size_t hdr = 12 + (size_t)cc * 4;       // base + CSRC list
        if (hdr > len) return 0;
        if (x) {
            if (hdr + 4 > len) return 0;
            uint16_t extLenWords = (uint16_t)((buf[hdr + 2] << 8) | buf[hdr + 3]);
            size_t extBytes = 4 + (size_t)extLenWords * 4;
            if (hdr + extBytes > len) return 0;
            hdr += extBytes;
        }
        return hdr;
    };

    // Fill jitter buffer until we have one AAC frame’s worth of samples
    const size_t need = (size_t)frameSize_ * (size_t)channels_;
    while (jb_.size() < need) {
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock_, pkt, sizeof(pkt), 0, (struct sockaddr*)&src, &srclen);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // No packet in timeout window: break and let underrun handling below fill silence
                maybeReconnect("timeout", true);
                break;
            }
            perror("[RTP] recvfrom");
            logMessage("[RTP] Error receiving packet");
            maybeReconnect("recvfrom error", false);
            break;
        }
        if (n < 12) {
            logMessage("[RTP] Bad packet (too short)");
            continue;
        }
        lastPacketTime_ = std::chrono::steady_clock::now();

        // --- BEGIN SSRC Filtering to lock to a single stream ---
        uint32_t currentSSRC = (uint32_t)((pkt[8] << 24) | (pkt[9] << 16) | (pkt[10] << 8) | pkt[11]);
        if (!ssrcInitialized_) {
            ssrc_ = currentSSRC;
            ssrcInitialized_ = true;
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // Add small delay to fix timing issue
        } else if (currentSSRC != ssrc_) {
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // Add small delay to fix timing issue
            continue; // Drop packet from interferring stream
        }
        // --- END SSRC Filtering ---

        // --- BEGIN Discontinuity check via Sequence Number ---
        uint16_t currentSeqNum = (uint16_t)((pkt[2] << 8) | pkt[3]);
        if (seqNumInitialized_ && currentSeqNum != (uint16_t)(lastSeqNum_ + 1)) {
            jb_.clear(); // Prevent stitching broken audio
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // Add small delay to fix timing issue
        }
        lastSeqNum_ = currentSeqNum;
        seqNumInitialized_ = true;
        // --- END Discontinuity check ---

        // RTP v2 guard
        uint8_t version = (pkt[0] >> 6) & 0x03;
        if (version != 2) {
            logMessage("[RTP] Ignored non-RTPv2 packet");
            continue;
        }

        size_t headerLen = rtpHeaderLen(pkt, (size_t)n);
        if (headerLen == 0 || headerLen >= (size_t)n) {
            logMessage("[RTP] Bad RTP header length");
            continue;
        }

        const size_t payloadLen = (size_t)n - headerLen;
        if (payloadLen == 0) continue;

        const uint8_t* p = pkt + headerLen;

        // Decide framing: prefer L24 if it fits; otherwise L16
        bool isL24 = (payloadLen % ((size_t)channels_ * 3) == 0);
        bool isL16 = (payloadLen % ((size_t)channels_ * 2) == 0);

        if (isL24) {
            // L24: 3 bytes per sample, big-endian. Downscale to 16-bit.
            size_t frames = payloadLen / ((size_t)channels_ * 3);
            const uint8_t* q = p;
            for (size_t f = 0; f < frames; ++f) {
                for (int ch = 0; ch < channels_; ++ch) {
                    int32_t s24 = (int32_t)((q[0] << 16) | (q[1] << 8) | (q[2]));
                    if (s24 & 0x00800000) s24 |= 0xFF000000; // sign-extend
                    int16_t s16 = (int16_t)(s24 >> 8);        // downscale to 16-bit
                    q += 3;

                    int32_t boosted = (int32_t)(s16 * gain_);
                    if (boosted > 32767) boosted = 32767;
                    if (boosted < -32768) boosted = -32768;

                    if (jb_.size() < jbCap_) {
                        jb_.push_back((int16_t)boosted);
                    }
                    // else: drop sample if jitter buffer is full
                }
            }
        } else if (isL16) {
            // L16: 2 bytes per sample, big-endian (Axia standard)
            size_t samples = payloadLen / 2;
            const uint8_t* q = p;
            for (size_t i = 0; i < samples; ++i) {
                int16_t s = (int16_t)((q[0] << 8) | q[1]);
                q += 2;

                int32_t boosted = (int32_t)(s * gain_);
                if (boosted > 32767) boosted = 32767;
                if (boosted < -32768) boosted = -32768;

                if (jb_.size() < jbCap_) {
                    jb_.push_back((int16_t)boosted);
                }
                // else: drop sample if jitter buffer is full
            }
        } else {
            logMessage("[RTP] Unrecognized PCM framing, payloadLen=" + std::to_string(payloadLen));
            continue;
        }

        // First-time audio confirmation
        if (!audioStarted_ && !jb_.empty()) {
            logMessage("[RTP] Successfully Connected — audio stream running (Gain=" + std::to_string(gain_) + ")");

            // --- BEGIN DEBUG LOGGING ---
            static bool firstPacketLogged = false;
            if (!firstPacketLogged) {
                firstPacketLogged = true;
                std::stringstream ss;
                ss << "[RTP DEBUG] First packet details:\n";
                ss << "  - Received " << n << " bytes\n";
                ss << "  - RTP Version: " << ((pkt[0] >> 6) & 0x03) << "\n";
                ss << "  - Payload Type: " << (int)(pkt[1] & 0x7F) << "\n";
                ss << "  - Sequence Number: " << (int)((pkt[2] << 8) | pkt[3]) << "\n";
                ss << "  - Timestamp: " << (unsigned int)((pkt[4] << 24) | (pkt[5] << 16) | (pkt[6] << 8) | pkt[7]) << "\n";
                ss << "  - SSRC: 0x" << std::hex << (unsigned int)((pkt[8] << 24) | (pkt[9] << 16) | (pkt[10] << 8) | pkt[11]) << std::dec << "\n";
                ss << "  - Header Length: " << headerLen << " bytes\n";
                ss << "  - Payload Length: " << payloadLen << " bytes\n";
                ss << "  - Payload hexdump (first 12 bytes): ";
                ss << std::hex << std::setfill('0');
                for (size_t i = 0; i < std::min((size_t)12, payloadLen); ++i) {
                    ss << "0x" << std::setw(2) << (int)p[i] << " ";
                }
                ss << std::dec;
                logMessage(ss.str());
            }
            // --- END DEBUG LOGGING ---

            audioStarted_ = true;
            lastHeartbeat_ = std::chrono::steady_clock::now();
        }
    }

    // Serve exactly one AAC frame
    if (jb_.size() >= need) {
        pcm.insert(pcm.end(), jb_.begin(), jb_.begin() + need);
        jb_.erase(jb_.begin(), jb_.begin() + need);
    } else {
        // Underrun: serve what we have, then fill with silence
        size_t have = jb_.size();
        if (have > 0) {
            pcm.insert(pcm.end(), jb_.begin(), jb_.end());
            jb_.clear();
        }
        size_t missing = need - have;
        if (missing > 0) {
            fillSilence(pcm, missing);
        }
        // throttled underrun log (every 5 s)
        static auto lastWarn = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastWarn).count() >= 5) {
            logMessage("[RTP] Underrun — served " + std::to_string(have) +
                       " samples, inserted " + std::to_string(missing) + " silence");
            lastWarn = now;
        }
    }

    // Heartbeat every 60 seconds
    if (audioStarted_) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastHeartbeat_).count() >= 60) {
            logMessage("[RTP] Heartbeat — audio stream healthy");
            lastHeartbeat_ = now;
        }
    }

    return true;
}

    void shutdown() override {
        // Do not close g_rtpSock here — it’s shared
        logMessage("[RTP] Shutdown complete (global socket still active)");
    }


    int frameSize() const override { return frameSize_; }

private:
    std::string addr_;
    int port_;
    std::string iface_;
    int channels_;
    int sampleRate_;
    int frameSize_;
    float gain_;
    int sock_ = -1;

    bool audioStarted_ = false;
    std::chrono::steady_clock::time_point lastHeartbeat_;
    std::chrono::steady_clock::time_point lastPacketTime_;
    std::chrono::steady_clock::time_point lastReconnectAttempt_;

    // Sequence number tracking for discontinuity detection
    bool seqNumInitialized_ = false;
    uint16_t lastSeqNum_ = 0;

    // SSRC tracking to lock to a single stream
    bool ssrcInitialized_ = false;
    uint32_t ssrc_ = 0;

    // Jitter buffer
    std::deque<int16_t> jb_;
    size_t jbCap_ = 0;


    void configureSocket() {
        int rcvbuf = 1 << 20;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        timeval tv{0, 300000}; // 300 ms
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    void resetStreamState() {
        jb_.clear();
        audioStarted_ = false;
        seqNumInitialized_ = false;
        ssrcInitialized_ = false;
        lastSeqNum_ = 0;
        lastHeartbeat_ = std::chrono::steady_clock::now();
        lastPacketTime_ = lastHeartbeat_;
    }

    bool maybeReconnect(const char* reason, bool fromTimeout) {
        const auto now = std::chrono::steady_clock::now();
        const auto reconnectAfter = std::chrono::seconds(2);
        const auto retryInterval = std::chrono::seconds(1);

        if (fromTimeout && (now - lastPacketTime_ < reconnectAfter)) return false;
        if (now - lastReconnectAttempt_ < retryInterval) return false;

        lastReconnectAttempt_ = now;
        logMessage(std::string("[RTP] Reconnect attempt (") + reason + ")");

        if (g_rtpSock >= 0) {
            close(g_rtpSock);
            g_rtpSock = -1;
        }

        Config cfg{};
        cfg.inputType = "rtp";
        cfg.rtpAddress = addr_;
        cfg.rtpPort = port_;
        cfg.rtpInterface = iface_;

        if (!initRtpMulticast(cfg)) {
            logMessage("[RTP] Reconnect failed (multicast subscribe)");
            return false;
        }

        sock_ = g_rtpSock;
        configureSocket();
        resetStreamState();
        logMessage("[RTP] Reconnected to multicast");
        return true;
    }

    void fillSilence(std::vector<int16_t>& pcm, size_t count) {
        pcm.insert(pcm.end(), count, 0);
    }
};

// ---------HLS Stream-----------
std::string hlsDirFromConfig(const Config& cfg) {
    if (!cfg.hlsPath.empty()) return cfg.hlsPath;
    return getProgramDir() + "/HLS";
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return f.good();
}

bool writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << text;
    return f.good();
}
bool isSafeMetaForExtinf(const std::string& s, size_t maxLen = 1024) {
    if (s.empty()) return false;
    if (s.size() > maxLen) return false;
    for (unsigned char c : s) {
        if (c < 0x20 && c != '\t') return false; // control chars except tab
        if (c == '\r' || c == '\n') return false; // must be single-line
    }
    return true;
}
// Helper: sanitize raw XML so it can safely appear on a single EXTINF line
std::string sanitizeXmlForExtinf(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    for (char c : xml) {
        if (c == '\r' || c == '\n') {
            out.push_back(' '); // replace newlines with spaces
        } else if (c == '"') {
            out.push_back('\''); // replace double quotes with single quotes
        } else {
            out.push_back(c);
        }
    }
    return out;
}
std::string formatUtcIso8601(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch());
    auto secs = duration_cast<seconds>(ms);
    auto millis = ms - secs;

    std::time_t tt = secs.count();
    std::tm gmt{};
    gmtime_r(&tt, &gmt);

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &gmt);

    std::ostringstream oss;
    oss << buf << "." << std::setw(3) << std::setfill('0') << millis.count() << "Z";
    return oss.str();
}

void hlsBuildPlaylist(HlsState& hs, int currentSeq, int window, int targetDuration) {
    int firstSeq = std::max(0, currentSeq - window + 1);
    int lastSeq  = currentSeq;//+ 5;

    std::ostringstream pl;
    pl << "#EXTM3U\n";
    pl << "#EXT-X-VERSION:6\n";
    pl << "#EXT-X-TARGETDURATION:" << targetDuration << "\n";
    pl << "#EXT-X-MEDIA-SEQUENCE:" << firstSeq << "\n";

    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    std::string fallbackMetaJson;
    if (cfgCopy.hlsMetaEnabled) {
        // Fetch the current global meta as a fallback only (used if a segment has no stored
        // snapshot, e.g. segments written before this code was deployed).
        {
            std::lock_guard<std::mutex> lock(g_metaMutex);
            fallbackMetaJson = g_currentMetaJson;
        }

        // Only include these tags when metadata is enabled
        pl << "#EXT-X-INDEPENDENT-SEGMENTS\n";
        pl << "#EXT-X-START:TIME-OFFSET=" << cfgCopy.hlsStartTimeOffset << ",PRECISE=YES\n";
    }

    const bool playlistAtRoot = (hs.playlist.rfind("/index.m3u8") != std::string::npos);
    const std::string segRelPrefix = playlistAtRoot ? "segments/" : "";

    // Base time: when firstSeq segment started
    auto baseTime = hs.segStart;

    for (int s = firstSeq; s <= lastSeq; ++s) {
        double segDur = hs.targetSeconds;

        // Compute actual start time for this segment
        auto segStartTime = baseTime + std::chrono::milliseconds((s - firstSeq) * (int)(segDur * 1000));

        if (cfgCopy.hlsMetaEnabled) {
            // Program date-time tag
            pl << "#EXT-X-PROGRAM-DATE-TIME:" << formatUtcIso8601(segStartTime) << "\n";
        }

        // EXTINF line
        pl << "#EXTINF:" << std::fixed << std::setprecision(3) << segDur;
        if (cfgCopy.hlsMetaEnabled) {
            // Use the metadata that was current when THIS specific segment was written.
            // This prevents a newly-arrived "next song" metadata from being stamped onto
            // segments that are still carrying the previous (currently playing) song.
            auto metaIt = hs.segmentMeta.find(s);
            const std::string& segMeta = (metaIt != hs.segmentMeta.end())
                                         ? metaIt->second
                                         : fallbackMetaJson;
            pl << ",";
            if (isSafeMetaForExtinf(segMeta)) {
                pl << segMeta;
            }
        }
        pl << "\n";

        // Segment URI
        pl << segRelPrefix << "segment-" << s << ".aac\n";
    }

    if (!writeTextFile(hs.playlist, pl.str())) {
        logMessage("[HLS] Failed to write playlist: " + hs.playlist);
    }
}

void hlsBuildMasterPlaylist(const std::string& masterPath,
                            int avgBitrate = 256000,
                            const std::string& codec = "mp4a.40.2") {
    std::ostringstream pl;
    pl << "#EXTM3U\n";
    pl << "#EXT-X-VERSION:3\n";
    pl << "#EXT-X-INDEPENDENT-SEGMENTS\n";
    pl << "#EXT-X-STREAM-INF:BANDWIDTH=" << avgBitrate
       << ",CODECS=\"" << codec << "\"\n";
    pl << "index.m3u8\n";

    writeTextFile(masterPath, pl.str());
}

// Delete segments older than 3× window; log each purge action
void hlsPurgeOldSegments(HlsState& hs, int currentSeq) {
    int keepCount = hs.window * 3;
    if (keepCount < 1) keepCount = 1;

    // Any seq <= cutoff is eligible to delete
    int cutoff = currentSeq - keepCount;
    if (cutoff <= hs.lastPurgedSeq) return; // nothing new beyond what we've purged

    for (int s = hs.lastPurgedSeq + 1; s <= cutoff; ++s) {
        if (s < 0) continue;
        std::string oldPath = hs.dir + "/segment-" + std::to_string(s) + ".aac";
        if (fileExists(oldPath)) {
            if (std::remove(oldPath.c_str()) == 0) {
                logMessage("[HLS] Purged old segment: " + oldPath);
            } else {
                logMessage("[HLS] Failed to purge: " + oldPath);
            }
        }
        // Also remove the per-segment metadata snapshot so the map doesn't grow unbounded.
        hs.segmentMeta.erase(s);
    }
    hs.lastPurgedSeq = cutoff;
}

// Remove all HLS artifacts in a directory; optionally delete index.m3u8; log each delete
void hlsClearDir(const std::string& segmentsDir, bool removePlaylist = false) {
    // Delete segments in segmentsDir
    DIR* d = opendir(segmentsDir.c_str());
    if (!d) {
        logMessage("[HLS] Could not open segments directory for cleanup: " + segmentsDir);
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        if (name.rfind("segment-", 0) == 0 && name.find(".aac") != std::string::npos) {
            std::string path = segmentsDir + "/" + name;
            if (std::remove(path.c_str()) == 0) {
                logMessage("[HLS] Deleted segment: " + path);
            } else {
                logMessage("[HLS] Failed to delete segment: " + path);
            }
        }
    }
    closedir(d);

    if (removePlaylist) {
        // Delete HLS/index.m3u8 at the parent root
        size_t slash = segmentsDir.find_last_of('/');
        std::string root = (slash == std::string::npos) ? segmentsDir : segmentsDir.substr(0, slash);
        std::string playlist = root + "/index.m3u8";
        if (fileExists(playlist)) {
            if (std::remove(playlist.c_str()) == 0) {
                logMessage("[HLS] Deleted playlist: " + playlist);
            } else {
                logMessage("[HLS] Failed to delete playlist: " + playlist);
            }
        }
    }
}


void hlsWriteSegment(HlsState& hs, int seq, const std::vector<uint8_t>& segment) {
    std::string segPath = hs.dir + "/segment-" + std::to_string(seq) + ".aac";
    if (writeFile(segPath, segment)) {
        logMessage("[HLS] Wrote segment " + std::to_string(seq) + " (" + std::to_string(segment.size()) + " bytes) -> " + segPath);
    } else {
        logMessage("[HLS] ERROR writing segment: " + segPath);
    }
    //purge older segments beyond 3× window; logs each purge
    hlsPurgeOldSegments(hs, seq);
}

void hlsThreadFunc() {
    logMessage("[HLS] Writer thread starting");

    // Snapshot config that matters for timing
    int sampleRate = 48000;
    int targetDurationSec = 6;
    int window = 5;

    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        sampleRate = g_config.sampleRate;
        targetDurationSec = std::max(2, g_config.hlsSegmentSeconds);
        window = std::max(3, g_config.hlsWindow);
    }

    const int samplesPerFrame = 1024; // AAC LC ADTS default
    const int64_t targetSamples = (int64_t)targetDurationSec * (int64_t)sampleRate;

    std::vector<uint8_t> segment;   // current segment bytes
    int64_t segmentSamples = 0;      // current segment accumulated samples

    auto findAdtsFrame = [](const std::deque<uint8_t>& buf, size_t startIdx, size_t& frameOffset, size_t& frameLen)->bool {
        // Search syncword 0xFFF (12 bits) => first byte 0xFF, next high 4 bits 0xF
        for (size_t i = startIdx; i + 7 < buf.size(); ++i) {
            uint8_t b0 = buf[i];
            uint8_t b1 = buf[i + 1];
            if (b0 == 0xFF && (b1 & 0xF0) == 0xF0) {
                // ADTS header present; extract frame length
                // adts_fixed_header (28 bits) + adts_variable_header
                // Frame length: 13 bits: bits [30..42] total (header+payload)
                // Positions (relative to i):
                // bytes: b3, b4, b5 hold the 13-bit frame length
                if (i + 5 >= buf.size()) return false;
                uint8_t b3 = buf[i + 3];
                uint8_t b4 = buf[i + 4];
                uint8_t b5 = buf[i + 5];

                // frame length = ((b3 & 0x03) << 11) | (b4 << 3) | ((b5 & 0xE0) >> 5)
                size_t len = ((size_t)(b3 & 0x03) << 11) | ((size_t)b4 << 3) | (((size_t)b5 & 0xE0) >> 5);
                if (len == 0) {
                    // malformed; skip this sync
                    continue;
                }
                if (i + len > buf.size()) {
                    // Not enough bytes yet
                    return false;
                }

                frameOffset = i;
                frameLen = len;
                return true;
            }
        }
        return false;
    };

    g_hls.segStart = std::chrono::system_clock::now();
    if (g_hls.seq < 0) g_hls.seq = 0;

    while (true) {
        {
            // Check global shutdown flag first
            if (!g_running) {
                break;
            }
            std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
            if (!g_hls.running) break;
        }

        size_t frameOffset = 0, frameLen = 0;
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(g_hls.mutex);

            // Try to find a complete ADTS frame
            found = findAdtsFrame(g_hls.buffer, 0, frameOffset, frameLen);
            if (found) {
                // Copy frame into current segment
                segment.insert(segment.end(),
                               g_hls.buffer.begin() + frameOffset,
                               g_hls.buffer.begin() + frameOffset + frameLen);
                segmentSamples += samplesPerFrame;

                // Erase consumed bytes
                g_hls.buffer.erase(g_hls.buffer.begin(), g_hls.buffer.begin() + frameOffset + frameLen);
            }
        }

        if (!found) {
            // Not enough data yet; back off briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            // If we've reached target duration, flush a segment
            if (segmentSamples >= targetSamples) {
                int seqToWrite;
                {
                    std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
                    seqToWrite = g_hls.seq++;
                }

                // Snapshot current metadata for this specific segment so that
                // hlsBuildPlaylist can attach the correct "now playing" to each
                // EXTINF entry rather than the most-recently-received metadata.
                {
                    std::lock_guard<std::mutex> lock(g_metaMutex);
                    g_hls.segmentMeta[seqToWrite] = g_currentMetaJson;
                }

                hlsWriteSegment(g_hls, seqToWrite, segment);

                // Rebuild playlist for sliding window
                hlsBuildPlaylist(g_hls, seqToWrite, window, targetDurationSec);

                // Also (re)build master playlist so it exists during normal operation
                {
                    std::lock_guard<std::mutex> lock(g_configMutex);
                    std::string masterPath = g_hls.dir + "/../master.m3u8";
                    hlsBuildMasterPlaylist(masterPath, 256000, "mp4a.40.2");
                }

                // Reset segment accumulator
                segment.clear();
                segmentSamples = 0;
                g_hls.segStart = std::chrono::system_clock::now();

            }
        }

        // Gentle pacing; the encode loop runs at frame-rate, this can be lighter
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // If stopping and we have a partial segment, you can choose to flush it or discard.
    if (!segment.empty()) {
        int seqToWrite;
        {
            std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
            seqToWrite = g_hls.seq++;
        }
        {
            std::lock_guard<std::mutex> lock(g_metaMutex);
            g_hls.segmentMeta[seqToWrite] = g_currentMetaJson;
        }
        hlsWriteSegment(g_hls, seqToWrite, segment);
        hlsBuildPlaylist(g_hls, seqToWrite, window, targetDurationSec);

        // Build master playlist on final flush too
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            std::string masterPath = g_hls.dir + "/../master.m3u8";
            hlsBuildMasterPlaylist(masterPath, 256000, "mp4a.40.2");
        }

        segment.clear();
        segmentSamples = 0;
    }

    logMessage("[HLS] Writer thread stopped");
}


void hlsOnEncodedFrame(const std::vector<uint8_t>& encoded) {
    if (!g_hls.running || encoded.empty()) return;
    std::lock_guard<std::mutex> lock(g_hls.mutex);
    // Append encoded bytes (ADTS AAC) into the rolling buffer
    g_hls.buffer.insert(g_hls.buffer.end(), encoded.begin(), encoded.end());
}

std::string readTextFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string compactJson(const std::string& raw) {
    try {
        auto j = nlohmann::json::parse(raw);
        return j.dump(); // compact (no spaces/newlines)
    } catch (...) {
        // If input is already compact or malformed, fallback
        std::string out = raw;
        out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
        out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
        return out;
    }
}

//----------End HLS-----------

//----------Port Audio--------
bool initPortAudio(AudioState& audio, int deviceIndex, int channels, int sampleRate) {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        logMessage(std::string("PortAudio init error: ") + Pa_GetErrorText(err));

        return false;
    }

    // Print all devices so you can see the correct index
    int numDevices = Pa_GetDeviceCount();
    logMessage("Available devices:");
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info) {
            logMessage("Index " + std::to_string(i) + ": " + std::string(info->name) +
                       " | Inputs=" + std::to_string(info->maxInputChannels) +
                       " | Outputs=" + std::to_string(info->maxOutputChannels) +
                       " | Default rate=" + std::to_string((int)info->defaultSampleRate));
        }
    }

    PaStreamParameters inParams{};
    inParams.device = (deviceIndex >= 0) ? deviceIndex : Pa_GetDefaultInputDevice();
    if (inParams.device == paNoDevice) {
        logMessage("No input device.\n");
        return false;
    }
    const PaDeviceInfo* di = Pa_GetDeviceInfo(inParams.device);
    logMessage("Using input: " + std::string(di->name));

    inParams.channelCount = channels;
    inParams.sampleFormat = paInt32; // try 32-bit first
    inParams.suggestedLatency = di->defaultHighInputLatency;

    // Try opening with Int32
    err = Pa_OpenStream(&audio.stream, &inParams, nullptr, sampleRate,
                        audio.frameSize, paNoFlag, nullptr, nullptr);
    if (err != paNoError) {
        logMessage(std::string("OpenStream error: ") + Pa_GetErrorText(err) + " — retrying as float32\n");
        inParams.sampleFormat = paFloat32;
        err = Pa_OpenStream(&audio.stream, &inParams, nullptr, sampleRate,
                            audio.frameSize, paNoFlag, nullptr, nullptr);
        if (err != paNoError) {
            logMessage(std::string("OpenStream error: ") + Pa_GetErrorText(err) + " — retrying as int16\n");
            inParams.sampleFormat = paInt16;
            err = Pa_OpenStream(&audio.stream, &inParams, nullptr, sampleRate,
                                audio.frameSize, paNoFlag, nullptr, nullptr);
            if (err != paNoError) {
                logMessage(std::string("OpenStream failed: ") +  Pa_GetErrorText(err));
                return false;
            }
            audio.format = paInt16;
        } else {
            audio.format = paFloat32;
        }
    } else {
        audio.format = paInt32;
    }

    err = Pa_StartStream(audio.stream);
    if (err != paNoError) {
        logMessage(std::string( "StartStream error: ") + Pa_GetErrorText(err) );
        return false;
    }
    return true;
}
//------------End Port Audio---------------

//------------ AAC encoder ----------------
struct AacState {
    HANDLE_AACENCODER enc = nullptr;
};

bool initAac(AacState& aac, int sampleRate, int channels, int bitrate) {
    if (aacEncOpen(&aac.enc, 0, channels) != AACENC_OK) {
        logMessage("aacEncOpen failed\n");
        return false;
    }
    // Use MPEG-4 AAC-LC (standard, most compatible)
    aacEncoder_SetParam(aac.enc, AACENC_AOT, 2); // AOT_AAC_LC
    aacEncoder_SetParam(aac.enc, AACENC_SAMPLERATE, sampleRate);
    aacEncoder_SetParam(aac.enc, AACENC_CHANNELMODE, (channels == 2) ? MODE_2 : MODE_1);
    aacEncoder_SetParam(aac.enc, AACENC_CHANNELORDER, 1); // WAV channel order (interleaved L,R)
    aacEncoder_SetParam(aac.enc, AACENC_BITRATE, bitrate); // Use configured bitrate
    aacEncoder_SetParam(aac.enc, AACENC_TRANSMUX, TT_MP4_ADTS); // ADTS framing
    aacEncoder_SetParam(aac.enc, AACENC_AFTERBURNER, 1); // Enable afterburner
    aacEncoder_SetParam(aac.enc, AACENC_BITRATEMODE, 0); // CBR mode

    // Finalize encoder configuration
    if (aacEncEncode(aac.enc, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
        logMessage("aacEncEncode init failed\n");
        return false;
    }

    // Retrieve and verify encoder info (critical step to finalize internal state)
    AACENC_InfoStruct info = {0};
    if (aacEncInfo(aac.enc, &info) != AACENC_OK) {
        logMessage("aacEncInfo failed\n");
        return false;
    }

    logMessage("[AAC] Encoder initialized: " + std::to_string(info.frameLength) +
               " samples/frame, max " + std::to_string(info.maxOutBufBytes) + " bytes/frame");
    return true;
}

bool encodeAac(AacState& aac, const std::vector<int16_t>& pcm, std::vector<uint8_t>& out) {
    // Validate input size
    if (pcm.empty()) {
        logMessage("[AAC] Warning: Empty PCM buffer");
        return true; // Not an error, just skip
    }

    // FDK-AAC with CHANNELORDER=1 expects INTERLEAVED samples (not planar!)
    // For stereo: L,R,L,R,L,R... which is what we have
    // numInSamples is TOTAL samples (frames * channels)

    AACENC_BufDesc inBuf = {0}, outBuf = {0};
    AACENC_InArgs inArgs = {0};
    AACENC_OutArgs outArgs = {0};

    int inIdentifier = IN_AUDIO_DATA;
    int inSize = (int)(pcm.size() * sizeof(int16_t));
    int inElemSize = sizeof(int16_t);
    void* inPtr = (void*)pcm.data();

    inBuf.numBufs = 1;
    inBuf.bufs = &inPtr;
    inBuf.bufferIdentifiers = &inIdentifier;
    inBuf.bufSizes = &inSize;
    inBuf.bufElSizes = &inElemSize;

    int outIdentifier = OUT_BITSTREAM_DATA;
    uint8_t outBuffer[16384]; // larger buffer to avoid overflow
    void* outPtr = outBuffer;
    int outSize = sizeof(outBuffer);
    int outElemSize = 1;

    outBuf.numBufs = 1;
    outBuf.bufs = &outPtr;
    outBuf.bufferIdentifiers = &outIdentifier;
    outBuf.bufSizes = &outSize;
    outBuf.bufElSizes = &outElemSize;

    // CRITICAL: numInSamples is total samples (frames * channels)
    // For 1024 frames stereo = 2048 total samples
    inArgs.numInSamples = (int)pcm.size();

    AACENC_ERROR err = aacEncEncode(aac.enc, &inBuf, &outBuf, &inArgs, &outArgs);
    if (err != AACENC_OK) {
        logMessage("[AAC] Encode error: " + std::to_string(err) +
                   " (input samples=" + std::to_string(inArgs.numInSamples) + ")");
        return false;
    }

    if (outArgs.numOutBytes > 0 && outArgs.numOutBytes <= (int)sizeof(outBuffer)) {
        out.insert(out.end(), outBuffer, outBuffer + outArgs.numOutBytes);
    }
    return true;
}


// ------------ MP3 encoder ------------

struct Mp3State {
    lame_t lame = nullptr;
};

bool initMp3(Mp3State& mp3, int sampleRate, int channels, int bitrateKbps) {
    mp3.lame = lame_init();
    if (!mp3.lame) {
        logMessage("[MP3] lame_init failed");
        return false;
    }
    lame_set_in_samplerate(mp3.lame, sampleRate);
    lame_set_num_channels(mp3.lame, channels);
    lame_set_brate(mp3.lame, bitrateKbps / 1000);     // libmp3lame expects kbps
    lame_set_quality(mp3.lame, 2);                    // recommended VBR/quality tradeoff
    if (lame_init_params(mp3.lame) < 0) {
        logMessage("[MP3] lame_init_params failed");
        lame_close(mp3.lame);
        mp3.lame = nullptr;
        return false;
    }
    return true;
}

bool encodeMp3(Mp3State& mp3, const std::vector<int16_t>& pcm, std::vector<uint8_t>& out, int channels) {
    const int samplesPerChannel = (int)pcm.size() / channels;
    std::vector<unsigned char> mp3buf(static_cast<int>(1.25 * pcm.size() + 7200)); // LAME recommended
    int n = 0;

    if (channels == 2) {
        std::vector<int16_t> left(samplesPerChannel), right(samplesPerChannel);
        for (int i = 0, j = 0; i < samplesPerChannel; ++i) {
            left[i]  = pcm[j++];
            right[i] = pcm[j++];
        }
        n = lame_encode_buffer(mp3.lame, left.data(), right.data(),
                               samplesPerChannel, mp3buf.data(), (int)mp3buf.size());
    } else {
        n = lame_encode_buffer(mp3.lame, pcm.data(), nullptr,
                               samplesPerChannel, mp3buf.data(), (int)mp3buf.size());
    }

    if (n < 0) {
        logMessage("[MP3] lame_encode_buffer error");
        return false;
    }
    out.insert(out.end(), mp3buf.begin(), mp3buf.begin() + n);
    return true;
}

void closeMp3(Mp3State& mp3, std::vector<uint8_t>& outTail) {
    if (!mp3.lame) return;
    unsigned char buf[1024];
    int n = lame_encode_flush(mp3.lame, buf, sizeof(buf));
    if (n > 0) outTail.insert(outTail.end(), buf, buf + n);
    lame_close(mp3.lame);
    mp3.lame = nullptr;
}
//-----------END MP3 Encoder--------

// ---------------- Icecast connection & streaming loop ----------------


static std::string base64(const std::string& s) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, valb = -6;
    for (uint8_t c : s) { val = (val << 8) + c; valb += 8; while (valb >= 0) { out.push_back(tbl[(val >> valb) & 0x3F]); valb -= 6; } }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

bool connectIcecastSource(const Config& cfg, int& sockfd, int& icyMetaIntOut) {
    IcecastEndpoint ep;
    if (!parseIcecastUrl(cfg.icecastUrl, ep)) {
        logMessage("[Icecast] Invalid URL: " + cfg.icecastUrl);
        return false;
    }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const int rv = getaddrinfo(ep.host.c_str(), ep.port.c_str(), &hints, &res);
    if (rv != 0) {
        logMessage(std::string("[Icecast] getaddrinfo: ") + gai_strerror(rv));
        return false;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) { perror("[Icecast] socket"); freeaddrinfo(res); return false; }

    // Optional local bind
    if (!cfg.icecastInterface.empty() && res->ai_family == AF_INET) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = inet_addr(cfg.icecastInterface.c_str());
        if (bind(sockfd, (struct sockaddr*)&local, sizeof(local)) < 0) {
            perror("[Icecast] bind local");
        }
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("[Icecast] connect");
        close(sockfd);
        sockfd = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    const std::string auth = base64(cfg.user + ":" + cfg.pass);
    std::ostringstream req;
    req << "SOURCE " << ep.mount << " HTTP/1.0\r\n"
        << "Authorization: Basic " << auth << "\r\n"
        << "Content-Type: audio/aac\r\n"
        << "Icy-MetaData: 1\r\n"
        << "Ice-Public: 1\r\n"
        << "Ice-Name: AAC Encoder\r\n"
        << "Ice-Genre: Misc\r\n"
        << "Ice-Description: Live stream\r\n"
        << "icy-metaint: " << cfg.icyMetaInt << "\r\n"
        << "\r\n";

    const std::string reqStr = req.str();
    if (send(sockfd, reqStr.c_str(), reqStr.size(), 0) < 0) {
        perror("[Icecast] send headers");
        close(sockfd);
        sockfd = -1;
        return false;
    }

    // Read full response headers until \r\n\r\n
    std::string resp;
    char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sockfd, buf, sizeof(buf), 0);
        if (n <= 0) {
            perror("[Icecast] recv");
            close(sockfd);
            sockfd = -1;
            return false;
        }
        resp.append(buf, n);
        if (resp.size() > 64 * 1024) break; // safety
    }

    // Check HTTP 200
    const bool okStatus = (resp.find("200") != std::string::npos) ||
                          (resp.find("OK")  != std::string::npos) ||
                          (resp.rfind("HTTP/1.0 200", 0) == 0) ||
                          (resp.rfind("HTTP/1.1 200", 0) == 0);
    if (!okStatus) {
        logMessage("[Icecast] Rejected source connection:\n" + resp);
        close(sockfd);
        sockfd = -1;
        return false;
    }

    // Parse server-provided icy-metaint if present
    icyMetaIntOut = cfg.icyMetaInt;
    {
        std::istringstream hs(resp);
        std::string line;
        while (std::getline(hs, line)) {
            // Normalize header line
            if (line.size() && line.back() == '\r') line.pop_back();
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string val = line.substr(pos + 1);
                // trim spaces
                auto trim = [](std::string& s){
                    while (!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back();
                };
                trim(key); trim(val);
                for (auto& c : key) c = (char)tolower(c);
                if (key == "icy-metaint") {
                    int serverMetaInt = std::atoi(val.c_str());
                    if (serverMetaInt > 0) icyMetaIntOut = serverMetaInt;
                }
            }
        }
    }

    logMessage("[Icecast-AAC] Source accepted by Icecast: " + ep.host + ep.mount +
               " (icy-metaint=" + std::to_string(icyMetaIntOut) + ")");
    return true;
}
    //--------------MP3 Icecast Connection--------------
bool connectIcecastSourceMp3(const Config& cfg, int& sockfd, int& icyMetaIntOut) {
    IcecastEndpoint ep;
    if (!parseIcecastUrl(cfg.mp3IcecastUrl, ep)) {
        logMessage("[Icecast-MP3] Invalid URL: " + cfg.mp3IcecastUrl);
        return false;
    }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rv = getaddrinfo(ep.host.c_str(), ep.port.c_str(), &hints, &res);
    if (rv != 0) {
        logMessage(std::string("[Icecast-MP3] getaddrinfo: ") + gai_strerror(rv));
        return false;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) { perror("[Icecast-MP3] socket"); freeaddrinfo(res); return false; }

    if (!cfg.icecastInterface.empty() && res->ai_family == AF_INET) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = inet_addr(cfg.icecastInterface.c_str());
        if (bind(sockfd, (struct sockaddr*)&local, sizeof(local)) < 0) {
            perror("[Icecast-MP3] bind local");
        }
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("[Icecast-MP3] connect");
        close(sockfd);
        sockfd = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    const std::string auth = base64(cfg.mp3User + ":" + cfg.mp3Pass);
    std::ostringstream req;
    req << "SOURCE " << ep.mount << " HTTP/1.0\r\n"
        << "Authorization: Basic " << auth << "\r\n"
        << "Content-Type: audio/mpeg\r\n"
        << "Icy-MetaData: 1\r\n"
        << "Ice-Public: 1\r\n"
        << "Ice-Name: MP3 Encoder\r\n"
        << "Ice-Genre: Misc\r\n"
        << "Ice-Description: Live stream MP3\r\n"
        << "icy-metaint: " << cfg.icyMetaInt << "\r\n"
        << "\r\n";

    const std::string reqStr = req.str();
    if (send(sockfd, reqStr.c_str(), reqStr.size(), 0) < 0) {
        perror("[Icecast-MP3] send headers");
        close(sockfd);
        sockfd = -1;
        return false;
    }

    std::string resp; char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(sockfd, buf, sizeof(buf), 0);
        if (n <= 0) { perror("[Icecast-MP3] recv"); close(sockfd); sockfd = -1; return false; }
        resp.append(buf, n);
        if (resp.size() > 64 * 1024) break;
    }

    const bool okStatus = (resp.find("200") != std::string::npos) ||
                          (resp.find("OK")  != std::string::npos) ||
                          (resp.rfind("HTTP/1.0 200", 0) == 0) ||
                          (resp.rfind("HTTP/1.1 200", 0) == 0);
    if (!okStatus) {
        logMessage("[Icecast-MP3] Rejected source connection:\n" + resp);
        close(sockfd);
        sockfd = -1;
        return false;
    }

    icyMetaIntOut = cfg.icyMetaInt;
    std::istringstream hs(resp);
    std::string line;
    while (std::getline(hs, line)) {
        if (line.size() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos), val = line.substr(pos + 1);
            auto trim = [](std::string& s){
                while (!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin());
                while (!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back();
            };
            trim(key); trim(val);
            for (auto& c : key) c = (char)tolower(c);
            if (key == "icy-metaint") {
                int serverMetaInt = std::atoi(val.c_str());
                if (serverMetaInt > 0) icyMetaIntOut = serverMetaInt;
            }
        }
    }

    logMessage("[Icecast-MP3] Source accepted by Icecast: " + ep.host + ep.mount +
               " (icy-metaint=" + std::to_string(icyMetaIntOut) + ")");
    return true;
}

template <typename ConnectFn>
bool connectIcecastWithRetry(const std::string& logPrefix,
                             ConnectFn connectFn,
                             int& sockfd,
                             int& icyMetaIntOut) {
    const auto deadline = std::chrono::steady_clock::now() + kIcecastTakeoverTimeout;
    int attempt = 0;

    while (g_running) {
        ++attempt;
        if (connectFn(sockfd, icyMetaIntOut)) {
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
        logMessage(logPrefix + " Connect attempt " + std::to_string(attempt) +
                   " failed; retrying in " + std::to_string(kIcecastRetryInterval.count()) +
                   "s (" + std::to_string(remaining) + "s remaining in takeover window)");
        std::this_thread::sleep_for(kIcecastRetryInterval);
    }

    logMessage(logPrefix + " Failed to connect within " +
               std::to_string(kIcecastTakeoverTimeout.count()) + "s takeover timeout");
    return false;
}
//------End MP3 Icecast--------------------



// ---------------- ICY metadata injection ----------------
std::vector<uint8_t> buildIcyMetadata(const std::string& meta) {
    // Cap metadata length to avoid overflow
    std::string safe = meta.substr(0, 255);
    std::string payload = "StreamTitle='" + safe + "';StreamUrl='';";

    size_t len = payload.size();
    size_t blocks = std::min((len + 15) / 16, (size_t)255); // length byte max 255

    std::vector<uint8_t> out;
    out.reserve(1 + blocks * 16);

    // First byte = number of 16-byte blocks
    out.push_back((uint8_t)blocks);

    // Copy payload up to blocks*16
    out.insert(out.end(), payload.begin(), payload.begin() + std::min(len, blocks * 16));

    // Pad with zeros to fill block
    while ((out.size() - 1) < blocks * 16) out.push_back(0);

    return out;
}

// ---------------- Polling-based streamLoop ----------------

// --- MP3 encoder control globals ---
std::atomic<bool> g_mp3EncodeEnabled{false};
std::mutex g_mp3StateMutex;
Mp3State g_mp3State;

void audioEngineLoop(std::unique_ptr<IAudioInput> input,
                     AacState aac,
                     int sampleRate,
                     int channels) {
    const int frameSize = input->frameSize();
    const double frameSeconds = static_cast<double>(frameSize) / static_cast<double>(sampleRate);
    const auto frameDuration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(frameSeconds));
    auto nextDeadline = std::chrono::steady_clock::now();

    logMessage("[engine] Audio engine started");

    while (g_engineRunning) {
        std::vector<int16_t> pcm;
        if (!input->readFrame(pcm)) {
            logMessage("[engine] audio input read failed");
            break;
        }

        // AAC always
        std::vector<uint8_t> aacFrame;
        if (!encodeAac(aac, pcm, aacFrame)) {
            logMessage("[engine] AAC encode failed");
            break;
        }

        // Distribute frame to active consumers
        const bool hlsActive = g_hls.running.load();
        const bool iceActive = g_iceRunning.load();

        if (hlsActive && iceActive) {
            // Both consumers active: HLS copies, Icecast moves
            hlsOnEncodedFrame(aacFrame);
            std::lock_guard<std::mutex> lk(g_encodedMutex);
            g_encodedFrames.push_back(std::move(aacFrame));
            g_encodedCv.notify_one();
        } else if (hlsActive) {
            // Only HLS active: move directly to HLS buffer
            std::lock_guard<std::mutex> lock(g_hls.mutex);
            g_hls.buffer.insert(g_hls.buffer.end(),
                               std::make_move_iterator(aacFrame.begin()),
                               std::make_move_iterator(aacFrame.end()));
        } else if (iceActive) {
            // Only Icecast active: move directly to Icecast queue
            std::lock_guard<std::mutex> lk(g_encodedMutex);
            g_encodedFrames.push_back(std::move(aacFrame));
            g_encodedCv.notify_one();
        }
        // If neither active, frame is discarded

        // MP3 optional (ONLY if you introduce the dynamic flag later in Step 4)
        if (g_mp3EncodeEnabled.load()) {
            std::vector<uint8_t> mp3Frame;
            {
                std::lock_guard<std::mutex> lock(g_mp3StateMutex);
                if (g_mp3State.lame) {
                    if (!encodeMp3(g_mp3State, pcm, mp3Frame, channels)) {
                        logMessage("[engine] MP3 encode failed");
                        g_mp3EncodeEnabled.store(false);
                    }
                }
            }
            if (!mp3Frame.empty()) {
                std::lock_guard<std::mutex> lk(g_encodedMutexMp3);
                g_encodedFramesMp3.push_back(std::move(mp3Frame));
                g_encodedCvMp3.notify_one();
            }
        }

        nextDeadline += frameDuration;
        std::this_thread::sleep_until(nextDeadline);
    }

    // Shutdown
    input->shutdown();
    if (aac.enc) aacEncClose(&aac.enc);

    // MP3 tail flush if enabled
    {
        std::lock_guard<std::mutex> lock(g_mp3StateMutex);
        if (g_mp3State.lame) {
            std::vector<uint8_t> tail;
            closeMp3(g_mp3State, tail);
            if (!tail.empty()) {
                std::lock_guard<std::mutex> lk(g_encodedMutexMp3);
                g_encodedFramesMp3.push_back(std::move(tail));
                g_encodedCvMp3.notify_one();
            }
        }
    }

    g_engineRunning = false;
    logMessage("[engine] Audio engine stopped");
}

//---------Start audio engine---------
bool startAudioEngineWithConfig() {
    if (g_engineRunning) {
        logMessage("[engine] Already running");
        return true;
    }

    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    // Ensure RTP multicast subscription if inputType is RTP
    if (cfgCopy.inputType == "rtp") {
        if (g_rtpSock < 0) {
            if (!initRtpMulticast(cfgCopy)) {
                logMessage("[engine] RTP multicast subscribe failed");
                return false;
            }
        }
    }

    // Initialize AAC encoder FIRST to determine required frame size
    AacState aac{};
    if (!initAac(aac, cfgCopy.sampleRate, cfgCopy.channels, cfgCopy.bitrate)) {
        logMessage("[engine] Failed to initialize AAC encoder");
        return false;
    }

    AACENC_InfoStruct info = {0};
    if (aacEncInfo(aac.enc, &info) != AACENC_OK) {
        logMessage("[engine] Failed to get AAC encoder info");
        aacEncClose(&aac.enc);
        return false;
    }
    const int requiredFrameSize = info.frameLength;

    // Build input (RTP or PortAudio) with encoder’s frame size
    std::unique_ptr<IAudioInput> input;
    if (cfgCopy.inputType == "rtp") {
        input = std::make_unique<RtpInput>(
            cfgCopy.rtpAddress, cfgCopy.rtpPort, cfgCopy.rtpInterface,
            cfgCopy.channels, cfgCopy.sampleRate, requiredFrameSize, cfgCopy.rtpGain);
    } else {
        input = std::make_unique<PortAudioInput>(
            cfgCopy.deviceIndex, cfgCopy.channels, cfgCopy.sampleRate, requiredFrameSize);
    }
    if (!input->init()) {
        logMessage("[engine] Audio input init failed (" + cfgCopy.inputType + ")");
        aacEncClose(&aac.enc);
        return false;
    }

    g_engineRunning = true;
    try {
        g_engineThread = std::thread(audioEngineLoop,
                                     std::move(input),
                                     aac,
                                     cfgCopy.sampleRate,
                                     cfgCopy.channels);
    } catch (...) {
        g_engineRunning = false;
        logMessage("[engine] Failed to start engine thread");
        return false;
    }
    return true;
}

//---------Stop audio engine---------
void stopAudioEngine() {
    if (!g_engineRunning) return;
    logMessage("[engine] Stop requested");
    g_engineRunning = false;
    g_encodedCv.notify_all();
    if (g_engineThread.joinable()) g_engineThread.join();
}

//---------Stop engine if no sinks---------
void maybeStopEngineIfIdle() {
    bool hlsActive = false;
    { std::lock_guard<std::mutex> lock(g_hlsCtrlMutex); hlsActive = g_hls.running; }
    if (!g_iceRunning && !g_iceRunningMp3 && !hlsActive) {
        stopAudioEngine();
    }
}


    //---------MP3 Icecast Writer Loop---------
void icecastWriterLoopMp3(int sockfd, int icyMetaInt) {
    g_iceRunningMp3.store(true);
    g_mp3StopRequested.store(false);
    logMessage("[Icecast-MP3] Sink writer started");

    size_t framesSent = 0;
    size_t bytesSentTotal = 0;
    int metaCount = 0;
    std::string lastMeta;

    auto send_all = [&](const uint8_t* data, int len) -> bool {
        int off = 0;
        while (off < len) {
            int rc = ::send(sockfd, data + off, len - off, 0);
            if (rc < 0) { perror("[Icecast-MP3] send"); return false; }
            off += rc;
        }
        return true;
    };

       while (g_iceRunningMp3.load() && !g_mp3StopRequested.load()) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(g_encodedMutexMp3);
            g_encodedCvMp3.wait(lk, []{
                return !g_engineRunning || !g_encodedFramesMp3.empty()
                       || !g_iceRunningMp3.load() || g_mp3StopRequested.load();
            });
            if (!g_iceRunningMp3.load() || g_mp3StopRequested.load()) break;
            if (!g_engineRunning && g_encodedFramesMp3.empty()) break;

            if (!g_encodedFramesMp3.empty()) {
                frame = std::move(g_encodedFramesMp3.front());
                g_encodedFramesMp3.pop_front();
            }
        }

        if (frame.empty()) continue;

        int sent = 0;
        while (sent < (int)frame.size()) {
            int spaceToMeta = icyMetaInt > 0 ? (icyMetaInt - metaCount) : (int)frame.size();
            int chunk = std::min(spaceToMeta, (int)frame.size() - sent);
            if (chunk > 0) {
                if (!send_all(frame.data() + sent, chunk)) {
                    g_iceRunningMp3.store(false);
                    break;
                }
                sent += chunk;
                metaCount += chunk;
                bytesSentTotal += chunk;
            }

            if (icyMetaInt > 0 && metaCount >= icyMetaInt) {
                bool mp3MetaEnabledCopy = true;
                { std::lock_guard<std::mutex> lock(g_configMutex);
                  mp3MetaEnabledCopy = g_config.mp3MetaEnabled; }

                if (mp3MetaEnabledCopy) {
                    std::string meta;
                    { std::lock_guard<std::mutex> lock(g_metaMutex); meta = g_currentMeta; }

                    if (meta != lastMeta) {
                        std::vector<uint8_t> block = buildIcyMetadata(meta);
                        if (!block.empty()) {
                            if (!send_all(block.data(), (int)block.size())) {
                                g_iceRunningMp3.store(false);
                                break;
                            }
                            lastMeta = meta;
                        } else {
                            uint8_t zero = 0;
                            if (!send_all(&zero, 1)) { g_iceRunningMp3.store(false); break; }
                        }
                    } else {
                        uint8_t zero = 0;
                        if (!send_all(&zero, 1)) { g_iceRunningMp3.store(false); break; }
                    }
                } else {
                    uint8_t zero = 0;
                    if (!send_all(&zero, 1)) { g_iceRunningMp3.store(false); break; }
                }

                metaCount = 0;
                framesSent++;
                if ((framesSent % 100) == 0) {
                    logMessage("[Icecast-MP3] Sent frames=" + std::to_string(framesSent) +
                               ", bytes=" + std::to_string(bytesSentTotal));
                }
            }
        }
    }

    ::close(sockfd);
    g_iceSockMp3 = -1;
    g_iceRunningMp3.store(false);
    logMessage("[Icecast-MP3] Sink writer stopped");
}


    // Starts metadata listener thread with current g_config.listenPort
    bool startMetadataListener() {
        std::lock_guard<std::mutex> guard(g_metaCtrlMutex);
        if (metaRunning) {
            logMessage("[metadata] Listener already running");
            return true;
        }

        int port;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            port = g_config.listenPort;   // critical
        }

        logMessage("[metadata] Starting listener on port " + std::to_string(port));
        metaShutdown = false;
        metaRunning = true;

        metaThread = std::thread([port]() {
            try {
                metadataServer(port);
            } catch (const std::exception& e) {
                logMessage(std::string("[metadataServer] Exception: ") + e.what());
            } catch (...) {
                logMessage("[metadataServer] Unknown exception in thread");
            }
        });

        return true;
    }



//---------AAC Icecast writer loop (unchanged logic)---------
void icecastWriterLoop(int sockfd, int icyMetaInt) {
    g_iceRunning = true;
    logMessage("[Icecast] Sink writer started");

    int metaCount = 0;
    std::string lastMeta;

    while (g_iceRunning) {
        if (!g_running) break;

        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(g_encodedMutex);
            g_encodedCv.wait(lk, []{
                return !g_engineRunning || !g_encodedFrames.empty() || !g_iceRunning;
            });
            if (!g_iceRunning) break;
            if (!g_engineRunning && g_encodedFrames.empty()) break;
            if (!g_encodedFrames.empty()) {
                frame = std::move(g_encodedFrames.front());
                g_encodedFrames.pop_front();
            }
        }

        if (frame.empty()) continue;

        int sent = 0;
        while (sent < (int)frame.size()) {
            int chunk = std::min(icyMetaInt - metaCount, (int)frame.size() - sent);
            if (chunk > 0) {
                if (send(sockfd, frame.data() + sent, chunk, 0) < 0) {
                    perror("send audio");
                    g_iceRunning = false;
                    break;
                }
                sent += chunk;
                metaCount += chunk;
            }

            if (metaCount >= icyMetaInt) {
                bool iceMetaEnabledCopy = true;
                { std::lock_guard<std::mutex> lock(g_configMutex); iceMetaEnabledCopy = g_config.iceMetaEnabled; }

                if (iceMetaEnabledCopy) {
                    std::string meta;
                    { std::lock_guard<std::mutex> lock(g_metaMutex); meta = g_currentMeta; }
                    if (meta != lastMeta) {
                        auto block = buildIcyMetadata(meta);
                        if (send(sockfd, block.data(), block.size(), 0) < 0) {
                            perror("send meta");
                            g_iceRunning = false;
                            break;
                        }
                        lastMeta = meta;
                    } else {
                        uint8_t zero = 0;
                        send(sockfd, &zero, 1, 0);
                    }
                } else {
                    uint8_t zero = 0;
                    send(sockfd, &zero, 1, 0);
                }
                metaCount = 0;
            }
        }
    }

    close(sockfd);
    g_iceSock = -1;
    g_iceRunning = false;
    logMessage("[Icecast-AAC] Sink writer stopped");
}
//---------Start AAC encoder with current config---------
bool startEncoderWithConfig() {
    // Start/ensure engine first
    if (!startAudioEngineWithConfig()) return false;

    // If Icecast already running, do nothing
    if (g_iceRunning) {
        logMessage("[Icecast-AAC] Icecast sink already running");
        aacRunning = true; // ensure flag is set
        return true;
    }

    // Connect Icecast
    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    int sockfd = -1;
    int icyMetaIntUse = 0;
    if (!connectIcecastWithRetry(
            "[Icecast-AAC]",
            [&](int& connectSock, int& connectMetaInt) {
                return connectIcecastSource(cfgCopy, connectSock, connectMetaInt);
            },
            sockfd,
            icyMetaIntUse)) {
        logMessage("[Icecast-AAC] Failed to connect to Icecast");
        maybeStopEngineIfIdle();
        return false;
    }
    g_iceSock = sockfd;
    g_iceMetaIntEffective = icyMetaIntUse;

    // Launch Icecast sink thread
    try {
        g_iceThread = std::thread(icecastWriterLoop, g_iceSock, g_iceMetaIntEffective);
    } catch (...) {
        close(g_iceSock);
        g_iceSock = -1;
        logMessage("[Icecast-AAC] Failed to start Icecast sink thread");
        return false;
    }

    g_iceRunning = true;
    aacRunning = true; // set AAC status flag

    logMessage("[Icecast-AAC] Icecast sink running at " + cfgCopy.icecastUrl +
               (cfgCopy.icecastInterface.empty() ? "" : (" via " + cfgCopy.icecastInterface)));
    return true;
}

//---------Stop AAC encoder---------
void stopEncoder() {
    if (!g_iceRunning) {
        logMessage("[Icecast-AAC] Icecast sink not running");
        aacRunning = false;
        return;
    }
    logMessage("[Icecast-AAC] Icecast sink stop requested");
    g_iceRunning = false;
    aacRunning = false; // clear AAC status flag
    g_encodedCv.notify_all();
    if (g_iceThread.joinable()) g_iceThread.join();

    maybeStopEngineIfIdle();
}
    //--------Start MP3 Encoder-------
bool startMp3EncoderWithConfig() {
    // Ensure engine is running (it produces MP3 frames when enabled)
    if (!g_engineRunning) {
        if (!startAudioEngineWithConfig()) {
            logMessage("[Icecast-MP3] Audio engine failed");
            return false;
        }
    }

    if (g_iceRunningMp3) {
        logMessage("[Icecast-MP3] Icecast MP3 sink already running");
        mp3Running = true;
        return true;
    }

    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }
    if (!cfgCopy.mp3Enabled) {
        logMessage("[Icecast-MP3] mp3Enabled=false; not starting");
        return false;
    }

    // Initialize shared MP3 encoder state
    {
        std::lock_guard<std::mutex> lock(g_mp3StateMutex);
        if (!g_mp3State.lame) {
            const int mp3BitrateKbps = std::max(32, cfgCopy.bitrate / 1000);
            if (!initMp3(g_mp3State, cfgCopy.sampleRate, cfgCopy.channels, mp3BitrateKbps)) {
                logMessage("[Icecast-MP3] MP3 init failed");
                return false;
            }
        }
        g_mp3EncodeEnabled.store(true);
    }

    int sockfd = -1;
    int icyMetaIntUse = 0;
    if (!connectIcecastWithRetry(
            "[Icecast-MP3]",
            [&](int& connectSock, int& connectMetaInt) {
                return connectIcecastSourceMp3(cfgCopy, connectSock, connectMetaInt);
            },
            sockfd,
            icyMetaIntUse)) {
        logMessage("[Icecast-MP3] Failed to connect to Icecast");
        maybeStopEngineIfIdle();
        return false;
    }
    g_iceSockMp3 = sockfd;
    g_iceMetaIntEffectiveMp3 = icyMetaIntUse;

    try {
        g_iceThreadMp3 = std::thread(icecastWriterLoopMp3, g_iceSockMp3, g_iceMetaIntEffectiveMp3);
    } catch (...) {
        close(g_iceSockMp3);
        g_iceSockMp3 = -1;
        logMessage("[Icecast-MP3] Failed to start Icecast sink thread");
        return false;
    }

    g_iceRunningMp3 = true;
    mp3Running = true;

    logMessage("[Icecast-MP3] Icecast sink running at " + cfgCopy.mp3IcecastUrl +
               (cfgCopy.icecastInterface.empty() ? "" : (" via " + cfgCopy.icecastInterface)));
    return true;
}


bool stopMp3EncoderWithConfig() {
    if (!g_iceRunningMp3) {
        logMessage("[Icecast-MP3] Icecast sink not running");
        mp3Running = false;
        return true;
    }

    logMessage("[Icecast-MP3] Icecast sink stop requested");
    g_mp3StopRequested.store(true);
    g_iceRunningMp3.store(false);
    mp3Running.store(false);

    g_encodedCvMp3.notify_all();
    if (g_iceThreadMp3.joinable()) g_iceThreadMp3.join();

    // Disable MP3 encode and flush tail
    {
        std::lock_guard<std::mutex> lock(g_mp3StateMutex);
        if (g_mp3State.lame) {
            std::vector<uint8_t> tail;
            closeMp3(g_mp3State, tail);
            if (!tail.empty()) {
                std::lock_guard<std::mutex> lk(g_encodedMutexMp3);
                g_encodedFramesMp3.push_back(std::move(tail));
                g_encodedCvMp3.notify_one();
            }
        }
        g_mp3EncodeEnabled.store(false);
    }

    maybeStopEngineIfIdle();
    logMessage("[Icecast-MP3] Icecast sink stopped");
    return true;
}
// ----------- HLS start -----------
bool startHlsWithConfig() {
    // Ensure engine is running to feed HLS
    if (!g_engineRunning) {
        if (!startAudioEngineWithConfig()) {
            logMessage("[HLS] Cannot start: audio engine failed");
            return false;
        }
    }

    if (g_hls.running) {
        logMessage("[HLS] Already running");
        return true;
    }

    // Reset state before starting
    g_hls.buffer.clear();
    g_hls.seq = -1;
    g_hls.lastPurgedSeq = -1;
    g_hls.segStart = {};
    g_hls.playlist.clear();
    g_hls.dir.clear();

    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    std::string hlsRoot = hlsDirFromConfig(cfgCopy);
    if (!ensureDirExists(hlsRoot)) {
        logMessage("[HLS] ERROR: Failed to ensure HLS root: " + hlsRoot);
        return false;
    }
    std::string segmentsDir = hlsRoot + "/segments";
    if (!ensureDirExists(segmentsDir)) {
        logMessage("[HLS] ERROR: Failed to ensure segments dir: " + segmentsDir);
        return false;
    }

    // Clean up any existing HLS files from previous runs
    hlsClearDir(segmentsDir, /*removePlaylist*/ true);
    logMessage("[HLS] Cleaned up existing segments and playlist on startup");

    g_hls.dir = segmentsDir;
    g_hls.playlist = hlsRoot + "/index.m3u8";
    g_hls.targetSeconds = std::max(2, cfgCopy.hlsSegmentSeconds);
    g_hls.window = std::max(3, cfgCopy.hlsWindow);
    g_hls.seq = -1;
    g_hls.lastPurgedSeq = -1;
    g_hls.buffer.clear();
    g_hls.segStart = {};

    g_hls.running = true;
    hlsRunning = true;   // UI flag
    try {
        g_hls.thread = std::thread(hlsThreadFunc);
        logMessage("[HLS] Running ...");
    } catch (...) {
        g_hls.running = false;
        hlsRunning = false;
        logMessage("[HLS] ERROR: Failed to start HLS thread");
        return false;
    }
    return true;
}

// ----------- HLS stop -----------
void stopHls() {
    {
        std::lock_guard<std::mutex> guard(g_hlsCtrlMutex);
        if (!g_hls.running) {
            logMessage("[HLS] Not running");
            hlsRunning = false;
            return;
        }
        logMessage("[HLS] Stop requested");
        g_hls.running = false;
        hlsRunning = false;
    }

    if (g_hls.thread.joinable()) {
        g_hls.thread.join();
    }
    g_hls.thread = std::thread(); // reset

    if (!g_hls.dir.empty()) {
        clearHlsOutput(fs::path(g_hls.dir).parent_path().string());
    }

    {
        std::lock_guard<std::mutex> guard(g_hlsCtrlMutex);
        g_hls.buffer.clear();
        g_hls.seq = 0;
        g_hls.lastPurgedSeq = -1;
        g_hls.segStart = {};
        g_hls.playlist.clear();
        g_hls.dir.clear();
    }

    logMessage("[HLS] Stopped successfully");

    // Check if engine can be stopped
    maybeStopEngineIfIdle();
}

//-----------Stop MP3 Encoder--------
void stopMp3Encoder() {
    if (!g_iceRunningMp3) {
        logMessage("[Icecast-MP3] Icecast sink not running");
        mp3Running = false;
        return;
    }
    logMessage("[Icecast-MP3] Icecast sink stop requested");

    g_iceRunningMp3 = false;   // clear engine flag
    mp3Running = false;        // clear UI flag

    g_encodedCvMp3.notify_all();
    if (g_iceThreadMp3.joinable()) g_iceThreadMp3.join();

    maybeStopEngineIfIdle();
    logMessage("[Icecast-MP3] Icecast sink stopped");
}



void startHeadless() {
    logMessage("[headless] Starting with config: " + g_configPath);
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logMessage("[headless] Config directory: " + g_config.configDir);
        logMessage("[headless] iceDataParse mapping file: " + g_config.iceDataParse);
    }

    int webPort;
    { std::lock_guard<std::mutex> lock(g_configMutex); webPort = g_config.webPort; }
    startHlsServerFromConfig();

    // Metadata listener
    startMetadataListener();

    // Control listener
    startControlListener();

    bool iceWantAac, hlsWant, mp3Want;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        iceWantAac = g_config.iceEnabled;
        hlsWant    = g_config.hlsEnabled;
        mp3Want    = g_config.mp3Enabled;
    }

    if (iceWantAac) {
        if (!startEncoderWithConfig()) {
            logMessage("[headless] AAC Icecast sink failed to start");
        }
    } else {
        logMessage("[headless] AAC Icecast sink disabled by config (iceEnabled=false)");
    }

    if (mp3Want) {
        if (!startMp3EncoderWithConfig()) {
            logMessage("[headless] MP3 Icecast sink failed to start");
        }
    } else {
        logMessage("[headless] MP3 Icecast sink disabled by config (mp3Enabled=false)");
    }

    if (hlsWant) {
        if (!startHlsWithConfig()) {
            logMessage("[headless] HLS failed to start (configured to enable)");
        }
    } else {
        logMessage("[headless] HLS disabled by config (hlsEnabled=false)");
    }

    logMessage("[headless] Running (" +
               std::string(iceWantAac ? "Icecast sink" : "no Icecast") +
               (hlsWant ? " + HLS" : "") +
               (mp3Want ? " + MP3" : "") +
               (g_config.controlEnabled ? " + control listener" : "") +
               " + metadata listener)");
}


// ---------------- Web UI ----------------
void runWebUI(AudioState& audio, AacState& aac,
              int sockfd, int channels, int sampleRate, int icyMetaInt) {
    // Snapshot config for thread safety (declare once)
    Config cfgCopy;
    { std::lock_guard<std::mutex> lock(g_configMutex); cfgCopy = g_config; }

    // Determine UI port from config (avoid name collision with any outer 'port')
    const int uiPort = (cfgCopy.webPort > 0) ? cfgCopy.webPort : 8020;

    httplib::Server svr;

    // Use absolute path for static assets
    const std::string staticRoot = g_appRoot + "/www";
    ensureDirExists(staticRoot);
    svr.set_mount_point("/static", "./www");

    // HLS root (for index.m3u8) and segments subdirectory
    std::string hlsRoot = hlsDirFromConfig(cfgCopy);
    ensureDirExists(hlsRoot);
    std::string segmentsDir = hlsRoot + "/segments";
    ensureDirExists(segmentsDir);

    // Update global HLS paths used by the writer and handlers
    {
        std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
        g_hls.dir = segmentsDir;                  // segments written here
        g_hls.playlist = hlsRoot + "/index.m3u8"; // playlist at root
    }

    // Explicit handler for master playlist
    svr.Get("/hls/master.m3u8", [](const httplib::Request&, httplib::Response& res) {
        std::string path = hlsDirFromConfig(g_config) + "/master.m3u8";
        std::ifstream f(path);
        if (!f.is_open()) { res.status = 404; return; }
        std::ostringstream ss; ss << f.rdbuf();
        res.set_content(ss.str(), "audio/mpegurl");
    });

    // Explicit handler for the HLS playlist with correct MIME type
    svr.Get("/hls/index.m3u8", [](const httplib::Request&, httplib::Response& res) {
        std::string path;
        { std::lock_guard<std::mutex> lock(g_hlsCtrlMutex); path = g_hls.playlist; }
        std::ifstream f(path);
        if (!f.is_open()) { res.status = 404; return; }
        std::ostringstream ss; ss << f.rdbuf();
        res.set_content(ss.str(), "audio/mpegurl");
    });

    // Explicit handler for AAC segments
    svr.Get(R"(/hls/segments/(.*\.aac))", [segmentsDir](const httplib::Request& req, httplib::Response& res) {
        std::string path = segmentsDir + "/" + req.matches[1].str();
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { res.status = 404; return; }

        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        f.seekg(0, std::ios::beg);

        res.set_content_provider(
            static_cast<size_t>(size), "audio/aac",
            [path](size_t offset, size_t length, httplib::DataSink &sink) {
                std::ifstream f(path, std::ios::binary);
                f.seekg(offset);
                std::vector<char> buf(length);
                f.read(buf.data(), length);
                sink.write(buf.data(), f.gcount());
                return true;
            }
        );
    });

    // HLS status
    svr.Get("/hlsstatus", [&](const httplib::Request&, httplib::Response& res) {
        json j;
        j["status"] = g_hls.running ? "running" : "stopped";
        j["path"] = g_hls.dir;
        j["window"] = g_hls.window;
        j["segmentSeconds"] = (int)std::ceil(g_hls.targetSeconds);
        res.set_content(j.dump(), "application/json");
    });

    std::thread encoderThread;

    logMessage("AACIceEncoderWebUI version " + std::string(APP_VERSION) + " starting up");

    // Start control listener
    svr.Post("/startcontrol", [&](const httplib::Request& req, httplib::Response& res) {
        startControlListener();
        res.set_content("Control listener start requested", "text/plain");
    });

    // Stop control listener
    svr.Post("/stopcontrol", [&](const httplib::Request& req, httplib::Response& res) {
        stopControlListener();
        res.set_content("Control listener stop requested", "text/plain");
    });

    // Control status
    svr.Get("/controlstatus", [&](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json j;
        j["status"] = controlRunning ? "running" : "stopped";
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            j["port"] = g_config.controlPort;
            j["enabled"] = g_config.controlEnabled;
        }
        res.set_content(j.dump(), "application/json");
    });


    // Start AAC Encoder
    svr.Post("/start", [&](const httplib::Request&, httplib::Response& res) {
        if (startEncoderWithConfig()) {
            res.set_content("Icecast sink started", "text/plain");
        } else {
            res.status = 500;
            res.set_content("Failed to start Icecast sink", "text/plain");
        }
    });

    // Stop AAC Encoder
    svr.Post("/stop", [&](const httplib::Request&, httplib::Response& res) {
        stopEncoder();
        res.set_content("Icecast sink stopped", "text/plain");
    });

    svr.Get("/aacstatus", [&](const httplib::Request&, httplib::Response& res){
    nlohmann::json j;
    j["status"] = aacRunning ? "running" : "stopped";
    j["url"] = g_config.icecastUrl;
    res.set_content(j.dump(), "application/json");
    });

    // Start HLS sink
    svr.Post("/starthls", [&](const httplib::Request&, httplib::Response& res) {
        if (startHlsWithConfig()) {
            res.set_content("HLS started", "text/plain");
        } else {
            res.status = 500;
            res.set_content("Failed to start HLS", "text/plain");
        }
    });

    // Stop HLS sink
    svr.Post("/stophls", [&](const httplib::Request&, httplib::Response& res) {
        stopHls();
        res.set_content("HLS stopped", "text/plain");
    });

    // Start MP3 encoder
    svr.Post("/startmp3", [&](const httplib::Request& req, httplib::Response& res) {
        if (startMp3EncoderWithConfig()) {
            res.set_content("MP3 encoder started", "text/plain");
        } else {
            res.status = 500;
            res.set_content("Failed to start MP3 encoder", "text/plain");
        }
    });

    // Stop MP3 encoder
    svr.Post("/stopmp3", [&](const httplib::Request& req, httplib::Response& res) {
        if (stopMp3EncoderWithConfig()) {
            res.set_content("MP3 encoder stopped", "text/plain");
        } else {
            res.status = 500;
            res.set_content("Failed to stop MP3 encoder", "text/plain");
        }
    });

    // MP3 status
    svr.Get("/mp3status", [&](const httplib::Request&, httplib::Response& res){
        nlohmann::json j;
        j["status"] = mp3Running ? "running" : "stopped";
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            j["url"] = g_config.mp3IcecastUrl;
        }
        res.set_content(j.dump(), "application/json");
    });




    svr.Get("/metastatus", [&](const httplib::Request&, httplib::Response& res) {
        std::string state = metaRunning ? "running" : "stopped";
        int port;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            port = g_config.listenPort;
        }
        res.set_content("{\"status\":\"" + state + "\",\"port\":" + std::to_string(port) + "}", "application/json");
    });

    // return current config
    svr.Get("/getconfig", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logMessage("[web] /getconfig requested");

        json j = {
            // Web UI port
            {"webPort", g_config.webPort},
            {"controlPort", g_config.controlPort},
            {"controlEnabled", g_config.controlEnabled},
            {"commandStart", g_config.commandStart},
            {"commandStop", g_config.commandStop},
            // Icecast settings
            {"iceEnabled", g_config.iceEnabled},
            {"iceMetaEnabled", g_config.iceMetaEnabled},
            {"user", g_config.user},
            {"pass", g_config.pass},
            {"icecastUrl", g_config.icecastUrl},
            {"mp3Enabled", g_config.mp3Enabled},
            {"mp3MetaEnabled", g_config.mp3MetaEnabled},
            {"mp3User", g_config.mp3User},
            {"mp3Pass", g_config.mp3Pass},
            {"mp3IcecastUrl", g_config.mp3IcecastUrl},
            {"icyMetaInt", g_config.icyMetaInt},
            {"icecastInterface", g_config.icecastInterface},

            // Station / logging
            {"amperwaveStationId", g_config.amperwaveStationId},
               {"iceDataParse", g_config.iceDataParse},
            {"logDir", g_config.logDir},

            // Audio
            {"bitrate", g_config.bitrate},
            {"channels", g_config.channels},
            {"sampleRate", g_config.sampleRate},
            {"deviceIndex", g_config.deviceIndex},
            {"listenPort", g_config.listenPort},

            // Input
            {"inputType", g_config.inputType},
            {"rtpAddress", g_config.rtpAddress},
            {"rtpPort", g_config.rtpPort},
            {"rtpInterface", g_config.rtpInterface},
            {"rtpGain", g_config.rtpGain},

            // HLS
            {"hlsEnabled", g_config.hlsEnabled},
            {"hlsSegmentSeconds", g_config.hlsSegmentSeconds},
            {"hlsWindow", g_config.hlsWindow},
            {"hlsStartTimeOffset", g_config.hlsStartTimeOffset},
            {"hlsPath", g_config.hlsPath},
            {"hlsMetaEnabled", g_config.hlsMetaEnabled}
        };

        res.set_content(j.dump(4), "application/json");
    });

    svr.Get("/status", [&](const httplib::Request&, httplib::Response& res) {
        std::string state = g_running ? "running" : "stopped";
        json j;
        j["status"] = state;
        j["version"] = APP_VERSION;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/metadata", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_metaMutex);
        std::string meta = g_currentMeta.empty() ? "No metadata" : g_currentMeta;

        json j;
        j["metadata"] = meta;
        j["history"] = g_metaHistory; // last few items

        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/setmeta", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            std::string newMeta = j.value("metadata", "");

            {
                std::lock_guard<std::mutex> lock(g_metaMutex);
                g_currentMeta = newMeta;
            }

            logMessage("[/setmeta] Manual metadata set: " + newMeta);

            std::string artist, title;
            auto dash = newMeta.find(" - ");
            if (dash != std::string::npos) {
                artist = newMeta.substr(0, dash);
                title  = newMeta.substr(dash + 3);
            } else {
                title = newMeta;
            }

            // Thread-safe config copy for AmperWave
            std::string stationId;
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                stationId = g_config.amperwaveStationId;

            }

            bool sent = sendNowPlayingToAmperWave(stationId, artist, title);
            logMessage("[metadataServer] AmperWave send result: " + std::string(sent ? "success" : "failure"));


            {
                std::lock_guard<std::mutex> lock(g_metaMutex);
                g_metaHistory.push_front(newMeta);
                if (g_metaHistory.size() > 5) g_metaHistory.pop_back();
            }

            res.set_content("Metadata updated and pushed to AmperWave", "text/plain");
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid JSON", "text/plain");
        }
    });

    // Start metadata listener
    svr.Post("/startmeta", [&](const httplib::Request&, httplib::Response& res) {
        if (startMetadataListener()) {
            int port; { std::lock_guard<std::mutex> lock(g_configMutex); port = g_config.listenPort; }
            res.set_content("Metadata listener started on port " + std::to_string(port), "text/plain");
        } else {
            res.status = 500;
            res.set_content("Metadata listener failed to start", "text/plain");
        }
    });

    svr.Post("/stopmeta", [&](const httplib::Request&, httplib::Response& res) {
        stopMetadataListener();
        res.set_content("Metadata listener stopped", "text/plain");
    });

    svr.Get("/devices", [&](const httplib::Request&, httplib::Response& res) {
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            res.status = 500;
            res.set_content("PortAudio init failed", "text/plain");
            return;
        }

        json arr = json::array();
        int numDevices = Pa_GetDeviceCount();
        for (int i = 0; i < numDevices; i++) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (!info) continue;
            arr.push_back({
                {"index", i},
                {"name", std::string(info->name)},
                {"inputs", info->maxInputChannels},
                {"outputs", info->maxOutputChannels},
                {"defaultRate", static_cast<int>(info->defaultSampleRate)}
            });
        }

        Pa_Terminate();
        res.set_content(arr.dump(), "application/json");
    });

    svr.Get("/logstream", [&](const httplib::Request&, httplib::Response& res) {
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t, httplib::DataSink &sink) {
            // Initial line
            {
                std::string init = "data: [system] Connected to log stream\n\n";
                sink.write(init.c_str(), init.size());
                logMessage("AACIceEncoderWebUI version " + std::string(APP_VERSION) + " starting up");

            }

            while (true) {
                std::deque<std::string> lines;
                {
                    std::lock_guard<std::mutex> lock(g_logMutex);
                    lines.swap(g_logQueue); // take all pending lines
                }

                for (auto& line : lines) {
                    std::string msg = "data: " + line + "\n\n";
                    sink.write(msg.c_str(), msg.size());
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!sink.is_writable()) break;
            }
            sink.done();
            return true;
        },
        nullptr
    );
});

// update config from JSON body
// Update configuration
// Import a config from uploaded JSON content (client reads file and posts JSON)
    svr.Post("/config", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            Config newCfg;
            { std::lock_guard<std::mutex> lock(g_configMutex); newCfg = g_config; }

            //Encoder Control Port values
            if (j.contains("controlPort")) newCfg.controlPort = j["controlPort"].get<int>();
            if (j.contains("controlEnabled"))  newCfg.controlEnabled = j["controlEnabled"].get<bool>();
            if (j.contains("commandStart")) newCfg.commandStart = j["commandStart"].get<std::string>();
            if (j.contains("commandStop"))  newCfg.commandStop = j["commandStop"].get<std::string>();

            // Apply posted values
            if (j.contains("iceEnabled"))      newCfg.iceEnabled = j["iceEnabled"].get<bool>();
            if (j.contains("iceMetaEnabled"))  newCfg.iceMetaEnabled = j["iceMetaEnabled"].get<bool>();
            if (j.contains("icecastUrl")) newCfg.icecastUrl = j["icecastUrl"].get<std::string>();
            if (j.contains("user")) newCfg.user = j["user"].get<std::string>();
            if (j.contains("pass")) newCfg.pass = j["pass"].get<std::string>();
            if (j.contains("mp3Enabled"))     newCfg.mp3Enabled = j["mp3Enabled"].get<bool>();
            if (j.contains("mp3MetaEnabled")) newCfg.mp3MetaEnabled = j["mp3MetaEnabled"].get<bool>();
            if (j.contains("mp3IcecastUrl"))  newCfg.mp3IcecastUrl = j["mp3IcecastUrl"].get<std::string>();
            if (j.contains("mp3User"))        newCfg.mp3User = j["mp3User"].get<std::string>();
            if (j.contains("mp3Pass"))        newCfg.mp3Pass = j["mp3Pass"].get<std::string>();
            if (j.contains("amperwaveStationId")) newCfg.amperwaveStationId = j["amperwaveStationId"].get<std::string>();
            if (j.contains("iceDataParse")) newCfg.iceDataParse = j["iceDataParse"].get<std::string>();
            if (j.contains("logDir")) newCfg.logDir = j["logDir"].get<std::string>();
            if (j.contains("sampleRate")) newCfg.sampleRate = j["sampleRate"].get<int>();
            if (j.contains("channels")) newCfg.channels = j["channels"].get<int>();
            if (j.contains("bitrate")) newCfg.bitrate = j["bitrate"].get<int>();
            if (j.contains("deviceIndex")) newCfg.deviceIndex = j["deviceIndex"].is_null() ? -1 : j["deviceIndex"].get<int>();
            if (j.contains("listenPort")) newCfg.listenPort = j["listenPort"].get<int>();
            if (j.contains("icyMetaInt")) newCfg.icyMetaInt = j["icyMetaInt"].get<int>();

            // RTP + interface
            if (j.contains("inputType")) newCfg.inputType = j["inputType"].get<std::string>();
            if (j.contains("rtpAddress")) newCfg.rtpAddress = j["rtpAddress"].get<std::string>();
            if (j.contains("rtpPort")) newCfg.rtpPort = j["rtpPort"].get<int>();
            if (j.contains("rtpInterface")) newCfg.rtpInterface = j["rtpInterface"].get<std::string>();
            if (j.contains("icecastInterface")) newCfg.icecastInterface = j["icecastInterface"].get<std::string>();
            if (j.contains("rtpGain")) newCfg.rtpGain = j["rtpGain"].is_number() ? j["rtpGain"].get<float>() : std::stof(j["rtpGain"].get<std::string>());

            //HLS
            if (j.contains("hlsEnabled"))           newCfg.hlsEnabled = j["hlsEnabled"].get<bool>();
            if (j.contains("hlsMetaEnabled"))         newCfg.hlsMetaEnabled = j["hlsMetaEnabled"].get<bool>();
            if (j.contains("hlsSegmentSeconds"))      newCfg.hlsSegmentSeconds = j["hlsSegmentSeconds"].get<int>();
            if (j.contains("hlsWindow"))              newCfg.hlsWindow = j["hlsWindow"].get<int>();
            if (j.contains("hlsStartTimeOffset"))     newCfg.hlsStartTimeOffset = j["hlsStartTimeOffset"].get<int>();
            if (j.contains("hlsPath"))                newCfg.hlsPath = j["hlsPath"].get<std::string>();

            { std::lock_guard<std::mutex> lock(g_configMutex);
              g_config = newCfg;
              if (!saveConfigToFile(g_config, g_configPath)) {
                  res.status = 500;
                  res.set_content("Failed to save config to " + g_configPath, "text/plain");
                  logMessage("[config] Failed to save to " + g_configPath);
                  return;
              }
            }
            logMessage("[config] Config saved successfully from POST");
            res.set_content("Config saved", "text/plain");
        } catch (const std::exception& e) {
            res.status = 400;
            std::string errMsg = std::string("JSON error: ") + e.what();
            logMessage("[config] " + errMsg);
            res.set_content(errMsg, "text/plain");
        } catch (...) {
            res.status = 400;
            logMessage("[config] Unknown exception during config POST");
            res.set_content("Invalid JSON or server error", "text/plain");
        }
    });


// Save current config to a chosen filename in the config folder
svr.Post("/configsaveas", [&](const httplib::Request& req, httplib::Response& res) {
    try {
        json j = json::parse(req.body);
        if (!j.contains("filename")) {
            res.status = 400;
            res.set_content("Missing filename", "text/plain");
            return;
        }
        std::string filename = j["filename"].get<std::string>();
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            cfg = g_config; // start with current config
        }
        //Encoder Control Port values
        if (j.contains("controlPort")) cfg.controlPort = j["controlPort"].get<int>();
        if (j.contains("controlEnabled"))  cfg.controlEnabled = j["controlEnabled"].get<bool>();
        if (j.contains("commandStart")) cfg.commandStart = j["commandStart"].get<std::string>();
        if (j.contains("commandStop"))  cfg.commandStop = j["commandStop"].get<std::string>();

        // Apply any posted overrides (same as /config)
        if (j.contains("iceEnabled"))      cfg.iceEnabled = j["iceEnabled"].get<bool>();
        if (j.contains("iceMetaEnabled"))  cfg.iceMetaEnabled = j["iceMetaEnabled"].get<bool>();
        if (j.contains("icecastUrl")) cfg.icecastUrl = j["icecastUrl"].get<std::string>();
        if (j.contains("user")) cfg.user = j["user"].get<std::string>();
        if (j.contains("pass")) cfg.pass = j["pass"].get<std::string>();
        if (j.contains("mp3Enabled"))     cfg.mp3Enabled = j["mp3Enabled"].get<bool>();
        if (j.contains("mp3MetaEnabled")) cfg.mp3MetaEnabled = j["mp3MetaEnabled"].get<bool>();
        if (j.contains("mp3IcecastUrl"))  cfg.mp3IcecastUrl = j["mp3IcecastUrl"].get<std::string>();
        if (j.contains("mp3User"))        cfg.mp3User = j["mp3User"].get<std::string>();
        if (j.contains("mp3Pass"))        cfg.mp3Pass = j["mp3Pass"].get<std::string>();
        if (j.contains("amperwaveStationId")) cfg.amperwaveStationId = j["amperwaveStationId"].get<std::string>();
        if (j.contains("iceDataParse")) cfg.iceDataParse = j["iceDataParse"].get<std::string>();
        if (j.contains("logDir")) cfg.logDir = j["logDir"].get<std::string>();
        if (j.contains("sampleRate")) cfg.sampleRate = j["sampleRate"].get<int>();
        if (j.contains("channels")) cfg.channels = j["channels"].get<int>();
        if (j.contains("bitrate")) cfg.bitrate = j["bitrate"].get<int>();
        if (j.contains("deviceIndex")) cfg.deviceIndex = j["deviceIndex"].is_null() ? -1 : j["deviceIndex"].get<int>();
        if (j.contains("listenPort")) cfg.listenPort = j["listenPort"].get<int>();
        if (j.contains("icyMetaInt")) cfg.icyMetaInt = j["icyMetaInt"].get<int>();
        if (j.contains("inputType")) cfg.inputType = j["inputType"].get<std::string>();
        if (j.contains("rtpAddress")) cfg.rtpAddress = j["rtpAddress"].get<std::string>();
        if (j.contains("rtpPort")) cfg.rtpPort = j["rtpPort"].get<int>();
        if (j.contains("rtpInterface")) cfg.rtpInterface = j["rtpInterface"].get<std::string>();
        if (j.contains("icecastInterface")) cfg.icecastInterface = j["icecastInterface"].get<std::string>();
        if (j.contains("rtpGain")) cfg.rtpGain = j["rtpGain"].is_number() ? j["rtpGain"].get<float>() : std::stof(j["rtpGain"].get<std::string>());
        if (j.contains("hlsEnabled"))           cfg.hlsEnabled = j["hlsEnabled"].get<bool>();
        if (j.contains("hlsMetaEnabled"))         cfg.hlsMetaEnabled = j["hlsMetaEnabled"].get<bool>();
        if (j.contains("hlsSegmentSeconds"))      cfg.hlsSegmentSeconds = j["hlsSegmentSeconds"].get<int>();
        if (j.contains("hlsWindow"))              cfg.hlsWindow = j["hlsWindow"].get<int>();
        if (j.contains("hlsStartTimeOffset"))     cfg.hlsStartTimeOffset = j["hlsStartTimeOffset"].get<int>();
        if (j.contains("hlsPath"))                cfg.hlsPath = j["hlsPath"].get<std::string>();

        if (!saveConfigToFile(cfg, filename)) {
            res.status = 500;
            res.set_content("Failed to save config to " + filename, "text/plain");
            logMessage("[configsaveas] Failed to save to " + filename);
            return;
        }
        logMessage("[configsaveas] Config saved to " + filename);
        res.set_content("Config saved to " + filename, "text/plain");
    } catch (const std::exception& e) {
        res.status = 400;
        std::string errMsg = std::string("JSON error: ") + e.what();
        logMessage("[configsaveas] " + errMsg);
        res.set_content(errMsg, "text/plain");
    } catch (...) {
        res.status = 400;
        logMessage("[configsaveas] Unknown exception during config POST");
        res.set_content("Invalid JSON or server error", "text/plain");
    }
});

    svr.Post("/configload", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::parse(req.body);
            std::string filename = j.value("filename", "");
            if (filename.empty()) {
                res.status = 400;
                res.set_content("Missing filename", "text/plain");
                return;
            }
            Config newCfg;
            if (!loadConfigFromFile(newCfg, filename)) {
                res.status = 500;
                res.set_content("Failed to load config", "text/plain");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                g_config = newCfg;
            }
            res.set_content("Config loaded from " + filename, "text/plain");
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid JSON", "text/plain");
        }
    });


// Updated HTML page with config form, error handling, metadata injection, and diagnostics
svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content(
        "<html><body>"
        "<h1>AAC Encoder Control (v" APP_VERSION ")</h1>"

        "Web Port: <input id='webPort' type='number' min='1' max='65535'><br>"
        "Control Port: <input id='controlPort' type='number' min='1' max='65535'><br>"
        "<label><input id='controlEnabled' type='checkbox'> Enable Controller</label><br>"
        "<button onclick=\"startControlListener()\">Start Control Listener</button>"
        "<button onclick=\"stopControlListener()\">Stop Control Listener</button>"
        "<div id='controlStatus' style='margin-top:6px; font-style:italic; color:#555;'>Control Listener: (unknown)</div>"
        "Start Command: <input id='commandStart'><br>"
        "Stop Command: <input id='commandStop'  ><br>"


        "<h2>Metadata Control</h2>"
        "<input id='manualMeta' placeholder='Enter Now Playing text'><br>"
        "<button onclick=\"sendMetadata()\">Update Metadata</button><br>"
        "Listen Port: <input id='listenPort' type='number' min='1' max='65535'><br>"
        "<button onclick=\"startMetaListener()\">Start Metadata Listener</button>"
        "<button onclick=\"stopMetaListener()\">Stop Metadata Listener</button>"
        "<div id='metaStatus' style='margin-top:6px; font-style:italic; color:#555;'>Metadata Listener: (unknown)</div>"


        "<div id='nowPlaying' style='margin-top:10px; font-weight:bold;'>Now Playing: (unknown)</div>"
        "<h4>Recent Metadata</h4>"
        "<ul id='metaHistory'></ul>"



        "<form onsubmit=\"updateConfig(); return false;\">"

        "<h2>Audio Input Configuration</h2>"
        "Bitrate (bps): <select id='bitrate'>"
        "  <option value='96000'>96 bit</option>"
        "  <option value='128000'>128 bit</option>"
        "  <option value='192000'>192 bit</option>"
        "  <option value='256000'>256 bit</option>"
        "</select><br>"
        "Channels: <input id='channels' type='number'><br>"
        "Sample Rate: <select id='sampleRate'>"
        "  <option value='44100'>44.1 Khz</option>"
        "  <option value='48000'>48 Khz</option>"
        "</select><br>"
        "Device: <select id='deviceIndex'></select><br>"
        "Input Type: <select id='inputType'>"
        "  <option value='portaudio'>PortAudio (Physical Device)</option>"
        "  <option value='rtp'>RTP (Livewire/AES67)</option>"
        "</select><br>"
        "RTP Address: <input id='rtpAddress'><br>"
        "RTP Port: <input id='rtpPort' type='number'><br>"
        "RTP Interface (e.g. eth0): <input id='rtpInterface'><br>"
        "<label for='rtpGain'>RTP Gain:</label><input type='number' id='rtpGain' step='0.1' value='1.0'><br>"

        "<h2>Icecast AAC Configuration</h2>"
        "<label><input id='iceEnabled' type='checkbox'> Enable Icecast stream</label><br>"
        "<label><input id='iceMetaEnabled' type='checkbox'> Send ICY metadata</label><br>"
        "Icecast URL: <input id='icecastUrl'><br>"
        "User: <input id='user'><br>"
        "Password: <input id='pass' type='password'><br>"
        "<button onclick=\"startEncoder()\">Start</button>"
        "<button onclick=\"stopEncoder()\">Stop</button>"
        "<div id='aacStatus' style='margin-top:6px; font-style:italic; color:#555;'>AAC: (unknown)</div>"


        "<h2>Icecast MP3 Configuration</h2>"
        "<label><input id='mp3Enabled' type='checkbox'> Enable Icecast stream</label><br>"
        "<label><input id='mp3MetaEnabled' type='checkbox'> Send ICY metadata</label><br>"
        "Icecast URL: <input id='mp3IcecastUrl'><br>"
        "User: <input id='mp3User'><br>"
        "Password: <input id='mp3Pass' type='password'><br>"
        "<button onclick=\"startMp3()\">Start</button>"
        "<button onclick=\"stopMp3()\">Stop</button>"
        "<div id='mp3Status' style='margin-top:6px; font-style:italic; color:#555;'>MP3: (unknown)</div>"


        "<h2>Icecast General Configuration</h2>"
        "<div id='error' style='color:red;'></div>"
        "<div id='success' style='color:green;'></div>"
        "Station ID: <input id='amperwaveStationId'><br>"
        "Icy Metadata Confiuration: <input id='iceDataParse'><br>"
        "Icy MetaInt: <input id='icyMetaInt' type='number'><br>"
        "Icecast Interface (local IP): <input id='icecastInterface'><br>"

        "<h2>HLS streaming</h2>"
        "<label><input id='hlsEnabled' type='checkbox'> Enable HLS</label><br>"
        "<label><input id='hlsMetaEnabled' type='checkbox'> Embed Metadata in HLS</label><br>"
        "Segment Seconds: <input id='hlsSegmentSeconds' type='number' min='2' max='20'><br>"
        "Window (segments): <input id='hlsWindow' type='number' min='3' max='60'><br>"
        "Start Time Offset (s): <input id='hlsStartTimeOffset' type='number' min='-300' max='0' title='EXT-X-START TIME-OFFSET. 0 = oldest segment, negative = seconds from live edge (e.g. -30)'><br>"
        "HLS Path (optional): <input id='hlsPath'><br>"
        "<div>Playlist URL: <code>/hls/index.m3u8</code></div>"
        "<button onclick=\"startHls()\">Start HLS</button>"
        "<button onclick=\"stopHls()\">Stop HLS</button>"
        "<div id='hlsStatus' style='margin-top:6px; font-style:italic; color:#555;'>HLS: (unknown)</div>"

        "<button type='submit'>Update Config</button>"
        "<button type='button' onclick='saveConfigAs()'>Save As</button>"
        "<button type='button' onclick='loadConfigFile()'>Load Config</button>"

        "<div id='logConsole' style='background:#111; color:#9f9; font-family:monospace; padding:10px; height:400px; overflow-y:auto; border:1px solid #333;'></div>"

        "</form>"


"<script>"
"fetch('/status').then(r=>r.json()).then(d=>{"
"  document.querySelector('h1').innerText = 'AAC Encoder Control (v' + d.version + ')';"
"});"

"function updateStatus(){"
"  fetch('/status').then(r=>r.json()).then(d=>{"
"    document.getElementById('status').innerText='Status: '+d.status;"
"  });"
"}"
"function loadConfig(){"
"  fetch('/getconfig')"
"    .then(r => { if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })"
"    .then(cfg => {"
"      document.getElementById('webPort').value = cfg.webPort || 8080;"
"      document.getElementById('controlPort').value = cfg.controlPort || 8081;"
"      document.getElementById('controlEnabled').checked = !!cfg.controlEnabled;"
"      document.getElementById('commandStart').value = cfg.commandStart || '';"
"      document.getElementById('commandStop').value = cfg.commandStop || '';"
"      document.getElementById('iceEnabled').checked = !!cfg.iceEnabled;"
"      document.getElementById('iceMetaEnabled').checked = !!cfg.iceMetaEnabled;"
"      document.getElementById('icecastUrl').value = cfg.icecastUrl || '';"
"      document.getElementById('user').value = cfg.user || '';"
"      document.getElementById('pass').value = cfg.pass || '';"
"      document.getElementById('mp3Enabled').checked = !!cfg.mp3Enabled;"
"      document.getElementById('mp3MetaEnabled').checked = !!cfg.mp3MetaEnabled;"
"      document.getElementById('mp3IcecastUrl').value = cfg.mp3IcecastUrl || '';"
"      document.getElementById('mp3User').value = cfg.mp3User || '';"
"      document.getElementById('mp3Pass').value = cfg.mp3Pass || '';"
"      document.getElementById('listenPort').value = cfg.listenPort || '';"
"      document.getElementById('bitrate').value = cfg.bitrate || 128000;"
"      document.getElementById('channels').value = cfg.channels || 2;"
"      document.getElementById('sampleRate').value = cfg.sampleRate || 48000;"
"      document.getElementById('deviceIndex').value = (cfg.deviceIndex ?? -1);"
"      document.getElementById('icyMetaInt').value = cfg.icyMetaInt || 8192;"
"      document.getElementById('amperwaveStationId').value = cfg.amperwaveStationId || '';"
"      document.getElementById('iceDataParse').value = cfg.iceDataParse || '';"
"      document.getElementById('inputType').value = cfg.inputType || 'portaudio';"
"      document.getElementById('rtpAddress').value = cfg.rtpAddress || '';"
"      document.getElementById('rtpPort').value = cfg.rtpPort || 5004;"
"      document.getElementById('rtpInterface').value = cfg.rtpInterface || '';"
"      document.getElementById('icecastInterface').value = cfg.icecastInterface || '';"
"      document.getElementById('rtpGain').value = cfg.rtpGain || 1.0;"
"      document.getElementById('hlsEnabled').checked = !!cfg.hlsEnabled;"
"      document.getElementById('hlsMetaEnabled').checked = !!cfg.hlsMetaEnabled; "
"      document.getElementById('hlsSegmentSeconds').value = cfg.hlsSegmentSeconds || 6;"
"      document.getElementById('hlsWindow').value = cfg.hlsWindow || 5;"
"      document.getElementById('hlsStartTimeOffset').value = (cfg.hlsStartTimeOffset !== undefined) ? cfg.hlsStartTimeOffset : 0;"
"      document.getElementById('hlsPath').value = cfg.hlsPath || '';"

"    })"
"    .catch(err => {"
"      document.getElementById('error').innerText = 'Failed to load config: ' + err;"
"      console.error('loadConfig error', err);"
"    });"
"}"

"function updateConfig(){"
"  const cfg={"
"    controlPort: parseInt(document.getElementById('controlPort').value),"
"    controlEnabled: document.getElementById('controlEnabled').checked,"
"    commandStart: document.getElementById('commandStart').value,"
"    commandStop: document.getElementById('commandStop').value,"
"    iceEnabled: document.getElementById('iceEnabled').checked,"
"    iceMetaEnabled: document.getElementById('iceMetaEnabled').checked, "
"    icecastUrl: document.getElementById('icecastUrl').value,"
"    user: document.getElementById('user').value,"
"    pass: document.getElementById('pass').value,"
"    mp3Enabled: document.getElementById('mp3Enabled').checked,"
"    mp3MetaEnabled: document.getElementById('mp3MetaEnabled').checked, "
"    mp3IcecastUrl: document.getElementById('mp3IcecastUrl').value,"
"    mp3User: document.getElementById('mp3User').value,"
"    mp3Pass: document.getElementById('mp3Pass').value,"
"    listenPort: parseInt(document.getElementById('listenPort').value),"
"    bitrate: parseInt(document.getElementById('bitrate').value),"
"    channels: parseInt(document.getElementById('channels').value),"
"    sampleRate: parseInt(document.getElementById('sampleRate').value),"
"    deviceIndex: parseInt(document.getElementById('deviceIndex').value),"
"    icyMetaInt: parseInt(document.getElementById('icyMetaInt').value),"
"    amperwaveStationId: document.getElementById('amperwaveStationId').value.trim(),"
"    iceDataParse: document.getElementById('iceDataParse').value,"
"    inputType: document.getElementById('inputType').value,"
"    rtpAddress: document.getElementById('rtpAddress').value,"
"    rtpPort: parseInt(document.getElementById('rtpPort').value),"
"    rtpInterface: document.getElementById('rtpInterface').value,"
"    icecastInterface: document.getElementById('icecastInterface').value,"
"    rtpGain: parseFloat(document.getElementById('rtpGain').value),"
"    hlsEnabled: document.getElementById('hlsEnabled').checked,"
"    hlsMetaEnabled: document.getElementById('hlsMetaEnabled').checked, "
"    hlsSegmentSeconds: parseInt(document.getElementById('hlsSegmentSeconds').value),"
"    hlsWindow: parseInt(document.getElementById('hlsWindow').value),"
"    hlsStartTimeOffset: parseInt(document.getElementById('hlsStartTimeOffset').value),"
"    hlsPath: document.getElementById('hlsPath').value,"

"  };"
"  fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)})"
"    .then(async r=>{"
"      const msg=await r.text();"
"      if(!r.ok){"
"        document.getElementById('error').innerText='Error: '+msg;"
"        document.getElementById('success').innerText='';"
"      } else {"
"        document.getElementById('error').innerText='';"
"        document.getElementById('success').innerText='Config saved successfully!';"
"        loadConfig();"
"      }"
"    })"
"    .catch(err=>{"
"      document.getElementById('error').innerText='Network error: '+err;"
"      document.getElementById('success').innerText='';"
"    });"
"}"

"function saveConfigAs(){"
"  const filename = prompt('Enter filename to save as (e.g. config-new.json):');"
"  if(!filename){ return; }"
"  const fullPath = 'config/' + filename;"
"  const cfg={"
"    controlPort: parseInt(document.getElementById('controlPort').value),"
"    controlEnabled: document.getElementById('controlEnabled').checked,"
"    commandStart: document.getElementById('commandStart').value,"
"    commandStop: document.getElementById('commandStop').value,"
"    iceEnabled: document.getElementById('iceEnabled').checked,"
"    iceMetaEnabled: document.getElementById('iceMetaEnabled').checked,"
"    icecastUrl: document.getElementById('icecastUrl').value,"
"    user: document.getElementById('user').value,"
"    pass: document.getElementById('pass').value,"
"    mp3Enabled: document.getElementById('mp3Enabled').checked,"
"    mp3MetaEnabled: document.getElementById('mp3MetaEnabled').checked,"
"    mp3IcecastUrl: document.getElementById('mp3IcecastUrl').value,"
"    mp3User: document.getElementById('mp3User').value,"
"    mp3Pass: document.getElementById('mp3Pass').value,"
"    listenPort: parseInt(document.getElementById('listenPort').value),"
"    bitrate: parseInt(document.getElementById('bitrate').value),"
"    channels: parseInt(document.getElementById('channels').value),"
"    sampleRate: parseInt(document.getElementById('sampleRate').value),"
"    deviceIndex: parseInt(document.getElementById('deviceIndex').value),"
"    icyMetaInt: parseInt(document.getElementById('icyMetaInt').value),"
"    amperwaveStationId: document.getElementById('amperwaveStationId').value.trim(),"
"    iceDataParse: document.getElementById('iceDataParse').value,"
"    inputType: document.getElementById('inputType').value,"
"    rtpAddress: document.getElementById('rtpAddress').value,"
"    rtpPort: parseInt(document.getElementById('rtpPort').value),"
"    rtpInterface: document.getElementById('rtpInterface').value,"
"    icecastInterface: document.getElementById('icecastInterface').value,"
"    rtpGain: parseFloat(document.getElementById('rtpGain').value),"
"    hlsEnabled: document.getElementById('hlsEnabled').checked,"
"    hlsMetaEnabled: document.getElementById('hlsMetaEnabled').checked,"
"    hlsSegmentSeconds: parseInt(document.getElementById('hlsSegmentSeconds').value),"
"    hlsWindow: parseInt(document.getElementById('hlsWindow').value),"
"    hlsPath: document.getElementById('hlsPath').value,"
"    filename: fullPath"
"  };"
"  fetch('/configsaveas',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)})"
"    .then(async r=>{"
"      const msg=await r.text();"
"      if(!r.ok){"
"        document.getElementById('error').innerText='Error: '+msg;"
"        document.getElementById('success').innerText='';"
"      } else {"
"        document.getElementById('error').innerText='';"
"        document.getElementById('success').innerText='Config saved successfully as '+fullPath;"
"        console.log('Config saved as', fullPath);"
"      }"
"    })"
"    .catch(err=>{"
"      document.getElementById('error').innerText='Network error: '+err;"
"      document.getElementById('success').innerText='';"
"    });"
"}"


"function loadConfigFile(){"
"  const filename = prompt('Enter filename to load (e.g. config.json):');"
"  if(!filename){ return; }"
"  const fullPath = 'config/' + filename;"
"  fetch('/configload',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body: JSON.stringify({ filename: fullPath })})"
"    .then(async r=>{"
"      const msg = await r.text();"
"      if(!r.ok){"
"        document.getElementById('error').innerText='Error: '+msg;"
"        document.getElementById('success').innerText='';"
"      } else {"
"        document.getElementById('error').innerText='';"
"        document.getElementById('success').innerText='Config file '+fullPath+' loaded successfully. App updated to use these settings.';"
"        console.log('Config loaded from', fullPath);"
"        loadConfig();"
"      }"
"    })"
"    .catch(err=>{"
"      document.getElementById('error').innerText='Network error: '+err;"
"      document.getElementById('success').innerText='';"
"    });"
"}"
"function updateHlsStatus(){"
"  fetch('/hlsstatus').then(r=>r.json()).then(d=>{"
"    if(d.status==='running'){"
"      document.getElementById('hlsStatus').innerText='HLS: running';"
"    } else {"
"      document.getElementById('hlsStatus').innerText='HLS: stopped';"
"    }"
"  }).catch(_=>{"
"    document.getElementById('hlsStatus').innerText='HLS: (error)';"
"  });"
"}"

"function startHls(){"
"  fetch('/starthls',{method:'POST'}).then(async r=>{"
"    updateHlsStatus();"
"  });"
"}"

"function stopHls(){"
"  fetch('/stophls',{method:'POST'}).then(async r=>{"
"    updateHlsStatus();"
"  });"
"}"


"function updateMp3Status(){"
"  fetch('/mp3status').then(r=>r.json()).then(d=>{"
"    if(d.status==='running'){"
"      document.getElementById('mp3Status').innerText='MP3: running (url: '+(d.url||'')+')';"
"    } else {"
"      document.getElementById('mp3Status').innerText='MP3: stopped (url: '+(d.url||'')+')';"
"    }"
"  }).catch(_=>{"
"    document.getElementById('mp3Status').innerText='MP3: (error)';"
"  });"
"}"

"function startMp3(){"
"  fetch('/startmp3',{method:'POST'}).then(async r=>{"
"    updateMp3Status();"
"  });"
"}"

"function stopMp3(){"
"  fetch('/stopmp3',{method:'POST'}).then(async r=>{"
"    updateMp3Status();"
"  });"
"}"



"function updateAacStatus(){"
"  fetch('/aacstatus').then(r=>r.json()).then(d=>{"
"    if(d.status==='running'){"
"      document.getElementById('aacStatus').innerText='AAC: running (url: '+(d.url||'')+')';"
"    } else {"
"      document.getElementById('aacStatus').innerText='AAC: stopped (url: '+(d.url||'')+')';"
"    }"
"  }).catch(_=>{"
"    document.getElementById('aacStatus').innerText='AAC: (error)';"
"  });"
"}"

"function startEncoder(){"
"  fetch('/start',{method:'POST'}).then(async r=>{"
"    updateAacStatus();"
"  });"
"}"
"function stopEncoder(){"
"  fetch('/stop',{method:'POST'}).then(async r=>{"
"    updateAacStatus();"
"  });"
"}"

"function updateMetadata(){"
"  fetch('/metadata')"
"    .then(r => { if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })"
"    .then(d => {"
"      document.getElementById('nowPlaying').innerText = 'Now Playing: ' + d.metadata;"
"      const list=document.getElementById('metaHistory');"
"      list.innerHTML='';"
"      d.history.forEach(item => {"
"        const li=document.createElement('li');"
"        li.innerText=item;"
"        li.onclick=()=>{document.getElementById('manualMeta').value=item;};"
"        list.appendChild(li);"
"      });"
"    })"
"    .catch(err => {"
"      document.getElementById('nowPlaying').innerText = 'Now Playing: (error)';"
"      console.error('updateMetadata error', err);"
"    });"
"}"

"setInterval(updateMetadata, 5000);"

"function sendMetadata(){"
"  const meta = document.getElementById('manualMeta').value.trim();"
"  if(meta===''){"
"    document.getElementById('metaMsg').innerText = 'Error: Metadata cannot be empty.';"
"    return;"
"  }"
"  fetch('/setmeta', {"
"    method: 'POST',"
"    headers: {'Content-Type':'application/json'},"
"    body: JSON.stringify({metadata:meta})"
"  })"
"    .then(async r => {"
"      const msg = await r.text();"
"      if(!r.ok){"
"        document.getElementById('metaMsg').innerText = 'Error: ' + msg;"
"      } else {"
"        document.getElementById('metaMsg').innerText = msg;"
"        updateMetadata();"
"        addToHistory(meta);"
"      }"
"    })"
"    .catch(err => {"
"      document.getElementById('metaMsg').innerText = 'Error: ' + err;"
"      console.error('setmeta error', err);"
"    });"
"}"
"function addToHistory(meta){"
"  const list=document.getElementById('metaHistory');"
"  const item=document.createElement('li');"
"  item.innerText=meta;"
"  item.onclick=()=>{document.getElementById('manualMeta').value=meta;};"
"  list.insertBefore(item,list.firstChild);"
"  while(list.childNodes.length>5){list.removeChild(list.lastChild);}"
"}"

"function startControlListener(){"
"  fetch('/startcontrol',{method:'POST'}).then(async r=>{"
"    document.getElementById('controlStatus').innerText=await r.text();"
"    updateControlStatus();"
"  });"
"}"

"function stopControlListener(){"
"  fetch('/stopcontrol',{method:'POST'}).then(async r=>{"
"    document.getElementById('controlStatus').innerText=await r.text();"
"    updateControlStatus();"
"  });"
"}"

"function updateControlStatus(){"
"  fetch('/controlstatus').then(r=>r.json()).then(d=>{"
"    if(d.status==='running'){"
"      document.getElementById('controlStatus').innerText='Controller Started on Port '+d.port;"
"    } else {"
"      document.getElementById('controlStatus').innerText='Controller Stopped';"
"    }"
"  }).catch(_=>{"
"    document.getElementById('controlStatus').innerText='Controller: (error)';"
"  });"
"}"

"function startMetaListener(){"
"  fetch('/startmeta',{method:'POST'}).then(async r=>{"
"    document.getElementById('metaStatus').innerText=await r.text();"
"    updateMetaStatus();"
"  });"
"}"
"function stopMetaListener(){"
"  fetch('/stopmeta',{method:'POST'}).then(async r=>{"
"    document.getElementById('metaStatus').innerText=await r.text();"
"    updateMetaStatus();"
"  });"
"}"
"function updateMetaStatus(){"
"  fetch('/metastatus').then(r=>r.json()).then(d=>{"
"    if(d.status==='running'){"
"      document.getElementById('metaStatus').innerText='Metadata Listener Started on Port '+d.port;"
"    } else {"
"      document.getElementById('metaStatus').innerText='Metadata Listener Stopped';"
"    }"
"  }).catch(_=>{"
"    document.getElementById('metaStatus').innerText='Metadata Listener: (error)';"
"  });"
"}"

"function loadDevices(){"
"  fetch('/devices')"
"    .then(r => { if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })"
"    .then(devs => {"
"      const sel = document.getElementById('deviceIndex');"
"      sel.innerHTML = '';"
"      devs.forEach(d => {"
"        const opt = document.createElement('option');"
"        opt.value = d.index;"
"        opt.text = d.index+': '+d.name+' (In='+d.inputs+', Out='+d.outputs+', Rate='+d.defaultRate+')';"
"        sel.appendChild(opt);"
"      });"
"    })"
"    .catch(err => {"
"      const sel = document.getElementById('deviceIndex');"
"      sel.innerHTML = '';"
"      const opt = document.createElement('option');"
"      opt.value = '';"
"      opt.text = 'Failed to load devices: '+err;"
"      sel.appendChild(opt);"
"      console.error('loadDevices error', err);"
"    });"
"}"

"function setupLogConsole(){"
"  const logDiv=document.getElementById('logConsole');"
"  if(!logDiv){return;}"
"  const evtSource=new EventSource('/logstream');"
"  logDiv.innerHTML='[system] Connected. Waiting for log lines…<br>';"
"  evtSource.onmessage=function(e){"
"    const safe=e.data.replace(/&/g,'&amp;').replace(/</g,'&lt;');"
"    logDiv.insertAdjacentHTML('beforeend',safe+'<br>');"
"    logDiv.scrollTop=logDiv.scrollHeight;"
"  };"
"  evtSource.onerror=function(){"
"    logDiv.insertAdjacentHTML('beforeend','<br>[system] Error on log stream.');"
"  };"
"}"

"updateStatus();"
"loadConfig();"
"loadDevices();"
"updateMetadata();"
"updateMetaStatus();"
"setupLogConsole();"
"</script>"

        "</body></html>",
        "text/html"
    );
});

    int port;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        port = g_config.webPort;
    }
    logMessage("[web] About to call svr.listen on port " + std::to_string(port));

    // Wrap listen in a check so you see if it fails
    if (!svr.listen("0.0.0.0", port)) {
        logMessage("[web] ERROR: Failed to bind or listen on port " + std::to_string(port));
    }

}

// ---------------- main() ----------------
void signalHandler(int) {
    logMessage("[signal] Shutdown requested");
    g_running = false;
    g_engineRunning = false;
    g_iceRunning = false;
    metaShutdown = true;
    {
        std::lock_guard<std::mutex> lock(g_hlsCtrlMutex);
        g_hls.running = false;
    }
}
int main(int argc, char** argv) {
    // Register Ctrl-C / kill handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    g_running = true;
    metaShutdown = false;
    metaRunning = false;

    bool headless = false;
    bool configSetByCli = false;

    // Set absolute app root FIRST
    initAppRoot(argv[0]); // sets g_appRoot and g_configDir

    // Prepare app folders and default config
    initAppLayout(); // sets g_configPath and ensures config/ + logs/ exist

    // Parse CLI flags
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--config" && i + 1 < argc) {
            g_configPath = argv[++i];
            configSetByCli = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --headless          Run in headless mode (no web UI)\n";
            std::cout << "  --config <file>     Specify config file path\n";
            std::cout << "  --help, -h          Show this help message\n\n";
            std::cout << "Environment Variables:\n";
            std::cout << "  AAC_ENCODER_CONFIG  Config file path (overridden by --config)\n\n";
            std::cout << "Config priority: CLI argument > Environment variable > Default\n";
            return 0;
        }
    }

    // Apply environment variable if CLI didn't set config
    if (!configSetByCli) {
        const char* envConfig = std::getenv("AAC_ENCODER_CONFIG");
        if (envConfig && envConfig[0] != '\0') {
            g_configPath = envConfig;
        } else {
            g_configPath = "/app/config/config.json";
        }
    }

    // ---------------- Defaults and load config ----------------
    {
        // Defaults (no lock during single-thread init)
        g_config.controlPort = 9010;
        g_config.controlEnabled = true;
        g_config.commandStart = "";
        g_config.commandStop = "";
        g_config.icecastUrl = "http://example.com/stream";
        g_config.user = "source";
        g_config.pass = "password";
        g_config.mp3IcecastUrl = "http://example.com/stream";
        g_config.mp3User = "source";
        g_config.mp3Pass = "password";
        g_config.amperwaveStationId = "";
        g_config.iceDataParse = "";
        g_config.sampleRate = 48000;
        g_config.channels = 2;
        g_config.bitrate = 128000;
        g_config.deviceIndex = -1;
        g_config.listenPort = 9000;
        g_config.icyMetaInt = 8192;
        g_config.logDir = "";

        g_config.inputType = "portaudio";
        g_config.rtpAddress = "";
        g_config.rtpPort = 5004;
        g_config.rtpInterface = "";
        g_config.icecastInterface = "";
        g_config.rtpGain = 1.0f;

        g_config.hlsEnabled = false;
        g_config.hlsSegmentSeconds = 6;
        g_config.hlsWindow = 5;
        g_config.hlsPath = "";

        g_config.iceEnabled = true;
        g_config.iceMetaEnabled = true;

        // Load config
        if (!loadConfigFromFile(g_config, g_configPath)) {
            std::cerr << "Config file not found, using defaults: " << g_configPath << std::endl;
        } else {
            std::cerr << "Loaded config from: " << g_configPath << std::endl;
        }
    }

    // Initialize log AFTER config so we can use logDir
    initLogFile();
    logMessage("Log initialized for AACIceEncoderWebUI");
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        logMessage("Log directory: " + g_config.logDir);
        logMessage("Using config file: " + g_configPath);
        logMessage("[main] g_appRoot=" + g_appRoot);
        logMessage("[main] g_configDir(default)=" + g_configDir);
        logMessage("[main] g_config.configDir(raw)=" + g_config.configDir);
        logMessage("[main] iceDataParse=" + g_config.iceDataParse);
    }

#ifdef NDEBUG
#define BUILD_TYPE "Release"
#else
#define BUILD_TYPE "Debug"
#endif
    logMessage(std::string("Build type: ") + BUILD_TYPE);
    logMessage(std::string("AACIceEncoderWebUI version ") + APP_VERSION +
               (headless ? " [Headless]" : " [UI]") + " starting up");

    // Initialize RTP multicast at startup if configured
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        if (g_config.inputType == "rtp") {
            if (!initRtpMulticast(g_config)) {
                logMessage("[RTP] Failed to subscribe to multicast at startup");
            }
        }
    }

    // Get configured port for message only
    int port;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        port = g_config.webPort;
    }

    // In main(): headless branch – delegate to startHeadless()
    if (headless) {
        logMessage("[main] Starting in headless mode");
        g_running = true;
        startHeadless();

        // Block until everything stops
        while (g_running || metaRunning || g_hls.running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Cleanup before exit
        stopEncoder();
        stopMp3EncoderWithConfig();
        stopHls();
        stopMetadataListener();
        stopAudioEngine();
        stopControlListener();

        // Close global RTP socket
        if (g_rtpSock >= 0) {
            close(g_rtpSock);
            g_rtpSock = -1;
            logMessage("[RTP] Global socket closed, multicast unsubscribed");
        }

        logMessage("[main] Headless mode exited cleanly");
        curl_global_cleanup();
        return 0;
    }

    // UI mode
    logMessage("Web UI available. Open http://localhost:" + std::to_string(port));
    AudioState dummyAudio;
    AacState dummyAac;
    int dummySock = -1;
    // Let runWebUI read webPort from g_config
    runWebUI(dummyAudio, dummyAac, dummySock, 0, 0, 0);

    // Close global RTP socket on exit (UI mode)
    if (g_rtpSock >= 0) {
        close(g_rtpSock);
        g_rtpSock = -1;
        logMessage("[RTP] Global socket closed, multicast unsubscribed");
    }

    curl_global_cleanup();
    return 0;
}
