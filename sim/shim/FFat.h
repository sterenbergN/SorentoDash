// FAT partition stand-in: files live in sim/state/ffat/.
#pragma once
#include <cstdio>
#include <filesystem>
#include <memory>
#include "Arduino.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
public:
  File() {}
  explicit File(FILE *f) : f_(f, [](FILE *p) { if (p) fclose(p); }) {}
  operator bool() const { return (bool)f_; }
  size_t read(uint8_t *buf, size_t n) { return f_ ? fread(buf, 1, n, f_.get()) : 0; }
  size_t write(const uint8_t *buf, size_t n) { return f_ ? fwrite(buf, 1, n, f_.get()) : 0; }
  bool seek(size_t pos) { return f_ && fseek(f_.get(), (long)pos, SEEK_SET) == 0; }
  size_t size() {
    if (!f_) return 0;
    long at = ftell(f_.get());
    fseek(f_.get(), 0, SEEK_END);
    long n = ftell(f_.get());
    fseek(f_.get(), at, SEEK_SET);
    return (size_t)n;
  }
  void close() { f_.reset(); }

private:
  std::shared_ptr<FILE> f_;
};

class FFatFS {
public:
  bool begin(bool = false, const char * = nullptr, uint8_t = 10, const char * = nullptr) {
    std::filesystem::create_directories(root());
    return true;
  }
  void end() {}
  bool format() {
    std::error_code e;
    std::filesystem::remove_all(root(), e);
    std::filesystem::create_directories(root());
    return true;
  }
  size_t totalBytes() { return 0x9E0000; }
  size_t usedBytes() {
    size_t n = 0;
    std::error_code e;
    for (auto &f : std::filesystem::directory_iterator(root(), e)) n += f.is_regular_file() ? (size_t)f.file_size() : 0;
    return n;
  }
  File open(const String &path, const char *mode = FILE_READ) {
    std::string m = mode;
    if (m == "r" && !std::filesystem::exists(full(path))) return File();
    std::string fm = m == "r" ? "rb" : m == "w" ? "wb" : m == "a" ? "ab" : m == "r+" ? "r+b" : "rb";
    return File(fopen(full(path).c_str(), fm.c_str()));
  }
  bool remove(const String &path) {
    std::error_code e;
    return std::filesystem::remove(full(path), e);
  }
  bool exists(const String &path) { return std::filesystem::exists(full(path)); }
  bool rename(const String &from, const String &to) {
    std::error_code e;
    std::filesystem::remove(full(to), e);  // like LittleFS: replaces an existing file
    std::filesystem::rename(full(from), full(to), e);
    return !e;
  }

private:
  static std::string root() { return sim::stateDir() + "/ffat"; }
  static std::string full(const String &p) { return root() + p.c_str(); }
};

inline FFatFS FFat;
