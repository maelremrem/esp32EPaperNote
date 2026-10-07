#include "network/web_server.h"
#include "network/server_url.h"
#include <cstdio>
#include <algorithm>
#include "freertos/task.h"
#include <cstring>
#include <memory>
#include <new>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"

#define WEB_ASSET(name, file) \
    extern const unsigned char name##_begin[] asm("_binary_web_" file "_start"); \
    extern const unsigned char name##_end[] asm("_binary_web_" file "_end");
WEB_ASSET(html, "index_html")
WEB_ASSET(css, "style_css")
WEB_ASSET(tokens, "tokens_css")
WEB_ASSET(script, "app_js")
#undef WEB_ASSET

namespace network {
namespace {
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
private:
    SemaphoreHandle_t mutex_;
};
void responseHeaders(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
esp_err_t reply(httpd_req_t *request, const char *code, const char *json) {
    responseHeaders(request);
    httpd_resp_set_status(request, code);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}
std::string requestHeader(httpd_req_t *request, const char *name) {
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0) return {};
    if (length > 255) return "invalid";
    char value[256]{};
    return httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) == ESP_OK ? value : "invalid";
}
const char *stateText(web::State state) {
    switch (state) {
        case web::State::Idle: return "idle";
        case web::State::Menu: return "menu";
        case web::State::Recording: return "recording";
        case web::State::Syncing: return "syncing";
    }
    return "unknown";
}
}

