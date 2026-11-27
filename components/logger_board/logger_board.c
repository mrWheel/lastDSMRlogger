//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.c - on-device TFT debug lines and user screens
//-- Date: 2026-11-27 · Build: 21
//--
//-- Everything is drawn through the documented API of the esp32_s3_piggyback
//-- component: tft_ec11_init once, then filled rectangles and fixed-width
//-- text. Every text on every screen is drawn in the component's bold
//-- rendering, so the strokes read a pixel heavier than the plain bitmaps.
//-- The component inputs are started so a small poll task can act on
//-- the AUX button, the encoder and the idle display time-out. Drawing is
//-- serialized with a mutex because WiFi callbacks, the receive task and the
//-- HTTP handlers all report state.
//--
//-- Before the first accepted telegram the panel shows the four debug lines.
//-- When the power screens become active the screen is fully cleared first.
//-- Every screen starts with one light blue heading row carrying the screen
//-- name left aligned and the screen function right aligned in the 16 px
//-- face, and ends with a light blue status row in the 11 px face carrying
//-- the accepted and rejected telegram counters; the body of every screen
//-- stays fully white with dark text. All screen text uses the component's
//-- pixel-height API (tft_ec11_draw_text_px_bold, heights 10..18 px). Nine
//-- screens exist: PHASE LOAD (one signed watt bar row per phase plus the
//-- total, the row header in the 14 px face and the bar staying a few
//-- pixels away from both panel edges, its scale following the ladder
//-- 100..18000 W), one PHASE POWER screen per phase plus a TOTAL screen
//-- (each showing the delivered and the returned watts of its channel as
//-- two large 18 px values under their left aligned captions) and three
//-- SYSTEM INFO screens (the device facts grouped in logical sections,
//-- every line in the 14 px face and a line too long for the panel split
//-- over two lines).
//-- The rotary encoder is the only means to walk through all screens one by
//-- one; the AUX button never switches a screen. It wakes a dark panel and,
//-- like every other physical button action, restarts the idle time-out.
//-- After the configured
//-- idle time (loggerBoardSetDisplayTimeoutMinutes, mirrored by the SPA
//-- display settings) the backlight goes off; an AUX press lights the panel
//-- again and restarts the timer. Every event on any button restarts the
//-- timer, whether the panel is lit or dark.
//--
//-- The stored display mode (the settings view's "Piggyback board" /
//-- "No Display" toggle, applied through loggerBoardSetEnabled) decides
//-- whether the board is driven at all: with "No Display" the panel is
//-- never initialised, the backlight stays off and the board inputs never
//-- start, and every function here is a harmless no-op. The mode may
//-- switch at runtime in both directions.
//-- ==========================================================================

#include "logger_board.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp32_s3_piggyback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logger_control.h"

static const char* tag = "loggerBoard";

//-- Scale 2 gives a 12 by 16 pixel cell: 25 characters fit on the 320 pixel
//-- wide panel with a small left margin.
#define BOARD_FONT_SCALE 2
#define BOARD_CHARS_PER_LINE 25
#define BOARD_LEFT_X 4
#define BOARD_LINE_PITCH (8 * BOARD_FONT_SCALE)

#define BOARD_LINE_PARSER 1
#define BOARD_LINE_SCREEN 2
#define BOARD_LINE_P1 3
#define BOARD_LINE_WIFI 4
#define BOARD_LINE_IP 5

#define BOARD_TEXT_MAX 48

//-- --------------------------------------------------------------------------
//-- Power panel geometry (layout adapts to the panel orientation reported
//-- by the component: 320x240 landscape or 240x320 portrait)
//-- --------------------------------------------------------------------------

//-- Heading row (light blue, black screen name left aligned and the screen
//-- function right aligned), then four equal rows and, at the very bottom
//-- of every screen, a light blue status row carrying the accepted and
//-- rejected telegram counters; the body stays white.
//-- Within a row: header band and a centre-zero bar carrying its integer
//-- scale labels; the watt value sits at the bottom of the row. The heights
//-- add up exactly: header band 24 + bar 8 + watt row 16 = 48, the row
//-- height on the 240 pixel panel (240 - 24 - 24) / 4.
#define PANEL_NAME_H 24
#define PANEL_TITLE_H PANEL_NAME_H
#define PANEL_HEADER_H 24
#define PANEL_BAR_H 8
#define PANEL_WATT_H 16
#define PANEL_STATUS_H 24
#define PANEL_ROWS 4

#define PANEL_BAR_Y PANEL_HEADER_H
#define PANEL_EDGE_MARGIN 4

//-- The bar track and its fill keep this many pixels away from both panel
//-- edges, so a full-scale bar never touches the border of the screen.
#define PANEL_BAR_MARGIN 6

//-- All panel text uses the documented pixel-height API of the component:
//-- one requested glyph-canvas height in physical TFT pixels (10 through
//-- 18 px) picks its own bitmap strike; the cell is exactly that tall and
//-- the monospaced advance (including the spacing columns) comes from
//-- tft_ec11_get_font_metrics_px(). The heading row (screen name and
//-- function) uses the 16 px face, the bar row headers (current, phase
//-- label, voltage), the phase power captions and the system information
//-- lines the 14 px face, the row watt values the 12 px face, the status
//-- row the 11 px face, the bar scale labels the 10 px face and the phase
//-- power values the 18 px face. Every text is drawn through the
//-- component's bold rendering.
#define FONT_TITLE_PX 16
#define FONT_STAMP_PX 11
#define FONT_VALUE_PX 10
#define FONT_HEADER_INFO_PX 16
#define FONT_ROW_PX 14
#define FONT_SYSTEM_PX 14
#define FONT_WATT_PX 12
#define FONT_BIG_PX 18

