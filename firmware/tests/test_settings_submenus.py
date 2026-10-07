#!/usr/bin/env python3
"""Execute production main with bounded hardware adapters; never touches an SD."""
import run_navigation_tests as harness
harness.TESTS = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main() {
    sd_mounted=audio_ready=true; state=AppState::Idle;
    menu_view=MenuView::Format; erase_selected=true; executeMenuItem();
    CHECK(store.formats==0 && ui.title=="Format unavailable");
    state=AppState::Menu; menu_view=MenuView::Settings;
    menu_index=3; executeMenuItem();
    CHECK(ui.screen=="submenu" && ui.title=="Storage" && store.formats==0);
    CHECK((ui.submenu_children==std::vector<bool>{true,true,false}));
    handleButton(app::ButtonEvent::ShortPress); handleButton(app::ButtonEvent::LongPress);
    CHECK(ui.screen=="format" && !erase_selected && store.formats==0);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(menu_view==MenuView::Storage && store.formats==0);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(menu_view==MenuView::Format && !erase_selected);
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(erase_selected);
    saved_notes={{"old",true}}; reader_text="old"; last_note_id="old"; last_note_text="old"; live_text="old";
    handleButton(app::ButtonEvent::LongPress);
    CHECK(store.formats==1 && sd_mounted && audio_ready);
    CHECK(saved_notes.empty() && reader_text.empty() && last_note_id.empty() && last_note_text.empty() && live_text.empty());
    CHECK(ui.screen=="info"); handleButton(app::ButtonEvent::ShortPress);
    CHECK(menu_view==MenuView::Storage);
    submenu_index=1; executeMenuItem(); handleButton(app::ButtonEvent::ShortPress);
    recorder.stopped=false; handleButton(app::ButtonEvent::LongPress);
    CHECK(store.formats==1); recorder.stopped=true;
    handleButton(app::ButtonEvent::ShortPress);
    menu_view=MenuView::Settings; menu_index=2; executeMenuItem();
    CHECK(ui.screen=="submenu" && ui.title=="Wi-Fi" && wifi.connects==0);
    CHECK((ui.submenu_children==std::vector<bool>{true,true,true,false,true,false}));
    executeMenuItem(); CHECK(ui.message.find("Offline")!=std::string::npos && ui.message.find("0.0.0.0")==std::string::npos);
    handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Wifi);
    wifi.online=true; executeMenuItem(); CHECK(ui.message.find("192.0.2.42")!=std::string::npos);
    handleButton(app::ButtonEvent::ShortPress); submenu_index=1; executeMenuItem();
    CHECK(ui.message.find("Home")!=std::string::npos && ui.message.find("synthetic-home")!=std::string::npos);
    handleButton(app::ButtonEvent::LongPress); submenu_index=2; executeMenuItem();
    CHECK(portal.active() && ui.screen=="portal" && web.stops==1);
    CHECK(!web.snapshots.empty() && web.snapshots.back().connecting);
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(!portal.active() && menu_view==MenuView::Wifi && web.starts==1);
    submenu_index=2; executeMenuItem(); portal.queued=true;
    pollPortal();
    CHECK(wifi.saves==1 && !portal.active() && menu_view==MenuView::Info);
    handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Wifi);
    executeMenuItem(); portal.timed_out=true; pollPortal();
    CHECK(!portal.active() && menu_view==MenuView::Info);
    handleButton(app::ButtonEvent::LongPress);
    portal.timed_out=false; wifi.save_ok=false; executeMenuItem(); portal.queued=true; pollPortal();
    CHECK(portal.active() && wifi.saves==2 && ui.title=="Wi-Fi save failed");
    handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Portal);
    handleButton(app::ButtonEvent::LongPress);
    portal.start_ok=false; executeMenuItem();
    CHECK(!portal.active() && menu_view==MenuView::Info && ui.title=="Portal failed");
    handleButton(app::ButtonEvent::LongPress);
    menu_view=MenuView::Storage; submenu_index=1; executeMenuItem();
    web.command=network::web::Command::Configure; web.queued=true; web.config_url="http://192.0.2.21:8000";
    handleWebCommand(); CHECK(web.result=="rejected" && store.formats==1);
    handleButton(app::ButtonEvent::ShortPress); store.format_ok=false;
    last_note_id="stale"; saved_notes={{"stale",true}};
    executeMenuItem();
    CHECK(!sd_mounted && audio_ready && store.formats==2 && saved_notes.empty() && last_note_id.empty());
    CHECK(ui.title=="Format failed" && recording_recovery_required);
    handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Storage);
    executeMenuItem(); handleButton(app::ButtonEvent::ShortPress); executeMenuItem();
    CHECK(store.formats==2 && ui.title=="Format unavailable");
    menu_view=MenuView::Wifi; submenu_index=4; wifi.online=true; portal.start_ok=true;
    executeMenuItem();
    CHECK(ui.title=="Web settings" && ui.message.find("PIN: 012345")!=std::string::npos);
    CHECK(ui.message.find("5 incorrect attempts")!=std::string::npos);
    std::cout << "PASS real-main Storage confirmation, workers, invalidation, Wi-Fi IP/profiles/portal lifecycle\n";
}
'''
if __name__ == '__main__': harness.main()
