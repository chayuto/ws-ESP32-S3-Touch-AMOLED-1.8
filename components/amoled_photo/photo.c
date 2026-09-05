#include "photo.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "photo";

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/*
 * Scale src (sw x sh, RGB565) into dst (PHOTO_W x PHOTO_H) so the whole photo is
 * visible: a landscape shot sits in the middle with bands above and below, a
 * portrait one fills the card. The bands take the photo's own average colour,
 * darkened, so they read as background rather than letterbox. Each destination
 * pixel averages its source box; integer arithmetic, one pass.
 */
static void fit(const uint16_t *src, int sw, int sh, uint16_t *dst)
{
    /* Inner rectangle: as wide as the card, or as tall, whichever fits. */
    int dw = PHOTO_W, dh = (int)((int64_t)sh * PHOTO_W / sw);
    if (dh > PHOTO_H) {
        dh = PHOTO_H;
        dw = (int)((int64_t)sw * PHOTO_H / sh);
    }
    int ox = (PHOTO_W - dw) / 2, oy = (PHOTO_H - dh) / 2;

    uint64_t tr = 0, tg = 0, tb = 0;
    for (int y = 0; y < dh; y++) {
        int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t *out = dst + (oy + y) * PHOTO_W + ox;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++) {
                const uint16_t *row = src + sy * sw;
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    uint16_t p = row[sx];
                    r += (p >> 11) & 31;
                    g += (p >> 5) & 63;
                    b += p & 31;
                    n++;
                }
            }
            if (n == 0) n = 1;
            r /= n; g /= n; b /= n;
            tr += r; tg += g; tb += b;
            out[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    /* Bands: average colour at 35% brightness. */
    uint64_t cnt = (uint64_t)dw * dh;
    uint16_t bg = (uint16_t)((((tr / cnt) * 35 / 100) << 11) | (((tg / cnt) * 35 / 100) << 5) | ((tb / cnt) * 35 / 100));
    for (int y = 0; y < PHOTO_H; y++) {
        uint16_t *row = dst + y * PHOTO_W;
        if (y < oy || y >= oy + dh) {
            for (int x = 0; x < PHOTO_W; x++) row[x] = bg;
        } else {
            for (int x = 0; x < ox; x++) row[x] = bg;
            for (int x = ox + dw; x < PHOTO_W; x++) row[x] = bg;
        }
    }
}

bool photo_from_jpeg(const uint8_t *jpeg, size_t len, uint8_t *dst, int *src_w, int *src_h)
{
    int64_t t0 = esp_timer_get_time();
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "decoder open failed");
        return false;
    }
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpeg, .inbuf_len = (int)len};
    jpeg_dec_header_info_t hdr = {0};
    bool ok = false;
    uint8_t *out = NULL;
    if (jpeg_dec_parse_header(dec, &io, &hdr) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "not a JPEG the decoder understands");
        goto done;
    }
    /* Decode at the largest reduction that still covers the card. Scale targets must be
     * multiples of 8; the decoder does 1/2, 1/4, 1/8. */
    int div = 1;
    while (div < 8 && hdr.width / (div * 2) >= PHOTO_W && hdr.height / (div * 2) >= PHOTO_H) {
        div *= 2;
    }
    int dw = hdr.width, dh = hdr.height;
    if (div > 1) {
        jpeg_dec_close(dec);
        cfg.scale.width = (hdr.width / div) & ~7;
        cfg.scale.height = (hdr.height / div) & ~7;
        dw = cfg.scale.width;
        dh = cfg.scale.height;
        if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
            return false;
        }
        io.inbuf = (uint8_t *)jpeg;
        io.inbuf_len = (int)len;
        if (jpeg_dec_parse_header(dec, &io, &hdr) != JPEG_ERR_OK) {
            goto done;
        }
    }
    int out_len = 0;
    jpeg_dec_get_outbuf_len(dec, &out_len);
    out = heap_caps_aligned_alloc(16, (size_t)out_len, MALLOC_CAP_SPIRAM);
    if (out == NULL) {
        ESP_LOGW(TAG, "no PSRAM for %d-byte decode", out_len);
        goto done;
    }
    io.outbuf = out;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "decode failed");
        goto done;
    }
    fit((const uint16_t *)out, dw, dh, (uint16_t *)dst);
    if (src_w) *src_w = hdr.width;
    if (src_h) *src_h = hdr.height;
    ESP_LOGI(TAG, "jpeg %ux%u -> 1/%d -> %dx%d in %lld ms", hdr.width, hdr.height, div, PHOTO_W, PHOTO_H,
             (long long)((esp_timer_get_time() - t0) / 1000));
    ok = true;
done:
    free(out);
    jpeg_dec_close(dec);
    return ok;
}

static bool load_raw(const char *path, uint8_t *dst)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t got = fread(dst, 1, PHOTO_BYTES, f);
    fclose(f);
    if (got != PHOTO_BYTES) {
        ESP_LOGW(TAG, "%s: %u bytes, expected %u", path, (unsigned)got, (unsigned)PHOTO_BYTES);
        return false;
    }
    return true;
}

static void cache_write(const char *cache_path, const uint8_t *src)
{
    char tmp[112];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return;
    }
    bool ok = fwrite(src, 1, PHOTO_BYTES, f) == PHOTO_BYTES;
    fclose(f);
    if (ok && rename(tmp, cache_path) == 0) {
        ESP_LOGI(TAG, "cached %s", cache_path);
    } else {
        unlink(tmp);
    }
}

bool photo_load(const char *path, uint8_t *dst)
{
    int64_t t0 = esp_timer_get_time();
    if (ends_with(path, ".rgb565")) {
        bool ok = load_raw(path, dst);
        if (ok) {
            ESP_LOGI(TAG, "%s in %lld ms", path, (long long)((esp_timer_get_time() - t0) / 1000));
        }
        return ok;
    }
    if (!(ends_with(path, ".jpg") || ends_with(path, ".jpeg"))) {
        ESP_LOGW(TAG, "%s: not a .jpg or .rgb565", path);
        return false;
    }

    /* A cached conversion that is at least as new as the JPEG wins. Caches from the
     * earlier crop-to-fill version were <stem>.rgb565; they are removed on sight. */
    char cache[112], legacy[112];
    const char *dot = strrchr(path, '.');
    snprintf(cache, sizeof(cache), "%.*s.fit.rgb565", (int)(dot - path), path);
    snprintf(legacy, sizeof(legacy), "%.*s.rgb565", (int)(dot - path), path);
    if (unlink(legacy) == 0) {
        ESP_LOGI(TAG, "removed old crop cache %s", legacy);
    }
    struct stat sj, sc;
    if (stat(cache, &sc) == 0 && stat(path, &sj) == 0 && sc.st_mtime >= sj.st_mtime) {
        if (load_raw(cache, dst)) {
            ESP_LOGI(TAG, "%s (cached) in %lld ms", path, (long long)((esp_timer_get_time() - t0) / 1000));
            return true;
        }
    }

    size_t len = 0;
    uint8_t *jpeg = read_all(path, &len);
    if (jpeg == NULL) {
        ESP_LOGW(TAG, "cannot read %s", path);
        return false;
    }
    bool ok = photo_from_jpeg(jpeg, len, dst, NULL, NULL);
    free(jpeg);
    if (ok) {
        cache_write(cache, dst);
    }
    return ok;
}
