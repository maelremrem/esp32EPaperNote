# 200 × 200 notebook UI

Design: the transcript is the page, not a status dashboard. Black ink (#000000) on white paper (#FFFFFF), with no grayscale, animation or invented telemetry. The existing bitmap is thickened horizontally inside the unchanged 8 × 12 cell; original edge pixels are retained, and rightmost ink expands inward. The same foreground mask works in black-on-white and white-on-black. Text uses 1× scale except the boot wordmark. Left alignment, 8px side margins, 23 glyphs per transcript line, 15px transcript line pitch. A top icon/header line, note identifier, seven transcript lines, local/sync status and divided Short/Long action hints keep REC small during transcription.

The home microphone is the primary record action. Settings contains Notes, Sync, Wi-Fi, Storage, Full refresh, About and Back; saved-note history offers paging and a Back entry. The active row has white symbols and bold text on black. Methods without a network argument show a question mark beside the Wi-Fi icon, never an assumed connection. Pending count describes the queue, not server availability or successful transcription. Missing SD does not disable settings: the warning offers Menu/Settings, and Storage shows `No SD`. Button dispatch and mount retries remain main-loop responsibilities.

Submenus show at most six rows and scroll the selected row into view. Format confirmation
states explicitly warn that all card data will be erased, with Cancel selected initially.
The portal screen shows the caller-provided generated credentials and address, valid for
10 minutes; its Short and Long actions both say Close.

Settings and submenus reserve x=189..191 for a 3px outlined vertical scrollbar, y=32..157;
the five-row note browser uses the same style at y=34..145, above its note-count status.
No bar is drawn when all entries fit. The thumb projects the displayed [first,last)
range onto the interior track with outward rounding and a positive one-pixel minimum.
Back belongs to the note-list total; a short final page has a correspondingly shorter
thumb. Selected-row fill ends at x=185; text/value/chevron cells end before x=188,
leaving clear separation from the scrollbar and the footer at y=164.

Navigable child views use an explicit `>` at x=172, inverted with the selected row.
Settings keeps observed Wi-Fi/SD state and refresh count to its left. Notes, Wi-Fi,
Storage, Full refresh and About are children; Sync and Back are actions. Submenus use
caller-provided per-entry flags rather than guessing semantics from label strings;
information, portal and confirmation views are children, direct actions are not.

Built-in labels and preview fixtures are English. Live text keeps the latest seven wrapped lines. Static notes keep the beginning with a truncation marker only when content is hidden. User transcripts retain their language; existing supported accented glyphs are transliterated to ASCII, and unsupported UTF-8 code points produce one `?`, not one per byte. Long words split at glyph boundaries. All text is explicitly bounded before the display's implicit wrap.

Every existing full refresh remains a full refresh (controller 0xF7); this redesign does not introduce partial refresh, change refresh timing or promise a frame rate. Host previews validate framebuffer drawing/layout, not panel waveform quality or physical latency.
