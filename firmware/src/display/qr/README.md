# Vendored QR Code generator (C)

Unmodified `qrcodegen.c` and `qrcodegen.h` from Project Nayuki:
https://github.com/nayuki/QR-Code-generator/tree/3c6d0b3cefb4e049dc337e82237c9644399716a8/c

Pinned revision: `3c6d0b3cefb4e049dc337e82237c9644399716a8`.
License: MIT; the complete upstream copyright/permission/warranty notice is
preserved at the top of both files. No network or runtime package is needed.

`../wifi_qr.cpp` limits production encoding to versions 1–5 and uses fixed
stack buffers. The upstream library supports larger codes, but these would not
fit the portal's 90px square at two pixels per module with the required quiet zone.
