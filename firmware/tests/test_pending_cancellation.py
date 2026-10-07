#!/usr/bin/env python3
"""Production physical main cancellation, with stopped-worker adapters."""
import run_navigation_tests as harness
harness.TESTS = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main() {
    sd_mounted=audio_ready=true; state=AppState::Menu;
    store.pending={"one","two"};
    web.download_active=true; startRecording(); CHECK(recorder.starts==0);
    menu_view=MenuView::CancelPending; cancel_all=true; cancel_selected=true; executeMenuItem(); CHECK(store.pendingCount()==2);
    CHECK(!syncPending() && api.calls==0); web.download_active=false;
    publishWeb(); CHECK(web.snapshots.back().usage_known && web.snapshots.back().total_bytes==1000 && web.snapshots.back().free_bytes==500);
    recorder.stopped=false; publishWeb(); CHECK(!web.snapshots.back().usage_known); recorder.stopped=true;
    menu_view=MenuView::Settings; menu_index=1; executeMenuItem();
    CHECK(menu_view==MenuView::Synchronization && api.calls==0);
    submenu_index=1; executeMenuItem();
    CHECK(menu_view==MenuView::Pending && pending_index==0);
    executeMenuItem(); CHECK(menu_view==MenuView::CancelPending && !cancel_selected);
    executeMenuItem(); CHECK(menu_view==MenuView::Pending && store.pendingCount()==2);
    executeMenuItem(); handleButton(app::ButtonEvent::ShortPress);
    CHECK(cancel_selected); executeMenuItem();
    CHECK(store.pending==std::vector<std::string>{"two"} && store.writes==0 && api.calls==0);
    CHECK(ui.title=="Cancellation complete");
    handleButton(app::ButtonEvent::ShortPress); CHECK(menu_view==MenuView::Pending);
    pending_index=pending_notes.size(); executeMenuItem(); CHECK(menu_view==MenuView::Synchronization);
    submenu_index=2; executeMenuItem(); CHECK(menu_view==MenuView::CancelPending && !cancel_selected);
    executeMenuItem(); CHECK(menu_view==MenuView::Synchronization && store.pendingCount()==1);
    executeMenuItem(); handleButton(app::ButtonEvent::ShortPress); recorder.stopped=false;
    executeMenuItem(); CHECK(store.pendingCount()==1 && ui.title=="Cancellation unavailable");
    recorder.stopped=true; handleButton(app::ButtonEvent::LongPress);
    wifi.online=true; recorder.stopped=false; CHECK(!syncPending() && api.calls==0); recorder.stopped=true;
    executeMenuItem(); handleButton(app::ButtonEvent::ShortPress); store.archive_ok=false;
    executeMenuItem(); CHECK(store.pendingCount()==1 && ui.title=="Cancellation incomplete");
    store.archive_ok=true; handleButton(app::ButtonEvent::LongPress);
    executeMenuItem(); handleButton(app::ButtonEvent::ShortPress); executeMenuItem();
    CHECK(store.pendingCount()==0 && store.writes==0);
    CHECK(store.archives==3); // selective success + failed/all retry
    std::cout << "PASS physical selective/all cancellation, default Back, stopped workers, failure retains pending\n";
}
'''
if __name__ == '__main__': harness.main()
