#include "clog.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "pmu.h"
#include "sdcard.h"
#include "sdlog.h"
#include "timesync.h"

/* Every record starts with wall time and uptime so lines can be joined to the text log. */
static int head(char *buf, size_t len, const char *type)
{
    char now[32];
    return snprintf(buf, len, "{\"t\":\"%s\",\"time\":\"%s\",\"up_ms\":%lld", type, timesync_now_str(now, sizeof(now)),
                    (long long)(esp_timer_get_time() / 1000));
}

void clog_session(const char *const *words, size_t count, int min_prob_pct, int sound_prob_pct)
{
    char buf[1024];
    const esp_app_desc_t *app = esp_app_get_description();
    int n = head(buf, sizeof(buf), "session");
    n += snprintf(buf + n, sizeof(buf) - n, ",\"fw\":\"%s\",\"min_pct\":%d,\"sound_pct\":%d,\"words\":[", app->version,
                  min_prob_pct, sound_prob_pct);
    for (size_t i = 0; i < count && n < (int)sizeof(buf) - 40; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\"", i ? "," : "", words[i]);
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");
    sdlog_aux_write(0, buf);
}

void clog_detection(const clog_cand_t *cands, int n_cands, int vad_speech, float volume_dbfs, const char *verdict,
                    uint32_t frame)
{
    char buf[512];
    int n = head(buf, sizeof(buf), "det");
    n += snprintf(buf + n, sizeof(buf) - n, ",\"frame\":%lu,\"vad\":%d,\"vol_dbfs\":%.1f,\"verdict\":\"%s\",\"cands\":[",
                  (unsigned long)frame, vad_speech, volume_dbfs, verdict);
    for (int i = 0; i < n_cands && n < (int)sizeof(buf) - 60; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"id\":%d,\"w\":\"%s\",\"p\":%.3f}", i ? "," : "", cands[i].id,
                      cands[i].text ? cands[i].text : "?", cands[i].prob);
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");
    sdlog_aux_write(0, buf);
}

void clog_env(float mic_avg_dbfs, float mic_peak_dbfs, int speech_pct)
{
    char buf[256];
    int n = head(buf, sizeof(buf), "env");
    pmu_status_t ps = {0};
    pmu_read(&ps);
    snprintf(buf + n, sizeof(buf) - n,
             ",\"mic_avg\":%.1f,\"mic_peak\":%.1f,\"speech_pct\":%d,\"internal\":%u,\"internal_min\":%u,\"psram\":%u,\"card\":%d"
             ",\"usb\":%d,\"vbat_mv\":%u,\"vsys_mv\":%u,\"charging\":%d}",
             mic_avg_dbfs, mic_peak_dbfs, speech_pct, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             sdcard_present(), ps.vbus_in, ps.vbat_mv, ps.vsys_mv, ps.charging);
    sdlog_aux_write(0, buf);
}

/* JSON string escaping - raw_string comes straight from the engine and is not trusted
 * to be free of quotes or control bytes. A malformed record breaks every downstream
 * reader, which is exactly when you need the records most. */
static int esc(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (in == NULL) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, cap - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return (int)o;
}

void clog_decode(const char *why, const char *string, const char *raw_string, int num, float top_prob,
                 int vad_speech, float volume_dbfs, uint32_t decode_ms, const char *final_text)
{
    char buf[640];
    char e_str[288], e_raw[288], e_fin[128];
    esc(e_str, sizeof(e_str), string);
    esc(e_raw, sizeof(e_raw), raw_string);
    esc(e_fin, sizeof(e_fin), final_text);
    int n = head(buf, sizeof(buf), "decode");
    snprintf(buf + n, sizeof(buf) - n,
             ",\"why\":\"%s\",\"num\":%d,\"top_prob\":%.3f,\"vad\":%d,\"vol_dbfs\":%.1f,\"decode_ms\":%lu"
             ",\"string\":\"%s\",\"raw_string\":\"%s\",\"text\":\"%s\",\"internal\":%u}",
             why ? why : "?", num, top_prob, vad_speech, volume_dbfs, (unsigned long)decode_ms, e_str, e_raw, e_fin,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    sdlog_aux_write(0, buf);
}
