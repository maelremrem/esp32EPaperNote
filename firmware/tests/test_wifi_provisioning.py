#!/usr/bin/env python3
"""Execute real captive HTTP/DNS/AP lifecycle with bounded host transports."""
import os,re,shlex,subprocess,tempfile
from pathlib import Path
import test_wifi_profiles as profiles
ROOT=profiles.ROOT
HTTP=r'''
#pragma once
#include <map>
#include <string>
#include <cstring>
struct httpd_req_t {void* user_ctx=nullptr; int content_len=0; std::map<std::string,std::string> headers,response_headers; std::string body,response,status; size_t read=0,chunk=7;};
using httpd_handle_t=void*;
constexpr int HTTP_GET=0,HTTP_POST=1,HTTPD_RESP_USE_STRLEN=-1;
struct httpd_uri_t {const char* uri;int method;int(*handler)(httpd_req_t*);void* user_ctx;};
struct httpd_config_t {int server_port=80,ctrl_port=32768,max_uri_handlers=8,max_open_sockets=7,stack_size=4096; bool lru_purge_enable=false; bool (*uri_match_fn)(const char*,const char*,size_t)=nullptr;};
inline httpd_config_t HTTPD_DEFAULT_CONFIG(){return {};}
inline std::map<std::string,httpd_uri_t> routes;
inline bool http_start_ok=true,route_ok=true; inline int http_stops=0;
inline int httpd_start(httpd_handle_t*h,httpd_config_t*){if(!http_start_ok)return -1;*h=(void*)1;return 0;}
inline int httpd_stop(httpd_handle_t){++http_stops;routes.clear();return 0;}
inline int httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*r){if(!route_ok)return -1;routes[r->uri]=*r;return 0;}
inline bool httpd_uri_match_wildcard(const char*,const char*,size_t){return true;}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*n){return r->headers[n].size();}
inline int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*n,char*p,size_t s){auto it=r->headers.find(n);if(it==r->headers.end()||it->second.empty()||it->second.size()+1>s)return -1;std::strcpy(p,it->second.c_str());return 0;}
inline int httpd_req_recv(httpd_req_t*r,char*p,size_t s){auto n=std::min(std::min(s,r->chunk),r->body.size()-r->read);if(!n)return -1;std::memcpy(p,r->body.data()+r->read,n);r->read+=n;return n;}
inline int httpd_resp_set_status(httpd_req_t*r,const char*s){r->status=s;return 0;}
inline int httpd_resp_set_type(httpd_req_t*,const char*){return 0;}
inline int httpd_resp_set_hdr(httpd_req_t*r,const char*n,const char*v){r->response_headers[n]=v;return 0;}
inline int httpd_resp_send(httpd_req_t*r,const char*b,int n){r->response.assign(b,n<0?std::strlen(b):size_t(n));return 0;}
'''
TRANSPORT=r'''
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
struct HostSem{std::mutex m;std::condition_variable cv;unsigned count;explicit HostSem(unsigned n):count(n){}};
using SemaphoreHandle_t=HostSem*;using TaskHandle_t=void*;
constexpr int pdPASS=1,portMAX_DELAY=-1;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new HostSem(1);}
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return new HostSem(0);}
inline int xSemaphoreTake(SemaphoreHandle_t s,int ticks){std::unique_lock<std::mutex> l(s->m);if(ticks==portMAX_DELAY)s->cv.wait(l,[&]{return s->count>0;});else if(!s->cv.wait_for(l,std::chrono::milliseconds(ticks),[&]{return s->count>0;}))return 0;--s->count;return 1;}
inline int xSemaphoreGive(SemaphoreHandle_t s){std::lock_guard<std::mutex> l(s->m);++s->count;s->cv.notify_one();return 1;}
inline void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
inline bool task_ok=true,socket_ok=true,bind_ok=true;inline int socket_closes=0;
inline int xTaskCreate(void(*f)(void*),const char*,int,void*a,int,TaskHandle_t*h){if(!task_ok)return 0;*h=(void*)1;std::thread(f,a).detach();return pdPASS;}
inline void vTaskDelete(void*){}
inline int host_socket(int,int,int){return socket_ok?42:-1;}
inline int host_bind(int,const sockaddr*,socklen_t){return bind_ok?0:-1;}
inline int host_setsockopt(int,int,int,const void*,socklen_t){return 0;}
inline int host_shutdown(int,int){return 0;}
inline int host_close(int){++socket_closes;return 0;}
inline std::mutex dns_transport;
inline std::vector<uint8_t> dns_query,dns_answer;
inline int host_recvfrom(int,void* out,size_t size,int,sockaddr*,socklen_t*){ {std::lock_guard<std::mutex> lock(dns_transport); if(!dns_query.empty()){auto n=std::min(size,dns_query.size());std::memcpy(out,dns_query.data(),n);dns_query.clear();return int(n);}} std::this_thread::sleep_for(std::chrono::milliseconds(1));return -1;}
inline int host_sendto(int,const void* in,size_t n,int,const sockaddr*,socklen_t){std::lock_guard<std::mutex> lock(dns_transport);dns_answer.assign(static_cast<const uint8_t*>(in),static_cast<const uint8_t*>(in)+n);return int(n);}
#define socket host_socket
#define bind host_bind
#define setsockopt host_setsockopt
#define shutdown host_shutdown
#define close host_close
#define recvfrom host_recvfrom
#define sendto host_sendto
'''
TEST=r'''
#define CHECK(c) do{if(!(c)){std::cerr<<__LINE__<<": "<<#c<<"\n";return 1;}}while(0)
httpd_req_t request(network::CaptivePortal&p,const std::string&body="") {httpd_req_t r;r.user_ctx=&p;r.headers={{"Host","192.168.4.1"},{"Origin","http://192.168.4.1"},{"Content-Type","application/x-www-form-urlencoded"}};r.body=body;r.content_len=int(body.size());return r;}
int main(){
 uint8_t packet[512]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,3,'a','p','p',0,0,1,0,1};
 auto original=std::vector<uint8_t>(packet,packet+21);
 CHECK(network::captiveDnsReply(packet,21,sizeof packet)==37);
 CHECK(packet[0]==0x12&&packet[1]==0x34&&packet[7]==1&&packet[21]==0xc0&&packet[22]==0x0c);
 CHECK(std::equal(original.begin()+12,original.end(),packet+12));
 CHECK(packet[33]==192&&packet[34]==168&&packet[35]==4&&packet[36]==1);
 std::copy(original.begin(),original.end(),packet);CHECK(network::captiveDnsReply(packet,21,36)==0);
 packet[12]=0xc0;CHECK(network::captiveDnsReply(packet,21,512)==0);
 std::copy(original.begin(),original.end(),packet);packet[18]=28;CHECK(network::captiveDnsReply(packet,21,512)==0);
 for(size_t n=0;n<21;++n){std::copy(original.begin(),original.end(),packet);CHECK(network::captiveDnsReply(packet,n,512)==0);}
 CHECK(network::captiveDnsReply(packet,513,512)==0);
 uint32_t random=12345;
 for(unsigned trial=0;trial<2000;++trial){for(auto &byte:packet){random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}CHECK(network::captiveDnsReply(packet,trial%513,512)<=512);}
 network::WifiManager w; network::CaptivePortal p;
 CHECK(p.start(w) && p.active());
 const std::string alphabet="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
 CHECK(p.password().size()==8 && p.password().find_first_not_of(alphabet)==std::string::npos);
 CHECK(p.token_.size()==32);
 const std::string first_password=p.password();
 {std::lock_guard<std::mutex> lock(dns_transport);dns_query=original;}
 bool answered=false;
 for(int i=0;i<200&&!answered;++i){ {std::lock_guard<std::mutex> lock(dns_transport);answered=dns_answer.size()==37;} std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
 CHECK(answered);
 {std::lock_guard<std::mutex> lock(dns_transport);CHECK(dns_answer[33]==192&&dns_answer[36]==1&&std::equal(original.begin()+12,original.end(),dns_answer.begin()+12));}
 auto get=request(p);get.headers.erase("Origin");network::CaptivePortal::index(&get);
 CHECK(get.status=="200 OK" && get.response.find(p.token_)!=std::string::npos && get.response.find("Home")!=std::string::npos);
 CHECK(get.response.find("id=scan")!=std::string::npos && get.response.find("name=h")!=std::string::npos && get.response.find("name=s")!=std::string::npos);
 CHECK(get.response_headers["Content-Security-Policy"].find("nonce-")!=std::string::npos);
 if(const char *output=std::getenv("CAPTIVE_HTML_OUTPUT")){FILE *file=std::fopen(output,"wb");CHECK(file);std::fwrite(get.response.data(),1,get.response.size(),file);std::fclose(file);FILE *headers=std::fopen((std::string(output)+".csp").c_str(),"wb");CHECK(headers);const auto &csp=get.response_headers["Content-Security-Policy"];std::fwrite(csp.data(),1,csp.size(),headers);std::fclose(headers);}
 CHECK(routes.count("/scan")==1 && routes.count("/scan/results")==1);
 auto scan=request(p,"csrf="+p.token_);network::CaptivePortal::scan(&scan);
 CHECK(scan.status=="202 Accepted" && scan_calls==0); // HTTP must never touch radio.
 auto repeat=request(p,"csrf="+p.token_);network::CaptivePortal::scan(&repeat);CHECK(repeat.status=="409 Conflict");
 auto status=request(p);status.headers["X-CSRF-Token"]=p.token_;network::CaptivePortal::scanResults(&status);
 CHECK(status.status=="200 OK" && status.response.find("queued")!=std::string::npos);
 p.pollScan();CHECK(scan_calls==1 && !scan_blocking);
 network::CaptivePortal::scanResults(&status);CHECK(status.response.find("scanning")!=std::string::npos);
 wifi_ap_record_t a{};std::memcpy(a.ssid,"Lab<>&",6);a.rssi=-32;a.authmode=3;a.primary=6;scan_records={a};
 wifi_event_sta_scan_done_t done{};handler(nullptr,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&done);p.pollScan();
 network::CaptivePortal::scanResults(&status);CHECK(status.response.find("complete")!=std::string::npos && status.response.find("Lab<>&")!=std::string::npos);
 CHECK(status.response.find("password")==std::string::npos);
 auto forbidden=request(p);network::CaptivePortal::scanResults(&forbidden);CHECK(forbidden.status=="403 Forbidden");
 auto scanbad=request(p,"csrf=wrong");network::CaptivePortal::scan(&scanbad);CHECK(scanbad.status=="403 Forbidden");
 auto scanforeign=request(p,"csrf="+p.token_);scanforeign.headers["Origin"]="http://evil.test";network::CaptivePortal::scan(&scanforeign);CHECK(scanforeign.status=="403 Forbidden");
 std::string form="csrf="+p.token_+"&h=New+Home&hp=synthetic123&s=&sp=";
 auto post=request(p,form);network::CaptivePortal::save(&post);
 CHECK(post.status=="202 Accepted" && committed.empty());
 network::WifiProfiles f;CHECK(p.takeProfiles(f)&&f[0].ssid=="New Home"&&f[0].password=="synthetic123");
 auto busy=request(p,form);network::CaptivePortal::save(&busy);CHECK(busy.status=="409 Conflict");
 CHECK(w.saveProfiles(f));p.complete(true);p.stop();CHECK(!p.active()&&!w.portalActive());
 CHECK(p.password().empty());
 CHECK(p.start(w));
 CHECK(p.password().size()==8 && p.password()!=first_password && p.password().find_first_not_of(alphabet)==std::string::npos);
 auto foreign=request(p);foreign.headers["Host"]="evil.test";network::CaptivePortal::index(&foreign);
 CHECK(foreign.status=="302 Found"&&foreign.response_headers["Location"]=="http://192.168.4.1/");
 auto noorigin=request(p,form);noorigin.headers.erase("Origin");network::CaptivePortal::save(&noorigin);CHECK(noorigin.status=="403 Forbidden");
 auto bad=request(p,"csrf=csrf&h=x&hp=synthetic123&s=&sp=");network::CaptivePortal::save(&bad);CHECK(bad.status=="400 Bad Request");
 auto duplicate=request(p,"csrf="+p.token_+"&h=a&h=b&hp=synthetic123&s=&sp=");network::CaptivePortal::save(&duplicate);CHECK(duplicate.status=="400 Bad Request");
 auto encoded=request(p,"csrf="+p.token_+"&h=Lab%26Home&hp=synthetic123&s=&sp=");network::CaptivePortal::save(&encoded);CHECK(encoded.status=="202 Accepted");CHECK(p.takeProfiles(f)&&f[0].ssid=="Lab&Home");p.complete(false);
 auto control=request(p,"csrf="+p.token_+"&h=bad%00name&hp=synthetic123&s=&sp=");network::CaptivePortal::save(&control);CHECK(control.status=="400 Bad Request");
 auto accidental=request(p,"csrf="+p.token_+"&h=Open&hp=&s=&sp=");network::CaptivePortal::save(&accidental);CHECK(accidental.status=="400 Bad Request");
 auto open=request(p,"csrf="+p.token_+"&h=Open&hp=&s=&sp=&ho=1");network::CaptivePortal::save(&open);CHECK(open.status=="202 Accepted");CHECK(p.takeProfiles(f)&&f[0].password.empty());p.complete(false);
 auto large=request(p,std::string(1600,'x'));network::CaptivePortal::save(&large);CHECK(large.status=="400 Bad Request");
 mock_now_us+=601000000;CHECK(p.expired());
 auto expired=request(p,"csrf="+p.token_+"&h=a&hp=synthetic123&s=&sp=");network::CaptivePortal::save(&expired);CHECK(expired.status=="403 Forbidden");p.stop();
 http_start_ok=false;CHECK(!p.start(w)&&!w.portalActive());http_start_ok=true;
 route_ok=false;CHECK(!p.start(w)&&!w.portalActive());route_ok=true;
 socket_ok=false;CHECK(!p.start(w)&&!w.portalActive());socket_ok=true;
 bind_ok=false;CHECK(!p.start(w)&&!w.portalActive());bind_ok=true;
 task_ok=false;CHECK(!p.start(w)&&!w.portalActive());task_ok=true;
 CHECK(p.start(w));CHECK(p.stop());
 CHECK(p.start(w));start_ok=false;CHECK(!p.stop());start_ok=true;CHECK(!p.start(w));
 std::cout<<"PASS real captive handlers: form/CSRF/Host/Origin/encoding/body/mailbox and AP/HTTP/DNS lifecycle failures\n";
}
'''
def main():
 strip=lambda s:re.sub(r'^#include[^\n]*$','',s,flags=re.M)
 mocks=(ROOT/'firmware/tests/stubs/navigation_mocks.h').read_text()
 for name in ('WifiManager','CaptivePortal'):mocks=re.sub(r'struct '+name+r' \{.*?\n\};','',mocks,flags=re.S)
 mocks=re.sub(r'struct WifiProfile[^\n]*\n|using WifiProfiles[^\n]*\n|enum class WifiAttemptStatus \{[^}]*\};','',mocks)
 mocks=mocks.replace('inline int nvs_commit(nvs_handle_t) {','inline int old_nvs_commit(nvs_handle_t) {')
 adapters=profiles.boot.ADAPTERS.replace('#include "network/wifi_manager.h"','')
 adapters=adapters.replace('inline int esp_wifi_set_mode(int) { return 0; }','inline int radio_mode=-1; inline int esp_wifi_set_mode(int mode) {radio_mode=mode;return 0;}')
 adapters=adapters.replace('struct wifi_ap_record_t {', 'struct wifi_ap_record_t { int8_t rssi=0; uint8_t primary=1; int authmode=0;')
 adapters=adapters.replace('struct wifi_config_t { struct {','struct wifi_config_t { struct { unsigned char ssid[32]{},password[64]{}; int ssid_len=0,authmode=0,max_connection=0; } ap; struct {')
 adapters=adapters.replace("inline int esp_netif_get_ip_info(","inline int old_esp_netif_get_ip_info(")
 extra=profiles.EXTRA.replace('#define ESP_PLATFORM 1\n','')
 with tempfile.TemporaryDirectory(prefix='wifi-portal-',dir=os.environ.get('TMPDIR')) as tmp:
  d=Path(tmp);(d/'esp_http_server.h').write_text(HTTP);(d/'mocks.h').write_text(mocks)
  (d/'test.cpp').write_text('#define ESP_PLATFORM 1\n#include "mocks.h"\n#include <atomic>\n#include <algorithm>\n#define private public\n#include "network/captive_portal.h"\n#undef private\n'+adapters+extra+TRANSPORT+'\ninline int nvs_commit(nvs_handle_t){if(blob_commit_error)return 1;committed=staged;return 0;}\n'+strip((ROOT/'firmware/src/network/wifi_manager.cpp').read_text())+strip((ROOT/'firmware/src/network/captive_portal.cpp').read_text())+TEST)
  subprocess.run(shlex.split(os.environ.get('CXX','g++'))+['-std=c++17','-pthread','-Wall','-Wextra','-Wno-unused-variable',*shlex.split(os.environ.get('HOST_TEST_FLAGS','')),'-I'+str(d),'-I'+str(ROOT/'firmware/src'),'-I'+str(ROOT/'firmware/include'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
  subprocess.run([str(d/'test')],check=True)
if __name__=='__main__':main()