//-- Bar scale ladder: the smallest step that fits the largest absolute
//-- watt of the frame. The steps roughly double and stay on round numbers,
//-- from 100 W up to 18000 W, so the top step covers the theoretical
//-- maximum of a three-phase 25 A / 230 V connection (3 x 25 x 230 =
//-- 17250 W); frames above 18000 W stay on that step and frames below
//-- 100 W stay on the 100 W step.
//-- Published for the SPA realtime view so its bars scale identically to
//-- the panel (logger_board.h).
const uint16_t loggerPowerScales[LOGGER_POWER_SCALE_COUNT] = {100u,  200u,  400u,   800u,  1500u,
                                                              3000u, 6000u, 12000u, 18000u};

//-- Panel colours (RGB565). The heading and status rows of every screen are
//-- light blue with black text; the body stays white with dark text.
#define PANEL_HEADER_COLOR TFT_EC11_RGB565(43, 60, 94)
#define PANEL_HEADING_COLOR TFT_EC11_RGB565(150, 200, 225)
#define PANEL_BAR_BG TFT_EC11_RGB565(232, 232, 236)
#define PANEL_TICK_COLOR TFT_EC11_RGB565(90, 90, 98)
#define PANEL_CONSUME_COLOR TFT_EC11_RGB565(255, 59, 48)
#define PANEL_PRODUCE_COLOR TFT_EC11_RGB565(34, 160, 60)

//-- Screens after the first accepted telegram; the rotary encoder walks
//-- through them in this order and nothing else: the bar screen, one power
//-- screen per phase plus the total and three system information screens.
typedef enum
{
  BOARD_SCREEN_PHASE_LOAD = 0,
  BOARD_SCREEN_PHASE_POWER_1,
  BOARD_SCREEN_PHASE_POWER_2,
  BOARD_SCREEN_PHASE_POWER_3,
  BOARD_SCREEN_PHASE_POWER_TOTAL,
  BOARD_SCREEN_SYSTEM_1,
  BOARD_SCREEN_SYSTEM_2,
  BOARD_SCREEN_SYSTEM_3,
  BOARD_SCREEN_COUNT
} BoardScreen;

//-- Idle time-out used until the stored settings apply their own value.
#define BOARD_DISPLAY_TIMEOUT_DEFAULT_MINUTES 10u

//-- The firmware version travels through the application's read-only
//-- getter (see app_main.c), like the other components read it.
extern const char* getAppVersion(void);

#define BOARD_INPUT_QUEUE_LEN 16u

//-- Landscape rotations of the component: the assembled orientation is the
//-- Kconfig default (3); rotation 1 is that same landscape turned 180
//-- degrees, so the flip only swaps the vertical mirror.
#define BOARD_ROTATION_NORMAL 3u
#define BOARD_ROTATION_UPSIDE_DOWN 1u

typedef struct
{
  bool available;
  bool prepared;
  bool enabled;
  bool panelActive;
  bool rotationUpsideDown;
  SemaphoreHandle_t mutex;
  SemaphoreHandle_t teardownAck;

  //-- Serialises the bring-up and tear-down paths (web save, telnet command
  //-- and the deferred "No Display" boot tear-down can otherwise run them
  //-- at the same time from different tasks).
  SemaphoreHandle_t lifecycle;
  TaskHandle_t inputTask;
  BoardScreen currentScreen;
  bool screenOn;
  TickType_t lastActivityTick;
  uint16_t displayTimeoutMinutes;
  loggerPowerSnapshot panel;
  char parserVersion[BOARD_TEXT_MAX];
  char screen[BOARD_TEXT_MAX];
  char p1Note[BOARD_TEXT_MAX];
  char wifiStatus[BOARD_TEXT_MAX];
  char wifiIp[BOARD_TEXT_MAX];
  char ssid[BOARD_TEXT_MAX];
  char ip[BOARD_TEXT_MAX];
} LoggerBoard;

static LoggerBoard board;

static const char* const phaseLabels[PANEL_ROWS] = {"Phase 1", "Phase 2", "Phase 3", "Total"};

//-- Heading names of the four power screens (one per phase plus the total).
static const char* const powerScreenNames[PANEL_ROWS] = {"PHASE 1", "PHASE 2", "PHASE 3", "TOTAL"};

static void drawLine(uint8_t row, const char* label, const char* value)
{
  char line[BOARD_CHARS_PER_LINE + 1];
  snprintf(line, sizeof line, "%s%s", label, value);

  tft_ec11_draw_text(BOARD_LEFT_X, (int)(row * BOARD_LINE_PITCH), BOARD_CHARS_PER_LINE, line);
}

static void refreshDebugLines(void)
{
  drawLine(BOARD_LINE_PARSER, "parser : ", board.parserVersion);
  drawLine(BOARD_LINE_SCREEN, "screen : ", board.screen);
  drawLine(BOARD_LINE_P1, "p1     : ", board.p1Note);
  drawLine(BOARD_LINE_WIFI, "wifi   : ", board.wifiStatus);
  drawLine(BOARD_LINE_IP, "ip     : ", board.wifiIp);
}

