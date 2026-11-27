#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define TFT_EC11_RGB565(r, g, b)                                                                   \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define TFT_EC11_BLACK 0x0000
#define TFT_EC11_WHITE 0xFFFF
#define TFT_EC11_RED 0xF800
#define TFT_EC11_GREEN 0x07E0
#define TFT_EC11_BLUE 0x001F
#define TFT_EC11_CYAN 0x07FF
#define TFT_EC11_MAGENTA 0xF81F
#define TFT_EC11_YELLOW 0xFFE0

  typedef enum
  {
    TFT_EC11_PRESS_DOWN,
    TFT_EC11_PRESS_SHORT,
    TFT_EC11_PRESS_MEDIUM,
    TFT_EC11_PRESS_LONG
  } tft_ec11_press_t;

  //-- Native bitmap font faces. Prefer the TFT_EC11_FONT_<matrix> names, which
  //-- are named after the character cell (the glyph bitmap is 1 px narrower
  //-- than the cell: the extra column is the inter-character gap). Each face
  //-- stores its own glyph bitmaps and is drawn pixel-accurate at its own
  //-- size; glyphs are never scaled or interpolated.
  //--   6x8    = 5x7 glyph     8x12   = 7x12 glyph    9x16   = 8x16 glyph
  //-- The legacy size names are kept as aliases for the same values and must
  //-- keep the original enum values for backwards compatibility.
  typedef enum
  {
    TFT_EC11_FONT_6x8 = 0,
    TFT_EC11_FONT_8x12 = 1,
    TFT_EC11_FONT_9x16 = 2,
    TFT_EC11_FONT_SMALL = TFT_EC11_FONT_6x8,
    TFT_EC11_FONT_MEDIUM = TFT_EC11_FONT_8x12,
    TFT_EC11_FONT_LARGE = TFT_EC11_FONT_9x16
  } tft_ec11_font_t;
  typedef enum
  {
    TFT_EC11_EVENT_ROTATE,
    TFT_EC11_EVENT_ENCODER_BUTTON,
    TFT_EC11_EVENT_AUX_BUTTON
  } tft_ec11_event_type_t;
  typedef struct
  {
    tft_ec11_event_type_t type;
    int32_t steps;
    tft_ec11_press_t press;
    uint32_t duration_ms;
  } tft_ec11_event_t;
  typedef struct
  {
    uint16_t background;
    uint16_t foreground;
    uint8_t font_scale;
  } tft_ec11_text_style_t;

  esp_err_t tft_ec11_init(void);
  esp_err_t tft_ec11_deinit(void);
  esp_err_t tft_ec11_set_rotation(uint8_t rotation);
  esp_err_t tft_ec11_set_inversion(bool enabled);
  esp_err_t tft_ec11_set_backlight(bool enabled);
  uint16_t tft_ec11_width(void);
  uint16_t tft_ec11_height(void);
  void tft_ec11_set_background(uint16_t color);
  void tft_ec11_set_text_style(uint16_t foreground, uint8_t scale);
  tft_ec11_text_style_t tft_ec11_get_text_style(void);
  esp_err_t tft_ec11_clear(void);
  esp_err_t tft_ec11_refresh(const uint16_t* frame_rgb565);
  esp_err_t tft_ec11_fill_rect(int x, int y, int width, int height, uint16_t color);
  esp_err_t tft_ec11_draw_text_simple(int x, int y, const char* text);
  esp_err_t tft_ec11_draw_text_styled(int x, int y, const char* text, uint16_t background,
                                      uint8_t font_scale, uint16_t foreground);
  //-- Same as tft_ec11_draw_text_styled with a bold option: every lit pixel
  //-- of the scalable 6x8 face is also drawn one pixel to the right, so the
  //-- strokes read a pixel heavier (the extra pixel may take the
  //-- inter-character gap). bold false keeps the plain rendering.
  esp_err_t tft_ec11_draw_text_styled_bold(int x, int y, const char* text, uint16_t background,
                                           uint8_t font_scale, uint16_t foreground, bool bold);
  esp_err_t tft_ec11_draw_text(int x, int y, size_t slot_chars, const char* text);
  esp_err_t tft_ec11_draw_text_font(int x, int y, const char* text, tft_ec11_font_t font,
                                    uint16_t background, uint16_t foreground);
  //-- Same as tft_ec11_draw_text_font with a bold option: every run of lit
  //-- glyph pixels is also drawn one pixel to the right, so the native faces
  //-- read a pixel heavier (the extra pixel may take the 1 px
  //-- inter-character gap). bold false keeps the plain rendering.
  esp_err_t tft_ec11_draw_text_font_bold(int x, int y, const char* text, tft_ec11_font_t font,
                                         uint16_t background, uint16_t foreground, bool bold);

