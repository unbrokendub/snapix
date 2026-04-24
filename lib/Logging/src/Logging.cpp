#include "Logging.h"

void logPrintf(const char* level, const char* origin, const char* format, ...) {
#if !SNAPIX_SERIAL_LOG_ENABLED
  (void)level;
  (void)origin;
  (void)format;
  return;
#else
  if (!logSerial) {
    return;
  }
  va_list args;
  va_start(args, format);
  char buf[256];
  char* c = buf;
  const char* const end = buf + sizeof(buf);

  int len = snprintf(c, end - c, "[%lu] ", millis());
  if (len > 0) c += (len < end - c) ? len : end - c - 1;

  const char* p = level;
  while (*p && c < end - 1) *c++ = *p++;
  if (c < end - 1) *c++ = ' ';

  len = snprintf(c, end - c, "[%s] ", origin);
  if (len > 0) c += (len < end - c) ? len : end - c - 1;

  vsnprintf(c, end - c, format, args);
  va_end(args);
  logSerial.print(buf);
#endif
}

MySerialImpl MySerialImpl::instance;

size_t MySerialImpl::printf(const char* format, ...) {
#if !SNAPIX_SERIAL_LOG_ENABLED
  (void)format;
  return 0;
#else
  va_list args;
  va_start(args, format);
  char buf[256];
  int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len > 0) {
    logSerial.print(buf);
  }
  return len > 0 ? len : 0;
#endif
}

size_t MySerialImpl::write(uint8_t b) {
#if SNAPIX_SERIAL_LOG_ENABLED
  return logSerial.write(b);
#else
  (void)b;
  return 1;
#endif
}

size_t MySerialImpl::write(const uint8_t* buffer, size_t size) {
#if SNAPIX_SERIAL_LOG_ENABLED
  return logSerial.write(buffer, size);
#else
  (void)buffer;
  return size;
#endif
}

void MySerialImpl::flush() {
#if SNAPIX_SERIAL_LOG_ENABLED
  logSerial.flush();
#endif
}
