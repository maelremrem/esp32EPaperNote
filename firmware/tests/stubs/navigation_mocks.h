#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
constexpr int pdPASS=1;
struct HostSemaphore { std::mutex mutex; std::condition_variable cv; bool signalled=false; };
using SemaphoreHandle_t=HostSemaphore*;
inline bool threaded_tasks=false,task_create_ok=true,semaphore_create_ok=true;
inline SemaphoreHandle_t xSemaphoreCreateBinary() {return semaphore_create_ok ? new HostSemaphore : nullptr;}
inline int xSemaphoreGive(SemaphoreHandle_t s) { std::lock_guard<std::mutex> l(s->mutex);s->signalled=true;s->cv.notify_all();return 1; }
inline int xSemaphoreTake(SemaphoreHandle_t s,int ms) { std::unique_lock<std::mutex> l(s->mutex);if(!s->cv.wait_for(l,std::chrono::milliseconds(ms),[&](){return s->signalled;}))return 0;s->signalled=false;return 1; }
inline void vSemaphoreDelete(SemaphoreHandle_t s) {delete s;}
inline void vTaskDelete(void*) {}
inline int xTaskCreate(void (*f)(void*),const char*,unsigned,void* arg,int,void*) {if(!task_create_ok)return 0;if(threaded_tasks)std::thread(f,arg).detach();else f(arg);return 1;}

