#include "ui_render.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vita2d.h>

#include "app_log.h"
#include "ui_common.h"

extern vita2d_pvf *g_ui_font;
extern vita2d_texture *g_ui_logo;
extern int g_common_dialog_active;

void ui_truncate_text(const char *source, char *out_text, size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(source)) {
    snprintf(out_text, out_size, "(empty)");
    return;
  }

  size_t source_len = strlen(source);
  if (source_len < out_size) {
    snprintf(out_text, out_size, "%s", source);
    return;
  }

  if (out_size <= 4U) {
    out_text[0] = '\0';
    return;
  }

  size_t keep = out_size - 4U;
  memcpy(out_text, source, keep);
  out_text[keep] = '.';
  out_text[keep + 1U] = '.';
  out_text[keep + 2U] = '.';
  out_text[keep + 3U] = '\0';
}

void ui_truncate_text_to_width(
    const char *source,
    float scale,
    float max_width,
    char *out_text,
    size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  out_text[0] = '\0';
  if (!has_text(source) || (max_width <= 0.0f)) {
    return;
  }

  snprintf(out_text, out_size, "%s", source);
  if (ui_estimate_text_width(out_text, scale) <= max_width) {
    return;
  }

  if (ui_estimate_text_width("...", scale) > max_width) {
    return;
  }

  while (has_text(out_text)) {
    size_t length = strlen(out_text);
    if (length == 0U) {
      break;
    }

    length -= 1U;
    while ((length > 0U) && (((unsigned char)out_text[length] & 0xC0U) == 0x80U)) {
      length -= 1U;
    }
    out_text[length] = '\0';

    char candidate[UI_EDITOR_BUFFER_LEN];
    snprintf(candidate, sizeof(candidate), "%s...", out_text);
    if (ui_estimate_text_width(candidate, scale) <= max_width) {
      snprintf(out_text, out_size, "%s", candidate);
      return;
    }
  }

  snprintf(out_text, out_size, "...");
}

void ui_mask_secret(const char *secret, char *out_masked, size_t out_size) {
  if ((out_masked == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(secret)) {
    snprintf(out_masked, out_size, "(empty)");
    return;
  }

  size_t length = strlen(secret);
  if (length >= out_size) {
    length = out_size - 1U;
  }

  memset(out_masked, '*', length);
  out_masked[length] = '\0';
}

void ui_format_field_display(const char *value, int secret, char *out_text, size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(value)) {
    snprintf(out_text, out_size, "Not configured");
    return;
  }

  if (secret) {
    ui_mask_secret(value, out_text, out_size);
    return;
  }

  snprintf(out_text, out_size, "%s", value);
}

float ui_snap_to_pixel(float value) {
  return floorf(value + 0.5f);
}

float ui_snap_to_text_grid(float value, float scale) {
  (void)scale;
  return ui_snap_to_pixel(value);
}

float ui_quantize_text_scale(float scale) {
  if (scale <= 0.0f) {
    return UI_TEXT_SCALE_MIN;
  }

  float stepped = floorf((scale / UI_TEXT_SCALE_STEP) + 0.5f) * UI_TEXT_SCALE_STEP;
  if (stepped < UI_TEXT_SCALE_MIN) {
    return UI_TEXT_SCALE_MIN;
  }
  if (stepped > UI_TEXT_SCALE_MAX) {
    return UI_TEXT_SCALE_MAX;
  }
  return stepped;
}

unsigned int ui_text_shadow_color(unsigned int text_color) {
  unsigned int alpha = (text_color >> 24) & 0xFFU;
  if (alpha == 0U) {
    return 0U;
  }

  unsigned int shadow_alpha = alpha / 3U;
  if (shadow_alpha < UI_TEXT_SHADOW_ALPHA_MIN) {
    shadow_alpha = UI_TEXT_SHADOW_ALPHA_MIN;
  }
  if (shadow_alpha > UI_TEXT_SHADOW_ALPHA_MAX) {
    shadow_alpha = UI_TEXT_SHADOW_ALPHA_MAX;
  }

  return RGBA8(0, 0, 0, shadow_alpha);
}

float ui_resolve_text_scale(float scale) {
  float resolved = scale * UI_TEXT_SCALE_BOOST;
  if (resolved < UI_TEXT_SCALE_MIN) {
    resolved = UI_TEXT_SCALE_MIN;
  }
  if (resolved > UI_TEXT_SCALE_MAX) {
    resolved = UI_TEXT_SCALE_MAX;
  }
  return ui_quantize_text_scale(resolved);
}

