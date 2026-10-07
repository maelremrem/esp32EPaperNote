#!/usr/bin/env python3
import run_navigation_tests as harness
harness.TESTS = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main() {
 state=AppState::Menu; wifi.online=true;
 menu_view=MenuView::Wifi; submenu_index=4; executeMenuItem();
 CHECK(web.session_opens==1 && ui.title=="Web settings" && ui.message.find("10 minutes")!=std::string::npos);
 CHECK(ui.message.find(network::ApiClient::token)==std::string::npos);
 menu_view=MenuView::Settings;
 sd_mounted=true; web.download_active=true; web.command=network::web::Command::Download; web.queued=true;
 handleWebCommand(); CHECK(web.download_provided && store.download_opens==1);
 web.download_active=false;
 web.command=network::web::Command::Configure; web.config_url="http://192.0.2.25:8080";
 web.settings.replace_token=true; network::web::copyText(web.settings.server_token,"runtime-secret");
 web.queued=true; handleWebCommand();
 CHECK(web.result=="ok" && network::ApiClient::token=="runtime-secret" && nvs_token=="runtime-secret");
 CHECK(web.snapshots.back().token_configured && web.snapshots.back().server_revision==1);
 nvs_commit_error=1; network::web::copyText(web.settings.server_token,"not-active"); web.queued=true; handleWebCommand();
 CHECK(web.result=="failed" && network::ApiClient::token=="runtime-secret" && web.snapshots.back().server_revision==1);
 nvs_commit_error=0; network::ApiClient::token="changed"; loadServerUrl(); CHECK(network::ApiClient::token=="runtime-secret");
 web.settings.replace_token=false; web.settings.server_token[0]=0; web.queued=true; handleWebCommand();
 CHECK(web.result=="ok" && network::ApiClient::token=="runtime-secret");
 web.settings.replace_token=true; web.settings.server_token[0]=0; web.queued=true; handleWebCommand();
 CHECK(web.result=="ok" && !network::ApiClient::tokenConfigured());
 loadServerUrl(); CHECK(!network::ApiClient::tokenConfigured());
 network::web::copyText(web.settings.server_token,"bad\r\nheader"); web.queued=true; handleWebCommand(); CHECK(web.result=="rejected");
 std::cout << "PASS real main token/URL NVS commit before atomic activation, write-only flag/revision, blank keeps/clear/reboot/header validation\n";
}
'''
if __name__=='__main__': harness.main()
