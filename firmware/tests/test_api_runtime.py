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
using esp_err_t=int;
constexpr int ESP_OK=0,HTTP_METHOD_POST=1;
inline const char* esp_err_to_name(int) { return "error"; }
struct esp_http_client_config_t {const char* url=nullptr; int timeout_ms=0; bool keep_alive_enable=false,disable_auto_redirect=false; int (*crt_bundle_attach)(void*)=nullptr;};
using esp_http_client_handle_t=void*;
inline std::string captured_url,auth,body;
inline bool complete=false;
inline void* esp_http_client_init(const esp_http_client_config_t* c) {assert(c->disable_auto_redirect); captured_url=c->url;complete=false;return reinterpret_cast<void*>(1);}
inline void esp_http_client_set_method(void*,int) {}
inline void esp_http_client_set_header(void*,const char* k,const char* v) {if(!strcmp(k,"Authorization")) auth=v;}
inline int esp_http_client_open(void*,int) {return 0;}
inline int esp_http_client_write(void*,const char*,int n) {return n;}
inline int esp_http_client_fetch_headers(void*) {return 0;}
inline int esp_http_client_get_status_code(void*) {return 200;}
inline bool esp_http_client_is_complete_data_received(void*) {return complete;}
inline int esp_http_client_read(void*,char* out,int n) {if(complete)return 0; int len=body.size();if(len>n)len=n;memcpy(out,body.data(),len);complete=true;return len;}
inline void esp_http_client_close(void*) {}
inline void esp_http_client_cleanup(void*) {}
'''
test=r'''
#include <cassert>
#include <cstdio>
#include "esp_http_client.h"
#include "network/api_client.h"
int main(int argc,char** argv) {
 assert(argc==2); FILE* f=fopen(argv[1],"wb"); for(int i=0;i<100;++i)fputc(0,f);fclose(f);
 using network::ApiClient;
 assert(ApiClient::baseUrl()=="http://192.0.2.20:8000");
 for(auto base:{"http://192.0.2.25:8001","https://10.0.0.8:8443"}) {
   ApiClient::setBaseUrl(base); ApiClient a;
   body=R"({"id":"note","status":"done","text":"final"})";
   assert(a.transcribe("note",argv[1]).ok);
   assert(captured_url==std::string(base)+"/api/v1/notes/note/transcribe");
   std::atomic<bool> cancelled{false}; unsigned char pcm[128]{};
   body=R"({"id":"note","status":"partial","text":"live","language":"fr","model":"whistle"})";
   assert(a.preview("note",pcm,sizeof(pcm),cancelled).ok);
   assert(captured_url==std::string(base)+"/api/v1/live/note");
   assert(auth=="Bearer public-test-token");
 }
 puts("PASS actual ApiClient runtime URL used for final and live, unchanged Bearer token");
}
'''
with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as tmp:
 d=Path(tmp)
 for name,text in {'esp_http_client.h':http,'esp_crt_bundle.h':'inline int esp_crt_bundle_attach(void*) {return 0;}','esp_log.h':'#define ESP_LOGI(...) ((void)0)','esp_timer.h':'#include <cstdint>\ninline int64_t esp_timer_get_time() { return 1; }','secrets.h':'#define VOICE_NOTES_API_BASE_URL "http://192.0.2.20:8000"\n#define VOICE_NOTES_API_TOKEN "public-test-token"\n','test.cpp':test}.items():
  (d/name).write_text(text)
 flags=os.environ.get('HOST_TEST_FLAGS','').split()
 subprocess.run(['cc',*flags,'-c',str(idf/'cJSON.c'),'-o',str(d/'json.o')],check=True)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-pthread',*flags,'-I'+str(d),'-I'+str(root/'firmware/src'),'-I'+str(root/'firmware/include'),'-I'+str(idf),str(d/'test.cpp'),str(root/'firmware/src/network/api_client.cpp'),str(d/'json.o'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test'),str(d/'note.wav')],check=True)
