#!/usr/bin/env python3
"""Real main synchronization: cooperative cancellation, persistence, retry gate."""
import run_navigation_tests as harness
harness.TESTS = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main() {
    sd_mounted=audio_ready=true; wifi.online=true; state=AppState::Menu;
    store.pending={"one","two","three"};
    threaded_tasks=true;
    const auto main_thread=std::this_thread::get_id();
    std::atomic<bool> waiting{false}; bool delivered=false;
    api.during_transcribe=[&]() {
        assert(std::this_thread::get_id()!=main_thread);
        if(api.calls==2) {
            waiting=true;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!sync_stop.load() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            assert(sync_stop.load());
        }
    };
    button_queue_hook=[&](void *out) {
        assert(std::this_thread::get_id()==main_thread);
        if(waiting.load() && !delivered) {
            assert(state==AppState::Syncing && store.writes==1 && store.archives==1);
            startRecording(); assert(recorder.starts==0);
            web.command=network::web::Command::Format; web.queued=true; handleWebCommand(); assert(web.result=="rejected" && store.formats==0);
            *static_cast<app::ButtonEvent*>(out)=app::ButtonEvent::LongPress; delivered=true; return pdTRUE;
        }
        return 0;
    };
    CHECK(!syncPending());
    CHECK(api.calls==2 && store.writes==1 && store.archives==1);
    CHECK((store.pending==std::vector<std::string>{"two","three"}));
    CHECK(last_note_id=="one" && state==AppState::Menu);
    CHECK(sync_cancelled && auto_sync_paused);
    button_queue_hook={}; threaded_tasks=false;
    api.during_transcribe={}; CHECK(syncPending());
    CHECK(store.pending.empty() && store.writes==3 && !sync_cancelled && !auto_sync_paused);
    store.pending={"four"}; state=AppState::Idle;
    api.during_transcribe=[&]() { handleButton(app::ButtonEvent::ShortPress); handleButton(app::ButtonEvent::LongPress); };
    CHECK(!syncPending() && store.pendingCount()==1 && store.writes==3 && recorder.starts==0);
    CHECK(ui.title=="Sync cancelled");
    api.during_transcribe={}; state=AppState::Idle; web.command=network::web::Command::Sync; web.queued=true;
    api.during_transcribe=[&]() { handleButton(app::ButtonEvent::LongPress); };
    handleWebCommand(); CHECK(web.result=="cancelled" && store.pendingCount()==1);
    api.during_transcribe={}; state=AppState::Menu;
    bool late=false;
    api.during_transcribe=[&]() {late=true;};
    button_queue_hook=[&](void *out) { if(!late)return 0;late=false;*static_cast<app::ButtonEvent*>(out)=app::ButtonEvent::LongPress;return 1; };
    int writes=store.writes; CHECK(!syncPending() && store.writes==writes && store.pendingCount()==1);
    api.during_transcribe={}; store.pending={"prefix","next"};
    store.during_write=[&]() {late=true;}; int calls=api.calls;
    CHECK(!syncPending() && api.calls==calls+1 && store.writes==writes+1 && store.pending==std::vector<std::string>{"next"});
    store.during_write={};button_queue_hook={};
    task_create_ok=false; CHECK(!syncPending() && !sync_cancelled && store.pendingCount()==1);task_create_ok=true;
    semaphore_create_ok=false;CHECK(!syncPending() && store.pendingCount()==1);semaphore_create_ok=true;
    // Exercise actual idle auto-retry loop after a boot-triggered cancelled batch.
    api.during_transcribe=[&]() {handleButton(app::ButtonEvent::LongPress);};
    state=AppState::Idle; menu_view=MenuView::Settings;calls=api.calls;
    int loops=0;
    queue_hook=[&]() { if(state==AppState::Syncing)return 0; if(++loops==3)throw std::runtime_error("checked idle retry"); state=AppState::Idle;menu_view=MenuView::Settings;mock_now_us+=config::SYNC_RETRY_MS*1000LL;return 0; };
    try {app_main();}catch(const std::runtime_error&) {}
    CHECK(auto_sync_paused && api.calls==calls+1 && store.pendingCount()==1 && loops==3);
    queue_hook={};api.during_transcribe={};
    std::cout << "PASS real main long-press sync stop, pending preservation, committed prefix, manual retry and truthful web result\n";
}
'''
if __name__ == '__main__': harness.main()
