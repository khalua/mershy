# REQUIRES esp_lcd_touch/driver added by hand: upstream gets esp_lcd_touch
# from its idf_component.yml, which was removed when vendoring (see
# components/README.md).
idf_component_register(SRCS "esp_lcd_touch_cst9217.c" INCLUDE_DIRS "include" REQUIRES "esp_lcd" "esp_lcd_touch" "driver")
