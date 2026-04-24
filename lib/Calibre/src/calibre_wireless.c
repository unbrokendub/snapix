/**
 * @file calibre_wireless.c
 * @brief Core implementation of Calibre Wireless protocol
 */

#include "calibre_wireless.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "calibre_internal.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#else
#include <time.h>
#endif

/* Log tag for this module */
#define TAG CAL_LOG_TAG_CORE

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static bool s_initialized = false;

/* ============================================================================
 * Error Handling
 * ============================================================================ */

const char* calibre_err_str(calibre_err_t err) {
  switch (err) {
    case CALIBRE_OK:
      return "OK";
    case CALIBRE_ERR_NOMEM:
      return "Out of memory";
    case CALIBRE_ERR_INVALID_ARG:
      return "Invalid argument";
    case CALIBRE_ERR_SOCKET:
      return "Socket error";
    case CALIBRE_ERR_CONNECT:
      return "Connection failed";
    case CALIBRE_ERR_TIMEOUT:
      return "Timeout";
    case CALIBRE_ERR_PROTOCOL:
      return "Protocol error";
    case CALIBRE_ERR_JSON_PARSE:
      return "JSON parse error";
    case CALIBRE_ERR_AUTH:
      return "Authentication failed";
    case CALIBRE_ERR_WRITE_FILE:
      return "File write error";
    case CALIBRE_ERR_SD_CARD:
      return "SD card error";
    case CALIBRE_ERR_DISCONNECTED:
      return "Disconnected";
    case CALIBRE_ERR_CANCELLED:
      return "Operation cancelled";
    case CALIBRE_ERR_BUSY:
      return "Busy";
    default:
      return "Unknown error";
  }
}

void calibre_set_error(calibre_conn_t* conn, calibre_err_t err, const char* msg) {
  if (conn) {
    conn->last_error = err;
    if (msg) {
      calibre_strlcpy(conn->error_msg, msg, sizeof(conn->error_msg));
    } else {
      conn->error_msg[0] = '\0';
    }
  }
}

