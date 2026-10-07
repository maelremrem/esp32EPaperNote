#include <cassert>
#include <cstdio>
#include <thread>
#include "network/web_server.h"
#include "cJSON.h"

httpd_req_t request(const char *uri, httpd_method_t method = HTTP_GET,
                    const char *auth = "Bearer public-test-token", const char *origin = "", size_t body = 0,
                    const char *host = "192.168.1.4") {
    httpd_req_t req; req.uri = uri; req.content_len = body; req.method=method;
    req.headers = {{"Host",host},{"Authorization",auth},{"Origin",origin}};
    for (const auto &route : routes) if (std::string(route.uri) == uri && route.method == method) {
        req.user_ctx = route.user_ctx; assert(route.handler(&req) == ESP_OK); return req;
    }
    assert(false && "missing route"); return req;
}
httpd_req_t configPost(const std::string &body) {
    httpd_req_t req; req.method=HTTP_POST; req.uri="/api/config/server"; req.content_len=body.size(); req.input=body;
    req.headers={{"Host","192.168.1.4"},{"Authorization","Bearer public-test-token"},{"Content-Type","application/json"}};
    for(auto &r:routes) if(std::string(r.uri)==req.uri && r.method==HTTP_POST) { req.user_ctx=r.user_ctx; r.handler(&req); return req; }
    assert(false && "missing config route"); return req;
}
int main() {
    network::WebServer server; assert(server.start()); assert(server.start()); assert(routes.size() == 10);
    network::WebStatus s; s.wifi=true; s.pending=2;
    network::web::copyText(s.ip,"192.168.1.4"); network::web::copyText(s.text,"line\n\"é\" <script>");
    server.publish(s);
    for (const char *path : {"/","/app.js","/style.css","/tokens.css"}) {
        auto r = request(path); assert(!r.body.empty()); assert(r.response_headers["X-Content-Type-Options"] == "nosniff");
        assert(r.body.find("public-test-token") == std::string::npos);
    }
    assert(request("/api/status",HTTP_GET,"").code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,"Bearer wrong").code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,"Bearer public-test-token","http://evil.test").code == "403 Forbidden");
    assert(request("/api/status",HTTP_GET,"Bearer public-test-token","",0,"evil.test").code == "403 Forbidden");
    assert(request("/api/status",HTTP_GET,std::string(256,'x').c_str()).code == "401 Unauthorized");
    assert(request("/api/status",HTTP_GET,"Bearer public-test-token","",0,std::string(256,'x').c_str()).code == "403 Forbidden");
    auto status = request("/api/status");
    assert(status.body.find("public-test-token") == std::string::npos);
    cJSON *json=cJSON_Parse(status.body.c_str()); assert(json);
    assert(std::string(cJSON_GetObjectItem(json,"text")->valuestring)==s.text); cJSON_Delete(json);
    assert(request("/api/command/start",HTTP_POST,"Bearer public-test-token","null").code == "403 Forbidden");
    assert(request("/api/command/start",HTTP_POST,"Bearer public-test-token","",1).code == "413 Payload Too Large");
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
    assert(request("/api/command/sync",HTTP_POST).code == "409 Conflict");
    network::web::copyText(s.base_url,"http://192.168.1.20:8000"); server.publish(s);
    auto config=request("/api/config/server"); assert(config.code=="200 OK");
    assert(config.response_headers["Cache-Control"]=="no-store");
    assert(config.body.find(s.base_url)!=std::string::npos && config.body.find("public-test-token")==std::string::npos);
    assert(request("/api/config/server",HTTP_GET,"").code=="401 Unauthorized");
    assert(request("/api/config/server",HTTP_POST,"Bearer public-test-token","http://evil.test").code=="403 Forbidden");
    for(auto body:{"{}", "bad", "{\"base_url\":42}", "{\"base_url\":\"http://127.0.0.1\"}", "{\"base_url\":\"http://192.168.1.4\"}", "{\"base_url\":\"http://192.168.1.20/path\"}", "{\"base_url\":\"http://192.168.1.20\\u0000junk\"}", "{\"base_url\":\"http://192.168.1.20\",\"extra\":1}", "{\"base_url\":\"http://192.168.1.20\"} junk"}) assert(configPost(body).code=="400 Bad Request");
    assert(configPost(std::string(257,'x')).code=="413 Payload Too Large");
    const std::string payload="{\"base_url\":\"https://192.168.1.21:8443\"}";
    s.state=network::web::State::Recording; server.publish(s); assert(configPost(payload).code=="409 Conflict");
    s.state=network::web::State::Idle; server.publish(s);
    assert(configPost(payload).code=="202 Accepted");
    assert(configPost(payload).code=="409 Conflict");
    assert(server.take(command,id) && command==network::web::Command::Configure);
    char url[64]{}; assert(server.takeConfig(url)); assert(std::string(url)=="https://192.168.1.21:8443");
    assert(request("/api/config/server").body.find(s.base_url)!=std::string::npos); // queued != applied
    server.complete(id,"failed"); assert(request("/api/config/server").body.find("failed")!=std::string::npos);
    // Read and publish on separate tasks; every bounded snapshot is internally consistent.
    std::thread writer([&] { for(int i=0;i<300;++i) {s.pending=i; server.publish(s);} });
    for(int i=0;i<300;++i) {auto r=request("/api/status"); assert(r.code=="200 OK"); json=cJSON_Parse(r.body.c_str()); assert(json); cJSON_Delete(json);}
    writer.join();
    puts("real web HTTP handlers: auth/Origin/Host/assets/JSON/queue/results/concurrent snapshot PASS");
}
