#include "network/captive_portal.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace network {
namespace {
constexpr size_t MAX_BODY = 1536;
std::string portalPassword() {
    // Eight characters is the WPA2 minimum. A 32-symbol alphabet gives an
    // unbiased five-bit choice per character without ambiguous I/O/0/1.
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static_assert(sizeof alphabet - 1 == 32, "Password alphabet must have 32 symbols");
    std::string password(8, 'A');
    for (char &c : password) c = alphabet[esp_random() & 31u];
    return password;
}
std::string hex(uint32_t n) {
    char b[9]; std::snprintf(b, sizeof b, "%08lx", static_cast<unsigned long>(n)); return b;
}
int unhex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decode(const std::string &value, std::string &out, size_t limit) {
    out.clear();
    for (size_t i=0; i<value.size(); ++i) {
        unsigned char c=value[i];
        if (c=='+') c=' ';
        else if (c=='%') {
            if (i+2>=value.size() || unhex(value[i+1])<0 || unhex(value[i+2])<0) return false;
            c=static_cast<unsigned char>(unhex(value[i+1])*16+unhex(value[i+2])); i+=2;
        }
        if (c<0x20 || c==0x7f || out.size()==limit) return false;
        out.push_back(c);
    }
    return true;
}
bool parseForm(const std::string &body, const std::string &token, WifiProfiles &profiles) {
    const char *names[]={"csrf","h","hp","s","sp","ho","so"};
    std::array<std::string,7> values;
    std::array<bool,7> found{};
    size_t start=0;
    while (start<body.size()) {
        const size_t end=body.find('&',start), stop=end==std::string::npos ? body.size() : end;
        const size_t equal=body.find('=',start);
        if (equal==std::string::npos || equal>=stop) return false;
        std::string name;
        if (!decode(body.substr(start,equal-start),name,4)) return false;
        size_t slot=0; while (slot<7 && name!=names[slot]) ++slot;
        if (slot==7 || found[slot]) return false;
        found[slot]=true;
        const size_t limit=(slot==1 || slot==3) ? 32 : 63;
        if (!decode(body.substr(equal+1,stop-equal-1),values[slot],limit)) return false;
        start=stop+1;
    }
    for (size_t i=0; i<5; ++i) if (!found[i]) return false;
    for (size_t i=5; i<7; ++i) if (found[i] && values[i]!="1") return false;
    for (size_t i=0; i<2; ++i) {
        const size_t ssid=1+i*2, password=ssid+1;
        if (found[5+i] && !values[password].empty()) return false;
        if (!values[ssid].empty() && values[password].empty() && !found[5+i]) return false;
    }
    if (values[0]!=token) return false;
    profiles={{{values[1],values[2]},{values[3],values[4]}}};
    bool any=false;
    for (const auto &p : profiles) {
        if (p.ssid.empty() && !p.password.empty()) return false;
        if (!p.password.empty() && (p.password.size()<8 || p.password.size()>63)) return false;
        for (unsigned char c : p.password) if (c>0x7e) return false;
        any=any || !p.ssid.empty();
    }
    return any;
}
esp_err_t reply(httpd_req_t *r, const char *status, const char *body) {
    httpd_resp_set_status(r,status);
    httpd_resp_set_type(r,"text/html; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'none'; form-action 'self'; frame-ancestors 'none'");
    return httpd_resp_send(r,body,HTTPD_RESP_USE_STRLEN);
}
std::string jsonString(const std::string &s) {
    std::string out="\"";
    for (unsigned char c : s) {
        if (c=='"' || c=='\\') { out+='\\'; out+=c; }
        else if (c<32) { char b[7]; std::snprintf(b,sizeof b,"\\u%04x",c); out+=b; }
        else out+=c;
    }
    return out+'"';
}
const char *scanName(WifiScanState s) {
    switch(s) {
    case WifiScanState::Queued:return "queued";case WifiScanState::Scanning:return "scanning";
    case WifiScanState::Complete:return "complete";case WifiScanState::Error:return "error";
    default:return "idle";
    }
}
bool readBody(httpd_req_t *r, std::string &body, size_t limit) {
    char type[64]{};
    if (r->content_len<=0 || static_cast<size_t>(r->content_len)>limit ||
        httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof type)!=ESP_OK ||
        std::strcmp(type,"application/x-www-form-urlencoded")) return false;
    body.assign(static_cast<size_t>(r->content_len),'\0'); size_t at=0;
    while (at<body.size()) {
        const int n=httpd_req_recv(r,&body[at],body.size()-at);
        if (n<=0 || static_cast<size_t>(n)>body.size()-at) return false;
        at+=n;
    }
    return true;
}
void clearProfiles(WifiProfiles &profiles) {
    for (auto &p : profiles) { std::fill(p.password.begin(),p.password.end(),'\0'); p.password.clear(); p.ssid.clear(); }
}
} // namespace

