# Hardware and software references

Useful upstream documentation used while preparing this starter project:

- Waveshare board documentation: https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- Waveshare examples: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
- ESP-IDF I2S documentation: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/i2s.html
- Espressif ES8311 example: https://github.com/espressif/esp-idf/tree/master/examples/peripherals/i2s/i2s_codec/i2s_es8311
- esp_codec_dev: https://components.espressif.com/components/espressif/esp_codec_dev
- faster-whisper: https://github.com/SYSTRAN/faster-whisper

## Board assumptions

The project targets the **V2** board. Hardware definitions are centralized in:

```text
firmware/include/board_pins.h
```

Validate the silkscreen on the physical board before flashing a battery-powered build.

## Historical visual references

The PNGs in `../screens/reference/` and `../screens/native_200x200/` are historical
mockups, not screenshots of the current implementation. Their embedded French
labels have not been edited or represented as translated. See
[`../screens/README.md`](../screens/README.md) for the asset inventory and current
English-render evidence.