const char* calibre_get_error_msg(const calibre_conn_t* conn) {
  if (!conn) return NULL;
  return conn->error_msg[0] ? conn->error_msg : NULL;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

void calibre_strlcpy(char* dst, const char* src, size_t size) {
  if (size == 0) return;
  size_t i;
  for (i = 0; i < size - 1 && src[i]; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

void calibre_gen_uuid(char* buf, size_t len) {
  if (len < 37) return;

#ifdef ESP_PLATFORM
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  uint32_t r4 = esp_random();
#else
  uint32_t r1 = (uint32_t)rand();
  uint32_t r2 = (uint32_t)rand();
  uint32_t r3 = (uint32_t)rand();
  uint32_t r4 = (uint32_t)rand();
#endif

  snprintf(buf, len, "%08x-%04x-4%03x-%04x-%04x%08x", r1, (r2 >> 16) & 0xFFFF, r2 & 0x0FFF,
           (r3 >> 16 & 0x3FFF) | 0x8000, r3 & 0xFFFF, r4);
}

void calibre_hash_password(const char* password, const char* challenge, char* out, size_t out_len) {
  if (out_len < 65) return;

#ifdef ESP_PLATFORM
  /* Use mbedTLS for ESP32 */
  mbedtls_sha256_context ctx;
  uint8_t hash[32];

  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0); /* 0 = SHA-256 */
  mbedtls_sha256_update(&ctx, (const uint8_t*)password, strlen(password));
  mbedtls_sha256_update(&ctx, (const uint8_t*)challenge, strlen(challenge));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  for (int i = 0; i < 32; i++) {
    snprintf(out + i * 2, 3, "%02x", hash[i]);
  }
#else
  /* Simplified for testing - in production, use a real hash */
  snprintf(out, out_len, "password_hash_placeholder");
#endif
}

void calibre_get_timestamp(char* buf, size_t len) {
  time_t now = time(NULL);
  struct tm* tm_info = gmtime(&now);
  strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

/* ============================================================================
 * Buffer Management
 * ============================================================================ */

calibre_err_t calibre_buf_init(calibre_buffer_t* buf, size_t capacity) {
  buf->data = (uint8_t*)malloc(capacity);
  if (!buf->data) {
    return CALIBRE_ERR_NOMEM;
  }
  buf->capacity = capacity;
  buf->len = 0;
  buf->pos = 0;
  return CALIBRE_OK;
}

void calibre_buf_free(calibre_buffer_t* buf) {
  if (buf->data) {
    free(buf->data);
    buf->data = NULL;
  }
  buf->capacity = 0;
  buf->len = 0;
  buf->pos = 0;
}

void calibre_buf_reset(calibre_buffer_t* buf) {
  buf->len = 0;
  buf->pos = 0;
}

calibre_err_t calibre_buf_append(calibre_buffer_t* buf, const uint8_t* data, size_t len) {
  if (buf->len + len > buf->capacity) {
    return CALIBRE_ERR_NOMEM;
  }
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return CALIBRE_OK;
}

/* ============================================================================
 * Minimal JSON Parser
 * ============================================================================ */

void json_parser_init(json_parser_t* p, const char* json, size_t len) {
  p->json = json;
  p->len = len;
  p->pos = 0;
}

const char* json_find_string(json_parser_t* p, const char* key, size_t* out_len) {
  size_t key_len = strlen(key);
  const char* ptr = p->json;
  const char* end = p->json + p->len;

  /* Search for "key": pattern */
  while (ptr < end - key_len - 3) {
    if (*ptr == '"') {
      if (strncmp(ptr + 1, key, key_len) == 0 && ptr[key_len + 1] == '"') {
        ptr += key_len + 2; /* Skip past key and closing quote */

        /* Skip whitespace and colon */
        while (ptr < end && (*ptr == ' ' || *ptr == ':' || *ptr == '\t')) {
          ptr++;
        }

        if (ptr < end && *ptr == '"') {
          ptr++; /* Skip opening quote */
          const char* value_start = ptr;

          /* Find closing quote (handle escapes) */
          while (ptr < end) {
            if (*ptr == '\\' && ptr + 1 < end) {
              ptr += 2; /* Skip escaped char */
            } else if (*ptr == '"') {
              *out_len = ptr - value_start;
              return value_start;
            } else {
              ptr++;
            }
          }
        }
      }
    }
    ptr++;
  }

  *out_len = 0;
  return NULL;
}

bool json_find_int(json_parser_t* p, const char* key, int64_t* out) {
  size_t key_len = strlen(key);
  const char* ptr = p->json;
  const char* end = p->json + p->len;

  while (ptr < end - key_len - 3) {
    if (*ptr == '"') {
      if (strncmp(ptr + 1, key, key_len) == 0 && ptr[key_len + 1] == '"') {
        ptr += key_len + 2;

        while (ptr < end && (*ptr == ' ' || *ptr == ':' || *ptr == '\t')) {
          ptr++;
        }

        if (ptr < end && (*ptr == '-' || (*ptr >= '0' && *ptr <= '9'))) {
          *out = strtoll(ptr, NULL, 10);
          return true;
        }
      }
    }
    ptr++;
  }
  return false;
}

bool json_find_bool(json_parser_t* p, const char* key, bool* out) {
  size_t key_len = strlen(key);
  const char* ptr = p->json;
  const char* end = p->json + p->len;

  while (ptr < end - key_len - 3) {
    if (*ptr == '"') {
      if (strncmp(ptr + 1, key, key_len) == 0 && ptr[key_len + 1] == '"') {
        ptr += key_len + 2;

        while (ptr < end && (*ptr == ' ' || *ptr == ':' || *ptr == '\t')) {
          ptr++;
        }

        if (ptr + 4 <= end && strncmp(ptr, "true", 4) == 0) {
          *out = true;
          return true;
        }
        if (ptr + 5 <= end && strncmp(ptr, "false", 5) == 0) {
          *out = false;
          return true;
        }
      }
    }
    ptr++;
  }
  return false;
}

/* ============================================================================
 * Device Config Functions
 * ============================================================================ */

void calibre_device_config_init(calibre_device_config_t* config) {
  memset(config, 0, sizeof(*config));
  calibre_strlcpy(config->device_name, "Snapix Reader", sizeof(config->device_name));
  calibre_strlcpy(config->manufacturer, "Snapix", sizeof(config->manufacturer));
  calibre_strlcpy(config->model, "X4", sizeof(config->model));
  calibre_gen_uuid(config->device_store_uuid, sizeof(config->device_store_uuid));
  config->cache_uses_lpath = 1;
  config->can_use_send = 1;
  config->can_receive = 1;
  config->can_delete_books = 0; /* Safer default */
}

calibre_err_t calibre_device_config_add_ext(calibre_device_config_t* config, const char* ext) {
  if (!config || !ext) {
    return CALIBRE_ERR_INVALID_ARG;
  }
  if (config->extension_count >= CALIBRE_MAX_EXTENSIONS) {
    return CALIBRE_ERR_NOMEM;
  }

  calibre_strlcpy(config->extensions[config->extension_count], ext, CALIBRE_MAX_EXT_LEN);
  config->extension_count++;
  return CALIBRE_OK;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

calibre_err_t calibre_init(void) {
  if (s_initialized) {
    return CALIBRE_OK;
  }

#ifndef ESP_PLATFORM
  /* Seed random for non-ESP platforms */
  srand((unsigned int)time(NULL));
#endif

  CAL_LOGI(TAG, "Calibre wireless library initialized");
  s_initialized = true;
  return CALIBRE_OK;
}

void calibre_deinit(void) {
  s_initialized = false;
  CAL_LOGI(TAG, "Calibre wireless library deinitialized");
}

/* ============================================================================
 * Connection Management
 * ============================================================================ */

calibre_conn_t* calibre_conn_create(const calibre_device_config_t* config, const calibre_callbacks_t* callbacks) {
  if (!s_initialized) {
    CAL_LOGE(TAG, "Library not initialized");
    return NULL;
  }

  calibre_conn_t* conn = (calibre_conn_t*)calloc(1, sizeof(calibre_conn_t));
  if (!conn) {
    CAL_LOGE(TAG, "Failed to allocate connection");
    return NULL;
  }

  /* Initialize config */
  if (config) {
    memcpy(&conn->config, config, sizeof(calibre_device_config_t));
  } else {
    calibre_device_config_init(&conn->config);
  }

  /* Initialize callbacks */
  if (callbacks) {
    memcpy(&conn->callbacks, callbacks, sizeof(calibre_callbacks_t));
  }

  /* Initialize sockets to invalid */
  conn->tcp_socket = -1;
  conn->tcp_listen_socket = -1;
  for (int i = 0; i < CALIBRE_BROADCAST_PORT_COUNT; i++) {
    conn->udp_sockets[i] = -1;
  }

  /* Default books directory */
  calibre_strlcpy(conn->books_dir, "/Calibre", sizeof(conn->books_dir));

  /* Initialize receive buffer */
  if (calibre_buf_init(&conn->recv_buf, CALIBRE_JSON_BUF_SIZE) != CALIBRE_OK) {
    CAL_LOGE(TAG, "Failed to allocate receive buffer");
    free(conn);
    return NULL;
  }

  conn->state = CALIBRE_STATE_IDLE;
  conn->listen_port = CALIBRE_DEFAULT_PORT;

  CAL_LOGI(TAG, "Connection created for device: %s", conn->config.device_name);
  return conn;
}

void calibre_conn_destroy(calibre_conn_t* conn) {
  if (!conn) return;

  /* Disconnect if connected */
  calibre_disconnect(conn);
  calibre_stop_discovery(conn);

  /* Close listen socket if open */
  if (conn->tcp_listen_socket >= 0) {
    close(conn->tcp_listen_socket);
    conn->tcp_listen_socket = -1;
  }

  /* Free receive buffer */
  calibre_buf_free(&conn->recv_buf);

  /* Free book reception context if active */
  if (conn->book_recv) {
    if (conn->book_recv->fd >= 0) {
      close(conn->book_recv->fd);
    }
    free(conn->book_recv);
  }

  free(conn);
  CAL_LOGI(TAG, "Connection destroyed");
}

/* ============================================================================
 * Storage Functions
 * ============================================================================ */

calibre_err_t calibre_set_books_dir(calibre_conn_t* conn, const char* path) {
  if (!conn || !path) {
    return CALIBRE_ERR_INVALID_ARG;
  }
  calibre_strlcpy(conn->books_dir, path, sizeof(conn->books_dir));
  return CALIBRE_OK;
}

calibre_err_t calibre_set_password(calibre_conn_t* conn, const char* password) {
  if (!conn) {
    return CALIBRE_ERR_INVALID_ARG;
  }
  if (password) {
    calibre_strlcpy(conn->password_hash, password, sizeof(conn->password_hash));
  } else {
    conn->password_hash[0] = '\0';
  }
  return CALIBRE_OK;
}

bool calibre_is_connected(const calibre_conn_t* conn) { return conn && conn->connected; }