size_t captiveDnsReply(uint8_t *packet, size_t length, size_t capacity) {
    if (!packet || length<12 || length>capacity || packet[4]!=0 || packet[5]!=1 ||
        (packet[2]&0xf8) || packet[6] || packet[7] || packet[8] || packet[9]) return 0;
    size_t at=12, labels=0;
    while (at<length && packet[at]) {
        const unsigned n=packet[at++];
        if (n>63 || ++labels>32 || at+n>=length) return 0;
        at+=n;
    }
    if (at+5>length || at-12>253 || packet[at] || packet[at+1]!=0 || packet[at+2]!=1 ||
        packet[at+3]!=0 || packet[at+4]!=1) return 0;
    const size_t end=at+5; // Preserve complete QNAME, QTYPE and QCLASS.
    if (end+16>capacity) return 0;
    packet[2]=static_cast<uint8_t>(0x80 | (packet[2]&1)); packet[3]=0x80;
    packet[6]=0; packet[7]=1; packet[8]=packet[9]=packet[10]=packet[11]=0;
    constexpr uint8_t answer[]={0xc0,0x0c,0,1,0,1,0,0,0,30,0,4,192,168,4,1};
    std::memcpy(packet+end,answer,sizeof answer);
    return end+sizeof answer;
}

void CaptivePortal::dnsLoop(void *arg) {
    auto *p=static_cast<CaptivePortal *>(arg);
    uint8_t packet[512];
    while (p->active()) {
        sockaddr_in peer{}; socklen_t size=sizeof peer;
        const int n=recvfrom(p->dns_socket_,packet,sizeof packet,0,reinterpret_cast<sockaddr *>(&peer),&size);
        if (n<=0) continue;
        const size_t bytes=captiveDnsReply(packet,static_cast<size_t>(n),sizeof packet);
        if (bytes) sendto(p->dns_socket_,packet,bytes,0,reinterpret_cast<sockaddr *>(&peer),size);
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(p->dns_done_));
    vTaskDelete(nullptr); // Self-exit only; main joins via dns_done_ before releasing transport.
}

