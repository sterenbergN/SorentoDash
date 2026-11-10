// Nested settings menu for the knob: sections open into short lists, a long
// press backs out one level. Items are built on demand (lambdas), so lists
// like the horns or service items always reflect the current data.
#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "UiTypes.h"

struct MenuItem {
  enum Kind : uint8_t { SUB, ACTION, TOGGLE, CHOICE, NUMBER, INFO, BACK };
  Kind kind = INFO;
  String label;
  std::function<String()> value;                    // line under the label
  std::function<uint32_t()> valueColor;             // optional colour for that line
  std::function<void()> action;                     // ACTION / TOGGLE / CHOICE
  std::function<std::vector<MenuItem>()> children;  // SUB
  String confirm;                                   // ACTION: ask first, with this question
  std::function<String()> detail;                   // INFO: full text shown when pressed (default: the value)

  // NUMBER: edited on its own screen
  std::function<float()> get;
  std::function<void(float)> set;
  float min = 0, max = 100, step = 1;
  int decimals = 0;
  const char *unit = "";
  bool accelerate = false;  // big values (odometer): spinning fast takes bigger steps

  static MenuItem sub(const String &l, std::function<std::vector<MenuItem>()> c, std::function<String()> v = nullptr) {
    MenuItem m;
    m.kind = SUB, m.label = l, m.children = c, m.value = v;
    return m;
  }
  static MenuItem act(const String &l, std::function<void()> a, std::function<String()> v = nullptr, const String &ask = "") {
    MenuItem m;
    m.kind = ACTION, m.label = l, m.action = a, m.value = v, m.confirm = ask;
    return m;
  }
  static MenuItem toggle(const String &l, std::function<bool()> get, std::function<void()> flip) {
    MenuItem m;
    m.kind = TOGGLE, m.label = l, m.action = flip;
    m.value = [get] { return String(get() ? "On" : "Off"); };
    return m;
  }
  static MenuItem choice(const String &l, std::function<String()> v, std::function<void()> next) {
    MenuItem m;
    m.kind = CHOICE, m.label = l, m.value = v, m.action = next;
    return m;
  }
  static MenuItem number(const String &l, std::function<float()> g, std::function<void(float)> s, float lo, float hi,
                         float st, int dec, const char *unit, bool accel = false) {
    MenuItem m;
    m.kind = NUMBER, m.label = l, m.get = g, m.set = s, m.min = lo, m.max = hi, m.step = st, m.decimals = dec;
    m.unit = unit, m.accelerate = accel;
    m.value = [g, dec, unit] { return String(g(), dec) + (*unit ? String(*unit == '%' ? "" : " ") + unit : String()); };
    return m;
  }
  static MenuItem info(const String &l, std::function<String()> v, std::function<String()> more = nullptr) {
    MenuItem m;
    m.kind = INFO, m.label = l, m.value = v, m.detail = more;
    return m;
  }
};

class Menu {
public:
  struct Level {
    String title;
    std::vector<MenuItem> items;  // includes the trailing Back
    Wheel wheel;
  };

  std::vector<Level> stack;
  std::function<std::vector<MenuItem>()> root;

  bool open() const { return !stack.empty(); }
  Level &top() { return stack.back(); }
  int count() { return top().items.size(); }
  MenuItem &current() { return top().items[top().wheel.index(count())]; }

  void openRoot() {
    stack.clear();
    push("Settings", root());
  }

  void push(const String &title, std::vector<MenuItem> items) {
    MenuItem back;
    back.kind = MenuItem::BACK;
    back.label = "Back";
    items.push_back(back);
    stack.push_back({title, items, Wheel()});
  }

  // Rebuild the current level in place (after an action changed the list).
  void refresh() {
    if (stack.size() < 2) {
      int at = top().wheel.target;
      openRoot();
      top().wheel.jump(at);
      return;
    }
    Level &parent = stack[stack.size() - 2];
    MenuItem &owner = parent.items[parent.wheel.index(parent.items.size())];
    if (owner.kind != MenuItem::SUB) return;
    int at = top().wheel.target;
    String title = top().title;
    stack.pop_back();
    push(title, owner.children());
    top().wheel.jump(min(at, count() - 1));
  }

  // Back one level; returns false when leaving the menu entirely.
  bool pop() {
    stack.pop_back();
    return !stack.empty();
  }

  // Open a path of section labels from the root, e.g. {"Service", "Log a service"}.
  void openPath(std::initializer_list<const char *> path) {
    openRoot();
    for (const char *label : path) {
      Level &l = top();
      for (size_t i = 0; i < l.items.size(); i++) {
        if (l.items[i].kind == MenuItem::SUB && l.items[i].label == label) {
          l.wheel.jump(i);
          push(l.items[i].label, l.items[i].children());
          break;
        }
      }
    }
  }

  // "SETTINGS > CAR > FUEL & MPG", shortened to the last two levels.
  String breadcrumb() {
    String s;
    size_t from = stack.size() > 2 ? stack.size() - 2 : 0;
    for (size_t i = from; i < stack.size(); i++) s += (i > from ? " > " : "") + stack[i].title;
    s.toUpperCase();
    return s;
  }
};
