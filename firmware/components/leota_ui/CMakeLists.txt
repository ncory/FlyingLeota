# The touchscreen from ui/, unchanged, with its fonts. LVGL comes from the component
# registry (idf_component.yml) and is set up by sdkconfig.defaults, not ui/lv_conf.h.
set(ui ${CMAKE_CURRENT_LIST_DIR}/../../../ui)
idf_component_register(
    SRCS ${ui}/src/ui.cpp
         ${ui}/fonts/leota_inter_14.c ${ui}/fonts/leota_inter_16.c ${ui}/fonts/leota_inter_16_semi.c
         ${ui}/fonts/leota_inter_20_bold.c ${ui}/fonts/leota_inter_28_bold.c
    INCLUDE_DIRS ${ui}/include
    REQUIRES leota_core lvgl__lvgl)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