void ui_begin_frame(void) {
  vita2d_start_drawing();
  vita2d_clear_screen();

  vita2d_draw_rectangle(0.0f, 0.0f, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT, UI_COLOR_BACKGROUND);
  vita2d_draw_rectangle(0.0f, 78.0f, UI_SCREEN_WIDTH, 418.0f, UI_COLOR_BACKGROUND_ALT);
  vita2d_draw_rectangle(0.0f, 0.0f, UI_SCREEN_WIDTH, 78.0f, UI_COLOR_HEADER);
  vita2d_draw_rectangle(0.0f, 0.0f, UI_SCREEN_WIDTH, 2.0f, UI_COLOR_ACCENT);
  vita2d_draw_rectangle(0.0f, 72.0f, 560.0f, 3.0f, UI_COLOR_ACCENT);
  vita2d_draw_rectangle(560.0f, 72.0f, 400.0f, 3.0f, UI_COLOR_GOLD);
  vita2d_draw_rectangle(0.0f, 78.0f, UI_SCREEN_WIDTH, 1.0f, UI_COLOR_PANEL_BORDER);
  vita2d_draw_rectangle(0.0f, 496.0f, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - 496.0f, UI_COLOR_FOOTER);
  vita2d_draw_rectangle(0.0f, 496.0f, UI_SCREEN_WIDTH, 2.0f, UI_COLOR_PANEL_BORDER);
  vita2d_draw_rectangle(0.0f, 495.0f, UI_SCREEN_WIDTH, 1.0f, UI_COLOR_PANEL_BORDER);
}

void ui_end_frame(void) {
  vita2d_end_drawing();
  if (g_common_dialog_active) {
    vita2d_common_dialog_update();
  }
  vita2d_swap_buffers();
}

/*
 * Draws the packaged LiveArea icon as a compact brand mark, with a geometric
 * fallback so the header still has visual weight if the PNG cannot be loaded.
 */
void ui_draw_brand_mark(float x, float y, float size) {
  if (size <= 0.0f) {
    return;
  }

  if (g_ui_logo == NULL) {
    ui_draw_panel(x, y, size, size, UI_COLOR_FIELD_ACTIVE, UI_COLOR_ACCENT);
    vita2d_draw_rectangle(x + 8.0f, y + 8.0f, size - 16.0f, 3.0f, UI_COLOR_MAGENTA);
    vita2d_draw_rectangle(x + 8.0f, y + 17.0f, size - 16.0f, 3.0f, UI_COLOR_GOLD);
    vita2d_draw_rectangle(x + 8.0f, y + 26.0f, size - 16.0f, 3.0f, UI_COLOR_ACCENT);
    return;
  }

  float scale = size / 128.0f;
  vita2d_draw_rectangle(
      ui_snap_to_pixel(x + 3.0f),
      ui_snap_to_pixel(y + 4.0f),
      ui_snap_to_pixel(size),
      ui_snap_to_pixel(size),
      UI_COLOR_PANEL_SHADOW);
  vita2d_draw_texture_tint_scale(g_ui_logo, ui_snap_to_pixel(x), ui_snap_to_pixel(y), scale, scale, UI_COLOR_TEXT);
}

static void ui_draw_check_lines(float x, float y, float size, unsigned int color) {
  float left_x = x + (size * 0.24f);
  float mid_x = x + (size * 0.43f);
  float right_x = x + (size * 0.80f);
  float low_y = y + (size * 0.62f);
  float mid_y = y + (size * 0.78f);
  float high_y = y + (size * 0.30f);

  vita2d_draw_line(left_x, low_y, mid_x, mid_y, color);
  vita2d_draw_line(left_x, low_y + 1.0f, mid_x, mid_y + 1.0f, color);
  vita2d_draw_line(mid_x, mid_y, right_x, high_y, color);
  vita2d_draw_line(mid_x, mid_y + 1.0f, right_x, high_y + 1.0f, color);
}

