//-- ==========================================================================
//-- lastDSMRlogger - P1 smart meter logger
//-- File: logger_testdata.h - internal test data generator
//-- Date: 2026-11-15 · Build: 1
//--
//-- Builds complete CRC-correct synthetic telegrams for the test selection
//-- (RX GPIO -1). The builder is free of OS and ESP dependencies so it can
//-- also be exercised on the host against the real parser; the random source
//-- is injected to keep it testable. The frames follow the shapes of the
//-- verified parser fixtures, so Dutch and Belgian profiles accept them.
//-- ==========================================================================

#ifndef LOGGER_TESTDATA_H
#define LOGGER_TESTDATA_H

#include <stddef.h>
#include <stdint.h>

#include "dsmr_parser.h"

//-- Random source: returns a value below limit. Injected by the caller.
typedef uint32_t (*loggerTestRandomFn)(uint32_t limit);

//-- Installs the random source and seeds the monotonic measurement counters
//-- so every run progresses differently. Call before building.
void loggerTestDataSeed(loggerTestRandomFn randomFn);

//-- Builds one complete accepted-shaped telegram for the given profile into
//-- buffer. Returns the byte length, or 0 when the frame did not fit.
size_t loggerTestDataBuild(const dsmrProfile *profile, char *buffer, size_t bufferSize);

#endif //-- LOGGER_TESTDATA_H
