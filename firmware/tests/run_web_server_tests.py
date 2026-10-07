#!/usr/bin/env python3
"""Exercise real web_server.cpp + IDF cJSON; mock only HTTP/FreeRTOS transport.
Never loads production credentials. Requires installed PlatformIO ESP-IDF package.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
idf = Path(os.environ.get('IDF_PATH', str(Path.home() / '.platformio/packages/framework-espidf')))
scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
http = r'''#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_RESP_USE_STRLEN = -1;
enum httpd_method_t { HTTP_GET, HTTP_POST };
using httpd_handle_t = void*;
struct httpd_req_t { void *user_ctx = nullptr; const char *uri = "/"; size_t content_len = 0;
    std::map<std::string,std::string> headers, response_headers; std::string code="200 OK", type, body, input; size_t offset=0; httpd_method_t method=HTTP_GET; };
struct httpd_uri_t {const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); void *user_ctx;};
struct httpd_config_t { int stack_size, max_uri_handlers, max_open_sockets; bool lru_purge_enable; int recv_wait_timeout, send_wait_timeout; };
inline httpd_config_t HTTPD_DEFAULT_CONFIG() {return {};}
inline std::vector<httpd_uri_t> routes;
inline esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t*) {*h=reinterpret_cast<void*>(1); return 0;}
inline esp_err_t httpd_stop(httpd_handle_t) {routes.clear(); return 0;}
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *r) {routes.push_back(*r); return 0;}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name) {return r->headers[name].size();}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t n) {
    if(r->headers[name].size()>=n) return -1;
    std::strcpy(out,r->headers[name].c_str()); return 0;
}
inline int httpd_req_recv(httpd_req_t *r,char *v,size_t n) { size_t len=std::min(n,r->input.size()-r->offset); if(!len) return -1; memcpy(v,r->input.data()+r->offset,len); r->offset+=len; return len; }
inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) {r->response_headers[k]=v; return 0;}
inline esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *v) {r->code=v; return 0;}
inline esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *v) {r->type=v; return 0;}
inline int fail_chunk_after=-1, chunk_calls=0, largest_chunk=0;
inline esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *v,int n) {
    if(n) { largest_chunk=std::max(largest_chunk,n); if(fail_chunk_after>=0 && chunk_calls++>=fail_chunk_after) return -1; r->body.append(v,n); } return 0;
}
inline esp_err_t httpd_resp_send(httpd_req_t *r,const char *v,int n) {r->body.assign(v,n<0?std::strlen(v):n); return 0;}
'''
freertos = '#pragma once\n#include <thread>\n#include <chrono>\nconstexpr unsigned portMAX_DELAY = ~0u;\ninline unsigned pdMS_TO_TICKS(unsigned v) { return v; }\ninline void vTaskDelay(unsigned v) { std::this_thread::sleep_for(std::chrono::milliseconds(v)); }\n'
semphr = '''#pragma once
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {static std::mutex m; return &m;}
inline bool xSemaphoreTake(SemaphoreHandle_t m,unsigned) {m->lock(); return true;}
inline void xSemaphoreGive(SemaphoreHandle_t m) {m->unlock();}
'''
with tempfile.TemporaryDirectory(prefix='web-server-tests-', dir=scratch) as directory:
    d = Path(directory)
    (d / 'freertos').mkdir()
    for file, content in {'esp_http_server.h':http,'freertos/FreeRTOS.h':freertos,'freertos/semphr.h':semphr,'freertos/task.h':'#include "FreeRTOS.h"\n',
                          'esp_timer.h':'#include <cstdint>\ninline int64_t mock_web_now=0; inline int64_t esp_timer_get_time() { return mock_web_now; }\n',
                          'esp_random.h':'inline unsigned mock_web_random=0x12345678; inline unsigned esp_random() { return ++mock_web_random; }\n',
                          'secrets.h':'#define VOICE_NOTES_API_TOKEN "public-test-token"\n'}.items():
        (d / file).write_text(content)
    assembly = '.section .rodata\n'
    for asset in ('index.html','style.css','tokens.css','app.js'):
        name = 'web_' + asset.replace('.','_')
        assembly += f'.global _binary_{name}_start\n_binary_{name}_start:\n'
        assembly += '.byte ' + ','.join(str(byte) for byte in (root / 'firmware/src/web' / asset).read_bytes()) + '\n'
        assembly += f'.global _binary_{name}_end\n_binary_{name}_end:\n'
    assembly += '.section .note.GNU-stack,"",@progbits\n'
    (d / 'assets.S').write_text(assembly)
    flags = os.environ.get('HOST_TEST_FLAGS','').split()
    cjson = idf / 'components/json/cJSON'
    subprocess.run(['cc',*flags,'-c',str(cjson / 'cJSON.c'),'-o',str(d / 'cJSON.o')],check=True)
    binary = str(d / 'web_server_test')
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-pthread',*flags,
                    '-I'+str(d),'-I'+str(root / 'firmware/src'),'-I'+str(cjson),
                    str(root / 'firmware/tests/web_server_test.cpp'),
                    str(root / 'firmware/src/network/web_server.cpp'),str(d / 'cJSON.o'),str(d / 'assets.S'),
                    '-o',binary],check=True)
    subprocess.run([binary],check=True)
