#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALBUM_ART_WIDTH  320
#define ALBUM_ART_HEIGHT 240
#define ALBUM_ART_BYTES  (ALBUM_ART_WIDTH * ALBUM_ART_HEIGHT * sizeof(uint16_t))

esp_err_t album_art_init(void);

/* Stage-prefetch artwork in background without changing the currently displayed artwork.
 * Returns a non-zero art_id token to pass later to album_art_commit(). */
uint32_t album_art_prepare(int station_index, const char *artist, const char *track);

/* Commit a previously prepared art_id to be displayed immediately (or as soon as ready). */
void album_art_commit(uint32_t art_id, int station_index);

/* Clear back to stylized fallback card for the given station. */
void album_art_clear(int station_index);

/* Current artwork version counter (increments whenever the displayed buffer updates). */
uint32_t album_art_get_version(void);

/* Thread-safe copy of the 320x240 RGB565 pixel buffer (little-endian uint16_t). */
bool album_art_copy_rgb565(uint16_t *dst_pixels, size_t dst_bytes, bool *out_is_cover, uint32_t *out_version);

#ifdef __cplusplus
}
#endif
