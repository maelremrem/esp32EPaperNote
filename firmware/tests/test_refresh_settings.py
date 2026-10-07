#!/usr/bin/env python3
"""Run real main.cpp with NVS and hardware adapters, no credentials."""
import test_boot_storage as harness
harness.TEST = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while (0)
int main() {
    store.mount_ok=false;
    try { app_main(); } catch(const std::runtime_error&) {}
    CHECK(epaper.partialRefreshLimit()==10);
    handleButton(app::ButtonEvent::LongPress);
    for(int i=0;i<4;++i) handleButton(app::ButtonEvent::ShortPress);
    CHECK(menu_index==4); handleButton(app::ButtonEvent::LongPress);
    CHECK(ui.screen=="refresh" && ui.selected==10);
    handleButton(app::ButtonEvent::ShortPress); CHECK(ui.selected==20);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(nvs_value==20 && epaper.partialRefreshLimit()==20 && menu_view==MenuView::Settings);
    CHECK(!sd_mounted);
    try { app_main(); } catch(const std::runtime_error&) {}
    CHECK(epaper.partialRefreshLimit()==20);
    for(uint8_t value : REFRESH_OPTIONS) {
        nvs_get_error=0; nvs_value=value;
        try { app_main(); } catch(const std::runtime_error&) {}
        CHECK(epaper.partialRefreshLimit()==value && ui.boot_limits.back()==value);
    }
    for(uint8_t value : {uint8_t(2),uint8_t(255),uint8_t(101)}) {
        nvs_value=value;
        try { app_main(); } catch(const std::runtime_error&) {}
        CHECK(epaper.partialRefreshLimit()==10);
    }
    for(int failure=0;failure<4;++failure) {
        nvs_open_error=failure==0; nvs_get_error=failure==1;
        nvs_set_error=failure==2; nvs_commit_error=failure==3;
        if(failure<2) {
            loadRefreshInterval(); CHECK(refresh_limit==10);
        }
        state=AppState::Menu; menu_view=MenuView::Settings; menu_index=4;
        executeMenuItem(); uint8_t previous=refresh_limit;
        handleButton(app::ButtonEvent::DoublePress);
        if(failure==1) nvs_open_error=1; // Reads failing alone need not prevent a write.
        handleButton(app::ButtonEvent::LongPress);
        CHECK(epaper.partialRefreshLimit()==previous && refresh_limit==previous);
        CHECK(ui.title=="Save failed" && ui.message.find("not saved")!=std::string::npos);
        handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Refresh);
        nvs_open_error=nvs_set_error=nvs_commit_error=0;
    }
    nvs_get_error=0;
    state=AppState::Menu; menu_view=MenuView::Settings; menu_index=4; executeMenuItem();
    auto initial=refresh_option;
    for(size_t i=0;i<sizeof(REFRESH_OPTIONS);++i) handleButton(app::ButtonEvent::ShortPress);
    CHECK(refresh_option==initial);
    handleButton(app::ButtonEvent::LongPress); CHECK(menu_view==MenuView::Settings);
    handleButton(app::ButtonEvent::ShortPress); CHECK(menu_index==5);
    handleButton(app::ButtonEvent::ShortPress); CHECK(menu_index==6);
    handleButton(app::ButtonEvent::ShortPress); CHECK(menu_index==0);
    for(size_t i=0;i<display::Ui::MENU_ITEMS;++i) {
        menu_view=MenuView::Settings; menu_index=i; executeMenuItem();
        CHECK(i==6 ? state==AppState::Idle : true);
        state=AppState::Menu;
    }
    std::cout << "PASS real-main defaults, all values, invalid/read/open errors, no-SD BOOT navigation, persistence/set/commit failures, wrap and all menu actions\n";
}
'''
if __name__ == '__main__':
    harness.main()
