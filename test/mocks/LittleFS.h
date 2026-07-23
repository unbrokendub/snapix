#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>

// Mock File class for LittleFS
class File {
 public:
  File() = default;

  void setBuffer(const std::string& data) {
    buffer_ = data;
    sharedBuffer_.reset();
    pos_ = 0;
    isOpen_ = true;
  }

  void setSharedBuffer(std::shared_ptr<std::string> data) {
    sharedBuffer_ = std::move(data);
    buffer_ = *sharedBuffer_;
    pos_ = 0;
    isOpen_ = true;
  }

  void seekToEnd() { pos_ = buffer_.size(); }

  operator bool() const { return isOpen_; }

  void close() {
    if (sharedBuffer_) *sharedBuffer_ = buffer_;
    isOpen_ = false;
    pos_ = 0;
  }

  size_t size() const { return buffer_.size(); }

  size_t position() const { return pos_; }

  bool seek(size_t pos) {
    if (pos > buffer_.size()) return false;
    pos_ = pos;
    return true;
  }

  int read(uint8_t* buf, size_t len) {
    if (!isOpen_) return -1;
    size_t toRead = std::min(len, buffer_.size() - pos_);
    if (toRead == 0) return 0;
    memcpy(buf, buffer_.data() + pos_, toRead);
    pos_ += toRead;
    return static_cast<int>(toRead);
  }

  int read() {
    if (!isOpen_ || pos_ >= buffer_.size()) return -1;
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

  bool available() const { return isOpen_ && pos_ < buffer_.size(); }

  size_t write(const uint8_t* buf, size_t len) {
    if (!isOpen_) return 0;
    if (pos_ + len > buffer_.size()) {
      buffer_.resize(pos_ + len);
    }
    memcpy(&buffer_[pos_], buf, len);
    pos_ += len;
    return len;
  }

  size_t write(uint8_t byte) { return write(&byte, 1); }
  void flush() {
    if (sharedBuffer_) *sharedBuffer_ = buffer_;
  }

 private:
  std::string buffer_;
  std::shared_ptr<std::string> sharedBuffer_;
  size_t pos_ = 0;
  bool isOpen_ = false;
};

// Mock LittleFS filesystem
class MockLittleFS {
 public:
  void registerFile(const std::string& path, const std::string& data) { files_[path] = data; }

  void clearFiles() {
    files_.clear();
    writtenFiles_.clear();
  }

  void clearWrittenFiles() { writtenFiles_.clear(); }

  File open(const char* path, const char* mode = "r") {
    File file;
    if (mode && std::strchr(mode, 'w')) {
      auto data = std::make_shared<std::string>();
      files_.erase(path);
      writtenFiles_[path] = data;
      file.setSharedBuffer(std::move(data));
      return file;
    }
    if (mode && (std::strchr(mode, '+') || std::strchr(mode, 'a'))) {
      std::shared_ptr<std::string> data;
      auto written = writtenFiles_.find(path);
      if (written != writtenFiles_.end()) {
        data = written->second;
      } else {
        auto registered = files_.find(path);
        if (registered == files_.end()) return file;
        data = std::make_shared<std::string>(registered->second);
        files_.erase(registered);
        writtenFiles_[path] = data;
      }
      file.setSharedBuffer(std::move(data));
      if (std::strchr(mode, 'a')) file.seekToEnd();
      return file;
    }
    auto written = writtenFiles_.find(path);
    if (written != writtenFiles_.end() && written->second) {
      file.setSharedBuffer(written->second);
      return file;
    }
    auto it = files_.find(path);
    if (it != files_.end()) {
      file.setBuffer(it->second);
    }
    return file;
  }

  bool exists(const char* path) {
    return files_.find(path) != files_.end() ||
           writtenFiles_.find(path) != writtenFiles_.end();
  }

  bool remove(const char* path) {
    const size_t a = files_.erase(path);
    const size_t b = writtenFiles_.erase(path);
    return a + b > 0;
  }

  bool rename(const char* from, const char* to) {
    if (failNextRename_) {
      failNextRename_ = false;
      return false;
    }
    auto written = writtenFiles_.find(from);
    if (written != writtenFiles_.end()) {
      remove(to);
      writtenFiles_[to] = written->second;
      writtenFiles_.erase(written);
      return true;
    }
    auto registered = files_.find(from);
    if (registered != files_.end()) {
      remove(to);
      files_[to] = registered->second;
      files_.erase(registered);
      return true;
    }
    return false;
  }

  void failNextRename() { failNextRename_ = true; }

  std::string getWrittenData(const std::string& path) const {
    const auto it = writtenFiles_.find(path);
    return it != writtenFiles_.end() && it->second ? *it->second : std::string();
  }

 private:
  std::map<std::string, std::string> files_;
  std::map<std::string, std::shared_ptr<std::string>> writtenFiles_;
  bool failNextRename_ = false;
};

extern MockLittleFS LittleFS;