//-- Pixel-height text API (independent of the enum faces above). The
//-- requested height is the number of rows of the glyph canvas in physical
//-- TFT pixels, 10 through 18 px inclusive; 11 px is the recommended default
//-- size. Every character of one face shares its baseline, canvas height
//-- and monospaced advance (the advance includes the spacing columns). The
//-- canvas includes room for descenders; the character cell is exactly
//-- height_px tall, so add your own vertical spacing between lines. The
//-- same UTF-8/Latin-1 accent folding and the same hollow-box fallback
//-- glyph as the native faces are used; on the 14 through 18 px faces the
//-- lowercase letters fold onto uppercase like on the legacy 5x7 face.
#define TFT_EC11_FONT_MIN_HEIGHT_PX 10
#define TFT_EC11_FONT_MAX_HEIGHT_PX 18

  typedef struct
  {
    uint8_t advance_px;
    uint8_t cell_height_px;
  } tft_ec11_font_metrics_t;

  //-- Query the monospaced advance and cell height of one supported pixel
  //-- height. Works before tft_ec11_init(). Returns ESP_ERR_INVALID_ARG for
  //-- a height outside TFT_EC11_FONT_MIN_HEIGHT_PX..TFT_EC11_FONT_MAX_HEIGHT_PX
  //-- or a null metrics pointer.
  esp_err_t tft_ec11_get_font_metrics_px(int height_px, tft_ec11_font_metrics_t* metrics);

  //-- Draw text with the bitmap face of the requested pixel height. x, y is
  //-- the top-left corner of the first cell. The draw first clears exactly
  //-- the cells it occupies (decoded character count x advance) with the
  //-- background colour, then paints the lit glyph pixels with the foreground
  //-- colour. An empty string, a null text pointer and an unsupported height
  //-- return ESP_ERR_INVALID_ARG before any LCD transfer; text drawn entirely
  //-- outside the display returns ESP_ERR_INVALID_ARG like the existing
  //-- native entry points, and genuine LCD transfer failures are propagated.
  //-- Each draw clears only its own cells: to replace a longer label with a
  //-- shorter one, clear the previous bounding rectangle first using
  //-- tft_ec11_get_font_metrics_px() and tft_ec11_fill_rect().
  esp_err_t tft_ec11_draw_text_px(int x, int y, const char* text, int height_px,
                                  uint16_t background, uint16_t foreground);

  //-- Same as tft_ec11_draw_text_px with a bold option: every run of lit
  //-- glyph pixels is also drawn one pixel to the right, so the faces read a
  //-- pixel heavier; the extra pixel stays inside the cell, including its
  //-- spacing columns, and the advance and height stay stable. bold false
  //-- keeps the plain rendering.
  esp_err_t tft_ec11_draw_text_px_bold(int x, int y, const char* text, int height_px,
                                       uint16_t background, uint16_t foreground, bool bold);

  esp_err_t tft_ec11_inputs_start(size_t queue_length);
  void tft_ec11_inputs_stop(void);
  QueueHandle_t tft_ec11_event_queue(void);
  bool tft_ec11_get_event(tft_ec11_event_t* event, TickType_t wait);

#ifdef __cplusplus
}
#endif
