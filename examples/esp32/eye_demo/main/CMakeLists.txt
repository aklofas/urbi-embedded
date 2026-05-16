# eye_demo main component.
#
# SRCS list grows phase-by-phase:
#   Phase 3:               eye_camera.c
#   Phase 4 (this commit): eye_display.c
#   Phase 5:               eye_button.c, eye_demo_main.c (provides app_main)

idf_component_register(
    SRCS
        "eye_camera.c"
        "eye_display.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        urbi
        espressif__esp32-camera
        freertos
        esp_timer
        esp_lcd
        driver
)
