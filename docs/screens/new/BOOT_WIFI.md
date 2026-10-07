# Boot Wi-Fi status frames

`display::Ui::showBootWifi(home, hotspot, ip = "")` renders caller-supplied `display::BootWifiStatus` values: `Pending`, `Connecting`, `Connected`, `Failed`, `Skipped`, and `Disabled`. The UI does not connect to networks, infer success, or change controller state. `showBoot()` remains unchanged.

Home Wi-Fi and Hotspot have separate English labels and status lines. The address line is omitted when the caller supplies no IP. Supplied addresses are bounded to the existing 23-character text width. The screen uses the existing thicker font and full-refresh path.

The `boot-wifi-*.png` images in this directory and `../reference-ui/` are actual 200×200 production framebuffer renders. Addresses in the connected previews are explicit host-test fixtures, not device telemetry. Previews cover pending, home connecting, fallback hotspot connecting, home connected with hotspot skipped, hotspot connected after home failure, both failed, and both disabled.

Regenerate with:

```sh
python3 tests/ui/render_test.py --preview docs/screens/new
python3 tests/ui/render_test.py --preview docs/screens/reference-ui
```

The new host test was run before implementation and failed because the requested enum/API were missing. After implementation, the real renderer checks every pair of six states, each with and without a supplied IPv4 address: exact state labels, no invented connected status/address, nonempty real pixels, one refresh, no clipped pixels, in-bounds text and nonoverlapping text boxes. The full 25-test rendering suite passes with g++ and with clang++ AddressSanitizer/UndefinedBehaviorSanitizer. These checks do not verify physical e-paper or actual Wi-Fi connectivity.
