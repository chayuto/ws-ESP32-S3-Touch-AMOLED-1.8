idf_component_register(SRCS "app_main.c" "audio_io.c" "recognizer.c" "book.c" "cards.c" "player.c" "sdcard.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "testclips/dog.pcm" "testclips/cat.pcm" "testclips/ball.pcm")
