#!/usr/bin/env python3
"""Actual Wi-Fi manager/NVS/AP lifecycle with IDF transport adapters."""
import os,re,shlex,subprocess,tempfile
from pathlib import Path
import test_boot_wifi as boot
ROOT=boot.ROOT
EXTRA=r'''
#define ESP_PLATFORM 1
constexpr int ESP_ERR_WIFI_NOT_CONNECT=77, WIFI_MODE_AP=1, WIFI_MODE_APSTA=2, WIFI_IF_AP=1, WIFI_STORAGE_RAM=0;
constexpr int WIFI_EVENT_SCAN_DONE=99;
struct wifi_event_sta_scan_done_t { uint32_t status=0; uint8_t number=0,scan_id=0; };
struct wifi_scan_config_t { bool show_hidden=false; };
inline int scan_start_error=0,scan_read_error=0,scan_calls=0,scan_stops=0;
inline bool scan_blocking=true;
inline std::vector<wifi_ap_record_t> scan_records;
inline int esp_wifi_scan_start(wifi_scan_config_t*,bool block){++scan_calls;scan_blocking=block;return scan_start_error;}
inline int esp_wifi_scan_stop(){++scan_stops;return 0;}
inline int esp_wifi_clear_ap_list(){return 0;}
inline int esp_wifi_scan_get_ap_num(uint16_t*n){*n=scan_records.size();return scan_read_error;}
inline int esp_wifi_scan_get_ap_records(uint16_t*n,wifi_ap_record_t*r){*n=std::min<size_t>(*n,scan_records.size());std::copy_n(scan_records.begin(),*n,r);return scan_read_error;}

#define ESP_EVENT_DEFINE_BASE(name) const char *name=#name
inline bool barrier_delayed=false;
inline std::vector<std::function<void()>> barriers;
inline int esp_event_post(esp_event_base_t base,int id,const void*data,size_t,int){
 unsigned generation=*static_cast<const unsigned*>(data);
 auto deliver=[=]{auto copy=generation;handler(nullptr,base,id,&copy);};
 if(barrier_delayed)barriers.push_back(deliver);else deliver();return 0;
}
inline void drain_barriers(){auto pending=barriers;barriers.clear();for(auto &f:pending)f();}

inline std::vector<unsigned char> committed,staged;
inline int blob_get_error=0, blob_set_error=0, blob_commit_error=0;
inline int nvs_get_blob(nvs_handle_t,const char*,void* p,size_t* n) { if(committed.empty() || blob_get_error || committed.size()>*n) return 1; std::memcpy(p,committed.data(),committed.size()); *n=committed.size(); return 0; }
inline int nvs_set_blob(nvs_handle_t,const char*,const void* p,size_t n) { if(blob_set_error) return 1; staged.assign((const unsigned char*)p,(const unsigned char*)p+n); return 0; }
inline int esp_wifi_set_storage(int) { return 0; }
inline int esp_wifi_stop() { associated=false; return 0; }
inline int ap_creates=0;
inline esp_netif_t ap_netif;
inline bool ap_ip_ok=true;
inline esp_netif_t *esp_netif_create_default_wifi_ap() { ++ap_creates; return &ap_netif; }
inline int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *p) { if(n==&ap_netif){p->ip={192,168,4,1};return ap_ip_ok?0:-1;} return old_esp_netif_get_ip_info(n,p); }
inline void esp_netif_destroy_default_wifi(esp_netif_t*) {}
'''
TEST=r'''
#define CHECK(c) do {if(!(c)){ std::cerr<<__LINE__<<": "<<#c<<"\n";return 1;}}while(0)
int main(){
 network::WifiManager manager;
 CHECK(manager.savedSsids()==std::vector<std::string>({"home","hotspot"}));
 network::WifiProfiles profiles={network::WifiProfile{"new home","synthetic123"},network::WifiProfile{"",""}};
 CHECK(manager.saveProfiles(profiles));
 CHECK(manager.savedSsids()==std::vector<std::string>({"new home",""}));
 auto before=committed;
 blob_commit_error=1; profiles[0].ssid="not committed";
 CHECK(!manager.saveProfiles(profiles) && manager.savedSsids()[0]=="new home");
 blob_commit_error=0; blob_set_error=1; CHECK(!manager.saveProfiles(profiles)); blob_set_error=0;
 nvs_open_error=1; CHECK(!manager.saveProfiles(profiles)); nvs_open_error=0;
 profiles[0].ssid="bad\nssid"; CHECK(!manager.saveProfiles(profiles));
 profiles[0].ssid=std::string(32,'X'); CHECK(manager.saveProfiles(profiles));
 CHECK(manager.connectPreferred() && selected==std::string(32,'X'));
 CHECK(manager.currentSsid()==std::string(32,'X'));
 network::WifiProfiles edits={network::WifiProfile{std::string(32,'X'),""},network::WifiProfile{"",""}};
 CHECK(manager.saveProfileEdits(edits, {{false,false}}));
 CHECK(manager.connectPreferred()); CHECK(last_password=="synthetic123");
 edits[0].ssid="different"; CHECK(!manager.saveProfileEdits(edits, {{false,false}}));
 CHECK(manager.saveProfileEdits(edits, {{true,false}}));
 CHECK(manager.connectPreferred()); CHECK(last_password.empty());
 edits[0].ssid=std::string(32,'X'); edits[0].password="synthetic123";
 CHECK(manager.saveProfileEdits(edits, {{false,false}}));
 handler(nullptr,IP_EVENT,IP_EVENT_STA_LOST_IP,nullptr);
 CHECK(!manager.connected() && manager.ipAddress().empty());
 CHECK(manager.connectPreferred());
 CHECK(manager.startPortalWifi("Whistle-123456","synthetic123"));
 CHECK(radio_mode==WIFI_MODE_APSTA);
 CHECK(manager.portalActive() && !manager.connected() && manager.ipAddress().empty() && !manager.connectPreferred());
 CHECK(manager.beginPortalScan()); CHECK(scan_calls==1 && !scan_blocking);
 CHECK(!manager.beginPortalScan());
 network::WifiScanResults found; std::string error; CHECK(manager.pollPortalScan(found,error)==network::WifiScanState::Scanning);
 wifi_ap_record_t a{}; std::memcpy(a.ssid,"Lab",3);a.rssi=-70;a.authmode=3;a.primary=6; scan_records={a};
 a.rssi=-30;scan_records.push_back(a);
 wifi_event_sta_scan_done_t done{};handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);
 CHECK(manager.pollPortalScan(found,error)==network::WifiScanState::Complete && found.size()==1 && found[0].rssi==-30 && found[0].channel==6 && found[0].authmode==3);
 manager.stopPortalWifi(); CHECK(!manager.portalActive());
 CHECK(!manager.beginPortalScan());
 CHECK(manager.startPortalWifi("Whistle-123456","synthetic123")); manager.stopPortalWifi();
 CHECK(ap_creates==1);
 ap_ip_ok=false; CHECK(!manager.startPortalWifi("Whistle-123456","synthetic123") && !manager.portalActive()); ap_ip_ok=true;
 committed[0]^=1; CHECK(network::WifiManager{}.savedSsids()[0]=="home");
 committed=before; std::fill(committed.begin()+4,committed.begin()+37,'X'); CHECK(network::WifiManager{}.savedSsids()[0]=="home");
 CHECK(logs.find("synthetic123")==std::string::npos);
 std::cout<<"PASS actual Wi-Fi bounded NVS/defaults/commit failures/validation/SSID/AP lifecycle\n";
}
'''
def main():
 strip=lambda s: re.sub(r'^#include[^\n]*$','',s,flags=re.M)
 mocks=(ROOT/'firmware/tests/stubs/navigation_mocks.h').read_text()
 for name in ['WifiManager','CaptivePortal','WifiProfile']:
  mocks=re.sub(r'struct '+name+r' \{.*?\n\};','',mocks,flags=re.S) if name!='WifiProfile' else re.sub(r'struct WifiProfile[^\n]*\n','',mocks)
 mocks=re.sub(r'using WifiProfiles[^\n]*\n','',mocks)
 mocks=re.sub(r'enum class WifiAttemptStatus \{[^}]*\};','',mocks)
 mocks=mocks.replace('inline int nvs_commit(nvs_handle_t) {','inline int old_nvs_commit(nvs_handle_t) {')
 adapters=boot.ADAPTERS.replace('#include "network/wifi_manager.h"','').replace('inline std::string selected;', 'inline std::string selected,last_password;')
 adapters=adapters.replace('inline int esp_wifi_set_mode(int) { return 0; }','inline int radio_mode=-1; inline int esp_wifi_set_mode(int mode) {radio_mode=mode;return 0;}')
 adapters=adapters.replace('struct wifi_ap_record_t {', 'struct wifi_ap_record_t { int8_t rssi=0; uint8_t primary=1; int authmode=0;')
 adapters=adapters.replace('struct wifi_config_t { struct {','struct wifi_config_t { struct { unsigned char ssid[32]{},password[64]{}; int ssid_len=0,authmode=0,max_connection=0; } ap; struct {')
 adapters=adapters.replace('selected=reinterpret_cast<char *>(c->sta.ssid);','last_password=reinterpret_cast<char *>(c->sta.password); selected.assign(reinterpret_cast<char *>(c->sta.ssid),strnlen(reinterpret_cast<char *>(c->sta.ssid),32));')
 adapters=adapters.replace('inline int esp_wifi_set_config(int,','inline int esp_wifi_set_config(int iface,').replace('selected.assign(', 'if(iface==WIFI_IF_STA) selected.assign(')
 adapters=adapters.replace('std::strncpy(reinterpret_cast<char *>(ap->ssid),selected.c_str(),31);','std::memcpy(ap->ssid,selected.data(),std::min(selected.size(),sizeof ap->ssid));')
 # WIFI_IF_AP declaration precedes adapter so route logic can use both.
 with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'),prefix='wifi-profiles-') as tmp:
  d=Path(tmp); (d/'mocks.h').write_text(mocks)
  adapters=adapters.replace("inline int esp_netif_get_ip_info(","inline int old_esp_netif_get_ip_info(")
  extra=EXTRA.replace('#define ESP_PLATFORM 1\n','')
  (d/'test.cpp').write_text('#define ESP_PLATFORM 1\n#include "mocks.h"\n#include "network/wifi_manager.h"\n'+adapters+extra+'\ninline int nvs_commit(nvs_handle_t){if(blob_commit_error){committed=staged;return 1;} committed=staged;return 0;}\n'+strip((ROOT/'firmware/src/network/wifi_manager.cpp').read_text())+TEST)
  subprocess.run(shlex.split(os.environ.get('CXX','g++'))+['-std=c++17','-Wall','-Wextra','-Wno-unused-variable',*shlex.split(os.environ.get('HOST_TEST_FLAGS','')),'-I'+str(d),'-I'+str(ROOT/'firmware/src'),'-I'+str(ROOT/'firmware/include'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
  subprocess.run([str(d/'test')],check=True)
if __name__=='__main__':main()
