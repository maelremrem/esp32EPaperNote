# Host framebuffer layout tests

From the repository root:

```sh
python3 tests/ui/render_test.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all' python3 tests/ui/render_test.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all' python3 tests/ui/render_test.py
python3 tests/ui/render_test.py --preview docs/screens/new
python3 tests/ui/render_test.py --preview docs/screens/reference-ui
```

Requires Python 3 and a C++17 compiler (`g++` by default; set `CXX` to override). PNG export uses Python's standard library only.

The harness compiles the real `ui.cpp`, real `font8x12.h`, and the exact drawing-method section of `epaper_display.cpp`. It substitutes only the SPI handle type and hardware refresh, exposes the framebuffer in the temporary host translation unit, and adds draw-call/invalid-pixel counters. It does not replicate text rendering or substitute a different font. Temporary build files go to `TMPDIR`; nothing is generated under the firmware source directory.

Coverage: all UI methods, including microphone home, inverted settings, saved-note list/scroll/return, empty history, paginated reader, SD mount failure and fatal startup errors; text extents before implicit panel wrapping; no text-box collisions; seven-line static/live transcript behavior; long words; glyph-based accented wrapping; unsupported UTF-8 code points; raw panel-edge clipping; empty and randomized transcripts; honest known/unknown network states; saturated and zero-total sync progress. Each screen invokes one refresh.

Reference redesign RED checks were exercised before implementation for the microphone home, settings/SD error APIs, list/reader APIs and fatal-error actions. Existing transcript, UTF-8, clipping and sync progress regressions remain covered. Battery percentage, unmeasured free space, brightness control and audio playback are deliberately absent. The SD icon represents a known mount result, not hot-plug detection; unknown state omits it. Unknown Wi-Fi has a question mark and disconnected Wi-Fi is crossed out.

Both `docs/screens/new/` and `docs/screens/reference-ui/` are regenerated from the current English UI with the thicker font. `overview.png` combines the four reference-inspired screens, enlarged with nearest-neighbor pixels. The individual PNGs are actual 200×200 framebuffer output. Note names/transcripts in these host previews are explicit English test fixtures, not recordings from the device.

Bold-font RED was verified against the original renderer: all 94 non-space glyphs failed the strict pixel-count increase assertion. The implementation retains the regular bitmap as an independent baseline, expands ink horizontally inside each existing 8×12 cell, and expands the rightmost column inward. Tests compare actual pixels for every printable ASCII character against the regular bitmap, retaining all original ink without cell overflow. Additional pixel comparisons cover white-on-black inversion, 3× scaling, unchanged 8px advance, and the complete bottom-right glyph. Wrapping and line pitch remain unchanged.

English-label and missing-SD RED checks also failed before implementation. `showStorageError()` is still API-compatible but now offers Short → Menu and Long → Settings instead of requiring a restart. All seven menu selections render without SD, the card state says `No SD`, and `showInfo("Storage", message)` retains Back actions for the caller's retry flow. The UI only renders navigation hints; firmware main owns button handling and mount retries. Transcript fixtures in multiple languages remain unchanged apart from existing ASCII transliteration/fallback; the UI does not translate user speech.

These are host layout/rendering tests, not ESP-IDF builds or physical e-paper tests. SPI initialization, panel waveforms, busy-pin timing, real Wi-Fi and server availability are outside this harness. The automatic full/partial driver transport is tested separately by `firmware/tests/test_epaper_refresh.py`.

The current refresh editor renders 1/5/10/20/50/100 partial-update counts and Full
only. Tests assert the full-refresh label, count and controls, actual refresh call,
text extents and collision-free layout. The seventh Settings row scrolls into view
rather than overlapping the footer. `refresh-interval.png`, `refresh-full-only.png`
and `settings-refresh.png` are actual current framebuffer previews. Firmware main
owns NVS and short-change/long-save-and-return handling; the host layout harness
never pretends to persist settings.
