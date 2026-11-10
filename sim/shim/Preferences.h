// NVS Preferences stand-in: one small text file per namespace under
// sim/state/prefs/, so settings survive simulator restarts.
#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include "Arduino.h"

class Preferences {
public:
  bool begin(const char *ns, bool = false) {
    std::lock_guard<std::recursive_mutex> l(m_);
    std::filesystem::create_directories(sim::stateDir() + "/prefs");
    path_ = sim::stateDir() + "/prefs/" + ns + ".txt";
    kv_.clear();
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
      size_t t = line.find('\t');
      if (t != std::string::npos) kv_[line.substr(0, t)] = unescape(line.substr(t + 1));
    }
    return true;
  }
  void end() {}
  bool isKey(const char *k) { std::lock_guard<std::recursive_mutex> l(m_); return kv_.count(k) > 0; }
  bool remove(const char *k) { std::lock_guard<std::recursive_mutex> l(m_); kv_.erase(k); save(); return true; }
  bool clear() { std::lock_guard<std::recursive_mutex> l(m_); kv_.clear(); save(); return true; }

  size_t putInt(const char *k, int32_t v) { return put(k, std::to_string(v)); }
  size_t putUInt(const char *k, uint32_t v) { return put(k, std::to_string(v)); }
  size_t putChar(const char *k, int8_t v) { return put(k, std::to_string(v)); }
  size_t putUChar(const char *k, uint8_t v) { return put(k, std::to_string(v)); }
  size_t putUShort(const char *k, uint16_t v) { return put(k, std::to_string(v)); }
  size_t putBool(const char *k, bool v) { return put(k, v ? "1" : "0"); }
  size_t putFloat(const char *k, float v) {
    char b[48];
    snprintf(b, sizeof(b), "%.9g", v);
    return put(k, b);
  }
  size_t putString(const char *k, const String &v) { return put(k, v.c_str()); }
  size_t putString(const char *k, const char *v) { return put(k, v); }

  int32_t getInt(const char *k, int32_t d = 0) { return get(k) ? (int32_t)std::stol(*get(k)) : d; }
  uint32_t getUInt(const char *k, uint32_t d = 0) { return get(k) ? (uint32_t)std::stoul(*get(k)) : d; }
  int8_t getChar(const char *k, int8_t d = 0) { return get(k) ? (int8_t)std::stoi(*get(k)) : d; }
  uint8_t getUChar(const char *k, uint8_t d = 0) { return get(k) ? (uint8_t)std::stoi(*get(k)) : d; }
  uint16_t getUShort(const char *k, uint16_t d = 0) { return get(k) ? (uint16_t)std::stoi(*get(k)) : d; }
  bool getBool(const char *k, bool d = false) { return get(k) ? *get(k) == "1" : d; }
  float getFloat(const char *k, float d = NAN) { return get(k) ? std::stof(*get(k)) : d; }
  String getString(const char *k, const String &d = String()) { return get(k) ? String(*get(k)) : d; }

private:
  std::string path_;
  std::map<std::string, std::string> kv_;
  std::recursive_mutex m_;

  const std::string *get(const char *k) {
    std::lock_guard<std::recursive_mutex> l(m_);
    auto it = kv_.find(k);
    return it == kv_.end() ? nullptr : &it->second;
  }
  size_t put(const char *k, const std::string &v) {
    std::lock_guard<std::recursive_mutex> l(m_);
    kv_[k] = v;
    save();
    return v.size() + 1;
  }
  void save() {
    std::ofstream f(path_, std::ios::trunc);
    for (auto &p : kv_) f << p.first << '\t' << escape(p.second) << '\n';
  }
  static std::string escape(const std::string &s) {
    std::string o;
    for (char c : s) {
      if (c == '\\') o += "\\\\";
      else if (c == '\n') o += "\\n";
      else if (c == '\t') o += "\\t";
      else o += c;
    }
    return o;
  }
  static std::string unescape(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char n = s[++i];
        o += n == 'n' ? '\n' : n == 't' ? '\t' : n;
      } else {
        o += s[i];
      }
    }
    return o;
  }
};
