# amoled_photo

Turns a photo into the 368×448 RGB565 buffer the panel wants.

```c
#include "photo.h"

uint8_t *card = heap_caps_malloc(PHOTO_BYTES, MALLOC_CAP_SPIRAM);
photo_load("/sdcard/book/dog.jpg", card);      // .jpg/.jpeg decoded, .rgb565 copied
photo_from_jpeg(bytes, len, card, &w, &h);      // from memory, e.g. an embedded file
```

- **Fit, not crop:** the whole photo is visible. A landscape shot sits in the middle with
  bands above and below in its own average colour at 35 % brightness.
- **Decoder scaling:** 1/2, 1/4 or 1/8 in `esp_new_jpeg` so a 12 MP phone photo fits
  PSRAM; then a box filter to the exact size. ~220–370 ms for a 1 MP source.
- **Cache:** a decoded JPEG is written beside itself as `<stem>.fit.rgb565` when the
  filesystem is writable; a cache at least as new as the JPEG is used instead of decoding.
  Older `<stem>.rgb565` crop caches are removed on sight.
- Output buffer: exactly `PHOTO_BYTES` (329,728). Decode scratch is 16-byte aligned PSRAM.

Host-side twin: `projects/02_word_book_en/tools/make_book.py` produces the same fit from
Python, for pre-converting on a laptop.

Verified on hardware 2026-09-05 in `02_word_book_en`.