//-- --------------------------------------------------------------------------
//-- Power panel drawing
//-- --------------------------------------------------------------------------

//-- True colour for a signed watt value on the white screen body:
//-- consumption red, feed-in green and a measured zero black so it stays
//-- readable on white.
static uint16_t signedValueColor(float value)
{
  if (value < 0.0f)
  {
    return PANEL_CONSUME_COLOR;
  }

  if (value > 0.0f)
  {
    return PANEL_PRODUCE_COLOR;
  }

  return TFT_EC11_BLACK;
}

//-- Smallest ladder step that fits the largest absolute watt, so the bar
//-- scale follows the size of the consumption and feed-in of the frame.
//-- Also answers the scale for the SPA realtime view (logger_board.h).
uint16_t loggerBoardPickPowerScale(const loggerPowerSnapshot* snapshot)
{
  float maxAbs = 0.0f;
  const float values[PANEL_ROWS] = {snapshot->phaseWatts[0], snapshot->phaseWatts[1],
                                    snapshot->phaseWatts[2], snapshot->totalWatts};

  for (int index = 0; index < PANEL_ROWS; index++)
  {
    float magnitude = fabsf(values[index]);

    if (magnitude > maxAbs)
    {
      maxAbs = magnitude;
    }
  }

  for (size_t step = 0u; step < LOGGER_POWER_SCALE_COUNT; step++)
  {
    if (maxAbs <= (float)loggerPowerScales[step])
    {
      return loggerPowerScales[step];
    }
  }

  return loggerPowerScales[LOGGER_POWER_SCALE_COUNT - 1u];
}

//-- Monospaced advance of one pixel-height face from the component's
//-- metrics query (the advance already includes the spacing columns); the
//-- cell is exactly height_px tall, so a line height needs no lookup.
static int fontAdvancePx(int height_px)
{
  tft_ec11_font_metrics_t metrics = {0, 0};

  //-- Only an unsupported height fails (the panel uses fixed 10..18 px
  //-- faces); keep the layout maths sane with a safe estimate if it does.
  if (tft_ec11_get_font_metrics_px(height_px, &metrics) != ESP_OK)
  {
    return height_px;
  }

  return (int)metrics.advance_px;
}

//-- All panel text goes through the component's bold pixel-height
//-- rendering: every lit glyph run is drawn a second time one pixel to the
//-- right so the strokes read a pixel heavier. The extra pixel stays inside
//-- the cell (including its spacing columns), so the advance and the cell
//-- height stay exactly what tft_ec11_get_font_metrics_px() reports.
static void drawLeftText(int y, const char* text, int height_px, uint16_t background,
                         uint16_t foreground)
{
  tft_ec11_draw_text_px_bold(PANEL_EDGE_MARGIN, y, text, height_px, background, foreground, true);
}

static void drawCenteredText(int y, int panelWidth, const char* text, int height_px,
                             uint16_t background, uint16_t foreground)
{
  int x = (panelWidth - (int)strlen(text) * fontAdvancePx(height_px)) / 2;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_px_bold(x, y, text, height_px, background, foreground, true);
}

static void drawRightAlignedText(int y, int panelWidth, const char* text, int rightMargin,
                                 int height_px, uint16_t background, uint16_t foreground)
{
  int x = panelWidth - (int)strlen(text) * fontAdvancePx(height_px) - rightMargin;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_px_bold(x, y, text, height_px, background, foreground, true);
}

//-- Every screen starts with one light blue heading row: the screen name
//-- left aligned and the screen function right aligned, both in black on
//-- the light blue bar.
static void drawScreenTitle(int panelWidth, const char* name, const char* rightText)
{
  tft_ec11_fill_rect(0, 0, panelWidth, PANEL_NAME_H, PANEL_HEADING_COLOR);
  tft_ec11_draw_text_px_bold(PANEL_EDGE_MARGIN, (PANEL_NAME_H - FONT_TITLE_PX) / 2, name,
                             FONT_TITLE_PX, PANEL_HEADING_COLOR, TFT_EC11_BLACK, true);

  if ((rightText != NULL) && (rightText[0] != '\0'))
  {
    drawRightAlignedText((PANEL_NAME_H - FONT_HEADER_INFO_PX) / 2, panelWidth, rightText,
                         PANEL_EDGE_MARGIN, FONT_HEADER_INFO_PX, PANEL_HEADING_COLOR,
                         TFT_EC11_BLACK);
  }
}

