#include "SnapixWebServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "../config.h"
#include "html/AppPageHtml.generated.h"

#define TAG "WEBSERVER"

// ── Lightweight NFD → NFC normalizer for filenames ───────────────
// macOS (Safari, Finder) sends filenames in NFD: "й" becomes "и" +
// combining breve (U+0306).  FAT32 stores the raw bytes, but macOS's
// FAT32 driver then can't stat/rename them because it normalizes the
// lookup path to NFC and the bytes don't match.
//
// We compose the most common Cyrillic and Latin decompositions here
// before writing to the SD card.  This is NOT a full Unicode NFC
// implementation — just enough for real-world filenames.
static uint32_t tryComposeNfc(uint32_t base, uint32_t combining) {
  switch (combining) {
    case 0x0300:  // combining grave
      switch (base) {
        case 'a': return 0xE0; case 'A': return 0xC0;
        case 'e': return 0xE8; case 'E': return 0xC8;
        case 'i': return 0xEC; case 'I': return 0xCC;
        case 'o': return 0xF2; case 'O': return 0xD2;
        case 'u': return 0xF9; case 'U': return 0xD9;
      }
      break;
    case 0x0301:  // combining acute
      switch (base) {
        case 'a': return 0xE1; case 'A': return 0xC1;
        case 'e': return 0xE9; case 'E': return 0xC9;
        case 'i': return 0xED; case 'I': return 0xCD;
        case 'o': return 0xF3; case 'O': return 0xD3;
        case 'u': return 0xFA; case 'U': return 0xDA;
        case 'y': return 0xFD; case 'Y': return 0xDD;
      }
      break;
    case 0x0302:  // combining circumflex
      switch (base) {
        case 'a': return 0xE2; case 'A': return 0xC2;
        case 'e': return 0xEA; case 'E': return 0xCA;
        case 'i': return 0xEE; case 'I': return 0xCE;
        case 'o': return 0xF4; case 'O': return 0xD4;
        case 'u': return 0xFB; case 'U': return 0xDB;
      }
      break;
    case 0x0303:  // combining tilde
      switch (base) {
        case 'a': return 0xE3; case 'A': return 0xC3;
        case 'n': return 0xF1; case 'N': return 0xD1;
        case 'o': return 0xF5; case 'O': return 0xD5;
      }
      break;
    case 0x0306:  // combining breve  (Cyrillic й/Й, Belarusian ў/Ў)
      switch (base) {
        case 0x0438: return 0x0439;  // и → й
        case 0x0418: return 0x0419;  // И → Й
        case 0x0443: return 0x045E;  // у → ў
        case 0x0423: return 0x040E;  // У → Ў
      }
      break;
    case 0x0308:  // combining diaeresis
      switch (base) {
        case 'a': return 0xE4; case 'A': return 0xC4;
        case 'e': return 0xEB; case 'E': return 0xCB;
        case 'i': return 0xEF; case 'I': return 0xCF;
        case 'o': return 0xF6; case 'O': return 0xD6;
        case 'u': return 0xFC; case 'U': return 0xDC;
        case 'y': return 0xFF; case 'Y': return 0x0178;
        case 0x0435: return 0x0451;  // е → ё
        case 0x0415: return 0x0401;  // Е → Ё
      }
      break;
    case 0x030C:  // combining caron  (č, š, ž…)
      switch (base) {
        case 'c': return 0x010D; case 'C': return 0x010C;
        case 's': return 0x0161; case 'S': return 0x0160;
        case 'z': return 0x017E; case 'Z': return 0x017D;
      }
      break;
    case 0x0327:  // combining cedilla
      switch (base) {
        case 'c': return 0xE7; case 'C': return 0xC7;
      }
      break;
  }
  return 0;  // no composition found — keep as-is
}