CaptivePortal::~CaptivePortal() {
    stop();
    if (mutex_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex_));
    if (dns_done_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(dns_done_));
}
bool CaptivePortal::init() {
    if (!mutex_) mutex_=xSemaphoreCreateMutex();
    if (!dns_done_) dns_done_=xSemaphoreCreateBinary();
    return mutex_ && dns_done_;
}
bool CaptivePortal::start(WifiManager &wifi) {
    if (!stop() || cleanup_failed_) return false;
    if (!init()) return false;
    ssid_="Whistle-"+hex(esp_random()).substr(2);
    password_=portalPassword();
    token_=hex(esp_random())+hex(esp_random())+hex(esp_random())+hex(esp_random());
    nonce_=hex(esp_random())+hex(esp_random());
    wifi_=&wifi;
    if (!wifi.startPortalWifi(ssid_.c_str(),password_.c_str())) { stop(); return false; }
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=80; cfg.max_uri_handlers=4; cfg.max_open_sockets=2; cfg.stack_size=6144;
    cfg.uri_match_fn=httpd_uri_match_wildcard;
    if (httpd_start(&server_,&cfg)!=ESP_OK) { stop(); return false; }
    const httpd_uri_t routes[]={{"/save",HTTP_POST,save,this},{"/scan",HTTP_POST,scan,this},{"/scan/results",HTTP_GET,scanResults,this},{"/*",HTTP_GET,index,this}};
    for (const auto &route : routes) if (httpd_register_uri_handler(server_,&route)!=ESP_OK) { stop(); return false; }
    dns_socket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    timeval timeout{1,0};
    sockaddr_in bind_addr{}; bind_addr.sin_family=AF_INET; bind_addr.sin_port=htons(53);
    // Restrict UDP listener to the AP address; no station-side provisioning service.
    bind_addr.sin_addr.s_addr=htonl(0xc0a80401);
    if (dns_socket_<0 || setsockopt(dns_socket_,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)<0 ||
        bind(dns_socket_,reinterpret_cast<sockaddr *>(&bind_addr),sizeof bind_addr)<0) { stop(); return false; }
    expires_at_=esp_timer_get_time()+600000000;
    active_=true;
    if (xTaskCreate(dnsLoop,"captive_dns",3072,this,4,reinterpret_cast<TaskHandle_t *>(&dns_task_))!=pdPASS) {
        dns_task_=nullptr; stop(); return false;
    }
    return true;
}
bool CaptivePortal::stop() {
    bool ok=true;
    active_=false;
    if (server_) { ok=httpd_stop(server_)==ESP_OK; server_=nullptr; } // Join all HTTP handlers first.
    if (dns_socket_>=0) shutdown(dns_socket_,SHUT_RDWR);
    if (dns_task_) {
        // recv has a timeout even if shutdown cannot wake UDP; never force-delete a worker.
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(dns_done_),portMAX_DELAY);
        dns_task_=nullptr;
    }
    if (dns_socket_>=0) { close(dns_socket_); dns_socket_=-1; }
    if (wifi_) { ok=wifi_->stopPortalWifi() && ok; wifi_=nullptr; }
    if (mutex_) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY);
        pending_ready_=pending_reserved_=false; clearProfiles(pending_);
        scan_state_=WifiScanState::Idle; scan_results_.clear(); scan_error_.clear();
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
    password_.clear(); token_.clear(); nonce_.clear();
    cleanup_failed_=cleanup_failed_ || !ok;
    return ok;
}
bool CaptivePortal::expired() const { return active() && esp_timer_get_time()>=expires_at_; }
bool CaptivePortal::acceptRequest(httpd_req_t *r, bool write) const {
    char host[32]{}, origin[48]{};
    if (httpd_req_get_hdr_value_str(r,"Host",host,sizeof host)!=ESP_OK ||
        (host!=address_ && host!=address_+":80")) return false;
    if (!httpd_req_get_hdr_value_len(r,"Origin")) return !write;
    if (httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof origin)!=ESP_OK) return false;
    return origin=="http://"+address_ || origin=="http://"+address_+":80";
}
esp_err_t CaptivePortal::index(httpd_req_t *r) {
    auto &p=*static_cast<CaptivePortal *>(r->user_ctx);
    if (!p.acceptRequest(r,false)) {
        httpd_resp_set_hdr(r,"Location","http://192.168.4.1/");
        return reply(r,"302 Found","Open http://192.168.4.1/ to configure Wi-Fi.");
    }
    // Offline device UI: system fonts, no external resources; manual form survives no JS.
    std::string html=R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>Whistle Wi-Fi setup</title><style nonce=")HTML"+p.nonce_+R"HTML(">
:root{color-scheme:light;--ink:#173149;--muted:#4e6475;--line:#c7d6df;--blue:#145b85;--paper:#fff;--wash:#eaf2f7}*{box-sizing:border-box}body{margin:0;background:var(--wash);color:var(--ink);font:16px/1.5 ui-sans-serif,system-ui,sans-serif}main{max-width:760px;margin:32px auto;padding:0 24px}header{padding:0 0 20px;border-bottom:2px solid var(--ink)}.brand{font-weight:750;display:flex;gap:10px;align-items:center}.mark{display:inline-block;width:22px;height:22px;border:3px solid var(--blue);border-radius:50%;box-shadow:inset 0 0 0 4px var(--wash);background:var(--blue)}h1{font-size:clamp(30px,6vw,42px);letter-spacing:-.04em;line-height:1.12;margin:22px 0 12px}p{margin:8px 0;color:var(--muted)}.scanbar{display:flex;align-items:center;justify-content:space-between;gap:16px;margin:20px 0 8px}.scanbar strong{display:block}.status{min-height:24px;font-size:14px;margin-bottom:20px}form{background:var(--paper);border:1px solid var(--line);border-radius:12px;padding:24px}.slots{display:grid;grid-template-columns:1fr 1fr;gap:28px}fieldset{border:0;padding:0;margin:0;min-width:0}legend{font-size:21px;font-weight:700;padding:0;margin-bottom:4px}.slot-note{font-size:14px;min-height:42px;margin:0 0 16px}label{display:block;font-size:14px;font-weight:650;margin:16px 0 6px}input,select,button{font:inherit}input:not([type=checkbox]),select{display:block;min-width:0;width:100%;height:46px;border:1px solid #8199aa;border-radius:6px;padding:8px 10px;color:var(--ink);background:white}select{text-overflow:ellipsis}input:focus-visible,select:focus-visible,button:focus-visible{outline:3px solid #247db3;outline-offset:3px}button{min-height:44px;border:1px solid var(--blue);border-radius:6px;padding:9px 16px;font-weight:650;background:white;color:var(--blue);cursor:pointer}button:disabled{opacity:.65;cursor:wait}.open{display:flex;align-items:center;gap:9px;font-weight:400;margin:12px 0}.open input{width:18px;height:18px;margin:0;flex-shrink:0}.hint{font-size:13px;color:var(--muted)}.submit{border-top:1px solid var(--line);margin-top:24px;padding-top:20px}.primary{background:var(--blue);color:white;width:100%}.submit p{font-size:13px;margin-bottom:0}footer{font-size:13px;margin:18px 0 28px;color:var(--muted)}[hidden]{display:none!important}@media(max-width:560px){main{margin:20px auto;padding:0 16px}form{padding:20px 16px}.slots{grid-template-columns:1fr;gap:24px}fieldset+fieldset{border-top:1px solid var(--line);padding-top:20px}.slot-note{min-height:0}.scanbar{align-items:flex-start;gap:10px}.scanbar button{padding:9px 12px;flex-shrink:0}header{padding-bottom:16px}}
</style></head><body><main><header><div class=brand><span class=mark aria-hidden=true></span>Whistle</div><h1>A connection for<br>your voice notes.</h1><p>Choose your home Wi-Fi and an optional hotspot.<br>The device tries Home first, then Hotspot.</p></header><div class=scanbar><div><strong>Nearby networks</strong><p class=hint>2.4 GHz Wi-Fi only</p></div><button id=scan type=button hidden>Scan networks</button></div><p id=status class=status role=status aria-live=polite>Enter a network name below. Scanning is available with JavaScript.</p><form method=post action=/save id=setup><div class=slots><fieldset><legend>Home</legend><p class=slot-note>Your everyday connection.</p><div class=picker hidden><label for=h-pick>Choose a nearby network</label><select id=h-pick><option value="">Enter manually or leave empty</option></select></div><label for=h>Network name (SSID)</label><input id=h name=h maxlength=32 autocomplete=off spellcheck=false><p class=hint>Hidden network? Enter its exact name here.</p><label for=hp>Password</label><input id=hp name=hp type=password maxlength=63 autocomplete=new-password><label class=open><input type=checkbox name=ho value=1 id=ho>Open Wi-Fi (no password)</label></fieldset><fieldset><legend>Hotspot</legend><p class=slot-note>Optional backup, such as your phone.</p><div class=picker hidden><label for=s-pick>Choose a nearby network</label><select id=s-pick><option value="">Enter manually or leave empty</option></select></div><label for=s>Network name (SSID)</label><input id=s name=s maxlength=32 autocomplete=off spellcheck=false><p class=hint>Leave both fields empty to disable this slot.</p><label for=sp>Password</label><input id=sp name=sp type=password maxlength=63 autocomplete=new-password><label class=open><input type=checkbox name=so value=1 id=so>Open Wi-Fi (no password)</label></fieldset></div><input name=csrf type=hidden value=")HTML"+p.token_+R"HTML("><div class=submit><button class=primary type=submit>Save Wi-Fi</button><p>This replaces both saved slots. Re-enter passwords, or explicitly choose Open Wi-Fi. Passwords are never displayed.</p></div></form><footer>Local setup only. Scanning may briefly slow this page.<br>The portal closes after 10 minutes or using BOOT on the device.</footer></main><script nonce=")HTML"+p.nonce_+R"HTML(">
(function(){'use strict';
const button=document.getElementById('scan'),status=document.getElementById('status'),form=document.getElementById('setup'),token=form.elements.csrf.value;
let networks=[],busy=false;
button.hidden=false;document.querySelectorAll('.picker').forEach(x=>x.hidden=false);
status.textContent='Scan for nearby networks, or enter an SSID manually.';
const security=n=>({0:'Open',1:'WEP (unsupported)',2:'WPA',3:'WPA2',4:'WPA/WPA2',5:'Enterprise (unsupported)',6:'WPA3',7:'WPA2/WPA3',8:'WAPI (unsupported)',9:'OWE (unsupported)'})[n.authmode]||'Security mode '+n.authmode;
function render(){['h','s'].forEach(id=>{const input=document.getElementById(id),pick=document.getElementById(id+'-pick');pick.replaceChildren();const manual=document.createElement('option');manual.value='';manual.textContent='Enter manually or leave empty';pick.appendChild(manual);networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' — '+n.rssi+' dBm · '+security(n);pick.appendChild(o)});pick.value=networks.some(n=>n.ssid===input.value)?input.value:''})}
['h','s'].forEach(id=>{const input=document.getElementById(id),pick=document.getElementById(id+'-pick'),pw=document.getElementById(id+'p'),open=document.getElementById(id+'o');pick.addEventListener('change',()=>{if(!pick.value){input.focus();return}input.value=pick.value;pw.value='';open.checked=false;pw.readOnly=false;input.setCustomValidity('');});input.addEventListener('input',()=>{pick.value=networks.some(n=>n.ssid===input.value)?input.value:'';document.getElementById('h').setCustomValidity('');document.getElementById('s').setCustomValidity('')});pw.addEventListener('input',()=>input.setCustomValidity(''));open.addEventListener('change',()=>{input.setCustomValidity('');if(open.checked)pw.value='';pw.readOnly=open.checked})});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
button.addEventListener('click',async()=>{if(busy)return;busy=true;button.disabled=true;button.textContent='Scanning…';status.textContent='Scan queued. Keep this page open.';
try{const start=await fetch('/scan',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'csrf='+encodeURIComponent(token),cache:'no-store'});if(!start.ok&&start.status!==409)throw Error('Could not start scan ('+start.status+').');
const until=Date.now()+20000;while(Date.now()<until){const response=await fetch('/scan/results',{headers:{'X-CSRF-Token':token},cache:'no-store'});if(!response.ok)throw Error('Could not read scan ('+response.status+').');const data=await response.json();if(data.state==='complete'){networks=data.networks;render();status.textContent=networks.length?'Choose a network below. Signal is shown in dBm; closer to zero is stronger.':'No named networks found. Retry, move closer, or enter a hidden SSID.';break}if(data.state==='error')throw Error(data.error||'Scan failed.');if(data.state==='idle')throw Error('Scan results expired. Scan again.');status.textContent=data.state==='queued'?'Scan queued. Keep this page open.':'Looking for nearby networks…';await delay(500)}if(Date.now()>=until)throw Error('Scan timed out. Retry or enter an SSID manually.');
}catch(error){status.textContent=error.message+' You can still enter an SSID manually.'}finally{busy=false;button.disabled=false;button.textContent='Scan again'}});
form.addEventListener('submit',event=>{let valid=true;['h','s'].forEach(id=>{const input=document.getElementById(id),pw=document.getElementById(id+'p'),open=document.getElementById(id+'o');let message='';if(new TextEncoder().encode(input.value).length>32)message='An SSID can contain at most 32 UTF-8 bytes.';else if(input.value&&!open.checked&&(pw.value.length<8||pw.value.length>63))message='Enter an 8–63 character password, or choose Open Wi-Fi.';input.setCustomValidity(message);if(message)valid=false});if(!form.elements.h.value&&!form.elements.s.value){form.elements.h.setCustomValidity('Enter at least one network.');valid=false}if(!valid){event.preventDefault();form.reportValidity()}});
})();
</script></body></html>)HTML";
    const std::string csp="default-src 'none'; script-src 'nonce-"+p.nonce_+"'; style-src 'nonce-"+p.nonce_+"'; connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'";
    httpd_resp_set_status(r,"200 OK"); httpd_resp_set_type(r,"text/html; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store"); httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"Referrer-Policy","no-referrer"); httpd_resp_set_hdr(r,"Content-Security-Policy",csp.c_str());
    return httpd_resp_send(r,html.c_str(),html.size());
}
esp_err_t CaptivePortal::scan(httpd_req_t *r) {
    auto &p=*static_cast<CaptivePortal *>(r->user_ctx);
    if (!p.active() || p.expired() || !p.acceptRequest(r,true)) return reply(r,"403 Forbidden","Forbidden");
    std::string body;
    if (!readBody(r,body,64)) return reply(r,"400 Bad Request","Invalid scan request");
    if (body!="csrf="+p.token_) return reply(r,"403 Forbidden","Forbidden");
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(p.mutex_),portMAX_DELAY);
    if (p.scan_state_==WifiScanState::Queued || p.scan_state_==WifiScanState::Scanning || p.pending_reserved_) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(p.mutex_)); return reply(r,"409 Conflict","Scan or save already busy");
    }
    const esp_err_t result=reply(r,"202 Accepted","Scan queued");
    if (result==ESP_OK) {
        p.scan_results_.clear(); p.scan_error_.clear(); p.scan_state_=WifiScanState::Queued;
        p.scan_queued_at_=esp_timer_get_time();
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(p.mutex_));
    return result;
}
esp_err_t CaptivePortal::scanResults(httpd_req_t *r) {
    auto &p=*static_cast<CaptivePortal *>(r->user_ctx);
    char csrf[40]{};
    if (!p.active() || p.expired() || !p.acceptRequest(r,false) ||
        httpd_req_get_hdr_value_str(r,"X-CSRF-Token",csrf,sizeof csrf)!=ESP_OK || csrf!=p.token_)
        return reply(r,"403 Forbidden","Forbidden");
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(p.mutex_),portMAX_DELAY);
    if (p.scan_state_==WifiScanState::Complete && esp_timer_get_time()-p.scan_completed_at_>=30000000) {
        p.scan_state_=WifiScanState::Idle; p.scan_results_.clear();
    }
    std::string json="{\"state\":"+jsonString(scanName(p.scan_state_))+",\"error\":"+jsonString(p.scan_error_)+",\"networks\":[";
    bool first=true;
    for (const auto &n:p.scan_results_) {
        if (!first) json+=',';
        first=false;
        json+="{\"ssid\":"+jsonString(n.ssid)+",\"rssi\":"+std::to_string(n.rssi)+",\"channel\":"+std::to_string(n.channel)+",\"authmode\":"+std::to_string(n.authmode)+"}";
    }
    json+="]}";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(p.mutex_));
    httpd_resp_set_status(r,"200 OK"); httpd_resp_set_type(r,"application/json; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store"); httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    return httpd_resp_send(r,json.c_str(),json.size());
}
void CaptivePortal::pollScan() {
    if (!active() || expired() || !wifi_) return;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY);
    const WifiScanState state=scan_state_;
    if (state==WifiScanState::Queued) scan_state_=WifiScanState::Scanning;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    if (state!=WifiScanState::Queued && state!=WifiScanState::Scanning) return;
    WifiScanResults results; std::string error;
    WifiScanState next;
    if (state==WifiScanState::Queued && esp_timer_get_time()-scan_queued_at_>=15000000) {
        next=WifiScanState::Error; error="Scan request expired. Retry.";
    } else {
        if (state==WifiScanState::Queued) wifi_->beginPortalScan();
        next=wifi_->pollPortalScan(results,error);
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY);
    scan_state_=next; scan_error_=error;
    if (next==WifiScanState::Complete) { scan_results_=std::move(results); scan_completed_at_=esp_timer_get_time(); }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}