//-- Centre-zero bar of the given height carrying its integer scale labels
//-- inside the bar. The whole track keeps PANEL_BAR_MARGIN pixels away from
//-- both panel edges, so even a full-scale bar never touches the border.
static void drawBar(int y, int panelWidth, float watts, uint16_t scale, int barHeight)
{
  int trackX = PANEL_BAR_MARGIN;
  int trackWidth = panelWidth - 2 * PANEL_BAR_MARGIN;
  int mid = trackX + trackWidth / 2;
  int halfSpan = trackWidth / 2 - 1;
  int labelY = y + (barHeight - FONT_VALUE_PX) / 2;
  char label[16];

  tft_ec11_fill_rect(trackX, y, trackWidth, barHeight, PANEL_BAR_BG);

  int half = (int)(fabsf(watts) * (float)halfSpan / (float)scale);

  if (half > halfSpan)
  {
    half = halfSpan;
  }

  if ((watts < 0.0f) && (half > 0))
  {
    //-- Consumption grows to the left of centre.
    tft_ec11_fill_rect(mid - half, y, half, barHeight, PANEL_CONSUME_COLOR);
  }
  else if ((watts > 0.0f) && (half > 0))
  {
    //-- Feed-in grows to the right of centre.
    tft_ec11_fill_rect(mid + 1, y, half, barHeight, PANEL_PRODUCE_COLOR);
  }

  //-- Scale labels as integers inside the bar so they sit in contrast on
  //-- the bar background and the coloured fill.
  snprintf(label, sizeof label, "-%" PRIu16, scale);
  tft_ec11_draw_text_px_bold(trackX + 2, labelY, label, FONT_VALUE_PX, PANEL_BAR_BG,
                             PANEL_TICK_COLOR, true);

  snprintf(label, sizeof label, "%" PRIu16, scale);
  drawRightAlignedText(labelY, panelWidth, label, PANEL_BAR_MARGIN + 2, FONT_VALUE_PX, PANEL_BAR_BG,
                       PANEL_TICK_COLOR);

  drawCenteredText(labelY, panelWidth, "0", FONT_VALUE_PX, PANEL_BAR_BG, PANEL_TICK_COLOR);

  //-- The centre tick stays on top so the zero label never covers it.
  tft_ec11_fill_rect(mid - 1, y, 2, barHeight, PANEL_TICK_COLOR);
}

static void drawPowerRow(int index, float watts, float amps, float volts, int y, int panelWidth,
                         int rowHeight, uint16_t scale)
{
  char text[24];
  int textY = y + (PANEL_HEADER_H - FONT_ROW_PX) / 2;
  int wattY = y + rowHeight - PANEL_WATT_H;
  int wattTextY = wattY + (PANEL_WATT_H - FONT_WATT_PX) / 2;

  //-- The whole row body is white: the header band, the bar margins and the
  //-- strip under the bar never show the cleared black background.
  tft_ec11_fill_rect(0, y, panelWidth, rowHeight, TFT_EC11_WHITE);

  //-- Header band on the white body: phase current left, voltage right and
  //-- the centred phase label, all in the 14 px face in black on white.

  snprintf(text, sizeof text, "%.1fA", (double)amps);
  drawLeftText(textY, text, FONT_ROW_PX, TFT_EC11_WHITE, TFT_EC11_BLACK);

  drawCenteredText(textY, panelWidth, phaseLabels[index], FONT_ROW_PX, TFT_EC11_WHITE,
                   TFT_EC11_BLACK);

  snprintf(text, sizeof text, "%.1fV", (double)volts);
  drawRightAlignedText(textY, panelWidth, text, PANEL_EDGE_MARGIN, FONT_ROW_PX, TFT_EC11_WHITE,
                       TFT_EC11_BLACK);

  drawBar(y + PANEL_BAR_Y, panelWidth, watts, scale, PANEL_BAR_H);

  //-- Full-width clear behind the watt row so a previous wider label never
  //-- leaves ghost pixels behind.
  tft_ec11_fill_rect(0, wattY, panelWidth, PANEL_WATT_H, TFT_EC11_WHITE);
  snprintf(text, sizeof text, "%.1f Watt", (double)watts);
  drawCenteredText(wattTextY, panelWidth, text, FONT_WATT_PX, TFT_EC11_WHITE,
                   signedValueColor(watts));

  //-- Thin divider under the row keeps the four white rows apart.
  tft_ec11_fill_rect(PANEL_EDGE_MARGIN, y + rowHeight - 1, panelWidth - 2 * PANEL_EDGE_MARGIN, 1,
                     PANEL_BAR_BG);
}

//-- Light blue status row at the very bottom of every screen: only the
//-- telegram counters, left aligned — the accepted telegrams and the ones
//-- received but rejected as erroneous. No date/time and no button hints.
static void drawStatusLine(int panelWidth, int panelHeight)
{
  int y = panelHeight - PANEL_STATUS_H;
  int textY = y + (PANEL_STATUS_H - FONT_STAMP_PX) / 2;
  char status[BOARD_TEXT_MAX + 24];

  snprintf(status, sizeof status, "Accepted %" PRIu32 " | Rejected %" PRIu32,
           board.panel.acceptedCount, board.panel.rejectedCount);

  int maxChars = (panelWidth - 2 * PANEL_EDGE_MARGIN) / fontAdvancePx(FONT_STAMP_PX);

  if ((maxChars >= 0) && ((int)strlen(status) > maxChars))
  {
    status[maxChars] = '\0';
  }

  tft_ec11_fill_rect(0, y, panelWidth, PANEL_STATUS_H, PANEL_HEADING_COLOR);
  drawLeftText(textY, status, FONT_STAMP_PX, PANEL_HEADING_COLOR, TFT_EC11_BLACK);
}

static void drawPanel(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  uint16_t scale = loggerBoardPickPowerScale(snapshot);
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  int rowHeight = (height - PANEL_TITLE_H - PANEL_STATUS_H) / PANEL_ROWS;

  //-- The heading row prints the screen name left and the screen function
  //-- right; the meter date/time travels on the status row instead.
  drawScreenTitle(width, "PHASE LOAD", "Watt bars");

  for (int row = 0; row < PANEL_ROWS; row++)
  {
    int y = PANEL_TITLE_H + row * rowHeight;
    float watts = (row < 3) ? snapshot->phaseWatts[row] : snapshot->totalWatts;
    float amps = (row < 3) ? snapshot->phaseAmps[row] : snapshot->totalAmps;
    float volts = (row < 3) ? snapshot->phaseVolts[row] : snapshot->totalVolts;

    drawPowerRow(row, watts, amps, volts, y, width, rowHeight, scale);
  }

  drawStatusLine(width, height);
}

