#!/usr/bin/env python3
import test_boot_storage as harness
harness.TEST = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while (0)
int main() {
 store.mount_ok=false; nvs_url="http://192.0.2.42:8000"; nvs_url_get_error=0;
 try { app_main(); } catch(const std::runtime_error&) {}
 CHECK(network::ApiClient::baseUrl()==network::ApiClient::defaultBaseUrl());
 CHECK(std::string(web.snapshots.back().base_url)==network::ApiClient::baseUrl());
 web.config_url="http://192.0.2.25:8001";
 web.command=network::web::Command::Configure; web.queued=true;
 handleWebCommand();
 CHECK(web.result=="ok" && network::ApiClient::baseUrl()==web.config_url);
 CHECK(nvs_url==web.config_url && !sd_mounted);
 network::ApiClient::setBaseUrl("changed");
 state=AppState::Idle;
 try { app_main(); } catch(const std::runtime_error&) {}
 CHECK(network::ApiClient::baseUrl()==web.config_url);
 nvs_url_get_error=1; loadServerUrl(); CHECK(network::ApiClient::baseUrl()==network::ApiClient::defaultBaseUrl());
 nvs_url_get_error=0; loadServerUrl(); CHECK(network::ApiClient::baseUrl()==web.config_url);
 for(auto s:{"http://127.0.0.1", "http://192.0.2.42:8000", "bad"}) {
   auto prior=network::ApiClient::baseUrl(); web.config_url=s; web.queued=true; handleWebCommand();
   CHECK(web.result=="rejected" && network::ApiClient::baseUrl()==prior);
 }
 for(auto busy:{AppState::Recording,AppState::Syncing}) {
   state=busy; web.config_url="http://192.0.2.26"; web.queued=true; handleWebCommand(); CHECK(web.result=="rejected");
 }
 state=AppState::Idle; stopping_recording=true;
 web.queued=true; handleWebCommand(); CHECK(web.result=="rejected"); stopping_recording=false;
 state=AppState::Menu;
 preview.stopped=false; web.queued=true; handleWebCommand(); CHECK(web.result=="rejected"); preview.stopped=true;
 for(int fail=0;fail<3;++fail) {
   auto prior=network::ApiClient::baseUrl();
   nvs_open_error=fail==0; nvs_set_error=fail==1; nvs_commit_error=fail==2;
   web.queued=true; handleWebCommand(); CHECK(web.result=="failed" && network::ApiClient::baseUrl()==prior);
   nvs_open_error=nvs_set_error=nvs_commit_error=0;
 }
 web.queued=true; handleWebCommand(); CHECK(web.result=="ok");
 nvs_open_error=1; loadServerUrl(); CHECK(network::ApiClient::baseUrl()==network::ApiClient::defaultBaseUrl()); nvs_open_error=0;
 nvs_url=std::string(200,'x'); loadServerUrl(); CHECK(network::ApiClient::baseUrl()==network::ApiClient::defaultBaseUrl());
 for(auto s:{"bad", "http://127.0.0.1", "http://192.0.2.42", "http://192.0.2.25/path"}) {
   nvs_url=s; loadServerUrl(); CHECK(network::ApiClient::baseUrl()==network::ApiClient::defaultBaseUrl());
 }
 state=AppState::Menu; sd_mounted=true; store.pending={"note"};
 api.during_transcribe=[&]() { assert(web.snapshots.back().state==AppState::Syncing); };
 syncPending(); CHECK(state==AppState::Menu);
 std::cout << "PASS real main runtime server config/no-SD/menu/busy/NVS failures/reboot fallback\n";
}
'''
if __name__=='__main__': harness.main()
