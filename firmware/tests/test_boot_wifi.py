#!/usr/bin/env python3
"""Real WiFi manager and main boot with host IDF adapters; no secrets/device access."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ADAPTERS = r'''
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include "network/wifi_manager.h"
inline std::string logs;
inline void logline(const char *fmt, ...) {
    char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); logs += b;
}
#define ESP_LOGI(tag, ...) logline(__VA_ARGS__)
#undef ESP_LOGW
#define ESP_LOGW(tag, ...) logline(__VA_ARGS__)
#define WIFI_HOME_SSID home_ssid.c_str()
#define WIFI_IPHONE_SSID hotspot_ssid.c_str()
#define WIFI_HOME_PASSWORD "fixture-home-password"
#define WIFI_IPHONE_PASSWORD "fixture-hotspot-password"
inline std::string home_ssid = "home", hotspot_ssid = "hotspot";
constexpr int ESP_ERR_INVALID_STATE = 3;
using esp_event_base_t = const char *;
inline const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip";
constexpr int WIFI_EVENT_STA_DISCONNECTED = 1, IP_EVENT_STA_GOT_IP = 2, IP_EVENT_STA_LOST_IP = 3, ESP_EVENT_ANY_ID = -1;
constexpr int WIFI_MODE_STA = 0, WIFI_PS_MIN_MODEM = 0, WIFI_AUTH_WPA2_PSK = 0, WIFI_AUTH_OPEN = 1, WIFI_IF_STA = 0;
using EventBits_t = unsigned;
using EventGroupHandle_t = unsigned *;
constexpr unsigned BIT0 = 1, BIT1 = 2;
constexpr int pdFALSE = 0;
struct esp_netif_t {};
struct ip_address { unsigned a=192,b=0,c=2,d=42; };
struct esp_netif_ip_info_t { ip_address ip; };
struct ip_event_got_ip_t { esp_netif_ip_info_t ip_info; };
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (p)->a, (p)->b, (p)->c, (p)->d
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t { struct { unsigned char ssid[32]{}, password[64]{}; struct { int authmode; } threshold; struct { bool capable,required; } pmf_cfg; } sta; };
struct wifi_ap_record_t { unsigned char ssid[32]{}; };
inline void (*handler)(void *, esp_event_base_t, int32_t, void *) = nullptr;
inline unsigned bits = 0;
inline std::vector<std::string> attempts;
inline std::string selected;
inline bool associated = false, have_ip = false, stale_disconnect = false, switching_race = false, init_ok = true;
inline int home_result = 1, hotspot_result = 1, sntp_calls = 0;
inline bool start_ok=true, connect_error=false;
inline std::function<void()> init_hook, connect_hook;
inline int esp_netif_init() { return 0; }
inline int esp_event_loop_create_default() { if(init_hook) init_hook(); return init_ok ? 0 : 9; }
inline esp_netif_t *esp_netif_create_default_wifi_sta() { static esp_netif_t n; return &n; }
inline int esp_wifi_init(wifi_init_config_t *) { return 0; }
inline int esp_event_handler_register(esp_event_base_t, int, decltype(handler) h, void *) { handler=h; return 0; }
inline int esp_wifi_set_mode(int) { return 0; }
inline int esp_wifi_set_ps(int) { return 0; }
inline int esp_wifi_start() { return start_ok ? 0 : 9; }
inline EventGroupHandle_t xEventGroupCreate() { return &bits; }
inline unsigned xEventGroupGetBits(EventGroupHandle_t) { return bits; }
inline unsigned xEventGroupSetBits(EventGroupHandle_t, unsigned b) { return bits |= b; }
inline unsigned xEventGroupClearBits(EventGroupHandle_t, unsigned b) { bits &= ~b; return bits; }
inline unsigned xEventGroupWaitBits(EventGroupHandle_t, unsigned wanted, int clear, int, int) {
    if (stale_disconnect) { handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr); stale_disconnect=false; }
    // A stale intentional disconnect is queued ahead of the new connection's GOT_IP.
    if (!(bits & wanted)) {
        int result = selected == "home" ? home_result : hotspot_result;
        if (result == 1) { ip_event_got_ip_t event; have_ip=true; handler(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,&event); }
        else if (result == 0) { associated=false; handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr); }
    }
    unsigned got=bits; if(clear) bits &= ~wanted; return got;
}
inline int esp_wifi_disconnect() { associated=false; have_ip=false; stale_disconnect=switching_race; return 0; }
inline int esp_wifi_set_config(int, wifi_config_t *c) { selected=reinterpret_cast<char *>(c->sta.ssid); return 0; }
inline int esp_wifi_connect() { if(connect_hook) connect_hook(); attempts.push_back(selected); associated=true; return connect_error && selected=="home" ? 9 : 0; }
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { if(!associated) return 9; std::strncpy(reinterpret_cast<char *>(ap->ssid),selected.c_str(),31); return 0; }
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *) { return esp_netif_create_default_wifi_sta(); }
inline int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *p) { if(!have_ip) return 9; *p={}; return 0; }
constexpr int SNTP_OPMODE_POLL = 0;
inline void esp_sntp_setoperatingmode(int) {}
inline void esp_sntp_setservername(int, const char *) {}
inline void esp_sntp_init() { ++sntp_calls; }
'''
TEST = r'''
#define CHECK(c) do { if(!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main(int argc, char **argv) {
    std::string scenario=argc>1 ? argv[1] : "home";
    std::string mode=(scenario=="server-fail" || scenario=="server-unconfigured") ? "home" : scenario;
    if(scenario=="server-fail") api.health_result=network::HealthStatus::Unavailable;
    if(scenario=="server-unconfigured") network::ApiClient::token.clear();
    if(mode=="fallback" || mode=="offline" || mode=="association") home_result=0;
    if(mode=="offline") hotspot_result=0;
    if(mode=="association") home_result=hotspot_result=2; // Association only, never GOT_IP.
    if(mode=="disabled") home_ssid.clear();
    if(mode=="empty") { home_ssid.clear(); hotspot_ssid.clear(); }
    if(mode=="init-fail") init_ok=false;
    if(mode=="start-fail") start_ok=false;
    if(mode=="connect-error") connect_error=true;
    if(mode=="race") { home_result=0; switching_race=true; }
    using S=network::WifiAttemptStatus;
    struct Report { size_t index; S status; std::string ip; };
    std::vector<Report> reports;
    network::WifiManager manager;
    const bool ok=manager.connectPreferred([&](size_t i,S s,const std::string &ip) { reports.push_back({i,s,ip}); });
    std::vector<std::pair<size_t,S>> expected;
    if(mode=="home") expected={{0,S::Connecting},{0,S::Connected},{1,S::Skipped}};
    if(mode=="fallback" || mode=="race" || mode=="connect-error") expected={{0,S::Connecting},{0,S::Failed},{1,S::Connecting},{1,S::Connected}};
    if(mode=="offline" || mode=="association") expected={{0,S::Connecting},{0,S::Failed},{1,S::Connecting},{1,S::Failed}};
    if(mode=="disabled") expected={{0,S::Disabled},{1,S::Connecting},{1,S::Connected}};
    if(mode=="empty") expected={{0,S::Disabled},{1,S::Disabled}};
    if(mode=="init-fail" || mode=="start-fail") expected={{0,S::Failed},{1,S::Failed}};
    CHECK(reports.size()==expected.size());
    for(size_t i=0;i<expected.size();++i) {
        CHECK(reports[i].index==expected[i].first && reports[i].status==expected[i].second);
        CHECK(reports[i].ip==(reports[i].status==S::Connected ? "192.0.2.42" : ""));
    }
    CHECK(ok==(mode=="home" || mode=="fallback" || mode=="disabled" || mode=="race" || mode=="connect-error"));
    CHECK(manager.connected()==ok);
    CHECK(attempts.size()==(mode=="empty" || mode=="init-fail" || mode=="start-fail" ? 0u : mode=="home" || mode=="disabled" ? 1u : 2u));
    CHECK(sntp_calls==(ok ? 1 : 0));
    attempts.clear(); bits=0; reports.clear(); stale_disconnect=mode=="race";
    store.mount_ok=false; recorder.init_ok=false;
    init_hook=[&]() { assert(!ui.boot_wifi.empty()); assert(ui.boot_wifi.front().home==display::BootWifiStatus::Pending); };
    connect_hook=[&]() { const auto &f=ui.boot_wifi.back(); assert((selected=="home" ? f.home : f.hotspot)==display::BootWifiStatus::Connecting); };
    store.during_init=[&]() { assert(ui.screen=="boot"); };
    api.during_health=[&]() { assert(ui.boot_progress.back().completed==3 && ui.boot_progress.back().stage=="Server" && ui.boot_progress.back().detail=="Checking /health"); };
    try { app_main(); } catch(const std::runtime_error &) {}
    CHECK(api.health_calls==(ok ? 1 : 0));
    CHECK(ui.boot_progress.size()>=3);
    CHECK(ui.boot_progress.front().completed==1 && ui.boot_progress.front().detail=="Unavailable. Settings can retry.");
    CHECK(ui.boot_progress.back().completed==4 && ui.boot_progress.back().stage=="Server");
    CHECK(ui.boot_progress.back().detail==(!ok ? "Skipped: no Wi-Fi" : scenario=="server-fail" ? "Unavailable. Sync can retry." : scenario=="server-unconfigured" ? "Skipped: not configured" : "Ready"));
    CHECK(ui.boot_wifi.size()==expected.size()+1);
    using U=display::BootWifiStatus;
    CHECK(ui.boot_wifi.front().home==U::Pending && ui.boot_wifi.front().hotspot==U::Pending);
    U home=U::Pending, hotspot=U::Pending;
    std::string ip;
    for(size_t i=0;i<expected.size();++i) {
        U status=U::Failed;
        switch(expected[i].second) {
            case S::Connecting: status=U::Connecting; break;
            case S::Connected: status=U::Connected; ip="192.0.2.42"; break;
            case S::Failed: status=U::Failed; break;
            case S::Skipped: status=U::Skipped; break;
            case S::Disabled: status=U::Disabled; break;
        }
        (expected[i].first==0 ? home : hotspot)=status;
        const auto &frame=ui.boot_wifi[i+1];
        CHECK(frame.home==home && frame.hotspot==hotspot && frame.ip==ip);
    }
    CHECK(ui.screen=="idle" && ui.renders_without_status==0);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(state==AppState::Menu && ui.screen=="settings");
    init_hook={}; connect_hook={};
    size_t frames=ui.boot_wifi.size(); reconnectWifi(); CHECK(ui.boot_wifi.size()==frames);
    CHECK(logs.find("fixture-home-password")==std::string::npos && logs.find("fixture-hotspot-password")==std::string::npos);
    if(ok) { handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr); CHECK(!wifi.connected()); }
    std::cout << "PASS " << scenario << " real manager + real main boot\n";
}
'''

def main():
    strip = lambda text: re.sub(r'^#include[^\n]*$', '', text, flags=re.MULTILINE)
    mocks=(ROOT/'firmware/tests/stubs/navigation_mocks.h').read_text()
    mocks=re.sub(r'struct WifiManager \{.*?\n\};', '', mocks, flags=re.DOTALL)
    mocks=re.sub(r'enum class WifiAttemptStatus \{[^}]*\};', '', mocks)
    mocks=re.sub(r'struct WifiProfile[^\n]*\n|using WifiProfiles[^\n]*\n', '', mocks)
    mocks='#include "network/wifi_manager.h"\n'+mocks
    scratch=Path(os.environ.get('TMPDIR',Path.home()/'.hermes/cache/scratch'))
    scratch.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='boot-wifi-',dir=scratch) as tmp:
        d=Path(tmp)
        (d/'navigation_mocks.h').write_text(mocks)
        (d/'test.cpp').write_text('#include "navigation_mocks.h"\n'+ADAPTERS+strip((ROOT/'firmware/src/network/wifi_manager.cpp').read_text())+strip((ROOT/'firmware/src/main.cpp').read_text())+TEST)
        command=shlex.split(os.environ.get('CXX','g++'))+['-std=c++17','-Wall','-Wextra','-Wno-unused-variable',*shlex.split(os.environ.get('HOST_TEST_FLAGS','')),'-I'+str(d),'-I'+str(ROOT/'firmware/src'),'-I'+str(ROOT/'firmware/include'),str(d/'test.cpp'),'-o',str(d/'test')]
        subprocess.run(command,check=True)
        for mode in os.environ.get('WIFI_TEST_MODES', 'home fallback offline association disabled empty init-fail race start-fail connect-error server-fail server-unconfigured').split():
            subprocess.run([str(d/'test'),mode],check=True)

if __name__=='__main__':
    main()
