// Desktop stand-in for Arduino's String, covering what the firmware uses.
// It also converts implicitly to const char*, because LovyanGFX only offers
// String overloads (drawString, textWidth) on real Arduino builds.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>

class String {
public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String(const String &o) = default;
  String(String &&o) = default;
  explicit String(char c) : s_(1, c) {}
  String(unsigned char v) : s_(std::to_string((unsigned)v)) {}
  String(signed char v) : s_(std::to_string((int)v)) {}
  String(short v) : s_(std::to_string(v)) {}
  String(unsigned short v) : s_(std::to_string(v)) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned int v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  String(long long v) : s_(std::to_string(v)) {}
  String(unsigned long long v) : s_(std::to_string(v)) {}
  String(float v, unsigned char decimals = 2) : s_(fmt(v, decimals)) {}
  String(double v, unsigned char decimals = 2) : s_(fmt(v, decimals)) {}

  String &operator=(const String &o) = default;
  String &operator=(String &&o) = default;
  String &operator=(const char *s) {
    s_ = s ? s : "";
    return *this;
  }

  // ---- access
  unsigned int length() const { return (unsigned)s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  const char *c_str() const { return s_.c_str(); }
  operator const char *() const { return s_.c_str(); }
  const std::string &str() const { return s_; }
  char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
  char operator[](unsigned int i) const { return charAt(i); }
  char operator[](int i) const { return charAt((unsigned)i); }
  char &operator[](unsigned int i) { return s_[i]; }
  char &operator[](int i) { return s_[(unsigned)i]; }
  const char *begin() const { return s_.data(); }
  const char *end() const { return s_.data() + s_.size(); }
  char *begin() { return &s_[0]; }
  char *end() { return &s_[0] + s_.size(); }
  void reserve(unsigned int n) { s_.reserve(n); }

  // ---- append
  bool concat(const String &o) { s_ += o.s_; return true; }
  bool concat(const char *o) { if (o) s_ += o; return true; }
  bool concat(const char *o, unsigned int n) { if (o) s_.append(o, n); return true; }
  bool concat(char c) { s_ += c; return true; }
  bool concat(int v) { s_ += std::to_string(v); return true; }
  String &operator+=(const String &o) { s_ += o.s_; return *this; }
  String &operator+=(const char *o) { if (o) s_ += o; return *this; }
  String &operator+=(char c) { s_ += c; return *this; }

  // ---- compare
  bool operator==(const String &o) const { return s_ == o.s_; }
  bool operator==(const char *o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String &o) const { return s_ != o.s_; }
  bool operator!=(const char *o) const { return !(*this == o); }
  bool operator<(const String &o) const { return s_ < o.s_; }
  bool operator>(const String &o) const { return s_ > o.s_; }
  bool equals(const String &o) const { return s_ == o.s_; }
  bool startsWith(const String &p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
  bool endsWith(const String &p) const {
    return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
  }

  // ---- search
  int indexOf(char c, unsigned int from = 0) const { return pos(s_.find(c, from)); }
  int indexOf(const String &t, unsigned int from = 0) const { return pos(s_.find(t.s_, from)); }
  int indexOf(const char *t, unsigned int from = 0) const { return pos(s_.find(t, from)); }
  int lastIndexOf(char c) const { return pos(s_.rfind(c)); }
  int lastIndexOf(char c, int from) const { return from < 0 ? -1 : pos(s_.rfind(c, (size_t)from)); }
  int lastIndexOf(const String &t) const { return pos(s_.rfind(t.s_)); }
  String substring(unsigned int a) const { return a >= s_.size() ? String() : String(s_.substr(a)); }
  String substring(unsigned int a, unsigned int b) const {
    if (a > b) std::swap(a, b);
    if (a >= s_.size()) return String();
    return String(s_.substr(a, std::min<size_t>(b, s_.size()) - a));
  }

  // ---- modify
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }
  void toUpperCase() { for (auto &c : s_) c = (char)toupper((unsigned char)c); }
  void toLowerCase() { for (auto &c : s_) c = (char)tolower((unsigned char)c); }
  void replace(const String &from, const String &to) {
    if (from.s_.empty()) return;
    size_t p = 0;
    while ((p = s_.find(from.s_, p)) != std::string::npos) {
      s_.replace(p, from.s_.size(), to.s_);
      p += to.s_.size();
    }
  }
  void replace(char from, char to) { for (auto &c : s_) if (c == from) c = to; }
  void remove(unsigned int i) { if (i < s_.size()) s_.erase(i); }
  void remove(unsigned int i, unsigned int n) { if (i < s_.size()) s_.erase(i, n); }

  // ---- convert
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(s_.c_str(), nullptr); }
  double toDouble() const { return strtod(s_.c_str(), nullptr); }

private:
  std::string s_;
  static int pos(size_t p) { return p == std::string::npos ? -1 : (int)p; }
  static std::string fmt(double v, unsigned char d) {
    char b[64];
    snprintf(b, sizeof(b), "%.*f", (int)d, v);
    return b;
  }
};

// Arduino's concatenation result type; here just a String.
class StringSumHelper : public String {
public:
  using String::String;
};

inline String operator+(const String &a, const String &b) { String r(a); r += b; return r; }
inline String operator+(const String &a, const char *b) { String r(a); r += b; return r; }
inline String operator+(const char *a, const String &b) { String r(a); r += b; return r; }
inline String operator+(const String &a, char c) { String r(a); r += c; return r; }
inline String operator+(char c, const String &b) { String r(c); r += b; return r; }
inline String operator+(const String &a, int v) { return a + String(v); }
inline String operator+(const String &a, unsigned v) { return a + String(v); }
inline String operator+(const String &a, long v) { return a + String(v); }
inline String operator+(const String &a, unsigned long v) { return a + String(v); }
inline String operator+(const String &a, unsigned char v) { return a + String(v); }
inline String operator+(const String &a, float v) { return a + String(v); }
inline String operator+(const String &a, double v) { return a + String(v); }
inline bool operator==(const char *a, const String &b) { return b == a; }
inline bool operator!=(const char *a, const String &b) { return !(b == a); }