void ui_draw_icon(UiIcon icon, float center_x, float center_y, float size, unsigned int color) {
  if ((icon == UI_ICON_NONE) || (size <= 0.0f)) {
    return;
  }

  float x = ui_snap_to_pixel(center_x - (size * 0.5f));
  float y = ui_snap_to_pixel(center_y - (size * 0.5f));
  float w = ui_snap_to_pixel(size);
  float h = ui_snap_to_pixel(size);
  float cx = ui_snap_to_pixel(center_x);
  float cy = ui_snap_to_pixel(center_y);

  switch (icon) {
    case UI_ICON_SYNC:
      vita2d_draw_line(x + (w * 0.24f), cy, x + (w * 0.46f), y + (h * 0.22f), color);
      vita2d_draw_line(x + (w * 0.46f), y + (h * 0.22f), x + (w * 0.76f), y + (h * 0.22f), color);
      vita2d_draw_line(x + (w * 0.76f), y + (h * 0.22f), x + (w * 0.66f), y + (h * 0.12f), color);
      vita2d_draw_line(x + (w * 0.76f), y + (h * 0.22f), x + (w * 0.66f), y + (h * 0.34f), color);
      vita2d_draw_line(x + (w * 0.76f), cy, x + (w * 0.54f), y + (h * 0.78f), color);
      vita2d_draw_line(x + (w * 0.54f), y + (h * 0.78f), x + (w * 0.24f), y + (h * 0.78f), color);
      vita2d_draw_line(x + (w * 0.24f), y + (h * 0.78f), x + (w * 0.34f), y + (h * 0.66f), color);
      vita2d_draw_line(x + (w * 0.24f), y + (h * 0.78f), x + (w * 0.34f), y + (h * 0.90f), color);
      break;
    case UI_ICON_STACK:
      vita2d_draw_line(x + (w * 0.20f), y + (h * 0.34f), cx, y + (h * 0.18f), color);
      vita2d_draw_line(cx, y + (h * 0.18f), x + (w * 0.80f), y + (h * 0.34f), color);
      vita2d_draw_line(x + (w * 0.20f), y + (h * 0.50f), cx, y + (h * 0.66f), color);
      vita2d_draw_line(cx, y + (h * 0.66f), x + (w * 0.80f), y + (h * 0.50f), color);
      vita2d_draw_line(x + (w * 0.24f), y + (h * 0.60f), cx, y + (h * 0.78f), color);
      vita2d_draw_line(cx, y + (h * 0.78f), x + (w * 0.76f), y + (h * 0.60f), color);
      break;
    case UI_ICON_SEARCH:
      vita2d_draw_line(x + (w * 0.20f), y + (h * 0.20f), x + (w * 0.56f), y + (h * 0.20f), color);
      vita2d_draw_line(x + (w * 0.20f), y + (h * 0.20f), x + (w * 0.20f), y + (h * 0.56f), color);
      vita2d_draw_line(x + (w * 0.56f), y + (h * 0.20f), x + (w * 0.56f), y + (h * 0.56f), color);
      vita2d_draw_line(x + (w * 0.20f), y + (h * 0.56f), x + (w * 0.56f), y + (h * 0.56f), color);
      vita2d_draw_line(x + (w * 0.52f), y + (h * 0.52f), x + (w * 0.82f), y + (h * 0.82f), color);
      break;
    case UI_ICON_SETTINGS:
      vita2d_draw_fill_circle(cx, cy, w * 0.22f, color);
      vita2d_draw_fill_circle(cx, cy, w * 0.11f, UI_COLOR_PANEL);
      vita2d_draw_rectangle(cx - 1.0f, y + (h * 0.10f), 2.0f, h * 0.20f, color);
      vita2d_draw_rectangle(cx - 1.0f, y + (h * 0.70f), 2.0f, h * 0.20f, color);
      vita2d_draw_rectangle(x + (w * 0.10f), cy - 1.0f, w * 0.20f, 2.0f, color);
      vita2d_draw_rectangle(x + (w * 0.70f), cy - 1.0f, w * 0.20f, 2.0f, color);
      vita2d_draw_line(x + (w * 0.22f), y + (h * 0.22f), x + (w * 0.34f), y + (h * 0.34f), color);
      vita2d_draw_line(x + (w * 0.78f), y + (h * 0.22f), x + (w * 0.66f), y + (h * 0.34f), color);
      vita2d_draw_line(x + (w * 0.22f), y + (h * 0.78f), x + (w * 0.34f), y + (h * 0.66f), color);
      vita2d_draw_line(x + (w * 0.78f), y + (h * 0.78f), x + (w * 0.66f), y + (h * 0.66f), color);
      break;
    case UI_ICON_CHECK:
      ui_draw_check_lines(x, y, w, color);
      break;
    case UI_ICON_GAMEPAD:
      vita2d_draw_line(x + (w * 0.16f), y + (h * 0.36f), x + (w * 0.84f), y + (h * 0.36f), color);
      vita2d_draw_line(x + (w * 0.16f), y + (h * 0.70f), x + (w * 0.84f), y + (h * 0.70f), color);
      vita2d_draw_line(x + (w * 0.16f), y + (h * 0.36f), x + (w * 0.16f), y + (h * 0.70f), color);
      vita2d_draw_line(x + (w * 0.84f), y + (h * 0.36f), x + (w * 0.84f), y + (h * 0.70f), color);
      vita2d_draw_rectangle(x + (w * 0.28f), y + (h * 0.48f), w * 0.18f, 2.0f, color);
      vita2d_draw_rectangle(x + (w * 0.35f), y + (h * 0.42f), 2.0f, h * 0.18f, color);
      vita2d_draw_fill_circle(x + (w * 0.64f), y + (h * 0.48f), w * 0.04f, color);
      vita2d_draw_fill_circle(x + (w * 0.73f), y + (h * 0.56f), w * 0.04f, color);
      break;
    case UI_ICON_INFO:
      vita2d_draw_fill_circle(cx, cy, w * 0.42f, color);
      vita2d_draw_fill_circle(cx, cy, w * 0.33f, UI_COLOR_FOOTER);
      vita2d_draw_rectangle(cx - 1.0f, y + (h * 0.42f), 2.0f, h * 0.28f, color);
      vita2d_draw_rectangle(cx - 1.0f, y + (h * 0.28f), 2.0f, 2.0f, color);
      break;
    case UI_ICON_STATUS:
      vita2d_draw_fill_circle(cx, cy, w * 0.30f, color);
      break;
    case UI_ICON_OPEN:
      vita2d_draw_rectangle(x + (w * 0.18f), y + (h * 0.26f), w * 0.46f, 2.0f, color);
      vita2d_draw_rectangle(x + (w * 0.18f), y + (h * 0.26f), 2.0f, h * 0.56f, color);
      vita2d_draw_rectangle(x + (w * 0.18f), y + (h * 0.80f), w * 0.56f, 2.0f, color);
      vita2d_draw_rectangle(x + (w * 0.72f), y + (h * 0.50f), 2.0f, h * 0.32f, color);
      vita2d_draw_line(x + (w * 0.48f), y + (h * 0.52f), x + (w * 0.82f), y + (h * 0.18f), color);
      vita2d_draw_line(x + (w * 0.62f), y + (h * 0.18f), x + (w * 0.82f), y + (h * 0.18f), color);
      vita2d_draw_line(x + (w * 0.82f), y + (h * 0.18f), x + (w * 0.82f), y + (h * 0.38f), color);
      break;
    case UI_ICON_BACK:
      vita2d_draw_line(x + (w * 0.72f), cy, x + (w * 0.28f), cy, color);
      vita2d_draw_line(x + (w * 0.28f), cy, x + (w * 0.46f), y + (h * 0.30f), color);
      vita2d_draw_line(x + (w * 0.28f), cy, x + (w * 0.46f), y + (h * 0.70f), color);
      break;
    default:
      break;
  }
}

