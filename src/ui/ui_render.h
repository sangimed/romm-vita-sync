#ifndef UI_RENDER_H
#define UI_RENDER_H

#include "ui_common.h"

void ui_truncate_text(const char *source, char *out_text, size_t out_size);
void ui_truncate_text_to_width(const char *source, float scale, float max_width, char *out_text, size_t out_size);
void ui_mask_secret(const char *secret, char *out_masked, size_t out_size);
void ui_format_field_display(const char *value, int secret, char *out_text, size_t out_size);

float ui_snap_to_pixel(float value);
float ui_snap_to_text_grid(float value, float scale);
float ui_quantize_text_scale(float scale);
float ui_resolve_text_scale(float scale);
unsigned int ui_text_shadow_color(unsigned int text_color);

void ui_begin_frame(void);
void ui_end_frame(void);
void ui_draw_text(float x, float y, unsigned int color, float scale, const char *format, ...);
float ui_estimate_text_width(const char *text, float scale);
float ui_estimate_text_height(float scale);
void ui_draw_text_right(float right_x, float y, unsigned int color, float scale, const char *text);
void ui_draw_text_center(float center_x, float y, unsigned int color, float scale, const char *text);
void ui_draw_truncated_text(float x, float y, float max_width, unsigned int color, float scale, const char *text);
void ui_draw_truncated_text_right(float right_x, float y, float max_width, unsigned int color, float scale, const char *text);
float ui_draw_wrapped_text_block(float x, float y, float max_width, unsigned int color, float scale, float line_spacing, int max_lines, const char *text);

void ui_draw_panel(float x, float y, float w, float h, unsigned int fill, unsigned int border);
void ui_draw_progress_bar(float x, float y, float w, float h, float ratio);
void ui_draw_field_row(float x, float y, float w, float h, int selected, const char *label, const char *value);
void ui_draw_button(float x, float y, float w, float h, int primary, int selected, int enabled, const char *title);
void ui_draw_game_row(float x, float y, float w, float h, int focused, int checked, const char *title, int card_count);

unsigned int ui_log_line_color(const char *line);
int ui_log_viewport_visible_lines(float h);
int ui_log_total_visual_lines(float viewport_width);
void ui_draw_log_viewport(float x, float y, float w, float h, int start_visual_line);

int ui_wrap_text_lines(const char *text, float scale, float max_width, char out_lines[][UI_WRAP_BUFFER_LEN], int max_lines, int append_ellipsis_on_truncate);

int ui_renderer_init(void);
void ui_renderer_term(void);

#endif
