idf_component_register(SRCS "app_main.c" "audio_io.c" "recognizer.c" "book.c" "cards.c" "player.c" "sdcard.c" "button.c" "sdlog.c" "pcf85063.c" "timesync.c" "photo.c" "fonts/lv_font_montserrat_72.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "testclips/dog.pcm" "testclips/cat.pcm" "testclips/ball.pcm" "../assets/photos/baby.jpg")
