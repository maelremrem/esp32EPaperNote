#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace app {

enum class ButtonEvent : uint8_t {
    ShortPress,
    DoublePress,
    LongPress,
};

class Button {
public:
    bool init();
    QueueHandle_t queue() const { return queue_; }

private:
    static void taskEntry(void *arg);
    void task();

    QueueHandle_t queue_ = nullptr;
};

} // namespace app
