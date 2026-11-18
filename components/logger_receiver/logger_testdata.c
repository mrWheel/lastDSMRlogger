//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_testdata.c - internal test data generator
//-- Date: 2026-11-18 · Build: 3
//--
//-- The generator produces one complete DSMR-shaped telegram per call with
//-- wall-clock meter stamps (random dates only until SNTP has set the clock)
//-- and measurement values. The phase objects walk a fixed sign sequence:
//-- one phase per frame turns from delivered into returned power until all
//-- three feed in, then one at a time turns back. Line shapes follow the
//-- accepted fixtures of the parser repository: the version line, the empty
//-- second line, per-country value scales and the M-Bus reading lines all
//-- match a profile that decodes them, and lines a profile does not define
//-- are skipped by the parser's unknown-field policy. The CRC-16/ARC trailer
//-- covers the bytes from '/' through '!' inclusive, so the frames are
//-- accepted exactly like real meter traffic.
//-- ==========================================================================

#include "logger_testdata.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_YEAR 26u

//-- System-time floor for a usable wall clock; before the first SNTP sync
//-- the clock still sits at the 1970 epoch.
#define TEST_CLOCK_SYNCED_EPOCH 1700000000

typedef struct
{
  uint32_t import1MilliKwh;
  uint32_t import2MilliKwh;
  uint32_t export1MilliKwh;
  uint32_t export2MilliKwh;
  uint32_t gasMilliM3;
  uint32_t waterMilliM3;
} TestCounters;

typedef struct
{
  char* buffer;
  size_t capacity;
  size_t length;
} TestFrame;

static loggerTestRandomFn testRandomFn;
static TestCounters testCounters;

static uint32_t testRandom(uint32_t limit)
{
  if ((testRandomFn == NULL) || (limit == 0u))
  {
    return 0u;
  }

  return testRandomFn(limit) % limit;
}

void loggerTestDataSeed(loggerTestRandomFn randomFn)
{
  testRandomFn = randomFn;

  //-- Random seeds so every run produces different measurement progressions.
  testCounters.import1MilliKwh = 100000000u + testRandom(50000000u);
  testCounters.import2MilliKwh = 50000000u + testRandom(50000000u);
  testCounters.export1MilliKwh = 10000000u + testRandom(20000000u);
  testCounters.export2MilliKwh = 5000000u + testRandom(20000000u);
  testCounters.gasMilliM3 = 20000000u + testRandom(60000000u);
  testCounters.waterMilliM3 = 5000000u + testRandom(40000000u);
}

//-- CRC-16/ARC as used by DSMR: initial value 0x0000, reflected polynomial
//-- 0xA001, LSB-first, covering the bytes from '/' through '!' inclusive.
static uint16_t testCrc16(const uint8_t* data, size_t length)
{
  uint16_t crc = 0x0000u;

  for (size_t index = 0u; index < length; index++)
  {
    crc ^= (uint16_t)data[index];

    for (uint8_t bit = 0; bit < 8u; bit++)
    {
      if ((crc & 0x0001u) != 0u)
      {
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      }
      else
      {
        crc = (uint16_t)(crc >> 1);
      }
    }
  }

  return crc;
}

//-- Appends formatted text to the frame under construction. False on
//-- overflow so a too-small caller buffer fails closed.
static bool testAppendFrame(TestFrame* frame, const char* format, ...)
{
  if (frame->length + 1u >= frame->capacity)
  {
    return false;
  }

  size_t remaining = frame->capacity - frame->length;
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(frame->buffer + frame->length, remaining, format, arguments);
  va_end(arguments);

  if ((written < 0) || ((size_t)written >= remaining))
  {
    return false;
  }

  frame->length += (size_t)written;
  return true;
}

