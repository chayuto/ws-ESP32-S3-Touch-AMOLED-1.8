idf_component_register(SRCS "app_main.c" "audio_io.c" "recognizer.c" "display.c" "sdcard.c" "button.c"
                            "sdlog.c" "pcf85063.c" "timesync.c" "wifi_sta.c" "maint.c" "devcmd.c"
                            "clog.c" "pmu.c" "thermal.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "maint.html")
