//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_board.c - on-device TFT debug lines and power panel
//-- Date: 2026-11-17 · Build: 14
//--
//-- Everything is drawn through the documented API of the esp32_s3_piggyback
//-- component: tft_ec11_init once, then filled rectangles and fixed-width
//-- text. The component inputs are started so a small poll task can act on
//-- the AUX button and the encoder. Drawing is serialized with a mutex
//-- because WiFi callbacks, the receive task and the HTTP handlers all
//-- report state.
//--
//-- Before the first accepted telegram the panel shows the four debug lines.
//-- When the power panel becomes active the screen is fully cleared first.
//-- Every screen then starts with one white header row carrying the screen
//-- name left aligned and an optional stamp right aligned (the meter
//-- date/time on the PHASE LOAD panel, the selected phase on the PER FASE
//-- screen), and ends with a white status row carrying variable status
//-- text and, on the AUX views, the return hint. The PHASE LOAD panel
//-- shows one row per phase plus Totaal with a header band (current left,
//-- phase label centred, voltage right), a half-height centre-zero signed
//-- watt bar (consumption red, feed-in green) with its integer scale
//-- labels inside the bar and the watt value.
//-- From the graphics panel a short AUX press opens the per-phase screen
//-- (net watts as a large centred number with a smaller unit, the phase
//-- current left and the voltage right on one row tight above a
//-- centre-zero bar on the panel scale, for the phase the encoder
//-- selects) and a medium-long press swaps the bars for the system
//-- information view; a short press returns to the graphics panel.
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

//-- Header row (white, black screen name left aligned and an optional
//-- right aligned stamp: the meter date/time on the PHASE LOAD panel and
//-- the selected phase on the PER FASE screen), then four equal rows and,
//-- at the very bottom of every screen, a white status row carrying
//-- variable status text.
//-- Within a row: header band and a centre-zero bar carrying its integer
//-- scale labels; the watt value sits at the bottom of the row. The panel
//-- bars are half the height of the per-phase bar.
#define PANEL_NAME_H 20
#define PANEL_TITLE_H PANEL_NAME_H
#define PANEL_HEADER_H 16
#define PANEL_BAR_H 8
#define PANEL_WATT_H 16
#define PANEL_STATUS_H 20
#define PANEL_ROWS 4
#define TEXT_BAR_H 16

#define PANEL_BAR_Y PANEL_HEADER_H
#define PANEL_EDGE_MARGIN 4

//-- All panel text uses the documented native faces of the component, named
//-- after their character cell (the advance includes the 1 px
//-- inter-character gap): 6x8, 8x12 and 9x16. The row texts (current, phase
//-- label, voltage), the header stamps and the system information lines use
//-- the 8x12 face; the per-phase watts keep the stretched face for its wide
//-- 12 by 16 digit cell.
#define FONT_TITLE TFT_EC11_FONT_8x12
#define FONT_STAMP TFT_EC11_FONT_6x8
#define FONT_VALUE TFT_EC11_FONT_6x8
#define FONT_HEADER_INFO TFT_EC11_FONT_8x12
#define FONT_ROW TFT_EC11_FONT_8x12
#define FONT_SYSTEM TFT_EC11_FONT_8x12
#define FONT_WATT TFT_EC11_FONT_9x16
#define FONT_INFO TFT_EC11_FONT_8x12

//-- Per-phase watts in the stretched face: 12 by 16 cell per character.
#define TEXT_DIGIT_SCALE 2u
#define TEXT_DIGIT_ADVANCE (6 * (int)TEXT_DIGIT_SCALE)
#define TEXT_DIGIT_HEIGHT (8 * (int)TEXT_DIGIT_SCALE)

//-- Bar scale ladder: the smallest step that fits the largest absolute
//-- watt of the frame; frames above 3800 W stay on the 3800 W step and
//-- frames below 100 W stay on the 100 W step.
//-- Published for the SPA realtime view so its bars scale identically to
//-- the panel (logger_board.h).
const uint16_t loggerPowerScales[LOGGER_POWER_SCALE_COUNT] = {100u,  250u,  500u,  1000u,
                                                              1700u, 2400u, 3100u, 3800u};

//-- Panel colours (RGB565).
#define PANEL_HEADER_COLOR TFT_EC11_RGB565(43, 60, 94)
#define PANEL_BAR_BG TFT_EC11_RGB565(232, 232, 236)
#define PANEL_TICK_COLOR TFT_EC11_RGB565(90, 90, 98)
#define PANEL_CONSUME_COLOR TFT_EC11_RGB565(255, 59, 48)
#define PANEL_PRODUCE_COLOR TFT_EC11_RGB565(34, 160, 60)

