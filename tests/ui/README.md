# Host framebuffer layout tests

## Staged boot / cancellable sync

Boot has a clockwise determinate ring at (100,78), thin outline and four completed
startup steps. The matching Starting header uses the existing bold font and icons.
Stage title is at y118; status is bounded below the ring at y138/153. Wi-Fi uses two
independent profile rows plus an observed IP, preserving all six states. SD is
unknown before mounting; only result frames supply readiness. Completion includes
failed/skipped checks and does not imply success or elapsed-time estimates.
Tests compare actual annular pixels for 0/2/4 steps and saturated progress, text
bounds/truncation and all Home/Hotspot state combinations. Sync has Long Cancel;
Stopping sync has only Wait controls while HTTP is joining.

The complete suite has 44 tests, including mandatory real framebuffer QR decoding.
To regenerate only the owned startup and new sync PNGs without overwriting other
screens or overview:

```sh
"$TMPDIR/portal-qr-venv/bin/python" tests/ui/render_test.py --preview-boot docs/screens/new
```

These are actual production framebuffer renders with synthetic observed states,
not proof of server/radio/SD connectivity or physical panel refresh timing.


Current Settings order is Notes, Sync, Wi-Fi, Storage, Full refresh, About, Back.
The suite includes the real Wi-Fi/Storage submenu labels, a synthetic seven-entry
submenu scrolling stress fixture, Cancel-default/Erase-selected format warnings,
and full portal SSID/password/address plus a ten-minute lifetime and Close controls.
`wifi-menu.png`, `storage-menu.png`, `format-cancel.png`, `format-erase.png` and
`portal.png` are actual framebuffer renders; synthetic credentials are not secrets.

From the repository root:

```sh
python3 tests/ui/render_test.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all' python3 tests/ui/render_test.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all' python3 tests/ui/render_test.py
python3 tests/ui/render_test.py --preview docs/screens/new
python3 tests/ui/render_test.py --preview docs/screens/reference-ui
```

Requires Python 3, C99 and C++17 compilers (`gcc`/`g++` by default; set `CC`/`CXX` to override), plus `zxing-cpp` and Pillow for mandatory framebuffer decoding. PNG export itself uses Python's standard library only. Use the scratch virtual environment below when these decoder packages are not already installed.

The harness compiles the real `ui.cpp`, real `font8x12.h`, and the exact drawing-method section of `epaper_display.cpp`. It substitutes only the SPI handle type and hardware refresh, exposes the framebuffer in the temporary host translation unit, and adds draw-call/invalid-pixel counters. It does not replicate text rendering or substitute a different font. Temporary build files go to `TMPDIR`; nothing is generated under the firmware source directory.

Coverage: all UI methods, including microphone home, inverted settings, saved-note list/scroll/return, empty history, paginated reader, SD mount failure and fatal startup errors; text extents before implicit panel wrapping; no text-box collisions; seven-line static/live transcript behavior; long words; glyph-based accented wrapping; unsupported UTF-8 code points; raw panel-edge clipping; empty and randomized transcripts; honest known/unknown network states; saturated and zero-total sync progress. Each screen invokes one refresh.

The settings order is exactly `Notes, Sync, Wi-Fi, Storage, Full refresh, About, Back`.
`showSubmenu(title, entries, selected, children = {})` is a generic six-row scrolling list with truthful
Short Next / Long Select hints. `showFormatConfirmation()` warns that all card data is
erased, defaults to Cancel, and toggles the selected Erase SD action. `showPortal()` retains
its `(ssid, password, address)` API and renders a real Wi-Fi join QR, the complete generated
SSID (up to 32 bytes across two text rows), caller-supplied eight-character password,
`192.168.4.1` address and `Close (10 min)` hint. Both short and long gestures close the
portal in firmware main; the compact portal footer replaces the generic two-action footer.
`showInfo()` uses Back for both actions.

The QR is `WIFI:T:WPA;S:<escaped SSID>;P:<escaped password>;;`, not a portal URL.
Backslash, semicolon, comma, colon and double quote are backslash-escaped. The unmodified
MIT-licensed Nayuki C encoder is pinned in `firmware/src/display/qr/` (see its README),
compiled as C in both ESP-IDF and the host harness. `wifi_qr.cpp` uses fixed stack buffers,
versions 1–5, automatic masking and boosted error correction where the same size permits.
Black modules are exactly 2×2 pixels on white with a four-module quiet zone; the maximum
square is 90×90 pixels. Invalid credentials and payloads exceeding display capacity show
`QR unavailable` / `Connect manually`, retain bounded manual text, and still refresh once.
No QR buffers are allocated on the heap and no credentials are logged by production code.

The decoder tests require `zxing-cpp` and Pillow and never silently skip. For an isolated
installation and both full-suite runs:

```sh
python3 -m venv "$TMPDIR/portal-qr-venv"
"$TMPDIR/portal-qr-venv/bin/pip" install zxing-cpp pillow
"$TMPDIR/portal-qr-venv/bin/python" tests/ui/render_test.py
CXX=clang++ UI_TEST_CXXFLAGS='-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer' "$TMPDIR/portal-qr-venv/bin/python" tests/ui/render_test.py
```

QR RED → GREEN decoded the actual production-rendered 200×200 framebuffer: the former
text-only portal returned no barcodes, and the QR implementation returns the exact fixture
payload. Additional coverage round-trips every escaped delimiter in a 32-byte SSID and
an eight-character password, checks complete manual SSID text, verifies 2×2 finder pixels
and all four white quiet borders, and exercises oversized/invalid input plus actual encoder
capacity failure. All 40 tests pass normally and with Clang ASan/UBSan; C and C++ are both
instrumented. The exported `portal.png` was visually inspected; all credentials are synthetic.
This is not evidence of a real phone joining Wi-Fi or scanning a physical e-paper panel.

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

Scrollbar/chevron changes were exercised RED → GREEN against real framebuffer pixels:
missing Settings/submenu bars, short-note lists incorrectly showing bars, page-based note
thumbs ignoring the real final-page size, and missing child-view chevrons each failed first.
The scrollbar/chevron suite had 37 tests; the current QR-inclusive suite has 40 and passes
with the Clang sanitizer command above.
Bars occupy x=189..191 only when total entries exceed the displayed count. Thumb endpoints
project the actual [first,last) entry range onto the interior track, rounded outward with
at least one pixel; notes count Back as an entry. Tests cover empty/one/full-page lists,
partial final pages, huge selections, 1001 entries, inverse chevron masks, text clearance,
and footer bounds. Settings/submenus have six rows; notes retain five rows per page.

Settings child views Notes, Wi-Fi, Storage, Full refresh and About show `>` separately
from state values; Sync and Back do not. Submenu navigation is explicit, not inferred
from titles or labels: the optional `std::vector<bool>` flags are indexed by entry;
missing flags mean no chevron and extra flags are ignored. Firmware callers should pass
`{true,true,true,false,false}` for Wi-Fi (IP, profiles, portal, reconnect, Back) and
`{true,true,false}` for Storage (status/retry information, format confirmation, Back).
Host main/UI adapters defining `showSubmenu` must accept the fourth parameter as well.
Previews `settings-scroll.png`, `notes-long.png` and `notes-long-last.png` show the real
end-of-Settings and nineteen-note first/final pages. No physical-panel claim is made.
