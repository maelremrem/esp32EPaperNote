# Screen assets

## Historical mockups: embedded French labels retained

These original raster assets are historical design references, not the current
English interface. Their embedded French text has not been translated; no edited
or fabricated replacement images are supplied.

| Historical reference | Reduced 200×200, 1-bit reference |
|---|---|
| `reference/01_idle_note.png` | `native_200x200/01_idle_note_200x200_1bit.png` |
| `reference/02_recording.png` | `native_200x200/02_recording_200x200_1bit.png` |
| `reference/03_offline_queue.png` | `native_200x200/03_offline_queue_200x200_1bit.png` |
| `reference/04_syncing.png` | `native_200x200/04_syncing_200x200_1bit.png` |
| `reference/05_menu.png` | `native_200x200/05_menu_200x200_1bit.png` |

Use these only for composition and panel constraints. Implement the interface
with native graphics primitives rather than using the mockups as framebuffers.

## Implementation evidence

- `web/`: actual Chrome renders of the English ESP32 console and LXC library,
  using explicitly synthetic API/database fixtures. See `web/README.md` for checks
  and limitations. Accented long-word fixtures verify UTF-8 wrapping.
- `new/` and `reference-ui/`: separate firmware-render evidence; these directories
  are maintained by the display workstream and are not modified by the web/docs
  localization work.

The product name `Carnet` and speech-recognition language identifiers such as
`fr` are intentionally preserved. English interface labels do not translate the
user's recorded speech or change the default French recognition language.
