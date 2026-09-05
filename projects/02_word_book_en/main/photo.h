#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTO_W      368
#define PHOTO_H      448
#define PHOTO_BYTES  (PHOTO_W * PHOTO_H * 2)

/*
 * Load a photo file into a PHOTO_BYTES RGB565 buffer, ready for the panel.
 *   .rgb565  raw, exactly PHOTO_BYTES: copied
 *   .jpg / .jpeg  decoded, centre-cropped to 368:448 and box-filtered down
 * A decoded JPEG is cached as <stem>.rgb565 beside it when the card is
 * writable, so the second showing is a straight read.
 */
bool photo_load(const char *path, uint8_t *dst);

/* Decode a JPEG already in memory. For the boot self-test. */
bool photo_from_jpeg(const uint8_t *jpeg, size_t len, uint8_t *dst, int *src_w, int *src_h);