//-- One half of a phase power screen: the caption left aligned a little off
//-- the left edge and the watt value in the large 18 px face centred below
//-- it, both in the channel colour (delivered red, returned green) on the
//-- white body.
static void drawPowerHalf(int y, int halfHeight, int panelWidth, const char* caption, float watts,
                          uint16_t color)
{
  char text[24];
  int stackHeight = FONT_ROW_PX + 8 + FONT_BIG_PX;
  int top = y + (halfHeight - stackHeight) / 2;

  drawLeftText(top, caption, FONT_ROW_PX, TFT_EC11_WHITE, color);

  snprintf(text, sizeof text, "%.1f W", (double)watts);
  drawCenteredText(top + FONT_ROW_PX + 8, panelWidth, text, FONT_BIG_PX, TFT_EC11_WHITE, color);
}

//-- Phase power screen: one per phase plus the total. The channel shows its
//-- delivered (import) and returned (export) watts as two large values
//-- stacked on the white body, the delivered value red and the returned
//-- value green.
static void drawPhasePowerView(BoardScreen screen)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  int powerIndex = (int)screen - (int)BOARD_SCREEN_PHASE_POWER_1;
  bool isTotal = (powerIndex == PANEL_ROWS - 1);
  float delivered =
      isTotal ? snapshot->totalDeliveredWatts : snapshot->phaseDeliveredWatts[powerIndex];
  float returned =
      isTotal ? snapshot->totalReturnedWatts : snapshot->phaseReturnedWatts[powerIndex];
  int halfHeight = (height - PANEL_TITLE_H - PANEL_STATUS_H) / 2;
  int midY = PANEL_TITLE_H + halfHeight;

  drawScreenTitle(width, powerScreenNames[powerIndex], "Import / export");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_WHITE);

  drawPowerHalf(PANEL_TITLE_H, halfHeight, width, "Delivered (Import)", delivered,
                PANEL_CONSUME_COLOR);
  drawPowerHalf(midY, halfHeight, width, "Returned (Export)", returned, PANEL_PRODUCE_COLOR);

  //-- Thin divider between the two halves keeps them apart.
  tft_ec11_fill_rect(PANEL_EDGE_MARGIN, midY - 1, width - 2 * PANEL_EDGE_MARGIN, 1, PANEL_BAR_BG);

  drawStatusLine(width, height);
}

//-- Line pitch of the system information screens: the 14 px cell plus a
//-- small gap, so every line stays on its own row.
#define SYSTEM_LINE_PITCH 16

//-- One group label of the system information screens: the section name in
//-- the dark navy tone, its facts in black on the white body below it.
static void drawSystemSection(int* y, const char* label)
{
  drawLeftText(*y, label, FONT_SYSTEM_PX, TFT_EC11_WHITE, PANEL_HEADER_COLOR);
  *y += SYSTEM_LINE_PITCH;
}

//-- One "name : value" line of the system information screens: a line too
//-- long for the panel is split over two lines at the last space before the
//-- edge, so no text ever wraps or runs off the screen mid word.
static void drawSystemLine(int* y, const char* text, int panelWidth)
{
  char first[64];
  int maxChars = (panelWidth - 2 * PANEL_EDGE_MARGIN) / fontAdvancePx(FONT_SYSTEM_PX);
  int split;

  if ((int)strlen(text) <= maxChars)
  {
    drawLeftText(*y, text, FONT_SYSTEM_PX, TFT_EC11_WHITE, TFT_EC11_BLACK);
    *y += SYSTEM_LINE_PITCH;
    return;
  }

  for (split = maxChars; split > 0; split--)
  {
    if (text[split] == ' ')
    {
      break;
    }
  }

  //-- No space to break at: keep the head of the single long word.
  if (split <= 0)
  {
    split = maxChars;
  }

  snprintf(first, sizeof first, "%.*s", split, text);
  drawLeftText(*y, first, FONT_SYSTEM_PX, TFT_EC11_WHITE, TFT_EC11_BLACK);
  *y += SYSTEM_LINE_PITCH;

  snprintf(first, sizeof first, "%.*s", maxChars, text + split + ((text[split] == ' ') ? 1 : 0));
  drawLeftText(*y, first, FONT_SYSTEM_PX, TFT_EC11_WHITE, TFT_EC11_BLACK);
  *y += SYSTEM_LINE_PITCH;
}

//-- System information screen one: the telegram counters and the P1 input
//-- profile, every line in the 14 px face.
static void drawSystemTelegramView(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  char line[64];
  int y = PANEL_TITLE_H + 2;

  drawScreenTitle(width, "SYSTEM INFO", "Reception");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_WHITE);

  drawSystemSection(&y, "TELEGRAMS");

  snprintf(line, sizeof line, "Accepted : %" PRIu32, snapshot->acceptedCount);
  drawSystemLine(&y, line, width);

  snprintf(line, sizeof line, "Rejected : %" PRIu32, snapshot->rejectedCount);
  drawSystemLine(&y, line, width);

  drawSystemSection(&y, "P1 INPUT");

  snprintf(line, sizeof line, "Country / profile : %s %s",
           (snapshot->countryCode[0] != '\0') ? snapshot->countryCode : "?",
           (snapshot->profileId[0] != '\0') ? snapshot->profileId : "?");
  drawSystemLine(&y, line, width);

  snprintf(line, sizeof line, "RX polarity : %s", snapshot->rxInverted ? "inverted" : "normal");
  drawSystemLine(&y, line, width);

  drawStatusLine(width, height);
}