bool WebServer::start() {
    if (server_) return true;
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    httpd_config_t options = HTTPD_DEFAULT_CONFIG();
    options.stack_size = 6144;
    options.max_uri_handlers = 16;
    options.max_open_sockets = 3;
    options.lru_purge_enable = true;
    options.recv_wait_timeout = options.send_wait_timeout = 3;
    if (httpd_start(&server_, &options) != ESP_OK) { server_ = nullptr; return false; }
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, asset, this}, {"/tokens.css", HTTP_GET, asset, this},
        {"/style.css", HTTP_GET, asset, this}, {"/app.js", HTTP_GET, asset, this},
        {"/api/status", HTTP_GET, status, this},
        {"/api/notes", HTTP_GET, notes, this},
        {"/api/download/audio", HTTP_GET, download, this},
        {"/api/download/markdown", HTTP_GET, download, this},
        {"/api/session/close", HTTP_POST, closeSession, this},
        {"/api/settings", HTTP_GET, settings, this},
        {"/api/settings", HTTP_POST, settings, this},
        {"/api/config/server", HTTP_GET, serverConfig, this},
        {"/api/config/server", HTTP_POST, serverConfig, this},
        {"/api/command/start", HTTP_POST, command, this},
        {"/api/command/stop", HTTP_POST, command, this},
        {"/api/command/sync", HTTP_POST, command, this}
    };
    for (const auto &route : routes) {
        if (httpd_register_uri_handler(server_, &route) == ESP_OK) continue;
        httpd_stop(server_); server_ = nullptr; return false;
    }
    return true;
}
std::string WebServer::openLocalSession() {
    if (!mutex_ || !server_) return {};
    Lock lock(mutex_);
    if (download_active_) return {};
    const std::string previous=session_code_;
    session_code_[0]=0; session_deadline_=0; failed_pin_attempts_=0;
    mailbox_=web::Mailbox{}; settings_=web::SettingsWrite{};
    // Rejection sampling avoids modulo bias; bound retries if the RNG is faulty.
    constexpr uint32_t range=1000000u;
    constexpr uint32_t limit=UINT32_MAX-(UINT32_MAX%range);
    for (unsigned attempt=0;attempt<64;++attempt) {
        const uint32_t random=esp_random();
        if (random>=limit) continue;
        char candidate[7]; std::snprintf(candidate,sizeof candidate,"%06lu",static_cast<unsigned long>(random%range));
        if (previous==candidate) continue;
        std::memcpy(session_code_,candidate,sizeof candidate);
        break;
    }
    if (!session_code_[0]) return {};
    session_deadline_=esp_timer_get_time()+600000000LL;
    mailbox_=web::Mailbox{}; command_id_=0; command_result_[0]=0; settings_=web::SettingsWrite{};
    return session_code_;
}
void WebServer::revokeLocalSession() {
    if (!mutex_) return;
    Lock lock(mutex_); session_code_[0]=0; session_deadline_=0; failed_pin_attempts_=0;
    mailbox_=web::Mailbox{}; settings_=web::SettingsWrite{};
}
void WebServer::stop() {
    revokeLocalSession();
    if (server_) { httpd_stop(server_); server_ = nullptr; }
    if (mutex_) { Lock lock(mutex_); mailbox_ = web::Mailbox{}; command_id_=0; command_result_[0]=0; config_url_[0]=0; settings_=web::SettingsWrite{}; }
}
void WebServer::publish(const WebStatus &status) {
    if (!mutex_) return;
    Lock lock(mutex_);
    if (!status.wifi || std::strcmp(snapshot_.ip,status.ip)) {
        session_code_[0]=0; session_deadline_=0; mailbox_=web::Mailbox{}; settings_=web::SettingsWrite{};
    }
    snapshot_ = status;
}
bool WebServer::take(web::Command &command, unsigned &id) {
    if (!mutex_) return false;
    Lock lock(mutex_);
    if (!session_code_[0] || esp_timer_get_time()>=session_deadline_) {
        mailbox_=web::Mailbox{}; settings_=web::SettingsWrite{}; return false;
    }
    return mailbox_.take(command, id);
}
bool WebServer::takeConfig(char (&url)[64]) {
    if (!mutex_) return false;
    Lock lock(mutex_);
    std::memcpy(url, config_url_, sizeof(url));
    return mailbox_.busy();
}
bool WebServer::takeSettings(web::SettingsWrite &settings) {
    if (!mutex_) return false;
    Lock lock(mutex_);
    settings=settings_;
    settings_=web::SettingsWrite{}; // Do not retain passwords after main takes ownership.
    return mailbox_.busy();
}
void WebServer::complete(unsigned id, const char *result) {
    if (!mutex_) return;
    Lock lock(mutex_);
    if (id != command_id_) return;
    web::copyText(command_result_, result);
    settings_=web::SettingsWrite{};
    mailbox_.complete();
}
bool WebServer::authorize(httpd_req_t *request) {
    char ip[16];
    { Lock lock(mutex_); std::memcpy(ip, snapshot_.ip, sizeof(ip)); }
    if (!web::sameDevice(requestHeader(request, "Host"), requestHeader(request, "Origin"), ip)) {
        reply(request, "403 Forbidden", "{\"error\":\"origin_or_host\"}"); return false;
    }
    const auto credential=requestHeader(request,"Authorization");
    bool authenticated=false, exhausted=false;
    { Lock lock(mutex_);
      const bool active=session_code_[0] && esp_timer_get_time()<session_deadline_;
      authenticated=active && web::authenticated(credential,session_code_);
      // Missing headers do not consume attempts; submitted guesses share one
      // device-wide budget across routes. Successful polling never resets it.
      if(active && !authenticated && !credential.empty() && ++failed_pin_attempts_>=5) {
          exhausted=true; session_code_[0]=0; session_deadline_=0;
          mailbox_=web::Mailbox{}; settings_=web::SettingsWrite{};
      }
    }
    if (!authenticated) {
        reply(request,"401 Unauthorized",exhausted ? "{\"error\":\"pin_attempts_exhausted\"}" : "{\"error\":\"physical_pairing_required\"}"); return false;
    }
    return true;
}
bool WebServer::downloadBusy() {
    if(!mutex_) return false;
    Lock lock(mutex_); return download_active_;
}
bool WebServer::takeDownload(char (&id)[97], bool &markdown) {
    if(!mutex_) return false;
    Lock lock(mutex_);
    if(!download_active_) return false;
    std::memcpy(id,download_id_,sizeof id); markdown=download_markdown_; return true;
}
void WebServer::provideDownload(unsigned lease, FILE *file, uint64_t bytes) {
    if(!mutex_) { if(file) std::fclose(file); return; }
    Lock lock(mutex_);
    if(!download_active_ || lease!=download_lease_) { if(file) std::fclose(file); return; }
    download_file_=file; download_bytes_=bytes; download_ready_=true;
}
esp_err_t WebServer::notes(httpd_req_t *request) {
    auto &self=*static_cast<WebServer *>(request->user_ctx);
    if(!self.authorize(request)) return ESP_OK;
    std::unique_ptr<WebStatus> snapshot(new(std::nothrow) WebStatus);
    if(!snapshot) return reply(request,"503 Service Unavailable","{\"error\":\"memory\"}");
    { Lock lock(self.mutex_); *snapshot=self.snapshot_; }
    cJSON *root=cJSON_CreateObject();
    cJSON *list=root ? cJSON_AddArrayToObject(root,"notes") : nullptr;
    bool ok=list;
    for(size_t i=0;ok && i<std::min(snapshot->notes_count,size_t{40});++i) {
        auto *note=cJSON_CreateObject();
        const auto &item=snapshot->notes[i];
        ok=note && cJSON_AddStringToObject(note,"id",item.id) &&
            cJSON_AddBoolToObject(note,"audio",item.audio) && cJSON_AddBoolToObject(note,"transcribed",item.transcribed);
        if(ok) cJSON_AddItemToArray(list,note); else cJSON_Delete(note);
    }
    char *body=ok ? cJSON_PrintUnformatted(root) : nullptr; cJSON_Delete(root);
    if(!body) return reply(request,"503 Service Unavailable","{\"error\":\"memory\"}");
    const auto sent=reply(request,"200 OK",body); cJSON_free(body); return sent;
}
esp_err_t WebServer::download(httpd_req_t *request) {
    auto &self=*static_cast<WebServer *>(request->user_ctx);
    if(!self.authorize(request)) return ESP_OK;
    const std::string uri=request->uri;
    const bool markdown=uri.rfind("/api/download/markdown?",0)==0;
    const std::string prefix=markdown ? "/api/download/markdown?id=" : "/api/download/audio?id=";
    const std::string note=uri.rfind(prefix,0)==0 ? uri.substr(prefix.size()) : "";
    if(!web::validNoteId(note)) return reply(request,"400 Bad Request","{\"error\":\"invalid_id\"}");
    unsigned lease=0;
    { Lock lock(self.mutex_); const auto &s=self.snapshot_;
      if(!self.download_active_ && s.mounted && web::allowed(web::Command::Download,s.state,s.stopping,s.recovery,s.wifi,s.pending,s.connecting))
          lease=self.mailbox_.submit(web::Command::Download);
      if(lease) {
          self.download_active_=true; self.download_ready_=false; self.download_file_=nullptr;
          self.download_markdown_=markdown; self.download_lease_=lease; web::copyText(self.download_id_,note);
          self.command_id_=lease; web::copyText(self.command_result_,"pending");
      }
    }
    if(!lease) return reply(request,"409 Conflict","{\"error\":\"busy_or_unavailable\"}");
    FILE *file=nullptr; uint64_t bytes=0; bool ready=false;
    for(unsigned attempt=0;attempt<80;++attempt) {
        { Lock lock(self.mutex_);
          ready=self.download_ready_;
          if(ready) { file=self.download_file_; self.download_file_=nullptr; bytes=self.download_bytes_; }
          if(ready || !self.session_code_[0] || esp_timer_get_time()>=self.session_deadline_ ||
              std::strcmp(self.command_result_,"pending")) break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    // Closing the FILE happens before releasing the exclusion lease, including aborts.
    const auto finish=[&](const char *result) {
        if(file) { std::fclose(file); file=nullptr; }
        Lock lock(self.mutex_);
        if(self.download_lease_==lease) {
            if(self.download_file_) { std::fclose(self.download_file_); self.download_file_=nullptr; }
            self.download_active_=self.download_ready_=false; self.download_lease_=0;
            self.mailbox_.complete(); web::copyText(self.command_result_,result);
        }
    };
    if(!ready || !file) { finish("failed"); return reply(request,ready ? "404 Not Found" : "409 Conflict","{\"error\":\"download_unavailable\"}"); }
    responseHeaders(request); httpd_resp_set_type(request,markdown ? "text/markdown; charset=utf-8" : "audio/wav");
    const std::string filename="attachment; filename=\""+note+(markdown ? ".md\"" : ".wav\"");
    httpd_resp_set_hdr(request,"Content-Disposition",filename.c_str());
    const auto credential=requestHeader(request,"Authorization");
    char buffer[2048]; uint64_t remaining=bytes;
    while(remaining) {
        bool authorized=false;
        { Lock lock(self.mutex_); authorized=esp_timer_get_time()<self.session_deadline_ && web::authenticated(credential,self.session_code_); }
        if(!authorized) { finish("failed"); return ESP_FAIL; }
        const size_t count=static_cast<size_t>(std::min(remaining,static_cast<uint64_t>(sizeof buffer)));
        if(std::fread(buffer,1,count,file)!=count || httpd_resp_send_chunk(request,buffer,count)!=ESP_OK) {
            finish("failed"); return ESP_FAIL;
        }
        remaining-=count;
    }
    const auto sent=httpd_resp_send_chunk(request,nullptr,0); finish(sent==ESP_OK ? "ok" : "failed"); return sent;
}

esp_err_t WebServer::closeSession(httpd_req_t *request) {
    auto &self=*static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    if (request->content_len) return reply(request,"400 Bad Request","{\"error\":\"body_not_allowed\"}");
    self.revokeLocalSession();
    return reply(request,"200 OK","{\"status\":\"closed\"}");
}
esp_err_t WebServer::asset(httpd_req_t *request) {
    const unsigned char *begin = html_begin, *end = html_end;
    const char *type = "text/html; charset=utf-8";
    if (!std::strcmp(request->uri, "/style.css")) { begin = css_begin; end = css_end; type = "text/css; charset=utf-8"; }
    else if (!std::strcmp(request->uri, "/tokens.css")) { begin = tokens_begin; end = tokens_end; type = "text/css; charset=utf-8"; }
    else if (!std::strcmp(request->uri, "/app.js")) { begin = script_begin; end = script_end; type = "text/javascript; charset=utf-8"; }
    responseHeaders(request);
    httpd_resp_set_type(request, type);
    return httpd_resp_send(request, reinterpret_cast<const char *>(begin), end - begin);
}
esp_err_t WebServer::status(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    // The bounded transcript snapshot belongs on the heap, not a 6 KiB task stack.
    std::unique_ptr<WebStatus> snapshot(new (std::nothrow) WebStatus);
    if (!snapshot) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    unsigned id; bool busy; char result[16];
    {
        Lock lock(self.mutex_);
        *snapshot = self.snapshot_;
        id = self.command_id_; busy = self.mailbox_.busy();
        std::memcpy(result, self.command_result_, sizeof(result));
    }
    cJSON *json = cJSON_CreateObject();
    if (!json) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    const auto &s = *snapshot;
    const bool complete =
        cJSON_AddStringToObject(json, "state", stateText(s.state)) &&
        cJSON_AddBoolToObject(json, "wifi", s.wifi) &&
        cJSON_AddStringToObject(json, "ip", s.ip) &&
        cJSON_AddBoolToObject(json, "mounted", s.mounted) &&
        cJSON_AddBoolToObject(json, "audio_ready", s.audio_ready) &&
        cJSON_AddStringToObject(json, "storage_error", s.storage_error) &&
        cJSON_AddNumberToObject(json, "pending", s.pending) &&
        cJSON_AddBoolToObject(json, "stopping", s.stopping) &&
        cJSON_AddBoolToObject(json, "recovery", s.recovery) &&
        cJSON_AddBoolToObject(json, "connecting", s.connecting) &&
        cJSON_AddStringToObject(json, "note_id", s.note_id) &&
        cJSON_AddStringToObject(json, "text", s.text) &&
        cJSON_AddStringToObject(json, "live_text", s.live_text) &&
        cJSON_AddNumberToObject(json, "command_id", id) &&
        cJSON_AddStringToObject(json, "command_result", result) &&
        cJSON_AddBoolToObject(json, "command_busy", busy);
    char *body = complete ? cJSON_PrintUnformatted(json) : nullptr;
    cJSON_Delete(json);
    if (!body) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    const esp_err_t sent = reply(request, "200 OK", body);
    cJSON_free(body);
    return sent;
}
esp_err_t WebServer::serverConfig(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    // URI handler user context is the same for GET/POST; method distinguishes them.
    if (request->method == HTTP_GET) {
        char url[64], result[16]; unsigned id, revision; bool busy, configured;
        { Lock lock(self.mutex_);
          std::memcpy(url, self.snapshot_.base_url, sizeof(url));
          std::memcpy(result, self.command_result_, sizeof(result));
          id = self.command_id_; busy = self.mailbox_.busy(); configured=self.snapshot_.token_configured; revision=self.snapshot_.server_revision; }
        cJSON *json = cJSON_CreateObject();
        if (!json) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
        const bool ok = cJSON_AddStringToObject(json, "base_url", url) &&
            cJSON_AddBoolToObject(json,"token_configured",configured) &&
            cJSON_AddNumberToObject(json,"server_revision",revision) &&
            cJSON_AddNumberToObject(json, "command_id", id) &&
            cJSON_AddStringToObject(json, "command_result", result) &&
            cJSON_AddBoolToObject(json, "command_busy", busy);
        char *body = ok ? cJSON_PrintUnformatted(json) : nullptr; cJSON_Delete(json);
        if (!body) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
        const auto sent = reply(request, "200 OK", body); cJSON_free(body); return sent;
    }
    if (request->content_len > 512) return reply(request, "413 Payload Too Large", "{\"error\":\"oversize\"}");
    if (!request->content_len || requestHeader(request, "Content-Type") != "application/json")
        return reply(request, "400 Bad Request", "{\"error\":\"json_required\"}");
    char body[513]{}; size_t received = 0;
    while (received < request->content_len) {
        const int n = httpd_req_recv(request, body + received, request->content_len - received);
        if (n <= 0) return reply(request, "400 Bad Request", "{\"error\":\"incomplete_body\"}");
        received += static_cast<size_t>(n);
    }
    // Reject raw/escaped controls and NUL; permit printable token quotes/backslashes.
    for (size_t i = 0; i < received; ++i) {
        if (static_cast<unsigned char>(body[i]) < 32)
            return reply(request, "400 Bad Request", "{\"error\":\"invalid_url\"}");
        if (body[i]=='\\' && (++i>=received || (body[i]!='\\' && body[i]!='"')))
            return reply(request, "400 Bad Request", "{\"error\":\"invalid_url\"}");
    }
    cJSON *json = cJSON_ParseWithLengthOpts(body, received + 1, nullptr, true);
    cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "base_url") : nullptr;
    char ip[16]; { Lock lock(self.mutex_); std::memcpy(ip, self.snapshot_.ip, sizeof(ip)); }
    auto *token=json ? cJSON_GetObjectItemCaseSensitive(json,"token") : nullptr;
    auto *clear=json ? cJSON_GetObjectItemCaseSensitive(json,"clear_token") : nullptr;
    const int fields=1+(token ? 1:0)+(clear ? 1:0);
    const bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == fields &&
        cJSON_IsString(item) && item->valuestring && validServerUrl(item->valuestring, ip) &&
        (!token || (cJSON_IsString(token) && token->valuestring && web::validServerToken(token->valuestring))) &&
        (!clear || cJSON_IsBool(clear)) && !(clear && cJSON_IsTrue(clear) && token && token->valuestring[0]);
    web::SettingsWrite write;
    if (valid) {
        write.replace_token=(token && token->valuestring[0]) || (clear && cJSON_IsTrue(clear));
        if (token) web::copyText(write.server_token,token->valuestring);
    }
    char url[64]{}; if (valid) web::copyText(url, item->valuestring); cJSON_Delete(json);
    if (!valid) return reply(request, "400 Bad Request", "{\"error\":\"invalid_url\"}");
    unsigned id = 0;
    { Lock lock(self.mutex_); const auto &s = self.snapshot_;
      if (web::allowed(web::Command::Configure, s.state, s.stopping, s.recovery, s.wifi, s.pending, s.connecting))
          id = self.mailbox_.submit(web::Command::Configure);
      if (id) { self.settings_=write; std::memcpy(self.config_url_, url, sizeof(url)); self.command_id_ = id; web::copyText(self.command_result_, "pending"); } }
    if (!id) return reply(request, "409 Conflict", "{\"error\":\"busy_or_unavailable\"}");
    char response[64]; std::snprintf(response, sizeof(response), "{\"id\":%u,\"status\":\"queued\"}", id);
    return reply(request, "202 Accepted", response);
}

esp_err_t WebServer::settings(httpd_req_t *request) {
    auto &self=*static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    if (request->method==HTTP_GET) {
        std::unique_ptr<WebStatus> s(new (std::nothrow) WebStatus);
        if (!s) return reply(request,"503 Service Unavailable","{\"error\":\"memory\"}");
        unsigned id; bool busy; char result[16];
        { Lock lock(self.mutex_); *s=self.snapshot_; id=self.command_id_; busy=self.mailbox_.busy(); std::memcpy(result,self.command_result_,sizeof result); }
        cJSON *json=cJSON_CreateObject();
        if (!json) return reply(request,"503 Service Unavailable","{\"error\":\"memory\"}");
        bool ok=cJSON_AddStringToObject(json,"home_ssid",s->ssid[0]) &&
            cJSON_AddStringToObject(json,"hotspot_ssid",s->ssid[1]) &&
            cJSON_AddStringToObject(json,"current_ssid",s->current_ssid) &&
            cJSON_AddStringToObject(json,"ip",s->ip) &&
            cJSON_AddBoolToObject(json,"mounted",s->mounted) &&
            cJSON_AddBoolToObject(json,"usage_known",s->usage_known) &&
            cJSON_AddNumberToObject(json,"total_bytes",s->total_bytes) &&
            cJSON_AddNumberToObject(json,"free_bytes",s->free_bytes) &&
            cJSON_AddBoolToObject(json,"audio_ready",s->audio_ready) &&
            cJSON_AddStringToObject(json,"storage_error",s->storage_error) &&
            cJSON_AddNumberToObject(json,"partial_limit",s->partial_limit) &&
            cJSON_AddNumberToObject(json,"format_challenge",s->format_challenge) &&
            cJSON_AddNumberToObject(json,"command_id",id) &&
            cJSON_AddStringToObject(json,"command_result",result) &&
            cJSON_AddBoolToObject(json,"command_busy",busy);
        char *body=ok ? cJSON_PrintUnformatted(json) : nullptr; cJSON_Delete(json);
        if (!body) return reply(request,"503 Service Unavailable","{\"error\":\"memory\"}");
        auto sent=reply(request,"200 OK",body); cJSON_free(body); return sent;
    }
    if (request->content_len>768) return reply(request,"413 Payload Too Large","{\"error\":\"oversize\"}");
    if (!request->content_len || requestHeader(request,"Content-Type")!="application/json")
        return reply(request,"400 Bad Request","{\"error\":\"json_required\"}");
    char body[769]{}; size_t received=0;
    while (received<request->content_len) {
        int n=httpd_req_recv(request,body+received,request->content_len-received);
        if (n<=0) return reply(request,"400 Bad Request","{\"error\":\"incomplete_body\"}");
        received+=n;
    }
    // No escaped controls/NUL: permit only escaped quote/backslash. UTF-8 SSIDs are literal.
    for (size_t i=0;i<received;++i) {
        if (static_cast<unsigned char>(body[i])<32) return reply(request,"400 Bad Request","{\"error\":\"invalid_settings\"}");
        if (body[i]=='\\') { if (++i>=received || (body[i]!='\\' && body[i]!='"')) return reply(request,"400 Bad Request","{\"error\":\"invalid_settings\"}"); }
    }
    cJSON *json=cJSON_ParseWithLengthOpts(body,received+1,nullptr,true);
    auto field=[](cJSON *o,const char *key) { return cJSON_GetObjectItemCaseSensitive(o,key); };
    auto string=[](cJSON *v,size_t max,bool ascii) {
        if (!cJSON_IsString(v) || !v->valuestring || std::strlen(v->valuestring)>max) return false;
        for (const unsigned char *p=reinterpret_cast<const unsigned char *>(v->valuestring);*p;++p)
            if (*p<32 || *p==127 || (ascii && *p>126)) return false;
        return true;
    };
    auto integer=[](cJSON *v,double max) { return cJSON_IsNumber(v) && v->valuedouble>=0 && v->valuedouble<=max && v->valuedouble==static_cast<unsigned>(v->valuedouble); };
    auto *action=field(json,"action");
    bool valid=cJSON_IsObject(json) && string(action,20,true);
    web::Command command=web::Command::Mount; web::SettingsWrite write;
    const char *name=valid ? action->valuestring : "";
    if (!std::strcmp(name,"wifi")) {
        command=web::Command::Wifi;
        auto *profiles=field(json,"profiles");
        valid=valid && cJSON_GetArraySize(json)==2 && cJSON_IsArray(profiles) && cJSON_GetArraySize(profiles)==2;
        bool any=false;
        for (int i=0;valid && i<2;++i) {
            auto *p=cJSON_GetArrayItem(profiles,i), *ssid=field(p,"ssid"), *password=field(p,"password"), *open=field(p,"open");
            valid=cJSON_IsObject(p) && cJSON_GetArraySize(p)==3 && string(ssid,32,false) && string(password,63,true) && cJSON_IsBool(open);
            if (!valid) break;
            const size_t length=std::strlen(password->valuestring);
            valid=(!length || length>=8) && (!cJSON_IsTrue(open) || !length) && (ssid->valuestring[0] || !length);
            web::copyText(write.ssid[i],ssid->valuestring); web::copyText(write.password[i],password->valuestring); write.open[i]=cJSON_IsTrue(open);
            any |= ssid->valuestring[0]!=0;
        }
        valid=valid && any;
    } else if (!std::strcmp(name,"display")) {
        command=web::Command::Display; auto *limit=field(json,"partial_limit");
        valid=valid && cJSON_GetArraySize(json)==2 && integer(limit,100);
        if (valid) { write.partial_limit=static_cast<unsigned>(limit->valuedouble); valid=write.partial_limit==0 || write.partial_limit==1 || write.partial_limit==5 || write.partial_limit==10 || write.partial_limit==20 || write.partial_limit==50 || write.partial_limit==100; }
    } else if (!std::strcmp(name,"format")) {
        command=web::Command::Format; auto *challenge=field(json,"challenge");
        valid=valid && cJSON_GetArraySize(json)==2 && integer(challenge,4294967295.0) && challenge->valuedouble>0;
        if (valid) write.challenge=static_cast<unsigned>(challenge->valuedouble);
    } else {
        valid=valid && cJSON_GetArraySize(json)==1;
        if (!std::strcmp(name,"mount")) command=web::Command::Mount;
        else if (!std::strcmp(name,"prepare_format")) command=web::Command::PrepareFormat;
        else if (!std::strcmp(name,"reconnect")) command=web::Command::Reconnect;
        else valid=false;
    }
    cJSON_Delete(json);
    if (!valid) return reply(request,"400 Bad Request","{\"error\":\"invalid_settings\"}");
    unsigned id=0;
    { Lock lock(self.mutex_); const auto &s=self.snapshot_;
      if (web::allowed(command,s.state,s.stopping,s.recovery,s.wifi,s.pending,s.connecting)) id=self.mailbox_.submit(command);
      if (id) { self.settings_=write; self.command_id_=id; web::copyText(self.command_result_,"pending"); } }
    if (!id) return reply(request,"409 Conflict","{\"error\":\"busy_or_unavailable\"}");
    char response[64]; std::snprintf(response,sizeof response,"{\"id\":%u,\"status\":\"queued\"}",id);
    return reply(request,"202 Accepted",response);
}

esp_err_t WebServer::command(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    if (request->content_len != 0) return reply(request, "413 Payload Too Large", "{\"error\":\"body_not_allowed\"}");
    web::Command command = web::Command::Start;
    if (!std::strcmp(request->uri, "/api/command/stop")) command = web::Command::Stop;
    else if (!std::strcmp(request->uri, "/api/command/sync")) command = web::Command::Sync;
    unsigned id = 0;
    {
        Lock lock(self.mutex_);
        const auto &s = self.snapshot_;
        if (web::allowed(command, s.state, s.stopping, s.recovery, s.wifi, s.pending, s.connecting)) id = self.mailbox_.submit(command);
        if (id) { self.command_id_ = id; web::copyText(self.command_result_, "pending"); }
    }
    if (!id) return reply(request, "409 Conflict", "{\"error\":\"busy_or_unavailable\"}");
    char json[64]; std::snprintf(json, sizeof(json), "{\"id\":%u,\"status\":\"queued\"}", id);
    return reply(request, "202 Accepted", json);
}
} // namespace network
