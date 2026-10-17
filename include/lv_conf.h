#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (128U * 1024U)
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY __attribute__((section(".large_buffer"), aligned(32)))
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
/* Out-of-memory and NULL-parameter checks halt in apx_lvgl_assert() (records file/line, keeps USB alive). */
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_ASSERT_HANDLER { extern void apx_lvgl_assert(const char *file, int line); apx_lvgl_assert(__FILE__, __LINE__); }
#define LV_USE_DRAW_SW 1
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_BUTTON 1
#define LV_USE_LABEL 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_BUILD_DEMOS 1
#define LV_USE_DEMO_WIDGETS 1
#define LV_USE_DEMO_BENCHMARK 1

#endif