//-- System information screen two: the meter clock and the network, every
//-- line in the 14 px face.
static void drawSystemNetworkView(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  char line[64];
  int y = PANEL_TITLE_H + 2;

  drawScreenTitle(width, "SYSTEM INFO", "Meter & net");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_WHITE);

  drawSystemSection(&y, "METER");

  snprintf(line, sizeof line, "Meter time : %s",
           (snapshot->meterTime[0] != '\0') ? snapshot->meterTime : "none");
  drawSystemLine(&y, line, width);

  drawSystemSection(&y, "NETWORK");

  snprintf(line, sizeof line, "SSID : %s", (board.ssid[0] != '\0') ? board.ssid : "none");
  drawSystemLine(&y, line, width);

  snprintf(line, sizeof line, "IP : %s", (board.ip[0] != '\0') ? board.ip : "none");
  drawSystemLine(&y, line, width);

  drawStatusLine(width, height);
}

//-- System information screen three: the device facts, every line in the
//-- 14 px face.
static void drawSystemDeviceView(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  char line[64];
  int y = PANEL_TITLE_H + 2;

  drawScreenTitle(width, "SYSTEM INFO", "Device");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_WHITE);

  drawSystemSection(&y, "DEVICE");

  snprintf(line, sizeof line, "Uptime : %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
           snapshot->uptimeSeconds / 86400u, (snapshot->uptimeSeconds / 3600u) % 24u,
           (snapshot->uptimeSeconds / 60u) % 60u, snapshot->uptimeSeconds % 60u);
  drawSystemLine(&y, line, width);

  snprintf(line, sizeof line, "Firmware : %s", getAppVersion());
  drawSystemLine(&y, line, width);

  drawStatusLine(width, height);
}

static void drawActiveView(void)
{
  if ((board.currentScreen >= BOARD_SCREEN_PHASE_POWER_1) &&
      (board.currentScreen <= BOARD_SCREEN_PHASE_POWER_TOTAL))
  {
    drawPhasePowerView(board.currentScreen);
    return;
  }

  if (board.currentScreen == BOARD_SCREEN_SYSTEM_1)
  {
    drawSystemTelegramView();
    return;
  }

  if (board.currentScreen == BOARD_SCREEN_SYSTEM_2)
  {
    drawSystemNetworkView();
    return;
  }

  if (board.currentScreen == BOARD_SCREEN_SYSTEM_3)
  {
    drawSystemDeviceView();
    return;
  }

  drawPanel();
}

static void drawActiveViewCleared(void)
{
  tft_ec11_clear();
  drawActiveView();
}

static void refreshAll(void)
{
  if (board.panelActive)
  {
    drawActiveView();
    return;
  }

  refreshDebugLines();
}

//-- Screen navigation helpers; every caller holds the drawing mutex.

//-- Switch to the given screen; the rotary encoder is the only means to
//-- browse, so there is no history to remember.
static void gotoScreen(BoardScreen next)
{
  if (next == board.currentScreen)
  {
    return;
  }

  board.currentScreen = next;
  drawActiveViewCleared();
}

//-- Light the panel after the idle time-out took it dark and restart the
//-- idle timer from this moment.
static void wakeScreen(void)
{
  tft_ec11_set_backlight(true);
  board.screenOn = true;
  board.lastActivityTick = xTaskGetTickCount();
  drawActiveViewCleared();
}

//-- Input handling: the rotary encoder is the only means to walk through
//-- all screens one by one. The AUX button never switches a screen (it is
//-- reserved for leaving a future menu) and only wakes a dark panel. Every
//-- event on any button — encoder turns, the encoder button and the AUX
//-- button, lit or dark — restarts the idle time-out. Events only act once
//-- the power screens have taken the display over.
static void handleBoardEvent(const tft_ec11_event_t* event)
{
  if (!board.panelActive)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);

  //-- Any physical button action restarts the idle time-out, whether the
  //-- panel is lit or already dark.
  board.lastActivityTick = xTaskGetTickCount();

  if (event->type == TFT_EC11_EVENT_AUX_BUTTON)
  {
    //-- Dark panel: any press of the AUX button lights it again. While the
    //-- panel is lit the AUX button does nothing on the screens.
    if (!board.screenOn)
    {
      wakeScreen();
    }

    xSemaphoreGive(board.mutex);
    return;
  }

  if ((event->type == TFT_EC11_EVENT_ROTATE) && (event->steps != 0) && board.screenOn)
  {
    int steps = (int)event->steps % (int)BOARD_SCREEN_COUNT;

    if (steps < 0)
    {
      steps += (int)BOARD_SCREEN_COUNT;
    }

    gotoScreen((BoardScreen)(((int)board.currentScreen + steps) % (int)BOARD_SCREEN_COUNT));
  }

  xSemaphoreGive(board.mutex);
}

