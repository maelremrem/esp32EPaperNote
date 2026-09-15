# Firmware

PlatformIO project using ESP-IDF.

## Setup

```bash
cp include/secrets.example.h include/secrets.h
nano include/secrets.h
pio run
pio run -t upload
pio device monitor
```

## Hardware button

The starter firmware uses one application button on `GPIO1`, wired to GND. This avoids using the BOOT/strapping pin as the normal UI input.

## Important hardware validation

The Waveshare V2 board has dedicated examples for its display, SD and ES8311 codec. This project keeps all pin assignments in `include/board_pins.h`. Before building a final enclosure, validate:

1. ePaper orientation and busy polarity;
2. microphone I2S channel and gain;
3. SDMMC pins in 1-bit mode;
4. audio power-enable behavior;
5. your actual external application-button GPIO.

The application architecture does not depend on those details, so board-level fixes remain localized.

## Storage flow

```text
recording/current.tmp
       ↓ stop
pending/<note-id>.wav
       ↓ successful API response
notes/<note-id>.md
archive/<note-id>.wav
```

The pending WAV is never removed before the Markdown note has been safely written.