void ui_draw_text(float x, float y, unsigned int color, float scale, const char *format, ...) {
  if ((g_ui_font == NULL) || !has_text(format)) {
    return;
  }

  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  float draw_scale = ui_resolve_text_scale(scale);
  float draw_x = ui_snap_to_text_grid(x, draw_scale);
  float draw_y = ui_snap_to_text_grid(y, draw_scale);

  unsigned int shadow_color = (draw_scale >= UI_TEXT_SHADOW_MIN_SCALE) ? ui_text_shadow_color(color) : 0U;
  if (shadow_color != 0U) {
    vita2d_pvf_draw_text(
        g_ui_font,
        ui_snap_to_text_grid(draw_x + UI_TEXT_SHADOW_OFFSET_X, draw_scale),
        ui_snap_to_text_grid(draw_y + UI_TEXT_SHADOW_OFFSET_Y, draw_scale),
        shadow_color,
        draw_scale,
        line);
  }

  vita2d_pvf_draw_text(g_ui_font, draw_x, draw_y, color, draw_scale, line);
}

float ui_estimate_text_width(const char *text, float scale) {
  if ((g_ui_font == NULL) || !has_text(text)) {
    return 0.0f;
  }
  return (float)vita2d_pvf_text_width(g_ui_font, ui_resolve_text_scale(scale), text);
}

float ui_estimate_text_height(float scale) {
  if (g_ui_font == NULL) {
    return 18.0f * ui_resolve_text_scale(scale);
  }

  int width = 0;
  int height = 0;
  vita2d_pvf_text_dimensions(g_ui_font, ui_resolve_text_scale(scale), "Ag", &width, &height);
  if (height <= 0) {
    return 18.0f * ui_resolve_text_scale(scale);
  }

  return (float)height;
}

void ui_draw_text_right(float right_x, float y, unsigned int color, float scale, const char *text) {
  if (!has_text(text)) {
    return;
  }

  float width = ui_estimate_text_width(text, scale);
  ui_draw_text(right_x - width, y, color, scale, "%s", text);
}

void ui_draw_text_center(float center_x, float y, unsigned int color, float scale, const char *text) {
  if (!has_text(text)) {
    return;
  }

  float width = ui_estimate_text_width(text, scale);
  ui_draw_text(center_x - (width * 0.5f), y, color, scale, "%s", text);
}

void ui_draw_truncated_text(
    float x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    const char *text) {
  char truncated[UI_EDITOR_BUFFER_LEN];
  ui_truncate_text_to_width(text, scale, max_width, truncated, sizeof(truncated));
  if (!has_text(truncated)) {
    return;
  }

  ui_draw_text(x, y, color, scale, "%s", truncated);
}

void ui_draw_truncated_text_right(
    float right_x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    const char *text) {
  char truncated[UI_EDITOR_BUFFER_LEN];
  ui_truncate_text_to_width(text, scale, max_width, truncated, sizeof(truncated));
  if (!has_text(truncated)) {
    return;
  }

  ui_draw_text_right(right_x, y, color, scale, truncated);
}

