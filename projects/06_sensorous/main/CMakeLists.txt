idf_component_register(SRCS "app_main.c" "display.c" "sdcard.c" "sdlog.c" "button.c" "devcmd.c"
                            "pcf85063.c" "timesync.c" "wifi_sta.c" "maint.c"
                            "pmu.c" "thermal.c"
                            "imu.c" "sound.c" "wifiscan.c" "blescan.c" "census.c" "srec.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "maint.html")