static String normalizeFilenameNFC(const String& input) {
  const char* s = input.c_str();
  const size_t len = input.length();
  String out;
  out.reserve(len);

  size_t i = 0;
  while (i < len) {
    // ── decode one UTF-8 character (the potential base) ──────────
    const size_t charStart = i;
    uint32_t base;
    const uint8_t b0 = static_cast<uint8_t>(s[i]);
    size_t charLen;
    if (b0 < 0x80) {
      base = b0; charLen = 1;
    } else if ((b0 & 0xE0) == 0xC0 && i + 1 < len) {
      base = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
      charLen = 2;
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < len) {
      base = ((b0 & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
             (static_cast<uint8_t>(s[i + 2]) & 0x3F);
      charLen = 3;
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < len) {
      base = ((b0 & 0x07) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
             ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
             (static_cast<uint8_t>(s[i + 3]) & 0x3F);
      charLen = 4;
    } else {
      out += s[i]; i++; continue;  // invalid / pass-through
    }

    const size_t nextPos = i + charLen;

    // ── peek at the next character: is it a combining mark? ─────
    if (nextPos + 1 < len && static_cast<uint8_t>(s[nextPos]) == 0xCC) {
      // U+0300..U+033F → CC 80..CC BF
      uint32_t comb = ((0xCC & 0x1F) << 6) | (static_cast<uint8_t>(s[nextPos + 1]) & 0x3F);
      uint32_t composed = tryComposeNfc(base, comb);
      if (composed) {
        // encode composed codepoint
        if (composed < 0x80) {
          out += static_cast<char>(composed);
        } else if (composed < 0x800) {
          out += static_cast<char>(0xC0 | (composed >> 6));
          out += static_cast<char>(0x80 | (composed & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (composed >> 12));
          out += static_cast<char>(0x80 | ((composed >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (composed & 0x3F));
        }
        i = nextPos + 2;  // skip base + combining mark
        continue;
      }
    }
    if (nextPos + 1 < len && static_cast<uint8_t>(s[nextPos]) == 0xCD) {
      // U+0340..U+037F → CD 80..CD BF  (rare, but check)
      uint32_t comb = ((0xCD & 0x1F) << 6) | (static_cast<uint8_t>(s[nextPos + 1]) & 0x3F);
      uint32_t composed = tryComposeNfc(base, comb);
      if (composed) {
        if (composed < 0x80) {
          out += static_cast<char>(composed);
        } else if (composed < 0x800) {
          out += static_cast<char>(0xC0 | (composed >> 6));
          out += static_cast<char>(0x80 | (composed & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (composed >> 12));
          out += static_cast<char>(0x80 | ((composed >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (composed & 0x3F));
        }
        i = nextPos + 2;
        continue;
      }
    }

    // no composition — copy original bytes
    for (size_t j = charStart; j < nextPos; j++) out += s[j];
    i = nextPos;
  }
  return out;
}

namespace snapix {

static void sendGzipHtml(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

bool SnapixWebServer::flushUploadBuffer() {
  if (upload_.bufferPos > 0 && upload_.file) {
    const size_t written = upload_.file.write(upload_.buffer.data(), upload_.bufferPos);
    if (written != upload_.bufferPos) {
      upload_.bufferPos = 0;
      return false;
    }
    upload_.bufferPos = 0;
  }
  return true;
}

SnapixWebServer::SnapixWebServer() = default;

SnapixWebServer::~SnapixWebServer() { stop(); }

void SnapixWebServer::begin() {
  if (running_) {
    LOG_DBG(TAG, "Server already running");
    return;
  }

  // Check network connection
  wifi_mode_t wifiMode = WiFi.getMode();
  bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  bool isInApMode = (wifiMode & WIFI_MODE_AP);

  if (!isStaConnected && !isInApMode) {
    LOG_ERR(TAG, "Cannot start - no network connection");
    return;
  }

  apMode_ = isInApMode;

  LOG_INF(TAG, "Creating server on port %d (free heap: %d)", port_, ESP.getFreeHeap());

  server_.reset(new WebServer(port_));
  if (!server_) {
    LOG_ERR(TAG, "Failed to create WebServer");
    return;
  }

  // Setup routes
  server_->on("/", HTTP_GET, [this] { handleRoot(); });
  server_->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server_->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server_->on("/download", HTTP_GET, [this] { handleDownload(); });
  server_->on("/upload", HTTP_POST, [this] { handleUploadPost(); }, [this] { handleUpload(); });
  server_->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });
  server_->on("/delete", HTTP_POST, [this] { handleDelete(); });
  server_->on("/rename", HTTP_POST, [this] { handleRename(); });
  server_->onNotFound([this] { handleNotFound(); });

  server_->begin();
  running_ = true;

  String ipAddr = apMode_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_INF(TAG, "Server started at http://%s/", ipAddr.c_str());
}

void SnapixWebServer::stop() {
  if (!running_ || !server_) {
    return;
  }

  LOG_INF(TAG, "Stopping server (free heap: %d)", ESP.getFreeHeap());

  running_ = false;
  delay(100);

  server_->stop();
  delay(50);
  server_.reset();

  // Clear upload state
  if (upload_.file) {
    upload_.file.close();
  }
  upload_.fileName = "";
  upload_.path = "/";
  upload_.size = 0;
  upload_.success = false;
  upload_.error = "";
  upload_.bufferPos = 0;
  upload_.buffer.clear();
  upload_.buffer.shrink_to_fit();

  LOG_INF(TAG, "Server stopped (free heap: %d)", ESP.getFreeHeap());
}

void SnapixWebServer::handleClient() {
  if (!running_ || !server_) {
    return;
  }
  server_->handleClient();
}

void SnapixWebServer::handleRoot() { sendGzipHtml(server_.get(), AppPageHtml, AppPageHtmlCompressedSize); }

void SnapixWebServer::handleNotFound() { server_->send(404, "text/plain", "404 Not Found"); }

void SnapixWebServer::handleStatus() {
  String ipAddr = apMode_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  char json[256];
  snprintf(json, sizeof(json),
           "{\"version\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"freeHeap\":%u,\"uptime\":%lu}",
           SNAPIX_VERSION, ipAddr.c_str(), apMode_ ? "AP" : "STA", apMode_ ? 0 : WiFi.RSSI(), ESP.getFreeHeap(),
           millis() / 1000);

  server_->send(200, "application/json", json);
}

void SnapixWebServer::handleFileListData() {
  String currentPath = "/";
  if (server_->hasArg("path")) {
    currentPath = server_->arg("path");
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  FsFile root = SdMan.open(currentPath.c_str());
  if (!root || !root.isDirectory()) {
    server_->send(404, "application/json", "[]");
    if (root) root.close();
    return;
  }

  server_->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_->send(200, "application/json", "");
  server_->sendContent("[");

  char name[256];
  bool seenFirst = false;
  FsFile file = root.openNextFile();

  while (file) {
    file.getName(name, sizeof(name));

    // Skip hidden items
    if (name[0] != '.' && !FsHelpers::isHiddenFsItem(name)) {
      JsonDocument doc;
      doc["name"] = name;
      doc["isDirectory"] = file.isDirectory();

      if (file.isDirectory()) {
        doc["size"] = 0;
        doc["isEpub"] = false;
      } else {
        doc["size"] = file.size();
        doc["isEpub"] = FsHelpers::isEpubFile(name);
      }

      char output[512];
      size_t written = serializeJson(doc, output, sizeof(output));
      if (written < sizeof(output)) {
        if (seenFirst) {
          server_->sendContent(",");
        } else {
          seenFirst = true;
        }
        server_->sendContent(output);
      }
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();
  server_->sendContent("]");
  server_->sendContent("");
}

void SnapixWebServer::handleUpload() {
  if (!running_ || !server_) return;

  HTTPUpload& upload = server_->upload();

  if (upload.status == UPLOAD_FILE_START) {
    upload_.fileName = normalizeFilenameNFC(upload.filename);
    upload_.size = 0;
    upload_.success = false;
    upload_.error = "";
    upload_.bufferPos = 0;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < UploadState::BUFFER_SIZE * 2) {
      upload_.error = "Insufficient memory for upload";
      return;
    }
    upload_.buffer.resize(UploadState::BUFFER_SIZE);

    if (server_->hasArg("path")) {
      upload_.path = server_->arg("path");
      if (!upload_.path.startsWith("/")) {
        upload_.path = "/" + upload_.path;
      }
      if (upload_.path.length() > 1 && upload_.path.endsWith("/")) {
        upload_.path = upload_.path.substring(0, upload_.path.length() - 1);
      }
    } else {
      upload_.path = "/";
    }

    LOG_INF(TAG, "Upload start: %s to %s", upload_.fileName.c_str(), upload_.path.c_str());

    String filePath = upload_.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += upload_.fileName;

    if (!FsHelpers::isSupportedBookFile(upload_.fileName.c_str()) &&
        !FsHelpers::isImageFile(upload_.fileName.c_str()) &&
        !FsHelpers::hasExtension(upload_.fileName.c_str(), ".epdfont") &&
        !FsHelpers::hasExtension(upload_.fileName.c_str(), ".bin") &&
        !FsHelpers::hasExtension(upload_.fileName.c_str(), ".theme")) {
      upload_.error = "Unsupported file type";
      LOG_ERR(TAG, "Rejected upload: %s (unsupported type)", upload_.fileName.c_str());
      return;
    }

    if (SdMan.exists(filePath.c_str())) {
      SdMan.remove(filePath.c_str());
    }

    if (!SdMan.openFileForWrite("WEB", filePath, upload_.file)) {
      upload_.error = "Failed to create file";
      LOG_ERR(TAG, "Failed to create: %s", filePath.c_str());
      return;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (upload_.file && upload_.error.isEmpty()) {
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        size_t space = UploadState::BUFFER_SIZE - upload_.bufferPos;
        size_t toCopy = remaining < space ? remaining : space;
        memcpy(upload_.buffer.data() + upload_.bufferPos, data, toCopy);
        upload_.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        if (upload_.bufferPos >= UploadState::BUFFER_SIZE) {
          if (!flushUploadBuffer()) {
            upload_.error = "Write failed - disk full?";
            upload_.file.close();
            return;
          }
        }
      }

      upload_.size += upload.currentSize;
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (upload_.file) {
      if (upload_.error.isEmpty() && !flushUploadBuffer()) {
        upload_.error = "Write failed - disk full?";
      }
      upload_.file.close();
      if (upload_.error.isEmpty()) {
        upload_.success = true;
        LOG_INF(TAG, "Upload complete: %s (%zu bytes)", upload_.fileName.c_str(), upload_.size);
      }
    }
    upload_.buffer.clear();
    upload_.buffer.shrink_to_fit();

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    upload_.bufferPos = 0;
    upload_.buffer.clear();
    upload_.buffer.shrink_to_fit();
    if (upload_.file) {
      upload_.file.close();
      String filePath = upload_.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += upload_.fileName;
      SdMan.remove(filePath.c_str());
    }
    upload_.error = "Upload aborted";
    LOG_ERR(TAG, "Upload aborted");
  }
}

void SnapixWebServer::handleUploadPost() {
  if (upload_.success) {
    server_->send(200, "text/plain", "File uploaded: " + upload_.fileName);
  } else {
    String error = upload_.error.isEmpty() ? "Unknown error" : upload_.error;
    server_->send(400, "text/plain", error);
  }
}

void SnapixWebServer::handleCreateFolder() {
  if (!server_->hasArg("name")) {
    server_->send(400, "text/plain", "Missing folder name");
    return;
  }

  String folderName = server_->arg("name");
  if (folderName.isEmpty()) {
    server_->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  String parentPath = "/";
  if (server_->hasArg("path")) {
    parentPath = server_->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (SdMan.exists(folderPath.c_str())) {
    server_->send(400, "text/plain", "Folder already exists");
    return;
  }

  if (SdMan.mkdir(folderPath.c_str())) {
    LOG_INF(TAG, "Created folder: %s", folderPath.c_str());
    server_->send(200, "text/plain", "Folder created");
  } else {
    server_->send(500, "text/plain", "Failed to create folder");
  }
}

void SnapixWebServer::handleDelete() {
  if (!server_->hasArg("path")) {
    server_->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server_->arg("path");
  String itemType = server_->hasArg("type") ? server_->arg("type") : "file";

  if (itemPath.isEmpty() || itemPath == "/") {
    server_->send(400, "text/plain", "Cannot delete root");
    return;
  }

  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  // Security: prevent deletion of hidden/system files
  String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".") || FsHelpers::isHiddenFsItem(itemName.c_str())) {
    server_->send(403, "text/plain", "Cannot delete system files");
    return;
  }

  if (!SdMan.exists(itemPath.c_str())) {
    server_->send(404, "text/plain", "Item not found");
    return;
  }

  bool success = false;
  if (itemType == "folder") {
    FsFile dir = SdMan.open(itemPath.c_str());
    if (dir && dir.isDirectory()) {
      FsFile entry = dir.openNextFile();
      if (entry) {
        entry.close();
        dir.close();
        server_->send(400, "text/plain", "Folder not empty");
        return;
      }
      dir.close();
    }
    success = SdMan.rmdir(itemPath.c_str());
  } else {
    success = SdMan.remove(itemPath.c_str());
  }

  if (success) {
    LOG_INF(TAG, "Deleted: %s", itemPath.c_str());
    server_->send(200, "text/plain", "Deleted");
  } else {
    server_->send(500, "text/plain", "Failed to delete");
  }
}

void SnapixWebServer::handleDownload() {
  if (!server_->hasArg("path")) {
    server_->send(400, "text/plain", "Missing path");
    return;
  }

  String filePath = server_->arg("path");
  if (filePath.isEmpty() || !filePath.startsWith("/") || filePath.indexOf("..") >= 0) {
    server_->send(400, "text/plain", "Invalid path");
    return;
  }

  // Security: block hidden/system files and dot-prefix paths
  String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);
  if (fileName.startsWith(".") || FsHelpers::isHiddenFsItem(fileName.c_str())) {
    server_->send(403, "text/plain", "Access denied");
    return;
  }

  FsFile file = SdMan.open(filePath.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server_->send(404, "text/plain", "File not found");
    return;
  }

  size_t fileSize = file.size();

  server_->sendHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
  server_->setContentLength(fileSize);
  server_->send(200, "application/octet-stream", "");

  uint8_t buf[512];
  while (file.available()) {
    const int readResult = file.read(buf, sizeof(buf));
    if (readResult <= 0) break;
    const size_t bytesRead = static_cast<size_t>(readResult);
    server_->client().write(buf, bytesRead);
  }

  file.close();
}

void SnapixWebServer::handleRename() {
  if (!server_->hasArg("path") || !server_->hasArg("newName")) {
    server_->send(400, "text/plain", "Missing path or newName");
    return;
  }

  String itemPath = server_->arg("path");
  String newName = server_->arg("newName");

  if (itemPath.isEmpty() || itemPath == "/" || newName.isEmpty() || itemPath.indexOf("..") >= 0) {
    server_->send(400, "text/plain", "Invalid parameters");
    return;
  }

  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  // Security: reject path traversal and hidden names
  if (newName.indexOf('/') >= 0 || newName.indexOf("..") >= 0 || newName.startsWith(".")) {
    server_->send(400, "text/plain", "Invalid new name");
    return;
  }

  // Security: block hidden/system files
  String oldName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (oldName.startsWith(".") || FsHelpers::isHiddenFsItem(oldName.c_str())) {
    server_->send(403, "text/plain", "Cannot rename system files");
    return;
  }

  if (!SdMan.exists(itemPath.c_str())) {
    server_->send(404, "text/plain", "Item not found");
    return;
  }

  // Build new path: same parent directory + new name
  int lastSlash = itemPath.lastIndexOf('/');
  String newPath = (lastSlash <= 0) ? "/" + newName : itemPath.substring(0, lastSlash) + "/" + newName;

  if (SdMan.exists(newPath.c_str())) {
    server_->send(400, "text/plain", "An item with that name already exists");
    return;
  }

  if (SdMan.rename(itemPath.c_str(), newPath.c_str())) {
    LOG_INF(TAG, "Renamed: %s -> %s", itemPath.c_str(), newPath.c_str());
    server_->send(200, "text/plain", "Renamed");
  } else {
    server_->send(500, "text/plain", "Failed to rename");
  }
}

}  // namespace snapix