static void ui_wrap_store_line(
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int *io_line_count,
    const char *line) {
  if (io_line_count == NULL) {
    return;
  }

  if ((out_lines != NULL) && (*io_line_count >= 0) && (*io_line_count < max_lines)) {
    snprintf(out_lines[*io_line_count], UI_WRAP_BUFFER_LEN, "%s", has_text(line) ? line : "");
  }
  *io_line_count += 1;
}

static void ui_wrap_append_ellipsis(char *line, float scale, float max_width) {
  if (line == NULL) {
    return;
  }

  if (!has_text(line)) {
    snprintf(line, UI_WRAP_BUFFER_LEN, "...");
    return;
  }

  char candidate[UI_WRAP_BUFFER_LEN];
  snprintf(candidate, sizeof(candidate), "%s...", line);
  while (has_text(line) && (ui_estimate_text_width(candidate, scale) > max_width)) {
    size_t length = strlen(line);
    if (length == 0U) {
      break;
    }
    line[length - 1U] = '\0';
    snprintf(candidate, sizeof(candidate), "%s...", line);
  }

  char final_line[UI_WRAP_BUFFER_LEN];
  snprintf(final_line, sizeof(final_line), "%s...", line);
  snprintf(line, UI_WRAP_BUFFER_LEN, "%s", final_line);
}

static void ui_wrap_push_word(
    const char *word,
    float scale,
    float max_width,
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int *io_line_count,
    char *current_line,
    size_t current_line_size) {
  if (!has_text(word) || (io_line_count == NULL) || (current_line == NULL) || (current_line_size == 0U)) {
    return;
  }

  char remaining[UI_WRAP_BUFFER_LEN];
  snprintf(remaining, sizeof(remaining), "%s", word);

  while (has_text(remaining)) {
    char candidate[UI_WRAP_BUFFER_LEN];
    if (has_text(current_line)) {
      snprintf(candidate, sizeof(candidate), "%s %s", current_line, remaining);
    } else {
      snprintf(candidate, sizeof(candidate), "%s", remaining);
    }

    if (ui_estimate_text_width(candidate, scale) <= max_width) {
      snprintf(current_line, current_line_size, "%s", candidate);
      return;
    }

    if (has_text(current_line)) {
      ui_wrap_store_line(out_lines, max_lines, io_line_count, current_line);
      current_line[0] = '\0';
      continue;
    }

    char chunk[UI_WRAP_BUFFER_LEN];
    size_t chunk_len = 0U;
    chunk[0] = '\0';

    while (remaining[chunk_len] != '\0') {
      char next_chunk[UI_WRAP_BUFFER_LEN];
      snprintf(next_chunk, sizeof(next_chunk), "%s%c", chunk, remaining[chunk_len]);
      if ((chunk[0] != '\0') && (ui_estimate_text_width(next_chunk, scale) > max_width)) {
        break;
      }

      snprintf(chunk, sizeof(chunk), "%s", next_chunk);
      chunk_len += 1U;
    }

    if (remaining[chunk_len] != '\0') {
      ui_wrap_store_line(out_lines, max_lines, io_line_count, chunk);
      memmove(remaining, remaining + chunk_len, strlen(remaining + chunk_len) + 1U);
      continue;
    }

    snprintf(current_line, current_line_size, "%s", chunk);
    return;
  }
}

int ui_wrap_text_lines(
    const char *text,
    float scale,
    float max_width,
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int append_ellipsis_on_truncate) {
  if (!has_text(text) || (max_lines <= 0) || (max_width <= 0.0f)) {
    return 0;
  }

  int line_count = 0;
  char current_line[UI_WRAP_BUFFER_LEN];
  current_line[0] = '\0';

  const char *cursor = text;
  while (*cursor != '\0') {
    if (*cursor == '\n') {
      ui_wrap_store_line(out_lines, max_lines, &line_count, current_line);
      current_line[0] = '\0';
      cursor += 1;
      continue;
    }

    while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r')) {
      cursor += 1;
    }
    if (*cursor == '\0') {
      break;
    }
    if (*cursor == '\n') {
      continue;
    }

    char word[UI_WRAP_BUFFER_LEN];
    size_t word_len = 0U;
    while ((*cursor != '\0') &&
           (*cursor != '\n') &&
           (*cursor != ' ') &&
           (*cursor != '\t') &&
           (*cursor != '\r')) {
      if ((word_len + 1U) < sizeof(word)) {
        word[word_len++] = *cursor;
      }
      cursor += 1;
    }
    word[word_len] = '\0';

    ui_wrap_push_word(word, scale, max_width, out_lines, max_lines, &line_count, current_line, sizeof(current_line));
  }

  if (has_text(current_line)) {
    ui_wrap_store_line(out_lines, max_lines, &line_count, current_line);
  }

  if (append_ellipsis_on_truncate && (line_count > max_lines) && (out_lines != NULL)) {
    ui_wrap_append_ellipsis(out_lines[max_lines - 1], scale, max_width);
  }

  return line_count;
}