esp_err_t CaptivePortal::save(httpd_req_t *r) {
    auto &p=*static_cast<CaptivePortal *>(r->user_ctx);
    if (!p.active() || p.expired() || !p.acceptRequest(r,true)) return reply(r,"403 Forbidden","Forbidden");
    char type[64]{};
    if (r->content_len<=0 || static_cast<size_t>(r->content_len)>MAX_BODY ||
        httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof type)!=ESP_OK ||
        std::strcmp(type,"application/x-www-form-urlencoded")) return reply(r,"400 Bad Request","Invalid request");
    std::string body(static_cast<size_t>(r->content_len),'\0'); size_t offset=0;
    while (offset<body.size()) {
        const int n=httpd_req_recv(r,&body[offset],body.size()-offset);
        if (n<=0 || static_cast<size_t>(n)>body.size()-offset) return reply(r,"400 Bad Request","Incomplete request");
        offset+=static_cast<size_t>(n);
    }
    WifiProfiles profiles;
    if (!parseForm(body,p.token_,profiles)) return reply(r,"400 Bad Request","Invalid fields");
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(p.mutex_),portMAX_DELAY);
    if (p.pending_reserved_) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(p.mutex_));
        return reply(r,"409 Conflict","Configuration busy");
    }
    p.pending_reserved_=true; p.pending_=profiles;
    // Send before publishing: main cannot stop HTTP before the queued response is sent.
    const esp_err_t result=reply(r,"202 Accepted","Queued. Check the device screen for save/connection completion. If saving fails, return to this form and retry.");
    p.pending_ready_=result==ESP_OK;
    if (result!=ESP_OK) { p.pending_reserved_=false; clearProfiles(p.pending_); }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(p.mutex_));
    clearProfiles(profiles); std::fill(body.begin(),body.end(),'\0');
    return result;
}
bool CaptivePortal::takeProfiles(WifiProfiles &profiles) {
    if (!mutex_) return false;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY);
    const bool ready=pending_ready_;
    if (ready) { profiles=pending_; pending_ready_=false; }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ready;
}
void CaptivePortal::complete(bool success) {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_),portMAX_DELAY);
    result_=success; pending_ready_=pending_reserved_=false; clearProfiles(pending_);
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}
} // namespace network
