# Vendored components

`esp_lcd_touch` (espressif, v1.2.1) and `esp_lcd_touch_cst9217` (waveshare,
v1.0.4), copied from the ESP component registry with their
`idf_component.yml` files removed.

**Why they aren't normal managed dependencies:** adding any dependency to
`src/idf_component.yml` makes the component manager re-solve every
dependency. PlatformIO pins that tool to 1.5.x, and during the re-solve it
crashes parsing `lvgl/lvgl` 9.6.0's registry metadata, which uses
`$CONFIG{...}` in dependency rules
(`pyparsing.exceptions.ParseException: ... found '$'`). Keeping these local
leaves `src/idf_component.yml` identical to what `dependencies.lock` was
solved against, so no re-solve happens.

`esp_lvgl_port` compiles in its touch support whenever a component named
`esp_lcd_touch` is in the build, so the local copy is enough.