float ui_draw_wrapped_text_block(
    float x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    float line_spacing,
    int max_lines,
    const char *text) {
  if (!has_text(text) || (max_lines <= 0)) {
    return y;
  }

  char lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
  int total_lines = ui_wrap_text_lines(text, scale, max_width, lines, max_lines, 1);
  int render_lines = total_lines;
  if (render_lines > max_lines) {
    render_lines = max_lines;
  }

  float line_height = ui_estimate_text_height(scale) + line_spacing;
  for (int i = 0; i < render_lines; ++i) {
    ui_draw_text(x, y + (line_height * (float)i), color, scale, "%s", lines[i]);
  }

  return y + (line_height * (float)render_lines);
}

void ui_draw_panel(float x, float y, float w, float h, unsigned int fill, unsigned int border) {
  if ((w <= 0.0f) || (h <= 0.0f)) {
    return;
  }

  x = ui_snap_to_pixel(x);
  y = ui_snap_to_pixel(y);
  w = ui_snap_to_pixel(w);
  h = ui_snap_to_pixel(h);

  vita2d_draw_rectangle(x + 2.0f, y + 2.0f, w, h, UI_COLOR_PANEL_SHADOW);
  vita2d_draw_rectangle(x, y, w, h, fill);
  if ((w > 4.0f) && (h > 4.0f)) {
    vita2d_draw_rectangle(x + 1.0f, y + 1.0f, w - 2.0f, 1.0f, UI_COLOR_PANEL_HIGHLIGHT);
  }
  vita2d_draw_rectangle(x, y, w, 1.0f, border);
  vita2d_draw_rectangle(x, y + h - 1.0f, w, 1.0f, border);
  vita2d_draw_rectangle(x, y, 1.0f, h, border);
  vita2d_draw_rectangle(x + w - 1.0f, y, 1.0f, h, border);
}

void ui_draw_progress_bar(float x, float y, float w, float h, float ratio) {
  if ((w <= 0.0f) || (h <= 0.0f)) {
    return;
  }

  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }

  ui_draw_panel(x, y, w, h, UI_COLOR_FIELD, UI_COLOR_PANEL_BORDER);
  float fill_width = (w - 2.0f) * ratio;
  if (fill_width < 0.0f) {
    fill_width = 0.0f;
  }

  if (fill_width > 0.0f) {
    vita2d_draw_rectangle(
        ui_snap_to_pixel(x + 1.0f),
        ui_snap_to_pixel(y + 1.0f),
        ui_snap_to_pixel(fill_width),
        ui_snap_to_pixel(h - 2.0f),
        UI_COLOR_ACCENT);
    vita2d_draw_rectangle(
        ui_snap_to_pixel(x + 1.0f),
        ui_snap_to_pixel(y + 1.0f),
        ui_snap_to_pixel(fill_width),
        2.0f,
        UI_COLOR_GOLD_SOFT);
  }
}

unsigned int ui_log_line_color(const char *line) {
  if (!has_text(line)) {
    return UI_COLOR_TEXT_DIM;
  }

  if (strncmp(line, "[ERROR]", 7) == 0) {
    return UI_COLOR_DANGER;
  }
  if (strncmp(line, "[WARN]", 6) == 0) {
    return UI_COLOR_WARNING;
  }
  if (strncmp(line, "[INFO]", 6) == 0) {
    return UI_COLOR_TEXT;
  }
  return UI_COLOR_TEXT_MUTED;
}

int ui_log_viewport_visible_lines(float h) {
  float usable_height = h - UI_LOG_TOP_PADDING - UI_LOG_BOTTOM_PADDING;
  if (usable_height < UI_LOG_LINE_HEIGHT) {
    return 1;
  }

  int visible_lines = (int)floorf(usable_height / UI_LOG_LINE_HEIGHT);
  if (visible_lines < 1) {
    visible_lines = 1;
  }
  return visible_lines;
}

int ui_log_total_visual_lines(float viewport_width) {
  float inner_width = viewport_width - 20.0f;
  if (inner_width <= 0.0f) {
    return 0;
  }

  int total_visual_lines = 0;
  int total_entries = app_log_history_count();
  for (int i = 0; i < total_entries; ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char wrapped_lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
    int wrapped_count = ui_wrap_text_lines(line, UI_LOG_TEXT_SCALE, inner_width, wrapped_lines, UI_WRAP_MAX_LINES_PER_BLOCK, 0);
    total_visual_lines += (wrapped_count > 0) ? wrapped_count : 1;
  }

  return total_visual_lines;
}

