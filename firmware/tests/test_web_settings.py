#!/usr/bin/env python3
"""Exercise actual main-owned web settings commands with hardware adapters."""
import test_boot_storage as harness
harness.TEST = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while (0)
int main() {
 state=AppState::Idle; sd_mounted=true; wifi.online=true;
 auto run=[&](network::web::Command c) { web.command=c; web.queued=true; handleWebCommand(); };
 web.settings.partial_limit=20; run(network::web::Command::Display);
 CHECK(web.result=="ok" && refresh_limit==20 && nvs_value==20);
 nvs_commit_error=1; web.settings.partial_limit=50; run(network::web::Command::Display);
 CHECK(web.result=="failed" && refresh_limit==20); nvs_commit_error=0;
 web.settings.partial_limit=7; run(network::web::Command::Display); CHECK(web.result=="rejected");
 state=AppState::Recording; run(network::web::Command::Mount); CHECK(web.result=="rejected"); state=AppState::Idle;
 preview.stopped=false; run(network::web::Command::Mount); CHECK(web.result=="rejected"); preview.stopped=true;
 sd_mounted=false; store.mount_ok=true; run(network::web::Command::Mount); CHECK(web.result=="ok" && sd_mounted);
 run(network::web::Command::PrepareFormat); CHECK(web.result=="ok" && web.snapshots.back().format_challenge!=0);
 web.settings.challenge=web.snapshots.back().format_challenge;
 mock_now_us+=61000000; run(network::web::Command::Format); CHECK(web.result=="rejected" && store.formats==0);
 run(network::web::Command::PrepareFormat); web.settings.challenge=web.snapshots.back().format_challenge;
 last_note_text="cached"; reader_text="cached"; saved_notes.push_back({"cached",true});
 run(network::web::Command::Format); CHECK(web.result=="ok" && store.formats==1 && saved_notes.empty() && last_note_text.empty() && reader_text.empty());
 run(network::web::Command::Format); CHECK(web.result=="rejected" && store.formats==1);
 run(network::web::Command::PrepareFormat); web.settings.challenge=web.snapshots.back().format_challenge;
 run(network::web::Command::Display); run(network::web::Command::Format); CHECK(web.result=="rejected" && store.formats==1);
 run(network::web::Command::Wifi); CHECK(web.result=="ok" && wifi.saves==1);
 CHECK(wifi.connects==0); // Saving never silently drops the response or switches IP.
 run(network::web::Command::Reconnect); CHECK(web.result=="ok" && wifi.connects==1);
 menu_view=MenuView::Format; run(network::web::Command::Mount); CHECK(web.result=="rejected");
 std::cout << "PASS real main web display/NVS/storage/worker guards/format TTL/replay/cache/Wi-Fi save-before-reconnect\n";
}
'''
if __name__=='__main__': harness.main()