#include "network/web_policy.h"
#include "network/server_url.h"
#include "network/live_preview_helpers.h"
#include "project_config.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(x) assert((x) == 0)
using esp_err_t = int;
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 1, ESP_ERR_NVS_NEW_VERSION_FOUND = 2;
inline int nvs_flash_init() { return 0; }
inline int nvs_flash_erase() { return 0; }
constexpr int ESP_OK = 0, NVS_READONLY = 0, NVS_READWRITE = 1;
using nvs_handle_t = unsigned;
inline int nvs_open_error=0, nvs_get_error=1, nvs_set_error=0, nvs_commit_error=0;
inline uint8_t nvs_value=10, nvs_staged=10;
inline int nvs_commits=0;
inline int nvs_open(const char*, int, nvs_handle_t* h) { *h=1; return nvs_open_error; }
inline int nvs_get_u8(nvs_handle_t, const char*, uint8_t* v) { *v=nvs_value; return nvs_get_error; }
inline int nvs_set_u8(nvs_handle_t, const char*, uint8_t v) { nvs_staged=v; return nvs_set_error; }
inline std::string nvs_url, nvs_url_staged, nvs_token, nvs_token_staged;
inline bool nvs_token_exists=false, nvs_token_pending=false;
inline int nvs_url_get_error=1;
inline int nvs_commit(nvs_handle_t) { ++nvs_commits; if(!nvs_commit_error) { nvs_value=nvs_staged; nvs_get_error=0; nvs_url=nvs_url_staged; nvs_url_get_error=0; if(nvs_token_pending) { nvs_token=nvs_token_staged; nvs_token_exists=true; nvs_token_pending=false; } } return nvs_commit_error; }
inline void nvs_close(nvs_handle_t) {}
inline int nvs_get_str(nvs_handle_t, const char* key, char* v, size_t* n) {
    if(std::string(key)=="token") { if(!nvs_token_exists || nvs_token.size()+1>*n) return 1; std::strcpy(v,nvs_token.c_str()); *n=nvs_token.size()+1; return 0; }
    if(nvs_url_get_error || nvs_url.size()+1>*n) return 1;
    std::strcpy(v,nvs_url.c_str()); *n=nvs_url.size()+1; return 0;
}
inline int nvs_set_str(nvs_handle_t, const char* key, const char* v) { if(std::string(key)=="token") { nvs_token_staged=v; nvs_token_pending=true; } else nvs_url_staged=v; return nvs_set_error; }
inline int64_t mock_now_us = 5000000;
inline int64_t esp_timer_get_time() { return mock_now_us; }
inline unsigned esp_random() { static unsigned n=100; return ++n; }
constexpr int pdTRUE = 1;
inline int pdMS_TO_TICKS(int n) { return n; }
inline std::function<int()> queue_hook;
inline std::function<int(void*)> button_queue_hook;
inline int xQueueReceive(int, void *out, int timeout) {
    if(button_queue_hook)return button_queue_hook(out);
    if (!timeout && !queue_hook) return 0;
    if (queue_hook) return queue_hook();
    throw std::runtime_error("main loop reached");
}
namespace app {
enum class ButtonEvent { ShortPress, DoublePress, LongPress };
struct Button { bool init_ok = true; bool init() { return init_ok; } int queue() { return 0; } };
}
namespace display {
enum class BootWifiStatus { Pending, Connecting, Connected, Failed, Skipped, Disabled };
struct EpaperDisplay { uint8_t limit = 10; bool init() { return true; } void setPartialRefreshLimit(uint8_t n) { limit=n; } uint8_t partialRefreshLimit() const { return limit; } };
struct SavedNote { std::string id; bool transcribed = false; };
struct Ui {
    EpaperDisplay *display;
    std::vector<uint8_t> boot_limits;
    explicit Ui(EpaperDisplay &d) : display(&d) {}
    static constexpr size_t MENU_ITEMS = 7;
    std::string screen, title, message, note_id;
    size_t selected = 0, page = 0, status_calls = 0, renders_without_status = 0;
    bool wifi_status = false, sd_status = false, fresh_status = false;
    bool recoverable = true;
    std::vector<SavedNote> notes;
    struct BootWifiFrame { BootWifiStatus home, hotspot; std::string ip; };
    std::vector<BootWifiFrame> boot_wifi;
    void showBootWifi(BootWifiStatus home, BootWifiStatus hotspot, const std::string &ip = {}) {
        boot_wifi.push_back({home, hotspot, ip}); render("boot_wifi");
    }
    static inline size_t pages = 3;
    void setStatus(bool wifi, bool sd) { wifi_status = wifi; sd_status = sd; fresh_status = true; ++status_calls; }
    void render(const std::string &name) { screen = name; if (!fresh_status) ++renders_without_status; fresh_status = false; }
    struct BootFrame { size_t completed; std::string stage, detail; };
    std::vector<BootFrame> boot_progress;
    void showBootProgress(size_t n,const std::string &s,const std::string &d) { boot_progress.push_back({n,s,d}); render("boot_progress"); }
    void showBoot() { boot_limits.push_back(display->partialRefreshLimit()); render("boot"); }
    void showStorageError() { render("storage_error"); }
    void showError(const std::string &t, const std::string &m, bool r = true) { title = t; message = m; recoverable = r; render("error"); }
    void showOffline(size_t, const std::string &, const std::string &) { render("offline"); }
    void showIdle(const std::string &, const std::string &, size_t, bool) { render("idle"); }
    void showSyncing(size_t, size_t, const std::string &) { render("sync"); }
    void showSyncStopping() { title="Stopping sync"; render("sync_stopping"); }
    void showLiveRecording(const std::string &, const std::string &, bool) { render("recording"); }
    void showMenu(size_t n, uint8_t = 10) { selected = n; render("settings"); }
    void showRefreshInterval(uint8_t n) { selected=n; render("refresh"); }
    void showSavedNotes(const std::vector<SavedNote> &n, size_t s) { notes = n; selected = s; render("notes"); }
    static size_t notePageCount(const std::string &) { return pages; }
    void showNote(const std::string &id, const std::string &text, size_t p) { note_id = id; message = text; page = p; render("reader"); }
    void showInfo(const std::string &t, const std::string &m) { title = t; message = m; render("info"); }
    void showSubmenu(const std::string &t, const std::vector<std::string> &e, size_t s,
                     const std::vector<bool> &children = {}) { title=t; selected=s; message=e.empty()?"":e[0]; submenu_children=children; render("submenu"); }
    std::vector<bool> submenu_children;
    void showStorageMenu(size_t s, bool, uint64_t, uint64_t) { showSubmenu("Storage",{"Status / retry mount","Format SD","Back"},s,{true,true,false}); }
    void showStorageStatus(const std::string &m,bool,uint64_t,uint64_t) { showInfo("Storage",m); }
    void showCancelConfirmation(bool selected,bool all,const std::string&) { showSubmenu(all ? "Cancel all pending?" : "Cancel transcription?",{"Back","Keep audio / cancel"},selected ? 1 : 0); }
    void showFormatConfirmation(bool e) { selected=e; render("format"); }
    void showPortal(const std::string &, const std::string &, const std::string &) { render("portal"); }
};
}
namespace storage {
struct Usage { bool known=false; uint64_t total=0, free=0; };
struct SavedNote { std::string id; bool transcribed = false, audio=false; };
struct NoteStore {
    int download_opens=0;
    FILE *openDownload(const std::string &,bool,uint64_t &bytes) { ++download_opens; bytes=0; return nullptr; }
    bool mount_ok = true, read_ok = true, write_ok = true, commit_ok = true, archive_ok = true;
    int reads = 0, writes = 0, archives = 0, commits = 0, discards = 0;
    size_t requested_limit = 0;
    std::vector<SavedNote> history;
    std::vector<std::string> pending;
    std::string transcript = "Saved transcript", read_id;
    std::function<void()> during_init;
    bool init() { if(during_init) during_init(); return mount_ok; }
    Usage usage() const { return {true,1000,500}; }
    std::string lastError() const { return mount_ok ? "" : "SD unavailable"; }
    bool format_ok=true;
    int formats=0;
    bool format() { ++formats; pending.clear(); history.clear(); return format_ok; }
    size_t pendingCount() const { return pending.size(); }
    std::vector<std::string> pendingIds() const { return pending; }
    std::vector<SavedNote> savedNotes(size_t limit = 100) {
        requested_limit = limit;
        return {history.begin(), history.begin() + std::min(limit, history.size())};
    }
    bool readTranscript(const std::string &id, std::string &text) { ++reads; read_id = id; text = read_ok ? transcript : ""; return read_ok; }
    std::string makeNoteId() { return "recorded"; }
    std::string recordingTempPath() { return "/sdcard/recording.wav"; }
    std::string pendingAudioPath(const std::string &id) { return "/sdcard/pending/" + id + ".wav"; }
    std::function<void()> during_write;
    bool writeTranscript(const std::string &, const std::string &, const std::string &, double, const std::string &) { ++writes; if(during_write)during_write(); return write_ok; }
    bool archiveAudio(const std::string &id) {
        ++archives;
        if (archive_ok) pending.erase(std::remove(pending.begin(), pending.end(), id), pending.end());
        return archive_ok;
    }
    bool cancelPending(const std::string &id) { return archiveAudio(id); }
    bool commitRecording(const std::string &) { ++commits; return commit_ok; }
    bool discardRecording() { ++discards; return true; }
};
}
namespace network {
struct WifiProfile { std::string ssid,password; };
using WifiProfiles=std::array<WifiProfile,2>;
enum class WifiAttemptStatus { Connecting, Connected, Failed, Skipped, Disabled };
struct WifiManager {
    bool online = false, connect_ok = true;
    int connects = 0;
    void init() {}
    std::vector<std::string> savedSsids() const { return {"synthetic-home","synthetic-hotspot"}; }
    bool save_ok=true;
    int saves=0;
    bool saveProfiles(const WifiProfiles&) { ++saves; return save_ok; }
    bool saveProfileEdits(const WifiProfiles &p, const std::array<bool,2>&) { return saveProfiles(p); }
    bool connected() const { return online; }
    bool connectPreferred(std::function<void(size_t, WifiAttemptStatus, const std::string &)> progress = {}) {
        ++connects;
        if (progress) progress(0, WifiAttemptStatus::Connecting, {});
        online = connect_ok;
        if (progress) {
            progress(0, online ? WifiAttemptStatus::Connected : WifiAttemptStatus::Failed, ipAddress());
            progress(1, online ? WifiAttemptStatus::Skipped : WifiAttemptStatus::Failed, {});
        }
        return online;
    }
    std::string currentSsid() const { return online ? "test-network" : ""; }
    std::string ipAddress() const { return online ? "192.0.2.42" : ""; }
};
struct CaptivePortal {
    bool running=false, start_ok=true, timed_out=false, queued=false;
    bool start(WifiManager&) { running=start_ok; return running; }
    bool stop() { running=false; queued=false; return true; }
    bool active() const { return running; }
    bool expired() const { return timed_out; }
    void pollScan() {}
    std::string ssid() const { return "Whistle-123456"; }
    std::string password() const { return "synthetic123"; }
    std::string address() const { return "192.168.4.1"; }
    bool takeProfiles(WifiProfiles&) { bool q=queued; queued=false; return q; }
    void complete(bool) {}
};
struct WebNote { char id[97]{}; bool transcribed=false, audio=false; };
struct WebStatus {
    web::State state = web::State::Idle;
    bool connecting = false, wifi = false, stopping = false, recovery = false;
    size_t pending = 0;
    size_t notes_count=0; WebNote notes[40]{};
    bool mounted=false, audio_ready=false;
    bool usage_known=false;
    bool token_configured=false;
    unsigned server_revision=0;
    uint64_t total_bytes=0, free_bytes=0;
    unsigned partial_limit=10, format_challenge=0;
    char ssid[2][33]{}, current_ssid[33]{}, storage_error[256]{};
    char base_url[64]{}, ip[64]{}, note_id[128]{}, text[4096]{}, live_text[4096]{};
};
struct WebServer {
    int starts=0, stops=0, session_opens=0;
    bool download_active=false, download_provided=false;
    bool downloadBusy() { return download_active; }
    bool takeDownload(char (&id)[97],bool &md) { web::copyText(id,"safe-note"); md=false; return download_active; }
    void provideDownload(unsigned,FILE*,uint64_t) { download_provided=true; }
    std::string openLocalSession() { ++session_opens; return "012345"; }
    void revokeLocalSession() {}
    void stop() { ++stops; queued=false; }
    bool queued = false;
    web::Command command = web::Command::Start;
    std::string result, config_url;
    web::SettingsWrite settings;
    bool takeSettings(web::SettingsWrite &s) { s=settings; return true; }
    bool takeConfig(char (&url)[64]) { web::copyText(url,config_url); return true; }
    std::vector<WebStatus> snapshots;
    bool start() { ++starts; return true; }
    void publish(const WebStatus &s) { snapshots.push_back(s); }
    bool take(web::Command &c, unsigned &id) { if (!queued) return false; queued = false; c = command; id = 1; return true; }
    void complete(unsigned, const char *r) { result = r; }
};
enum class HealthStatus { Ready, Unavailable, Skipped };
struct ApiClient {
    int health_calls=0;
    HealthStatus health_result=HealthStatus::Ready;
    std::function<void()> during_health;
    HealthStatus health() { ++health_calls; if(during_health) during_health(); return token.empty() ? HealthStatus::Skipped : health_result; }
    static inline std::string token="public-test-token";
    static bool tokenConfigured() { return !token.empty(); }
    static void setTarget(const std::string &u,const std::string &t,bool replace=true) { url=u; if(replace)token=t; }
    static inline std::string url="http://192.0.2.20:8000";
    static std::string defaultBaseUrl() { return "http://192.0.2.20:8000"; }
    static std::string baseUrl() { return url; }
    static void setBaseUrl(const std::string &s) { url=s; }
    struct Result { bool ok = true, cancelled=false; std::string text = "result", language = "fr"; double duration = 1; std::string model = "whistle", error = "failed"; };
    bool ok = true;
    int calls = 0;
    std::string path;
    std::function<void()> during_transcribe;
    Result transcribe(const std::string &, const std::string &p, const std::atomic<bool> * = nullptr) { ++calls; path = p; if(during_transcribe) during_transcribe(); Result r; r.ok = ok; return r; }
};
using TranscriptResult=ApiClient::Result;
struct LivePreview {
    bool stopped = true;
    int stops = 0;
    bool start(const std::string &) { return true; }
    void requestStop() { ++stops; }
    bool waitStopped(int) { return stopped; }
    void snapshot(std::string &) {}
};
}
namespace audio {
struct AudioRecorder {
    bool running = false, stopped = true, clean = true, start_ok = true, init_ok = true;
    size_t bytes = 6400;
    int starts = 0, stops = 0;
    bool init() { return init_ok; }
    bool start(const std::string &, network::LivePreview *) { ++starts; running = start_ok; return start_ok; }
    void requestStop() { ++stops; }
    bool waitStopped(int) { return stopped; }
    bool isRecording() { return running; }
    bool savedCleanly() { return clean; }
    size_t recordedBytes() { return bytes; }
};
}