static void boardInputsTask(void* arg)
{
  tft_ec11_event_t event;
  (void)arg;

  for (;;)
  {
    //-- A tear-down asked this task to leave (the display mode switched to
    //-- "No Display"): stop consuming and exit.
    if (!board.enabled)
    {
      break;
    }

    if (tft_ec11_get_event(&event, pdMS_TO_TICKS(200)))
    {
      handleBoardEvent(&event);
    }

    if (!board.enabled)
    {
      break;
    }

    //-- Idle time-out: the backlight goes off the configured number of
    //-- minutes after the last physical button action; the AUX button
    //-- lights the panel again.
    if (board.available && board.panelActive)
    {
      xSemaphoreTake(board.mutex, portMAX_DELAY);

      if (board.screenOn && (board.displayTimeoutMinutes > 0u) &&
          ((xTaskGetTickCount() - board.lastActivityTick) >=
           pdMS_TO_TICKS(60000u * (uint32_t)board.displayTimeoutMinutes)))
      {
        tft_ec11_set_backlight(false);
        board.screenOn = false;
      }

      xSemaphoreGive(board.mutex);
    }
  }

  //-- Exit path: only the consumer of the component's event queue may stop
  //-- the inputs, so the queue is never deleted while this task waits on
  //-- it; then announce the departure to a waiting tear-down and leave.
  tft_ec11_inputs_stop();
  board.inputTask = NULL;
  xSemaphoreGive(board.teardownAck);
  vTaskDelete(NULL);
}

static void update(char* field, size_t fieldSize, const char* value)
{
  if (!board.available)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  snprintf(field, fieldSize, "%s", (value != NULL) ? value : "");

  //-- While the power panel is active nothing else is drawn on the screen;
  //-- the strings stay stored for a future refresh.
  if (!board.panelActive)
  {
    refreshAll();
  }

  xSemaphoreGive(board.mutex);
}

//-- Prepares the board state once: the zeroed struct with its mutex and the
//-- input-task exit handshake semaphore, plus the boot defaults. A runtime
//-- re-enable must not redo this: it would drop the live mutex and reset
//-- the strings currently on view.
static bool boardEnsurePrepared(void)
{
  if (board.prepared)
  {
    return true;
  }

  memset(&board, 0, sizeof board);
  board.mutex = xSemaphoreCreateMutex();
  board.teardownAck = xSemaphoreCreateBinary();
  board.lifecycle = xSemaphoreCreateMutex();

  if ((board.mutex == NULL) || (board.teardownAck == NULL) || (board.lifecycle == NULL))
  {
    return false;
  }

  board.prepared = true;
  board.currentScreen = BOARD_SCREEN_PHASE_LOAD;
  board.displayTimeoutMinutes = BOARD_DISPLAY_TIMEOUT_DEFAULT_MINUTES;
  snprintf(board.screen, sizeof board.screen, "boot");
  snprintf(board.p1Note, sizeof board.p1Note, "waiting");
  snprintf(board.wifiStatus, sizeof board.wifiStatus, "starting");
  return true;
}

