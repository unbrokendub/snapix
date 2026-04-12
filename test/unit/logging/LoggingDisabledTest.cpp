#include <cstdio>

#include "HardwareSerial.h"

CaptureState captureState;
HWCDC Serial;

static unsigned long millisValue = 0;
unsigned long millis() { return millisValue; }

#include <Logging.h>

static int passCount = 0;
static int failCount = 0;

static void pass(const char* name) {
  printf("  \xE2\x9C\x93 PASS: %s\n", name);
  passCount++;
}

static void fail(const char* name, const char* detail) {
  fprintf(stderr, "  \xE2\x9C\x97 FAIL: %s\n", name);
  if (detail) fprintf(stderr, "    %s\n", detail);
  failCount++;
}

static void expectTrue(bool cond, const char* name) {
  if (cond) {
    pass(name);
  } else {
    fail(name, "condition was false");
  }
}

static void reset() {
  captureState.output.clear();
  captureState.enabled = true;
  millisValue = 0;
}

int main() {
  printf("\n========================================\n");
  printf("Test Suite: Logging Disabled Tests\n");
  printf("========================================\n");

  {
    reset();
    LOG_ERR("TEST", "silent");
    LOG_INF("TEST", "silent");
    LOG_DBG("TEST", "silent");
    expectTrue(captureState.output.empty(), "LOG_* macros do not emit output when serial logging is disabled");
  }

  {
    reset();
    logPrintf("[INF]", "TEST", "hidden\n");
    expectTrue(captureState.output.empty(), "logPrintf is a no-op when serial logging is disabled");
  }

  {
    reset();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    Serial.begin(115200);
    Serial.printf("hidden %d\n", 7);
#pragma GCC diagnostic pop
    expectTrue(captureState.output.empty(), "Serial wrapper does not emit output when serial logging is disabled");
    expectTrue(!Serial, "Serial wrapper reports disabled state");
  }

  printf("\n========================================\n");
  printf("Test Suite: Logging Disabled Tests - Summary\n");
  printf("========================================\n");
  printf("Total tests: %d\n", passCount + failCount);
  printf("  Passed: %d\n", passCount);
  printf("  Failed: %d\n", failCount);
  printf("\n%s\n", failCount == 0 ? "\xE2\x9C\x93 ALL TESTS PASSED" : "\xE2\x9C\x97 SOME TESTS FAILED");
  printf("========================================\n");

  return failCount > 0 ? 1 : 0;
}
