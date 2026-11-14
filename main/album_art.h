#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALBUM_ART_WIDTH  128
#define ALBUM_ART_HEIGHT 128
#define ALBUM_ART_BYTES  (ALBUM_ART_WIDTH * ALBUM_ART_HEIGHT * sizeof(uint16_t))

esp_err_t album_art_init(void);

/* Request artwork update for the current station/artist/track (safe to call repeatedly; deduplicated). */
void album_art_request(int station_index, const char *artist, const char *track);

/* Clear back to stylized fallback card for the given station. */
void album_art_clear(int station_index);

/* Current artwork version counter (increments whenever the buffer updates). */
uint32_t album_art_get_version(void);

/* Thread-safe copy of the 128x128 RGB565 pixel buffer (little-endian uint16_t). */
bool album_art_copy_rgb565(uint16_t *dst_pixels, size_t dst_bytes, bool *out_is_cover, uint32_t *out_version);

#ifdef __cplusplus
}
#endif