//-- Starts the panel and the board inputs (the "Piggyback board" side of
//-- the display mode). If the panel cannot be brought up, every other
//-- function becomes a harmless no-op so the logger keeps running without
//-- a display.
static esp_err_t boardBringUp(void)
{
  if (board.available)
  {
    return ESP_OK;
  }

  esp_err_t result = tft_ec11_init();

  if (result != ESP_OK)
  {
    //-- No panel: the logger stays fully functional without debug lines.
    LOGGER_LOGW(tag, "display init failed: %s, continuing without debug lines",
                esp_err_to_name(result));
    return result;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_set_text_style(TFT_EC11_WHITE, BOARD_FONT_SCALE);
  tft_ec11_clear();

  //-- The panel follows the rotation the settings carry even when it was
  //-- stored while the panel was down (boot with "No Display", re-enabled
  //-- later): tft_ec11_init comes up on the component default.
  tft_ec11_set_rotation(board.rotationUpsideDown ? BOARD_ROTATION_UPSIDE_DOWN
                                                 : BOARD_ROTATION_NORMAL);

  board.available = true;
  board.screenOn = true;
  board.lastActivityTick = xTaskGetTickCount();
  refreshAll();
  xSemaphoreGive(board.mutex);

  //-- The encoder browses the panel views and the AUX button wakes a dark
  //-- panel; when the inputs cannot be started the panel keeps working
  //-- without them.
  esp_err_t inputs = tft_ec11_inputs_start(BOARD_INPUT_QUEUE_LEN);

  if (inputs == ESP_OK)
  {
    if (xTaskCreate(boardInputsTask, "boardInputs", 3072, NULL, 4, &board.inputTask) != pdPASS)
    {
      board.inputTask = NULL;

      //-- No consumer ever waited on the queue: stopping the inputs from
      //-- here is safe.
      tft_ec11_inputs_stop();
      LOGGER_LOGW(tag, "input task could not start, AUX button and encoder disabled");
    }
  }
  else
  {
    LOGGER_LOGW(tag, "board inputs not started: %s", esp_err_to_name(inputs));
  }

  LOGGER_LOGI(tag, "debug lines ready on the board display");
  return ESP_OK;
}

//-- Stops the board inputs and tears the panel down (the "No Display" side
//-- of the display mode). The input task is the consumer of the component's
//-- event queue, so it is asked to leave (board.enabled dropped) and waits
//-- for the task to stop the inputs from its own context and delete itself;
//-- only then is the panel torn down under the drawing mutex.
static void boardTearDown(void)
{
  if (!board.available)
  {
    return;
  }

  board.enabled = false;

  if (board.inputTask != NULL)
  {
    if (xSemaphoreTake(board.teardownAck, pdMS_TO_TICKS(1500u)) != pdTRUE)
    {
      LOGGER_LOGW(tag, "board input task did not stop within time");
    }
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  tft_ec11_deinit();
  board.available = false;
  board.panelActive = false;
  xSemaphoreGive(board.mutex);

  LOGGER_LOGI(tag, "display off: panel and board inputs stopped");
}

esp_err_t loggerBoardSetEnabled(bool enabled)
{
  if (!boardEnsurePrepared())
  {
    return ESP_ERR_NO_MEM;
  }

  //-- One bring-up or tear-down at a time: the web save, the telnet command
  //-- and the deferred "No Display" boot tear-down run in different tasks
  //-- and may ask for a switch while another one is still busy; the whole
  //-- switch (init/deinit, input start/stop and the state flags) is atomic
  //-- under this lock, so a half-torn-down panel can never be re-animated.
  xSemaphoreTake(board.lifecycle, portMAX_DELAY);

  esp_err_t result = ESP_OK;

  if (enabled != board.enabled)
  {
    board.enabled = enabled;

    if (enabled)
    {
      result = boardBringUp();
    }
    else
    {
      boardTearDown();
    }
  }

  xSemaphoreGive(board.lifecycle);
  return result;
}

esp_err_t loggerBoardInit(void)
{
  return loggerBoardSetEnabled(true);
}

void loggerBoardSetRotation(bool upsideDown)
{
  if (!board.prepared)
  {
    return;
  }

  //-- The wanted rotation is remembered even while the panel is down (the
  //-- "No Display" mode): boardBringUp applies it on a later re-enable.
  board.rotationUpsideDown = upsideDown;

  if (!board.available)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  tft_ec11_set_rotation(upsideDown ? BOARD_ROTATION_UPSIDE_DOWN : BOARD_ROTATION_NORMAL);

  //-- The panel geometry follows the width and height the component reports,
  //-- so a full redraw lays the active view out for the new orientation.
  tft_ec11_clear();
  refreshAll();
  xSemaphoreGive(board.mutex);

  LOGGER_LOGI(tag, "display orientation: %s", upsideDown ? "upside down" : "normal");
}

void loggerBoardSetDisplayTimeoutMinutes(uint16_t minutes)
{
  if (!board.prepared)
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  board.displayTimeoutMinutes = minutes;

  //-- A freshly applied time-out starts counting from this moment; a dark
  //-- panel stays dark until the AUX button lights it again.
  board.lastActivityTick = xTaskGetTickCount();
  xSemaphoreGive(board.mutex);

  LOGGER_LOGI(tag, "display time-out: %" PRIu16 " minute(s)", minutes);
}

void loggerBoardShowScreen(const char* screen)
{
  update(board.screen, sizeof board.screen, screen);
}

void loggerBoardShowParserVersion(const char* version)
{
  update(board.parserVersion, sizeof board.parserVersion, version);
}

void loggerBoardNoteTelegram(uint32_t sequence)
{
  char note[BOARD_TEXT_MAX];
  snprintf(note, sizeof note, "telegram %lu accepted", (unsigned long)sequence);
  update(board.p1Note, sizeof board.p1Note, note);
}

void loggerBoardWifiPortal(const char* url)
{
  update(board.wifiStatus, sizeof board.wifiStatus, "portal open");

  char ipLine[BOARD_TEXT_MAX];
  snprintf(ipLine, sizeof ipLine, "%s", (url != NULL) ? url : "http://192.168.4.1/");
  update(board.wifiIp, sizeof board.wifiIp, ipLine);
}

void loggerBoardWifiConnected(const char* ssid, const char* ip)
{
  if (!board.prepared)
  {
    return;
  }

  char status[BOARD_TEXT_MAX];
  snprintf(status, sizeof status, "joined %s", (ssid != NULL) ? ssid : "?");
  update(board.wifiStatus, sizeof board.wifiStatus, status);

  char ipLine[BOARD_TEXT_MAX];
  snprintf(ipLine, sizeof ipLine, "http://%s", (ip != NULL) ? ip : "?");
  update(board.wifiIp, sizeof board.wifiIp, ipLine);

  //-- Plain SSID and IP for the system information view.
  xSemaphoreTake(board.mutex, portMAX_DELAY);
  snprintf(board.ssid, sizeof board.ssid, "%s", (ssid != NULL) ? ssid : "");
  snprintf(board.ip, sizeof board.ip, "%s", (ip != NULL) ? ip : "");
  xSemaphoreGive(board.mutex);
}

void loggerBoardShowPower(const loggerPowerSnapshot* snapshot)
{
  if (!board.available || (snapshot == NULL))
  {
    return;
  }

  xSemaphoreTake(board.mutex, portMAX_DELAY);
  board.panel = *snapshot;

  //-- The first power frame takes over the whole screen: the boot debug
  //-- lines are wiped before the panel is drawn.
  if (!board.panelActive)
  {
    tft_ec11_clear();
  }

  board.panelActive = true;

  //-- A new telegram refreshes the screen on view; while the panel is dark
  //-- nothing is drawn and the wake redraw presents the fresh frame.
  if (board.screenOn)
  {
    drawActiveView();
  }

  xSemaphoreGive(board.mutex);
}