//-- Screen modes after the first accepted telegram: the graphics panel,
//-- the AUX power text screen and the AUX medium-press system view.
typedef enum
{
  BOARD_MODE_GRAPHICS,
  BOARD_MODE_POWER_TEXT,
  BOARD_MODE_SYSTEM
} BoardMode;

#define BOARD_INPUT_QUEUE_LEN 16u

//-- Landscape rotations of the component: the assembled orientation is the
//-- Kconfig default (3); rotation 1 is that same landscape turned 180
//-- degrees, so the flip only swaps the vertical mirror.
#define BOARD_ROTATION_NORMAL 3u
#define BOARD_ROTATION_UPSIDE_DOWN 1u

typedef struct
{
  bool available;
  bool panelActive;
  SemaphoreHandle_t mutex;
  BoardMode mode;
  uint8_t powerIndex;
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

static const char* const phaseLabels[PANEL_ROWS] = {"Fase 1", "Fase 2", "Fase 3", "Totaal"};

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

//-- True colour for a signed watt value: consumption red, feed-in green.
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

  return TFT_EC11_WHITE;
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

//-- Cell metrics of the documented native faces: the advance carries the
//-- 1 px inter-character gap and the cell height is the cleared line height
//-- (6x8, 8x12, 9x16, 13x24 and 17x32 per the component README).
static int fontAdvance(tft_ec11_font_t font)
{
  switch (font)
  {
  case TFT_EC11_FONT_17x32:
    return 17;

  case TFT_EC11_FONT_13x24:
    return 13;

  case TFT_EC11_FONT_9x16:
    return 9;

  case TFT_EC11_FONT_8x12:
    return 8;

  default:
    return 6;
  }
}

static int fontCellHeight(tft_ec11_font_t font)
{
  switch (font)
  {
  case TFT_EC11_FONT_17x32:
    return 32;

  case TFT_EC11_FONT_13x24:
    return 24;

  case TFT_EC11_FONT_9x16:
    return 16;

  case TFT_EC11_FONT_8x12:
    return 12;

  default:
    return 8;
  }
}

static void drawLeftText(int y, const char* text, tft_ec11_font_t font, uint16_t background,
                         uint16_t foreground)
{
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, y, text, font, background, foreground);
}

static void drawCenteredText(int y, int panelWidth, const char* text, tft_ec11_font_t font,
                             uint16_t background, uint16_t foreground)
{
  int x = (panelWidth - (int)strlen(text) * fontAdvance(font)) / 2;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_font(x, y, text, font, background, foreground);
}

static void drawRightAlignedText(int y, int panelWidth, const char* text, int rightMargin,
                                 tft_ec11_font_t font, uint16_t background, uint16_t foreground)
{
  int x = panelWidth - (int)strlen(text) * fontAdvance(font) - rightMargin;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_font(x, y, text, font, background, foreground);
}

//-- Every screen starts with one white header row: the screen name left
//-- aligned and an optional stamp right aligned (the meter date/time on
//-- the PHASE LOAD panel, the selected phase on the PER FASE screen), both
//-- in black on the white bar.
static void drawScreenTitle(int panelWidth, const char* name, const char* rightText)
{
  tft_ec11_fill_rect(0, 0, panelWidth, PANEL_NAME_H, TFT_EC11_WHITE);
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, (PANEL_NAME_H - fontCellHeight(FONT_TITLE)) / 2, name,
                          FONT_TITLE, TFT_EC11_WHITE, TFT_EC11_BLACK);

  if ((rightText != NULL) && (rightText[0] != '\0'))
  {
    drawRightAlignedText((PANEL_NAME_H - fontCellHeight(FONT_HEADER_INFO)) / 2, panelWidth,
                         rightText, PANEL_EDGE_MARGIN, FONT_HEADER_INFO, TFT_EC11_WHITE,
                         TFT_EC11_BLACK);
  }
}