void ui_draw_log_viewport(
    float x,
    float y,
    float w,
    float h,
    int start_visual_line) {
  ui_draw_panel(x, y, w, h, UI_COLOR_PANEL_ALT, UI_COLOR_PANEL_BORDER);

  int visible_lines = ui_log_viewport_visible_lines(h);
  int total_visual_lines = ui_log_total_visual_lines(w);
  int max_start = total_visual_lines - visible_lines;
  if (max_start < 0) {
    max_start = 0;
  }

  int start = clamp_int(start_visual_line, 0, max_start);
  int end = start + visible_lines;

  float line_y = y + UI_LOG_TOP_PADDING;
  int visual_index = 0;
  int rendered_lines = 0;
  int total_entries = app_log_history_count();
  for (int i = 0; (i < total_entries) && (visual_index < end); ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char wrapped_lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
    int wrapped_count = ui_wrap_text_lines(line, UI_LOG_TEXT_SCALE, w - 20.0f, wrapped_lines, UI_WRAP_MAX_LINES_PER_BLOCK, 0);
    if (wrapped_count <= 0) {
      continue;
    }

    for (int j = 0; (j < wrapped_count) && (visual_index < end); ++j) {
      if (visual_index >= start) {
        ui_draw_text(x + 10.0f, line_y, ui_log_line_color(line), UI_LOG_TEXT_SCALE, "%s", wrapped_lines[j]);
        line_y += UI_LOG_LINE_HEIGHT;
        rendered_lines += 1;
        if (rendered_lines >= visible_lines) {
          return;
        }
      }
      visual_index += 1;
    }
  }
}

void ui_draw_field_row(float x, float y, float w, float h, int selected, const char *label, const char *value) {
  unsigned int fill = selected ? UI_COLOR_FIELD_ACTIVE : UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int value_color = has_text(value) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  ui_draw_panel(x, y, w, h, fill, border);
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
    vita2d_draw_rectangle(ui_snap_to_pixel(x + 4.0f), ui_snap_to_pixel(y), 2.0f, ui_snap_to_pixel(h), UI_COLOR_MAGENTA);
  }

  float inner_x = x + 14.0f;
  float inner_w = w - 28.0f;
  if (h < 42.0f) {
    char compact_label[96];
    char compact_value[UI_EDITOR_BUFFER_LEN];
    float label_scale = 0.78f;
    float value_scale = 0.84f;
    float label_w = ui_estimate_text_width(label, label_scale) + 12.0f;
    if (label_w > (inner_w * 0.38f)) {
      label_w = inner_w * 0.38f;
    }
    ui_truncate_text_to_width(label, label_scale, label_w, compact_label, sizeof(compact_label));
    ui_truncate_text_to_width(value, value_scale, inner_w - label_w, compact_value, sizeof(compact_value));
    ui_draw_text(inner_x, y + (h * 0.64f), UI_COLOR_TEXT_DIM, label_scale, "%s", compact_label);
    ui_draw_truncated_text(inner_x + label_w, y + (h * 0.64f), inner_w - label_w, value_color, value_scale, compact_value);
    return;
  }

  float label_scale = (h >= 52.0f) ? 0.78f : 0.78f;
  float value_scale = (h >= 52.0f) ? 0.88f : 0.84f;
  float label_y = y + ((h >= 52.0f) ? 16.0f : 14.0f);
  float value_y = y + ((h >= 52.0f) ? 32.0f : 29.0f);
  float line_spacing = (h >= 52.0f) ? 1.0f : 0.0f;
  ui_draw_truncated_text(inner_x, label_y, inner_w, UI_COLOR_TEXT_DIM, label_scale, label);
  ui_draw_wrapped_text_block(inner_x, value_y, inner_w, value_color, value_scale, line_spacing, 2, value);
}

void ui_draw_button_with_icon(
    float x,
    float y,
    float w,
    float h,
    int primary,
    int selected,
    int enabled,
    UiIcon icon,
    const char *title) {
  unsigned int fill = UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int text_color = enabled ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;
  unsigned int icon_color = enabled ? (primary ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED) : UI_COLOR_TEXT_DIM;

  if (primary) {
    fill = enabled ? (selected ? UI_COLOR_BUTTON_ACTIVE : UI_COLOR_BUTTON) : UI_COLOR_BUTTON_DISABLED;
    border = enabled ? (selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_BUTTON_BORDER) : UI_COLOR_PANEL_BORDER;
    text_color = enabled ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED;
  } else if (selected) {
    fill = UI_COLOR_FIELD_ACTIVE;
  }

  ui_draw_panel(x, y, w, h, fill, border);
  if (primary && enabled) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x + 1.0f), ui_snap_to_pixel(y + 1.0f), ui_snap_to_pixel(w - 2.0f), 2.0f, UI_COLOR_GOLD_SOFT);
  }
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
    vita2d_draw_rectangle(ui_snap_to_pixel(x + w - 4.0f), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), primary ? UI_COLOR_GOLD : UI_COLOR_MAGENTA);
  }

  char display_title[128];
  float title_scale = primary ? 0.86f : 0.82f;
  if (icon == UI_ICON_NONE) {
    ui_truncate_text_to_width(title, title_scale, w - 18.0f, display_title, sizeof(display_title));
    ui_draw_text_center(x + (w * 0.5f), y + (h * 0.66f), text_color, title_scale, display_title);
    return;
  }

  float icon_size = h * 0.42f;
  float icon_x = x + 30.0f;
  float text_x = x + 54.0f;
  float text_w = w - 64.0f;
  ui_draw_icon(icon, icon_x, y + (h * 0.50f), icon_size, icon_color);
  ui_truncate_text_to_width(title, title_scale, text_w, display_title, sizeof(display_title));
  ui_draw_truncated_text(text_x, y + (h * 0.66f), text_w, text_color, title_scale, display_title);
}

