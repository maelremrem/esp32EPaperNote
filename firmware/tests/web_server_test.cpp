#include <cassert>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <cerrno>
#include "network/web_server.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
std::string local_auth;

httpd_req_t request(const char *uri, httpd_method_t method = HTTP_GET,
                    const char *auth = nullptr, const char *origin = "", size_t body = 0,
                    const char *host = "192.168.1.4", bool expect_failure=false) {
    httpd_req_t req; req.uri = uri; req.content_len = body; req.method=method;
    req.headers = {{"Host",host},{"Authorization",auth ? auth : local_auth},{"Origin",origin}};
    for (const auto &route : routes) if (std::string(route.uri) == std::string(uri).substr(0,std::string(uri).find('?')) && route.method == method) {
        req.user_ctx = route.user_ctx; assert(route.handler(&req) == (expect_failure ? ESP_FAIL : ESP_OK)); return req;
    }
    assert(false && "missing route"); return req;
}
httpd_req_t configPost(const std::string &body, const char *path="/api/config/server") {
    httpd_req_t req; req.method=HTTP_POST; req.uri=path; req.content_len=body.size(); req.input=body;
    req.headers={{"Host","192.168.1.4"},{"Authorization",local_auth},{"Content-Type","application/json"}};
    for(auto &r:routes) if(std::string(r.uri)==req.uri && r.method==HTTP_POST) { req.user_ctx=r.user_ctx; r.handler(&req); return req; }
    assert(false && "missing config route"); return req;
}
int main() {
    network::WebServer server; assert(server.start()); assert(server.start()); assert(routes.size() == 16);
    network::WebStatus s; s.wifi=true; s.pending=2;
    network::web::copyText(s.ip,"192.168.1.4"); network::web::copyText(s.text,"line\n\"é\" <script>");
    server.publish(s);
    assert(request("/api/status",HTTP_GET,"Bearer public-test-token").code=="401 Unauthorized");
    mock_web_random=0;
    local_auth="Bearer " + server.openLocalSession();
    assert(local_auth=="Bearer 000001");
    for (const char *path : {"/","/app.js","/style.css","/tokens.css"}) {
        auto r = request(path); assert(!r.body.empty()); assert(r.response_headers["X-Content-Type-Options"] == "nosniff");
        assert(r.body.find("public-test-token") == std::string::npos);
    }
    assert(request("/api/status",HTTP_GET,"").code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,"Bearer wrong").code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,local_auth.c_str(),"http://evil.test").code == "403 Forbidden");
    assert(request("/api/status",HTTP_GET,local_auth.c_str(),"",0,"evil.test").code == "403 Forbidden");
    assert(request("/api/status",HTTP_GET,std::string(256,'x').c_str()).code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,local_auth.c_str(),"",0,std::string(256,'x').c_str()).code == "403 Forbidden");
    auto status = request("/api/status");
    assert(status.body.find("public-test-token") == std::string::npos);
    cJSON *json=cJSON_Parse(status.body.c_str()); assert(json);
    assert(std::string(cJSON_GetObjectItem(json,"text")->valuestring)==s.text); cJSON_Delete(json);
    assert(request("/api/command/start",HTTP_POST,local_auth.c_str(),"null").code == "403 Forbidden");
    assert(request("/api/command/start",HTTP_POST,local_auth.c_str(),"",1).code == "413 Payload Too Large");
    auto queued=request("/api/command/start",HTTP_POST); assert(queued.code == "202 Accepted");
    assert(queued.body.find("queued") != std::string::npos);
    assert(request("/api/command/start",HTTP_POST).code == "409 Conflict");
    network::web::Command command; unsigned id=0; assert(server.take(command,id)); assert(id==1);
    assert(command==network::web::Command::Start); assert(!server.take(command,id));
    assert(request("/api/command/sync",HTTP_POST).code == "409 Conflict");
    server.complete(99,"ok"); assert(request("/api/command/start",HTTP_POST).code == "409 Conflict");
    server.complete(1,"rejected"); assert(request("/api/status").body.find("rejected")!=std::string::npos);
    s.state=network::web::State::Recording; server.publish(s);
    assert(request("/api/command/start",HTTP_POST).code == "409 Conflict");
    assert(request("/api/command/sync",HTTP_POST).code == "409 Conflict");
    assert(request("/api/command/stop",HTTP_POST).code == "202 Accepted");
    assert(server.take(command,id)); assert(command==network::web::Command::Stop);
    s.stopping=true; server.publish(s); server.complete(id,"stopping");
    assert(request("/api/command/stop",HTTP_POST).code == "409 Conflict");
    s.state=network::web::State::Idle; s.stopping=false; s.recovery=true; server.publish(s);
    assert(request("/api/command/start",HTTP_POST).code == "409 Conflict");
    assert(request("/api/command/sync",HTTP_POST).code == "202 Accepted");
    assert(server.take(command,id)); server.complete(id,"failed");
    assert(request("/api/status").body.find("failed")!=std::string::npos);
    s.wifi=false; server.publish(s);
    assert(request("/api/command/sync",HTTP_POST).code == "401 Unauthorized");
    s.wifi=true; server.publish(s); local_auth="Bearer "+server.openLocalSession();
    network::web::copyText(s.base_url,"http://192.168.1.20:8000"); server.publish(s);
    auto config=request("/api/config/server"); assert(config.code=="200 OK");
    assert(config.response_headers["Cache-Control"]=="no-store");
    assert(config.body.find(s.base_url)!=std::string::npos && config.body.find("public-test-token")==std::string::npos);
    assert(request("/api/config/server",HTTP_GET,"").code=="401 Unauthorized");
    assert(request("/api/config/server",HTTP_POST,local_auth.c_str(),"http://evil.test").code=="403 Forbidden");
    for(auto body:{"{}", "bad", "{\"base_url\":42}", "{\"base_url\":\"http://127.0.0.1\"}", "{\"base_url\":\"http://192.168.1.4\"}", "{\"base_url\":\"http://192.168.1.20/path\"}", "{\"base_url\":\"http://192.168.1.20\\u0000junk\"}", "{\"base_url\":\"http://192.168.1.20\",\"extra\":1}", "{\"base_url\":\"http://192.168.1.20\"} junk"}) assert(configPost(body).code=="400 Bad Request");
    assert(configPost(std::string(513,'x')).code=="413 Payload Too Large");
    assert(configPost("{\"base_url\":\"http://192.168.1.21:8080\",\"token\":\"runtime-only\"}").code=="202 Accepted");
    assert(server.take(command,id)); network::web::SettingsWrite target;
    assert(server.takeSettings(target) && target.replace_token && std::string(target.server_token)=="runtime-only");
    server.complete(id,"ok");
    assert(configPost(R"({"base_url":"http://192.168.1.21:8080","token":"quoted\"back\\slash"})").code=="202 Accepted");
    assert(server.take(command,id) && server.takeSettings(target));
    assert(std::string(target.server_token)=="quoted\"back\\slash"); server.complete(id,"ok");
    assert(request("/api/config/server").body.find("runtime-only")==std::string::npos);
    assert(configPost("{\"base_url\":\"http://192.168.1.21\",\"token\":\"bad\\r\\nheader\"}").code=="400 Bad Request");
    const std::string payload="{\"base_url\":\"https://192.168.1.21:8443\"}";
    s.state=network::web::State::Recording; server.publish(s); assert(configPost(payload).code=="409 Conflict");
    s.state=network::web::State::Idle; server.publish(s);
    assert(configPost(payload).code=="202 Accepted");
    assert(configPost(payload).code=="409 Conflict");
    assert(server.take(command,id) && command==network::web::Command::Configure);
    char url[64]{}; assert(server.takeConfig(url)); assert(std::string(url)=="https://192.168.1.21:8443");
    assert(request("/api/config/server").body.find(s.base_url)!=std::string::npos); // queued != applied
    server.complete(id,"failed"); assert(request("/api/config/server").body.find("failed")!=std::string::npos);
    assert(request("/api/settings",HTTP_GET,"").code=="401 Unauthorized");
    assert(request("/api/settings",HTTP_POST,local_auth.c_str(),"http://evil.test").code=="403 Forbidden");
    s.usage_known=true; s.total_bytes=1000; s.free_bytes=500; server.publish(s);
    auto settings=request("/api/settings"); assert(settings.code=="200 OK");
    assert(settings.body.find("\"total_bytes\":1000")!=std::string::npos && settings.body.find("\"free_bytes\":500")!=std::string::npos);
    assert(settings.body.find("password")==std::string::npos);
    for (auto body:{"{}", "{\"action\":\"display\",\"partial_limit\":7}", "{\"action\":\"display\",\"partial_limit\":10.5}", "{\"action\":\"mount\",\"extra\":1}", "{\"action\":\"mount\",\"action\":\"mount\"}", "{\"action\":\"format\",\"challenge\":0}", "{\"action\":\"wifi\",\"profiles\":[]}"})
        assert(configPost(body,"/api/settings").code=="400 Bad Request");
    assert(configPost(std::string(769,'x'),"/api/settings").code=="413 Payload Too Large");
    assert(configPost("{\"action\":\"display\",\"partial_limit\":20}","/api/settings").code=="202 Accepted");
    assert(server.take(command,id) && command==network::web::Command::Display);
    network::web::SettingsWrite write; assert(server.takeSettings(write) && write.partial_limit==20);
    assert(configPost("{\"action\":\"mount\"}","/api/settings").code=="409 Conflict");
    server.complete(id,"ok");
    const char *wifi="{\"action\":\"wifi\",\"profiles\":[{\"ssid\":\"a\\\"b\",\"password\":\"synthetic123\",\"open\":false},{\"ssid\":\"\",\"password\":\"\",\"open\":false}]}";
    assert(configPost(wifi,"/api/settings").code=="202 Accepted");
    assert(server.take(command,id) && command==network::web::Command::Wifi);
    assert(server.takeSettings(write) && std::string(write.ssid[0])=="a\"b" && std::string(write.password[0])=="synthetic123");
    assert(request("/api/settings").body.find("synthetic123")==std::string::npos);
    server.complete(id,"ok");
    s.state=network::web::State::Recording; server.publish(s);
    assert(configPost("{\"action\":\"mount\"}","/api/settings").code=="409 Conflict");
    s.state=network::web::State::Idle; server.publish(s);
    assert(request("/api/notes").code=="200 OK");
    assert(request("/api/download/audio?id=../outside").code=="400 Bad Request");
    s.mounted=true; server.publish(s);
    const std::string wav(9000,'W');
    std::thread download_main([&] {
        network::web::Command c; unsigned lease;
        while(!server.take(c,lease)) std::this_thread::yield();
        assert(c==network::web::Command::Download);
        char note[97]{}; bool markdown=true; assert(server.takeDownload(note,markdown));
        assert(std::string(note)=="safe-note" && !markdown && server.downloadBusy());
        FILE *file=tmpfile(); assert(file); fwrite(wav.data(),1,wav.size(),file); rewind(file);
        server.provideDownload(lease,file,wav.size());
    });
    auto downloaded=request("/api/download/audio?id=safe-note"); download_main.join();
    assert(downloaded.body==wav && downloaded.type=="audio/wav");
    assert(downloaded.response_headers["Content-Disposition"]=="attachment; filename=\"safe-note.wav\"");
    assert(!server.downloadBusy() && largest_chunk<=2048);
    fail_chunk_after=1; chunk_calls=0; int closed_fd=-1;
    std::thread abort_main([&] {
        network::web::Command c; unsigned lease;
        while(!server.take(c,lease)) std::this_thread::yield();
        FILE *file=tmpfile(); assert(file); closed_fd=fileno(file); fwrite(wav.data(),1,wav.size(),file); rewind(file);
        server.provideDownload(lease,file,wav.size());
    });
    request("/api/download/audio?id=safe-note",HTTP_GET,nullptr,"",0,"192.168.1.4",true); abort_main.join();
    assert(!server.downloadBusy() && fcntl(closed_fd,F_GETFD)==-1 && errno==EBADF);
    fail_chunk_after=-1;
    std::thread missing_main([&] { network::web::Command c; unsigned lease; while(!server.take(c,lease)) std::this_thread::yield(); server.provideDownload(lease,nullptr,0); });
    assert(request("/api/download/markdown?id=missing").code=="404 Not Found"); missing_main.join();
    assert(!server.downloadBusy());
    // Read and publish on separate tasks; every bounded snapshot is internally consistent.
    std::thread writer([&] { for(int i=0;i<300;++i) {s.pending=i; server.publish(s);} });
    for(int i=0;i<300;++i) {auto r=request("/api/status"); assert(r.code=="200 OK"); json=cJSON_Parse(r.body.c_str()); assert(json); cJSON_Delete(json);}
    writer.join();
    assert(request("/api/session/close",HTTP_POST).code=="200 OK");
    assert(request("/api/status").code=="401 Unauthorized");
    local_auth="Bearer " + server.openLocalSession();
    const auto old_auth=local_auth;
    local_auth="Bearer " + server.openLocalSession();
    assert(request("/api/status",HTTP_GET,old_auth.c_str()).code=="401 Unauthorized");
    mock_web_now+=600000001;
    assert(request("/api/settings").code=="401 Unauthorized");
    // Wrong guesses are global across routes; valid requests never reset the budget.
    local_auth="Bearer " + server.openLocalSession();
    assert(local_auth.size()==13);
    s.state=network::web::State::Idle; s.recovery=false; s.connecting=false; server.publish(s);
    assert(request("/api/command/start",HTTP_POST).code=="202 Accepted");
    for(int attempt=0;attempt<4;++attempt) {
        assert(request("/api/settings",HTTP_GET,"Bearer 999999","http://evil.test").code=="403 Forbidden");
        assert(request(attempt%2 ? "/api/notes" : "/api/status",HTTP_GET,"Bearer 999999").code=="401 Unauthorized");
        assert(request("/api/status").code=="200 OK");
    }
    auto exhausted=request("/api/config/server",HTTP_GET,"Bearer 999999");
    assert(exhausted.code=="401 Unauthorized" && exhausted.body.find("pin_attempts_exhausted")!=std::string::npos);
    assert(request("/api/status").code=="401 Unauthorized");
    assert(!server.take(command,id)); // Guesses also invalidate queued writes.
    local_auth="Bearer " + server.openLocalSession();
    assert(request("/api/status").code=="200 OK");
    // Faulty/high RNG values must not bias output or spin indefinitely.
    mock_web_random=4293999999u;
    assert(server.openLocalSession().empty());
    assert(request("/api/status").code=="401 Unauthorized");
    puts("real web HTTP handlers: auth/Origin/Host/assets/JSON/queue/results/concurrent snapshot PASS");
}