//-- Centre-zero bar of the given height carrying its integer scale labels
//-- inside the bar; the panel draws it at half height and the per-phase
//-- screen at the full height.
static void drawBar(int y, int panelWidth, float watts, uint16_t scale, int barHeight)
{
  int mid = panelWidth / 2;
  int halfSpan = mid - PANEL_EDGE_MARGIN;
  int labelY = y + (barHeight - fontCellHeight(FONT_VALUE)) / 2;
  char label[16];

  tft_ec11_fill_rect(0, y, panelWidth, barHeight, PANEL_BAR_BG);

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
  tft_ec11_draw_text_font(2, labelY, label, FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  snprintf(label, sizeof label, "%" PRIu16, scale);
  drawRightAlignedText(labelY, panelWidth, label, 2, FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  drawCenteredText(labelY, panelWidth, "0", FONT_VALUE, PANEL_BAR_BG, PANEL_TICK_COLOR);

  //-- The centre tick stays on top so the zero label never covers it.
  tft_ec11_fill_rect(mid - 1, y, 2, barHeight, PANEL_TICK_COLOR);
}

static void drawPowerRow(int index, float watts, float amps, float volts, int y, int panelWidth,
                         int rowHeight, uint16_t scale)
{
  char text[24];
  int textY = y + (PANEL_HEADER_H - fontCellHeight(FONT_ROW)) / 2;
  int wattY = y + rowHeight - PANEL_WATT_H;

  //-- Header band: phase current left, voltage right and the centred phase
  //-- label, all in the 8x12 native face.
  tft_ec11_fill_rect(0, y, panelWidth, PANEL_HEADER_H, PANEL_HEADER_COLOR);

  snprintf(text, sizeof text, "%.1fA", (double)amps);
  drawLeftText(textY, text, FONT_ROW, PANEL_HEADER_COLOR, TFT_EC11_WHITE);

  drawCenteredText(textY, panelWidth, phaseLabels[index], FONT_ROW, PANEL_HEADER_COLOR,
                   TFT_EC11_WHITE);

  snprintf(text, sizeof text, "%.1fV", (double)volts);
  drawRightAlignedText(textY, panelWidth, text, PANEL_EDGE_MARGIN, FONT_ROW, PANEL_HEADER_COLOR,
                       TFT_EC11_WHITE);

  drawBar(y + PANEL_BAR_Y, panelWidth, watts, scale, PANEL_BAR_H);

  //-- Full-width clear behind the watt row so a previous wider label never
  //-- leaves ghost pixels behind.
  tft_ec11_fill_rect(0, wattY, panelWidth, PANEL_WATT_H, TFT_EC11_BLACK);
  snprintf(text, sizeof text, "%.1f Watt", (double)watts);
  drawCenteredText(wattY, panelWidth, text, FONT_WATT, TFT_EC11_BLACK, signedValueColor(watts));
}

//-- White status row at the very bottom of every screen: the current
//-- device status text left and, on the AUX views, the return hint right
//-- aligned.
static void drawStatusLine(int panelWidth, int panelHeight, bool showBackHint)
{
  int y = panelHeight - PANEL_STATUS_H;

  tft_ec11_fill_rect(0, y, panelWidth, PANEL_STATUS_H, TFT_EC11_WHITE);
  tft_ec11_draw_text_font(PANEL_EDGE_MARGIN, y + (PANEL_STATUS_H - fontCellHeight(FONT_STAMP)) / 2,
                          board.p1Note, FONT_STAMP, TFT_EC11_WHITE, TFT_EC11_BLACK);

  if (showBackHint)
  {
    drawRightAlignedText(y + (PANEL_STATUS_H - fontCellHeight(FONT_STAMP)) / 2, panelWidth,
                         "AUX: Terug", PANEL_EDGE_MARGIN, FONT_STAMP, TFT_EC11_WHITE,
                         TFT_EC11_BLACK);
  }
}

static void drawPanel(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  uint16_t scale = loggerBoardPickPowerScale(snapshot);
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  int rowHeight = (height - PANEL_TITLE_H - PANEL_STATUS_H) / PANEL_ROWS;

  //-- The header row prints the screen name left and the meter date/time
  //-- right; the accepted-telegram counter is no longer shown.
  drawScreenTitle(width, "PHASE LOAD",
                  (snapshot->meterTime[0] != '\0') ? snapshot->meterTime : "no meter time");

  for (int row = 0; row < PANEL_ROWS; row++)
  {
    int y = PANEL_TITLE_H + row * rowHeight;
    float watts = (row < 3) ? snapshot->phaseWatts[row] : snapshot->totalWatts;
    float amps = (row < 3) ? snapshot->phaseAmps[row] : snapshot->totalAmps;
    float volts = (row < 3) ? snapshot->phaseVolts[row] : snapshot->totalVolts;

    drawPowerRow(row, watts, amps, volts, y, width, rowHeight, scale);
  }

  drawStatusLine(width, height, false);
}

//-- AUX per-phase screen for the phase the encoder selected (0-2 Fase 1-3,
//-- 3 Totaal): the selected phase right aligned in the header row, the net
//-- watts as a large centred number in the stretched face with a smaller
//-- unit next to it, the current left and the voltage right on one smaller
//-- row tight under it, and a centre-zero bar directly below that row on
//-- the same ladder step the graphics panel picked for this frame.
static void drawPowerTextView(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  uint8_t index = board.powerIndex;
  float watts = (index < 3u) ? snapshot->phaseWatts[index] : snapshot->totalWatts;
  float amps = (index < 3u) ? snapshot->phaseAmps[index] : snapshot->totalAmps;
  float volts = (index < 3u) ? snapshot->phaseVolts[index] : snapshot->totalVolts;
  uint16_t scale = loggerBoardPickPowerScale(snapshot);
  char digits[16];
  char text[16];
  int unitGap = 3;
  int valueHeight = TEXT_DIGIT_HEIGHT;
  int infoHeight = fontCellHeight(FONT_INFO);

  drawScreenTitle(width, "PER FASE", phaseLabels[index]);
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_BLACK);

  snprintf(digits, sizeof digits, "%.1f", (double)watts);

  //-- Value row, current/voltage row and bar form one block that is centred
  //-- vertically in the free body area between header and status row; the
  //-- bar sits tight under the current/voltage row.
  int contentHeight = valueHeight + 12 + infoHeight + TEXT_BAR_H;
  int bodyHeight = height - PANEL_TITLE_H - PANEL_STATUS_H;
  int y = PANEL_TITLE_H + (bodyHeight - contentHeight) / 2;

  if (y < PANEL_TITLE_H)
  {
    y = PANEL_TITLE_H;
  }

  //-- Number and unit share one centred group so the unit hugs the digits
  //-- instead of sitting at the screen centre; the smaller unit aligns to
  //-- the bottom of the large digits. The digits use the stretched face
  //-- (12 by 16 cell).
  int digitsWidth = (int)strlen(digits) * TEXT_DIGIT_ADVANCE;
  int groupWidth = digitsWidth + unitGap + fontAdvance(FONT_INFO);
  int x = (width - groupWidth) / 2;

  if (x < 0)
  {
    x = 0;
  }

  tft_ec11_draw_text_styled(x, y, digits, TFT_EC11_BLACK, TEXT_DIGIT_SCALE,
                            signedValueColor(watts));
  tft_ec11_draw_text_font(x + digitsWidth + unitGap, y + valueHeight - infoHeight, "W", FONT_INFO,
                          TFT_EC11_BLACK, signedValueColor(watts));
  y += valueHeight + 12;

  snprintf(text, sizeof text, "%.1f A", (double)amps);
  drawLeftText(y, text, FONT_INFO, TFT_EC11_BLACK, TFT_EC11_WHITE);

  snprintf(text, sizeof text, "%.1f V", (double)volts);
  drawRightAlignedText(y, width, text, PANEL_EDGE_MARGIN, FONT_INFO, TFT_EC11_BLACK,
                       TFT_EC11_WHITE);
  y += infoHeight;

  drawBar(y, width, watts, scale, TEXT_BAR_H);

  drawStatusLine(width, height, true);
}

//-- AUX medium-press view: reception counters, network and configuration,
//-- every line in the 8x12 native face.
static void drawSystemView(void)
{
  const loggerPowerSnapshot* snapshot = &board.panel;
  int width = (int)tft_ec11_width();
  int height = (int)tft_ec11_height();
  char line[64];
  int y = PANEL_TITLE_H + 10;
  const int pitch = 16;

  drawScreenTitle(width, "SYSTEMINFO", "");
  tft_ec11_fill_rect(0, PANEL_TITLE_H, width, height - PANEL_TITLE_H - PANEL_STATUS_H,
                     TFT_EC11_BLACK);

  snprintf(line, sizeof line, "Goedgekeurde telegrammen : %" PRIu32, snapshot->acceptedCount);
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Afgekeurde telegrammen : %" PRIu32, snapshot->rejectedCount);
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "IP-adres : %s", (board.ip[0] != '\0') ? board.ip : "geen");
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "SSID : %s", (board.ssid[0] != '\0') ? board.ssid : "geen");
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Uptime : %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
           snapshot->uptimeSeconds / 86400u, (snapshot->uptimeSeconds / 3600u) % 24u,
           (snapshot->uptimeSeconds / 60u) % 60u, snapshot->uptimeSeconds % 60u);
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "Land/protocol : %s %s",
           (snapshot->countryCode[0] != '\0') ? snapshot->countryCode : "?",
           (snapshot->profileId[0] != '\0') ? snapshot->profileId : "?");
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);
  y += pitch;

  snprintf(line, sizeof line, "P1 RX : %s", snapshot->rxInverted ? "inverted" : "normal");
  drawLeftText(y, line, FONT_SYSTEM, TFT_EC11_BLACK, TFT_EC11_WHITE);

  drawStatusLine(width, height, true);
}