//-- Builds YYMMDDHHMMSS from the device wall clock plus the country's DST or
//-- offset suffix as used by the verified fixtures. Until SNTP has set the
//-- clock a random month, day and time of day keeps offline frames decodable.
static void testFormatTimestamp(char* out, size_t outSize, const char* suffix)
{
  time_t now = time(NULL);
  struct tm brokenTime;

  if ((now >= (time_t)TEST_CLOCK_SYNCED_EPOCH) && (localtime_r(&now, &brokenTime) != NULL))
  {
    snprintf(out, outSize, "%02d%02d%02d%02d%02d%02d%s", (brokenTime.tm_year + 1900) % 100,
             brokenTime.tm_mon + 1, brokenTime.tm_mday, brokenTime.tm_hour, brokenTime.tm_min,
             brokenTime.tm_sec, suffix);
    return;
  }

  uint32_t month = 1u + testRandom(12u);
  uint32_t day = 1u + testRandom(28u);
  uint32_t hour = testRandom(24u);
  uint32_t minute = testRandom(60u);
  uint32_t second = testRandom(60u);

  snprintf(out, outSize,
           "%02" PRIu32 "%02" PRIu32 "%02" PRIu32 "%02" PRIu32 "%02" PRIu32 "%02" PRIu32 "%s",
           (uint32_t)TEST_YEAR, month, day, hour, minute, second, suffix);
}

static void testFormatEnergy(char* out, size_t outSize, uint32_t milli, const char* unit)
{
  snprintf(out, outSize, "%06" PRIu32 ".%03" PRIu32 "*%s", milli / 1000u, milli % 1000u, unit);
}

static void testFormatPower(char* out, size_t outSize, uint32_t milli)
{
  snprintf(out, outSize, "%03" PRIu32 ".%03" PRIu32 "*kW", milli / 1000u, milli % 1000u);
}

typedef struct
{
  uint32_t watt;
  uint32_t volt;
  bool negative;
} TestPhase;

//-- One phase per frame turns from delivered into returned power until all
//-- three feed in, then one at a time turns back, so the negative-phase
//-- count walks the sequence 0, 1, 2, 3, 2, 1 and repeats.
static uint8_t testNextNegativePhaseCount(void)
{
  static const uint8_t pattern[6] = {0u, 1u, 2u, 3u, 2u, 1u};
  static uint8_t step = 0u;
  uint8_t count = pattern[step];

  step = (uint8_t)((step + 1u) % 6u);
  return count;
}

//-- Phase objects following the corrected profile table (cross-checked
//-- against dsmr2Lib): delivered kW per phase on 1-0:21/41/61.7.0,
//-- returned kW per phase on 1-0:22/42/62.7.0, current on 1-0:31/51/71.7.0
//-- and voltage on 1-0:32/52/72.7.0. A negative phase reports its watt on
//-- the returned object and zero on the delivered one, so the panel shows
//-- it on the feed-in side of the centre-zero bars.
static bool testAppendPhaseObjects(TestFrame* frame, const TestPhase* phases)
{
  static const char* const deliveredObis[3] = {"1-0:21.7.0", "1-0:41.7.0", "1-0:61.7.0"};
  static const char* const returnedObis[3] = {"1-0:22.7.0", "1-0:42.7.0", "1-0:62.7.0"};
  static const char* const ampObis[3] = {"1-0:31.7.0", "1-0:51.7.0", "1-0:71.7.0"};
  static const char* const voltObis[3] = {"1-0:32.7.0", "1-0:52.7.0", "1-0:72.7.0"};

  bool ok = true;

  for (uint8_t phase = 0u; phase < 3u; phase++)
  {
    char value[32];
    uint32_t delivered = phases[phase].negative ? 0u : phases[phase].watt;
    uint32_t returned = phases[phase].negative ? phases[phase].watt : 0u;

    testFormatPower(value, sizeof value, delivered);
    ok = ok && testAppendFrame(frame, "%s(%s)\r\n", deliveredObis[phase], value);

    testFormatPower(value, sizeof value, returned);
    ok = ok && testAppendFrame(frame, "%s(%s)\r\n", returnedObis[phase], value);

    //-- The current follows the active watt at the phase voltage so the
    //-- ampere chip agrees with the bar instead of drifting away from it.
    uint32_t centiAmps = (phases[phase].watt * 100u) / phases[phase].volt;
    ok = ok && testAppendFrame(frame, "%s(00%" PRIu32 ".%02" PRIu32 "*A)\r\n", ampObis[phase],
                               centiAmps / 100u, centiAmps % 100u);

    ok = ok && testAppendFrame(frame, "%s(%03" PRIu32 ".%" PRIu32 "*V)\r\n", voltObis[phase],
                               phases[phase].volt, testRandom(10u));
  }

  return ok;
}

