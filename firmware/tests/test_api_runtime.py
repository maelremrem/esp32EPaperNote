#!/usr/bin/env python3
"""Actual ApiClient final + preview HTTP URL capture with host transport adapters."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
idf=Path(os.environ.get('IDF_PATH',str(Path.home()/'.platformio/packages/framework-espidf')))/'components/json/cJSON'
http=r'''
#pragma once
#include <string>
#include <cassert>
#include <cstring>
#include <functional>
using esp_err_t=int;
constexpr int ESP_OK=0,HTTP_METHOD_POST=1,HTTP_METHOD_GET=0;
inline const char* esp_err_to_name(int) { return "error"; }
struct esp_http_client_config_t {const char* url=nullptr; int timeout_ms=0; bool keep_alive_enable=false,disable_auto_redirect=false; int (*crt_bundle_attach)(void*)=nullptr;};
using esp_http_client_handle_t=void*;
inline std::string captured_url,auth,body;
inline size_t body_offset=0;
inline std::function<void()> open_hook,write_hook,fetch_hook,read_hook;
constexpr int ESP_ERR_HTTP_EAGAIN=7007;
inline bool complete=false,init_fail=false; inline int status_code=200,open_error=0,read_error=0,fetch_error=0,timeout=0,method=-1,cleanups=0;
inline void* esp_http_client_init(const esp_http_client_config_t* c) {assert(c->disable_auto_redirect); captured_url=c->url; timeout=c->timeout_ms;auth.clear();complete=false;body_offset=0;return init_fail ? nullptr : reinterpret_cast<void*>(1);}
inline void esp_http_client_set_method(void*,int m) {method=m;}
inline void esp_http_client_set_header(void*,const char* k,const char* v) {if(!strcmp(k,"Authorization")) auth=v;}
inline int esp_http_client_open(void*,int) {if(open_hook)open_hook();return open_error;}
inline int esp_http_client_write(void*,const char*,int n) {if(write_hook)write_hook();return n;}
inline int esp_http_client_fetch_headers(void*) {if(fetch_hook)fetch_hook();return fetch_error;}
inline int esp_http_client_get_status_code(void*) {return status_code;}
inline bool esp_http_client_is_complete_data_received(void*) {return complete;}
inline int esp_http_client_read(void*,char* out,int n) {if(read_hook)read_hook();if(read_error)return read_error; if(complete)return 0; int len=body.size()-body_offset;if(len>n)len=n;memcpy(out,body.data()+body_offset,len);body_offset+=len;complete=body_offset==body.size();return len;}
inline void esp_http_client_close(void*) {}
inline void esp_http_client_cleanup(void*) {++cleanups;}
'''
test=r'''
#include <cassert>
#include <cstdio>
#include "esp_http_client.h"
#include "network/api_client.h"
#include "esp_timer.h"
int main(int argc,char** argv) {
 assert(argc==2); FILE* f=fopen(argv[1],"wb"); for(int i=0;i<100;++i)fputc(0,f);fclose(f);
 using network::ApiClient;
 assert(ApiClient::baseUrl()=="http://192.0.2.20:8000");
 for(auto base:{"http://192.0.2.25:8001","https://10.0.0.8:8443"}) {
   ApiClient::setTarget(base,"runtime-token"); ApiClient a;
   body=R"({"id":"note","status":"done","text":"final"})";
   assert(a.transcribe("note",argv[1]).ok);
   assert(captured_url==std::string(base)+"/api/v1/notes/note/transcribe");
   std::atomic<bool> cancelled{false}; unsigned char pcm[128]{};
   body=R"({"id":"note","status":"partial","text":"live","language":"fr","model":"whistle"})";
   assert(a.preview("note",pcm,sizeof(pcm),cancelled).ok);
   assert(captured_url==std::string(base)+"/api/v1/live/note");
   assert(auth=="Bearer runtime-token");
 }
 ApiClient::setTarget("https://10.0.0.8:8443","runtime-token");
 body=R"({"status":"ok","service":"ESP32 Voice Notes STT","model_loaded":false})";
 assert(ApiClient{}.health()==network::HealthStatus::Ready);
 assert(captured_url=="https://10.0.0.8:8443/health" && method==HTTP_METHOD_GET && timeout>0 && timeout<=5000 && auth.empty());
 for(auto invalid:{"not json", "{}", R"({"status":"ok","service":"other"})", R"({"status":"error","service":"ESP32 Voice Notes STT"})", R"({"status":"ok","service":"ESP32 Voice Notes STT"}trailing)"}) {
   body=invalid; assert(ApiClient{}.health()==network::HealthStatus::Unavailable);
 }
 body=R"({"status":"ok","service":"ESP32 Voice Notes STT"})";
 for(int code:{301,302,401,404,500}) {status_code=code; assert(ApiClient{}.health()==network::HealthStatus::Unavailable);}
 fetch_error=-ESP_ERR_HTTP_EAGAIN; assert(ApiClient{}.health()==network::HealthStatus::Unavailable); fetch_error=0;
 status_code=200; open_error=9; assert(ApiClient{}.health()==network::HealthStatus::Unavailable); open_error=0;
 read_error=-1; assert(ApiClient{}.health()==network::HealthStatus::Unavailable); read_error=0;
 init_fail=true; assert(ApiClient{}.health()==network::HealthStatus::Unavailable); init_fail=false;
 read_hook=[&]() {http_now=6000001;}; assert(ApiClient{}.health()==network::HealthStatus::Unavailable);read_hook={};http_now=1;
 body=std::string(2048,'x'); assert(ApiClient{}.health()==network::HealthStatus::Unavailable);
 ApiClient::setTarget("", "runtime-token"); assert(ApiClient{}.health()==network::HealthStatus::Skipped);
 ApiClient::setTarget("http://192.0.2.25:8001", ""); assert(ApiClient{}.health()==network::HealthStatus::Skipped);
 ApiClient::setTarget("http://192.0.2.25:8001", "runtime-token");
 status_code=200; body=R"({"id":"note","status":"done","text":"final"})";
 std::atomic<bool> stop{true};
 auto cancelled=ApiClient{}.transcribe("note",argv[1],&stop);
 assert(!cancelled.ok && cancelled.cancelled);
 for(auto hook:{&open_hook,&write_hook,&fetch_hook,&read_hook}) {
   stop=false; *hook=[&]() {stop=true;}; int before=cleanups;
   auto result=ApiClient{}.transcribe("note",argv[1],&stop);
   assert(!result.ok && result.cancelled && result.error=="Sync cancelled" && cleanups==before+1 && timeout<=2000);
   *hook={};
 }
 stop=false; int waits=0; fetch_error=-ESP_ERR_HTTP_EAGAIN;
 fetch_hook=[&]() { if(++waits==3)fetch_error=0; };
 assert(ApiClient{}.transcribe("note",argv[1],&stop).ok && waits==3); fetch_hook={};
 read_error=-ESP_ERR_HTTP_EAGAIN; waits=0;
 read_hook=[&]() {if(++waits==3)read_error=0;};
 assert(ApiClient{}.transcribe("note",argv[1],&stop).ok && waits==3);read_hook={};
 fetch_error=-ESP_ERR_HTTP_EAGAIN;
 fetch_hook=[&]() {http_now+=2000000;};
 assert(!ApiClient{}.transcribe("note",argv[1],&stop).ok);fetch_hook={};fetch_error=0;http_now=1;
 status_code=401; body="runtime-token";
 assert(ApiClient{}.transcribe("note",argv[1]).error.find("runtime-token")==std::string::npos);
 status_code=200;
 ApiClient::setTarget("http://192.0.2.25:8001","");
 assert(!ApiClient{}.transcribe("note",argv[1]).ok);
 assert(!ApiClient::tokenConfigured());
 puts("PASS actual ApiClient atomic target, final/live, public health contract/errors/deadlines and cooperative sync cancellation");
}
'''
with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as tmp:
 d=Path(tmp)
 for name,text in {'esp_http_client.h':http,'esp_crt_bundle.h':'inline int esp_crt_bundle_attach(void*) {return 0;}','esp_log.h':'#define ESP_LOGI(...) ((void)0)','esp_timer.h':'#pragma once\n#include <cstdint>\ninline int64_t http_now=1;\ninline int64_t esp_timer_get_time() { return http_now; }','secrets.h':'#define VOICE_NOTES_API_BASE_URL "http://192.0.2.20:8000"\n#define VOICE_NOTES_API_TOKEN "public-test-token"\n','test.cpp':test}.items():
  (d/name).write_text(text)
 flags=os.environ.get('HOST_TEST_FLAGS','').split()
 subprocess.run(['cc',*flags,'-c',str(idf/'cJSON.c'),'-o',str(d/'json.o')],check=True)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-pthread',*flags,'-I'+str(d),'-I'+str(root/'firmware/src'),'-I'+str(root/'firmware/include'),'-I'+str(idf),str(d/'test.cpp'),str(root/'firmware/src/network/api_client.cpp'),str(d/'json.o'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test'),str(d/'note.wav')],check=True)
