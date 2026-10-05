#pragma once
// Natural-ish date words for the GUI's When / Deadline fields and quick-entry sigils. Pure C++ (no GTK), so it is
// unit-testable on its own. Everything resolves to what Store already speaks: "" (cleared), "someday", or an ISO
// YYYY-MM-DD date.
#include <cctype>
#include <ctime>
#include <string>

#include "util.h"

namespace guidates {

struct Parsed {
  bool ok = false;
  std::string value;  // "" = cleared, "someday", or YYYY-MM-DD
  std::string label;  // human preview: "Today", "Tomorrow", "Fri 9 Oct", "Someday", "No date"
};

inline std::tm tmFromIso(const std::string& iso) {
  std::tm t{};
  t.tm_isdst = -1;
  if (iso.size() >= 10) {
    t.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
    t.tm_mon = std::stoi(iso.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(iso.substr(8, 2));
  }
  t.tm_hour = 12;  // noon: immune to DST edges when adding days
  std::mktime(&t);
  return t;
}

inline std::string isoFromTm(std::tm t) {
  std::mktime(&t);  // normalise overflowed fields
  char buf[16];
  std::strftime(buf, sizeof(buf), "%F", &t);
  return buf;
}

inline std::string addDays(const std::string& iso, int days) {
  std::tm t = tmFromIso(iso);
  t.tm_mday += days;
  return isoFromTm(t);
}

inline bool validIso(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) return false;
  int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
  if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970) return false;
  std::tm t = tmFromIso(s);
  return t.tm_mon == m - 1 && t.tm_mday == d;  // rejects 2026-02-31
}

// "Today" / "Tomorrow" / "Yesterday" / "Fri 9 Oct" (+ year when it isn't this year).
inline std::string friendly(const std::string& iso) {
  if (iso.empty()) return "";
  if (iso == "someday") return "Someday";
  if (!validIso(iso)) return iso;
  std::string t = today();
  if (iso == t) return "Today";
  if (iso == addDays(t, 1)) return "Tomorrow";
  if (iso == addDays(t, -1)) return "Yesterday";
  std::tm tm = tmFromIso(iso);
  char buf[32];
  bool sameYear = iso.substr(0, 4) == t.substr(0, 4);
  std::strftime(buf, sizeof(buf), "%a ", &tm);
  std::string out = buf + std::to_string(tm.tm_mday);  // no %-d: not portable
  std::strftime(buf, sizeof(buf), sameYear ? " %b" : " %b %Y", &tm);
  return out + buf;
}

// Whole days from today to `iso` (negative = past).
inline int daysFromToday(const std::string& iso) {
  std::tm a = tmFromIso(today()), b = tmFromIso(iso);
  double secs = std::difftime(std::mktime(&b), std::mktime(&a));
  return (int)(secs / 86400.0 + (secs >= 0 ? 0.5 : -0.5));
}

namespace detail {
inline int weekdayIndex(const std::string& w) {  // 0=Sun
  static const char* full[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
  if (w.size() < 3) return -1;
  for (int i = 0; i < 7; ++i)
    if (std::string(full[i]).rfind(w, 0) == 0) return i;
  return -1;
}
inline int monthIndex(const std::string& w) {  // 1..12
  static const char* full[] = {"january", "february", "march",     "april",   "may",      "june",
                               "july",    "august",   "september", "october", "november", "december"};
  if (w.size() < 3) return 0;
  for (int i = 0; i < 12; ++i)
    if (std::string(full[i]).rfind(w, 0) == 0) return i + 1;
  return 0;
}
inline bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!isdigit((unsigned char)c)) return false;
  return true;
}
inline std::string monthDay(int month, int day) {
  std::string t = today();
  int year = std::stoi(t.substr(0, 4));
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  if (!validIso(buf)) return "";
  if (std::string(buf) < t) std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year + 1, month, day);  // already past -> next year
  return validIso(buf) ? buf : "";
}
}  // namespace detail

// allowSomeday: When accepts "someday"; Deadline does not.
inline Parsed parse(const std::string& raw, bool allowSomeday = true) {
  Parsed r;
  std::string s = lower(trimmed(raw));
  auto done = [&](const std::string& value) {
    r.ok = true;
    r.value = value;
    r.label = value.empty() ? "No date" : friendly(value);
    return r;
  };
  if (s.empty() || s == "none" || s == "clear" || s == "-" || s == "no") return done("");
  std::string t = today();
  if (s == "today" || s == "tod" || s == "now" || s == "tonight" || s == "evening") return done(t);
  if (s == "tomorrow" || s == "tom" || s == "tmr" || s == "tmrw" || s == "tmw") return done(addDays(t, 1));
  if (s == "yesterday") return done(addDays(t, -1));
  if (allowSomeday && (s == "someday" || s == "sd" || s == "some")) return done("someday");
  if (s == "next week" || s == "nextweek" || s == "nw") {
    int wd = tmFromIso(t).tm_wday;  // days until next Monday
    return done(addDays(t, ((8 - wd) % 7) == 0 ? 7 : (8 - wd) % 7));
  }
  if (validIso(s)) return done(s);

  // weekday: always the next such day strictly after today ("fri" on a Friday means next Friday)
  if (int wd = detail::weekdayIndex(s); wd >= 0) {
    int cur = tmFromIso(t).tm_wday;
    int delta = (wd - cur + 7) % 7;
    return done(addDays(t, delta == 0 ? 7 : delta));
  }

  // relative: +3, +3d, 3d, +2w, 2w, +1m, in 3 days, in 2 weeks
  std::string rel = s;
  if (rel.rfind("in ", 0) == 0) rel = trimmed(rel.substr(3));
  if (!rel.empty() && rel[0] == '+') rel = rel.substr(1);
  {
    size_t i = 0;
    while (i < rel.size() && isdigit((unsigned char)rel[i])) ++i;
    if (i > 0) {
      int n = std::stoi(rel.substr(0, i));
      std::string unit = trimmed(rel.substr(i));
      if (n >= 0 && n < 5000) {
        if (unit.empty() || unit == "d" || unit == "day" || unit == "days") return done(addDays(t, n));
        if (unit == "w" || unit == "wk" || unit == "week" || unit == "weeks") return done(addDays(t, 7 * n));
        if (unit == "m" || unit == "mo" || unit == "month" || unit == "months") {
          std::tm tm = tmFromIso(t);
          tm.tm_mon += n;
          return done(isoFromTm(tm));
        }
      }
    }
  }

  // "oct 12", "12 oct", "12 october"
  {
    size_t sp = s.find(' ');
    if (sp != std::string::npos) {
      std::string a = s.substr(0, sp), b = trimmed(s.substr(sp + 1));
      int m = 0, d = 0;
      if (detail::allDigits(b) && (m = detail::monthIndex(a))) d = std::stoi(b);
      else if (detail::allDigits(a) && (m = detail::monthIndex(b))) d = std::stoi(a);
      if (m && d) {
        auto iso = detail::monthDay(m, d);
        if (!iso.empty()) return done(iso);
      }
    }
  }
  // "10-12" or "10/12" (month-day; this year, or next if already past)
  for (char sep : {'-', '/'}) {
    size_t p = s.find(sep);
    if (p != std::string::npos && p > 0) {
      std::string a = s.substr(0, p), b = s.substr(p + 1);
      if (detail::allDigits(a) && detail::allDigits(b) && a.size() <= 2 && b.size() <= 2) {
        auto iso = detail::monthDay(std::stoi(a), std::stoi(b));
        if (!iso.empty()) return done(iso);
      }
    }
  }
  return r;  // ok == false
}

}  // namespace guidates