size_t loggerTestDataBuild(const dsmrProfile* profile, char* buffer, size_t bufferSize)
{
  TestFrame frame;
  frame.buffer = buffer;
  frame.capacity = bufferSize;
  frame.length = 0u;

  bool belgian = ((profile != NULL) && (profile->countryCode != NULL) &&
                  ((profile->countryCode[0] == 'B') || (profile->countryCode[0] == 'b')));

  const char* versionObis = ((profile != NULL) && (profile->versionObis != NULL))
                                ? profile->versionObis
                                : (belgian ? "0-0:96.1.4" : "1-3:0.2.8");

  char version[16];

  if ((profile != NULL) && (profile->acceptedVersions != NULL) &&
      (profile->acceptedVersions[0] != NULL))
  {
    snprintf(version, sizeof version, "%s", profile->acceptedVersions[0]);
  }
  else
  {
    snprintf(version, sizeof version, "%s", belgian ? "50221" : "50");
  }

  char meterTime[32];
  char mbusTime[32];
  testFormatTimestamp(meterTime, sizeof meterTime, belgian ? "+0100" : "S");
  testFormatTimestamp(mbusTime, sizeof mbusTime, belgian ? "+0100" : "S");

  testCounters.import1MilliKwh += testRandom(1000u);
  testCounters.import2MilliKwh += testRandom(1000u);
  testCounters.export1MilliKwh += testRandom(200u);
  testCounters.export2MilliKwh += testRandom(200u);
  testCounters.gasMilliM3 += testRandom(500u);
  testCounters.waterMilliM3 += testRandom(300u);

  char value[32];
  bool ok = true;

  ok = ok && testAppendFrame(&frame, "/TESTMETER-0001\r\n");
  ok = ok && testAppendFrame(&frame, "\r\n");
  ok = ok && testAppendFrame(&frame, "%s(%s)\r\n", versionObis, version);
  ok = ok && testAppendFrame(&frame, "0-0:96.1.1(5445535400000000000000000000%02X)\r\n",
                             (unsigned)testRandom(256u));
  ok = ok && testAppendFrame(&frame, "0-0:1.0.0(%s)\r\n", meterTime);

  testFormatEnergy(value, sizeof value, testCounters.import1MilliKwh, "kWh");
  ok = ok && testAppendFrame(&frame, "1-0:1.8.1(%s)\r\n", value);
  testFormatEnergy(value, sizeof value, testCounters.import2MilliKwh, "kWh");
  ok = ok && testAppendFrame(&frame, "1-0:1.8.2(%s)\r\n", value);
  testFormatEnergy(value, sizeof value, testCounters.export1MilliKwh, "kWh");
  ok = ok && testAppendFrame(&frame, "1-0:2.8.1(%s)\r\n", value);
  testFormatEnergy(value, sizeof value, testCounters.export2MilliKwh, "kWh");
  ok = ok && testAppendFrame(&frame, "1-0:2.8.2(%s)\r\n", value);

  if (!belgian)
  {
    //-- Aggregate readings are defined for 5.0 only; under the 4.0/4.2
    //-- profiles the parser skips these lines by its unknown-field policy.
    testFormatEnergy(value, sizeof value,
                     testCounters.import1MilliKwh + testCounters.import2MilliKwh, "kWh");
    ok = ok && testAppendFrame(&frame, "1-0:1.8.0(%s)\r\n", value);
    testFormatEnergy(value, sizeof value,
                     testCounters.export1MilliKwh + testCounters.export2MilliKwh, "kWh");
    ok = ok && testAppendFrame(&frame, "1-0:2.8.0(%s)\r\n", value);
  }

  ok = ok && testAppendFrame(&frame, "0-0:96.14.0(000%" PRIu32 ")\r\n", 1u + testRandom(2u));

  //-- The phase sign walk drives both the per-phase objects and the totals:
  //-- each total is the sum of the phases active on its side, so the total
  //-- bar agrees with the three phase bars of the power panel.
  TestPhase phases[3];
  uint8_t negativeCount = testNextNegativePhaseCount();
  uint32_t totalDeliveredWatt = 0u;
  uint32_t totalReturnedWatt = 0u;

  for (uint8_t phase = 0u; phase < 3u; phase++)
  {
    //-- 100..1200 W per active phase keeps the three-phase totals inside the
    //-- 100..3800 W ladder of the power panel.
    phases[phase].watt = 100u + testRandom(1101u);
    phases[phase].volt = 225u + testRandom(11u);
    phases[phase].negative = (phase < negativeCount);

    if (phases[phase].negative)
    {
      totalReturnedWatt += phases[phase].watt;
    }
    else
    {
      totalDeliveredWatt += phases[phase].watt;
    }
  }

  testFormatPower(value, sizeof value, totalDeliveredWatt);
  ok = ok && testAppendFrame(&frame, "1-0:1.7.0(%s)\r\n", value);
  testFormatPower(value, sizeof value, totalReturnedWatt);
  ok = ok && testAppendFrame(&frame, "1-0:2.7.0(%s)\r\n", value);

  if (belgian)
  {
    //-- eMUCs demand fields: under the 1.x profile the parser skips these
    //-- three lines as unknown fields.
    testFormatPower(value, sizeof value, testRandom(50000u));
    ok = ok && testAppendFrame(&frame, "1-0:1.4.0(%s)\r\n", value);
    testFormatPower(value, sizeof value, testRandom(50000u));
    ok = ok && testAppendFrame(&frame, "1-0:1.6.0(%s)(%s)\r\n", value, meterTime);
    //-- The LOG prefix group declares the number of following entries: one
    //-- entry means prefix (1), as in the verified fixture.
    ok = ok && testAppendFrame(&frame, "0-0:98.1.0(1)(%06" PRIu32 ")(%s)\r\n", testRandom(1000000u),
                               meterTime);

    ok = ok && testAppendPhaseObjects(&frame, phases);

    //-- The gas channel reads 0-1:24.2.3 in the eMUCs profile.
    ok = ok && testAppendFrame(&frame, "0-1:24.1.0(001)\r\n");
    testFormatEnergy(value, sizeof value, testCounters.gasMilliM3, "m3");
    ok = ok && testAppendFrame(&frame, "0-1:24.2.3(%s)\r\n", value);
    ok = ok && testAppendFrame(&frame, "0-1:24.3.1(%s)\r\n", mbusTime);
  }
  else
  {
    ok = ok && testAppendPhaseObjects(&frame, phases);
    ok = ok && testAppendFrame(&frame, "0-0:96.7.21(000%" PRIu32 ")\r\n", testRandom(10u));
    ok = ok && testAppendFrame(&frame, "0-0:96.7.9(000%" PRIu32 ")\r\n", testRandom(2u));

    //-- Gas on channel 1 and water on channel 2; channels that are not
    //-- declared as expected device types are skipped by the parser.
    ok = ok && testAppendFrame(&frame, "0-1:24.1.0(001)\r\n");
    testFormatEnergy(value, sizeof value, testCounters.gasMilliM3, "m3");
    ok = ok && testAppendFrame(&frame, "0-1:24.2.1(%s)\r\n", value);
    ok = ok && testAppendFrame(&frame, "0-1:24.3.1(%s)\r\n", mbusTime);
    ok = ok && testAppendFrame(&frame, "0-2:24.1.0(002)\r\n");
    testFormatEnergy(value, sizeof value, testCounters.waterMilliM3, "m3");
    ok = ok && testAppendFrame(&frame, "0-2:24.2.1(%s)\r\n", value);
    ok = ok && testAppendFrame(&frame, "0-2:24.3.1(%s)\r\n", mbusTime);
  }

  ok = ok && testAppendFrame(&frame, "!");

  if (!ok)
  {
    return 0u;
  }

  //-- The CRC covers the bytes from '/' through '!' inclusive.
  uint16_t crc = testCrc16((const uint8_t*)frame.buffer, frame.length);

  if (!testAppendFrame(&frame, "%04X\r\n", (unsigned)crc))
  {
    return 0u;
  }

  return frame.length;
}
