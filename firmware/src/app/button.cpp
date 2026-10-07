#include "app/button.h"

#include "board_pins.h"
#include "project_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace app {

static const char *TAG = "button";

bool Button::init() {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << board::APP_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        return false;
    }

    queue_ = xQueueCreate(8, sizeof(ButtonEvent));
    if (!queue_) {
        return false;
    }

    return xTaskCreate(taskEntry, "button", 3072, this, 6, nullptr) == pdPASS;
}

void Button::taskEntry(void *arg) {
    static_cast<Button *>(arg)->task();
}

void Button::task() {
    bool last_raw = gpio_get_level(board::APP_BUTTON) != 0;
    bool stable = last_raw;
    bool ignore_until_release = !stable;
    int64_t last_change_us = esp_timer_get_time();
    int64_t press_start_us = 0;
    int64_t first_click_us = 0;
    bool long_sent = false;

    while (true) {
        const bool raw = gpio_get_level(board::APP_BUTTON) != 0;
        const int64_t now = esp_timer_get_time();

        if (raw != last_raw) {
            last_raw = raw;
            last_change_us = now;
        }

        if (raw != stable && (now - last_change_us) / 1000 >= config::BUTTON_DEBOUNCE_MS) {
            stable = raw;
            // BOOT can still be held after flashing. Its first release is not a click.
            if (ignore_until_release) {
                if (stable) ignore_until_release = false;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!stable) {
                press_start_us = now;
                long_sent = false;
            } else {
                const int64_t held_ms = (now - press_start_us) / 1000;
                if (!long_sent && held_ms < config::BUTTON_LONG_MS) {
                    if (first_click_us != 0 && (now - first_click_us) / 1000 <= config::BUTTON_DOUBLE_MS) {
                        ButtonEvent ev = ButtonEvent::DoublePress;
                        xQueueSend(queue_, &ev, 0);
                        first_click_us = 0;
                    } else {
                        first_click_us = now;
                    }
                }
            }
        }

        if (!stable && !long_sent && press_start_us != 0 &&
            (now - press_start_us) / 1000 >= config::BUTTON_LONG_MS) {
            ButtonEvent ev = ButtonEvent::LongPress;
            xQueueSend(queue_, &ev, 0);
            long_sent = true;
            first_click_us = 0;
        }

        if (first_click_us != 0 && (now - first_click_us) / 1000 > config::BUTTON_DOUBLE_MS) {
            ButtonEvent ev = ButtonEvent::ShortPress;
            xQueueSend(queue_, &ev, 0);
            first_click_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace app
