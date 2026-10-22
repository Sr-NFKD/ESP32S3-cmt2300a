idf_component_register(SRCS "rf_ui.c" "fonts/rf_font_menu_32.c" "fonts/rf_font_debug_22.c"
    INCLUDE_DIRS "include" REQUIRES lvgl__lvgl rf_protocol)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
