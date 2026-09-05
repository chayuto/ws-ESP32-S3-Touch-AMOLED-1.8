# Photo → screen buffer for the 368x448 AMOLED: JPEG decode (esp_new_jpeg), scale to
# fit with tinted bands, RGB565 out, optional on-card cache. Shared by every project
# in this repo that shows pictures. Add ../../components to EXTRA_COMPONENT_DIRS.
idf_component_register(SRCS "photo.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__esp_new_jpeg esp_timer)