static void drawActiveView(void)
{
  if (board.mode == BOARD_MODE_POWER_TEXT)
  {
    drawPowerTextView();
    return;
  }

  if (board.mode == BOARD_MODE_SYSTEM)
  {
    drawSystemView();
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

//-- Input task: short AUX press toggles between the graphics panel and the
//-- power text screen, a medium-long press swaps the bars for the system
//-- view (a short press returns), and the encoder cycles the phase shown on
//-- the text screen. Events only act once the panel has taken over.
static void handleBoardEvent(const tft_ec11_event_t* event)
{
  if (!board.panelActive)
  {
    return;
  }

  if (event->type == TFT_EC11_EVENT_AUX_BUTTON)
  {
    if (event->press != TFT_EC11_PRESS_SHORT)
    {
      if ((event->press == TFT_EC11_PRESS_MEDIUM) && (board.mode == BOARD_MODE_GRAPHICS))
      {
        xSemaphoreTake(board.mutex, portMAX_DELAY);
        board.mode = BOARD_MODE_SYSTEM;
        drawActiveViewCleared();
        xSemaphoreGive(board.mutex);
      }

      return;
    }

    xSemaphoreTake(board.mutex, portMAX_DELAY);

    if (board.mode == BOARD_MODE_GRAPHICS)
    {
      board.mode = BOARD_MODE_POWER_TEXT;
      board.powerIndex = PANEL_ROWS - 1u;
    }
    else
    {
      board.mode = BOARD_MODE_GRAPHICS;
    }

    drawActiveViewCleared();
    xSemaphoreGive(board.mutex);
    return;
  }

  if ((event->type == TFT_EC11_EVENT_ROTATE) && (board.mode == BOARD_MODE_POWER_TEXT) &&
      (event->steps != 0))
  {
    int wrapped =
        (((int)board.powerIndex + (int)event->steps) % PANEL_ROWS + PANEL_ROWS) % PANEL_ROWS;

    xSemaphoreTake(board.mutex, portMAX_DELAY);
    board.powerIndex = (uint8_t)wrapped;
    drawActiveView();
    xSemaphoreGive(board.mutex);
  }
}

static void boardInputsTask(void* arg)
{
  tft_ec11_event_t event;
  (void)arg;

  for (;;)
  {
    if (tft_ec11_get_event(&event, pdMS_TO_TICKS(200)))
    {
      handleBoardEvent(&event);
    }
  }
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

esp_err_t loggerBoardInit(void)
{
  memset(&board, 0, sizeof board);
  board.mutex = xSemaphoreCreateMutex();

  if (board.mutex == NULL)
  {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = tft_ec11_init();

  if (result != ESP_OK)
  {
    //-- No panel: the logger stays fully functional without debug lines.
    LOGGER_LOGW(tag, "display init failed: %s, continuing without debug lines",
                esp_err_to_name(result));
    return result;
  }

  tft_ec11_set_background(TFT_EC11_BLACK);
  tft_ec11_set_text_style(TFT_EC11_WHITE, BOARD_FONT_SCALE);
  tft_ec11_clear();

  board.available = true;
  board.mode = BOARD_MODE_GRAPHICS;
  snprintf(board.screen, sizeof board.screen, "boot");
  snprintf(board.p1Note, sizeof board.p1Note, "waiting");
  snprintf(board.wifiStatus, sizeof board.wifiStatus, "starting");
  refreshAll();

  //-- The AUX button and the encoder drive the panel views; when the inputs
  //-- cannot be started the panel keeps working without them.
  esp_err_t inputs = tft_ec11_inputs_start(BOARD_INPUT_QUEUE_LEN);

  if (inputs == ESP_OK)
  {
    if (xTaskCreate(boardInputsTask, "boardInputs", 3072, NULL, 4, NULL) != pdPASS)
    {
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

void loggerBoardSetRotation(bool upsideDown)
{
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

  //-- A new telegram only refreshes the graphics panel; the AUX views keep
  //-- the stored frame and wait for their own short press to return.
  if (board.mode == BOARD_MODE_GRAPHICS)
  {
    drawPanel();
  }

  xSemaphoreGive(board.mutex);
}