void ui_draw_button(float x, float y, float w, float h, int primary, int selected, int enabled, const char *title) {
  ui_draw_button_with_icon(x, y, w, h, primary, selected, enabled, UI_ICON_NONE, title);
}

/*
 * Renders selected game rows without relying on a text glyph, keeping the
 * checkbox crisp at small Vita UI sizes.
 */
static void ui_draw_check_mark(float x, float y, float size, unsigned int color) {
  ui_draw_check_lines(x, y, size, color);
}

void ui_draw_game_row(
    float x,
    float y,
    float w,
    float h,
    int focused,
    int checked,
    const char *title,
    int card_count) {
  unsigned int fill = focused ? UI_COLOR_FIELD_ACTIVE : (checked ? UI_COLOR_ACCENT_SOFT : UI_COLOR_PANEL_ALT);
  unsigned int border = focused ? UI_COLOR_PANEL_BORDER_ACTIVE : (checked ? UI_COLOR_BUTTON_BORDER : UI_COLOR_PANEL_BORDER);
  unsigned int title_color = focused ? UI_COLOR_TEXT : (checked ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED);
  unsigned int count_color = checked ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  ui_draw_panel(x, y, w, h, fill, border);
  if (checked) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 3.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
    vita2d_draw_rectangle(ui_snap_to_pixel(x + 3.0f), ui_snap_to_pixel(y), 2.0f, ui_snap_to_pixel(h), UI_COLOR_MAGENTA);
  }

  float checkbox_size = h - 16.0f;
  if (checkbox_size < 12.0f) {
    checkbox_size = 12.0f;
  }
  float checkbox_x = x + 10.0f;
  float checkbox_y = y + ((h - checkbox_size) * 0.5f);
  ui_draw_panel(
      checkbox_x,
      checkbox_y,
      checkbox_size,
      checkbox_size,
      checked ? UI_COLOR_BUTTON : UI_COLOR_FIELD,
      checked ? UI_COLOR_BUTTON_BORDER : UI_COLOR_PANEL_BORDER);
  if (checked) {
    ui_draw_check_mark(checkbox_x, checkbox_y, checkbox_size, UI_COLOR_TEXT);
  }

  char count_text[32];
  snprintf(count_text, sizeof(count_text), "%d card%s", card_count, (card_count == 1) ? "" : "s");

  float count_scale = 0.80f;
  float count_width = ui_estimate_text_width(count_text, count_scale);
  float title_x = checkbox_x + checkbox_size + 14.0f;
  float title_width = (x + w - 12.0f) - title_x - count_width;
  if (title_width < 80.0f) {
    title_width = 80.0f;
  }

  ui_draw_truncated_text(title_x, y + (h * 0.66f), title_width, title_color, 0.86f, title);
  ui_draw_text_right(x + w - 12.0f, y + (h * 0.66f), count_color, count_scale, count_text);
}

int ui_renderer_init(void) {
  int status = vita2d_init();
  if (status < 0) {
    return status;
  }

  vita2d_system_pvf_config latin_font_config = {SCE_PVF_LANGUAGE_LATIN, NULL};
  g_ui_font = vita2d_load_system_pvf(1, &latin_font_config);
  if (g_ui_font == NULL) {
    g_ui_font = vita2d_load_default_pvf();
  }
  if (g_ui_font == NULL) {
    vita2d_fini();
    return -1;
  }

  g_ui_logo = vita2d_load_PNG_file("app0:/sce_sys/icon0.png");

  return 0;
}

void ui_renderer_term(void) {
  vita2d_wait_rendering_done();
  if (g_ui_font != NULL) {
    vita2d_free_pvf(g_ui_font);
    g_ui_font = NULL;
  }
  if (g_ui_logo != NULL) {
    vita2d_free_texture(g_ui_logo);
    g_ui_logo = NULL;
  }
  vita2d_fini();
}
