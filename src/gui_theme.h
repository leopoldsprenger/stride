#pragma once
// Palette + stylesheet for the GTK4 front end. Everything visual flows from one Palette so the cairo-drawn bits
// (icons, the animated checkbox) and the CSS-styled bits can never drift apart. Light/dark follows the system
// unless forced, and re-applies live when the system flips.
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "util.h"

struct Rgba {
  double r = 0, g = 0, b = 0, a = 1;
};

inline Rgba rgbHex(const std::string& hex, double a = 1.0) {
  Rgba c;
  c.a = a;
  std::string h = hex;
  if (!h.empty() && h[0] == '#') h = h.substr(1);
  if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
  unsigned v = 0;
  if (h.size() == 6) std::sscanf(h.c_str(), "%x", &v);
  c.r = ((v >> 16) & 0xff) / 255.0;
  c.g = ((v >> 8) & 0xff) / 255.0;
  c.b = (v & 0xff) / 255.0;
  return c;
}
inline Rgba withAlpha(Rgba c, double a) {
  c.a = a;
  return c;
}
inline std::string cssColor(const Rgba& c) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.3f)", (int)(c.r * 255 + 0.5), (int)(c.g * 255 + 0.5),
                (int)(c.b * 255 + 0.5), c.a);
  return buf;
}
inline void setSource(cairo_t* cr, const Rgba& c, double alphaMul = 1.0) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * alphaMul);
}

struct Palette {
  bool dark = true;
  Rgba bg, side, card, fg, fg2, fg3, line, hover, select, accent, onAccent;
  Rgba red, onRed, yellow, teal, tan, green;
  Rgba toast, onToast;
};
inline Palette gPal;  // the active palette; read by the cairo draw functions

// ---------------------------------------------------------------------------------------------------------------
// colour maths
// ---------------------------------------------------------------------------------------------------------------
namespace colorx {

inline double lum(const Rgba& c) {
  auto f = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * f(c.r) + 0.7152 * f(c.g) + 0.0722 * f(c.b);
}
inline double contrast(const Rgba& a, const Rgba& b) {
  double la = lum(a), lb = lum(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}
inline Rgba mix(const Rgba& a, const Rgba& b, double t) {
  return Rgba{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
inline Rgba opaque(Rgba c) {
  c.a = 1;
  return c;
}
// The text colour that reads best on `bg`.
inline Rgba onColor(const Rgba& bg) {
  return lum(bg) > 0.42 ? rgbHex("#1c1c1e") : rgbHex("#ffffff");
}

struct Hsl {
  double h = 0, s = 0, l = 0;  // h in degrees
};
inline Hsl toHsl(const Rgba& c) {
  double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  Hsl o;
  o.l = (mx + mn) / 2;
  double d = mx - mn;
  if (d < 1e-9) return o;
  o.s = o.l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  if (mx == c.r) o.h = (c.g - c.b) / d + (c.g < c.b ? 6 : 0);
  else if (mx == c.g) o.h = (c.b - c.r) / d + 2;
  else o.h = (c.r - c.g) / d + 4;
  o.h *= 60;
  return o;
}
inline Rgba fromHsl(double h, double s, double l, double a = 1) {
  h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 360.0;
  s = std::clamp(s, 0.0, 1.0);
  l = std::clamp(l, 0.0, 1.0);
  auto hue = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  if (s < 1e-9) return Rgba{l, l, l, a};
  double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  return Rgba{hue(p, q, h + 1.0 / 3), hue(p, q, h), hue(p, q, h - 1.0 / 3), a};
}
// GTK's shade(): scales lightness and saturation.
inline Rgba shade(const Rgba& c, double f) {
  Hsl h = toHsl(c);
  return fromHsl(h.h, h.s * f, h.l * f, c.a);
}
// Nudges `c` toward `toward` until it reads at least `ratio`:1 against `bg` (accent rings, red deadlines...).
inline Rgba ensureVisible(Rgba c, const Rgba& bg, const Rgba& toward, double ratio) {
  for (int i = 0; i < 12 && contrast(c, bg) < ratio; ++i) c = mix(c, toward, 0.14);
  return c;
}
inline bool eq(const Rgba& a, const Rgba& b) {
  return std::fabs(a.r - b.r) < 1e-4 && std::fabs(a.g - b.g) < 1e-4 && std::fabs(a.b - b.b) < 1e-4 &&
         std::fabs(a.a - b.a) < 1e-4;
}

}  // namespace colorx

inline bool samePalette(const Palette& a, const Palette& b) {
  using colorx::eq;
  return a.dark == b.dark && eq(a.bg, b.bg) && eq(a.side, b.side) && eq(a.card, b.card) && eq(a.fg, b.fg) &&
         eq(a.fg2, b.fg2) && eq(a.fg3, b.fg3) && eq(a.line, b.line) && eq(a.hover, b.hover) &&
         eq(a.select, b.select) && eq(a.accent, b.accent) && eq(a.onAccent, b.onAccent) && eq(a.red, b.red) &&
         eq(a.yellow, b.yellow) && eq(a.teal, b.teal) && eq(a.tan, b.tan) && eq(a.green, b.green);
}

// The built-in palettes, used when the theme is forced to light or dark (STRIDE_THEME=light|dark).
inline Palette makePalette(bool dark, const std::string& accentHex) {
  Palette p;
  p.dark = dark;
  Rgba accent = rgbHex(accentHex.empty() ? (dark ? "#5b9cff" : "#1f7cf3") : accentHex);
  p.accent = accent;
  p.onAccent = colorx::onColor(accent);
  if (dark) {
    p.bg = rgbHex("#232427");
    p.side = rgbHex("#1b1c1f");
    p.card = rgbHex("#2d2e33");
    p.fg = rgbHex("#ececf0");
    p.fg2 = rgbHex("#a1a2aa");
    p.fg3 = rgbHex("#6c6d76");
    p.line = Rgba{1, 1, 1, 0.085};
    p.hover = Rgba{1, 1, 1, 0.045};
    p.select = withAlpha(accent, 0.24);
    p.red = rgbHex("#ff6259");
    p.yellow = rgbHex("#ffd23f");
    p.teal = rgbHex("#3cc3d8");
    p.tan = rgbHex("#d9b878");
    p.green = rgbHex("#44d16a");
  } else {
    p.bg = rgbHex("#ffffff");
    p.side = rgbHex("#f3f4f6");
    p.card = rgbHex("#ffffff");
    p.fg = rgbHex("#1c1c1e");
    p.fg2 = rgbHex("#68686f");
    p.fg3 = rgbHex("#a4a4ab");
    p.line = Rgba{0, 0, 0, 0.085};
    p.hover = Rgba{0, 0, 0, 0.04};
    p.select = withAlpha(accent, 0.14);
    p.red = rgbHex("#f0463c");
    p.yellow = rgbHex("#f5b800");
    p.teal = rgbHex("#1fa3b8");
    p.tan = rgbHex("#c29b52");
    p.green = rgbHex("#2fb350");
  }
  p.onRed = colorx::onColor(p.red);
  p.toast = dark ? rgbHex("#f2f2f5") : rgbHex("#26272b");
  p.onToast = dark ? rgbHex("#1c1c1e") : rgbHex("#f2f2f5");
  return p;
}

// ---------------------------------------------------------------------------------------------------------------
// reading colours from a GTK theme
//
// "auto" (the default) takes every colour from the GTK theme that is active right now: the named colours the theme
// defines (libadwaita-style window_bg_color / accent_bg_color / ..., or the classic theme_bg_color /
// theme_selected_bg_color / ...) are looked up live, so switching themes re-colours the app. A theme's stylesheet
// can also be pointed at explicitly (STRIDE_THEME_CSS=/path/to/gtk.css, or theme_css= in Stride's config file), in
// which case its @define-color lines are evaluated directly.
// ---------------------------------------------------------------------------------------------------------------

using ColorSource = std::function<bool(const char*, Rgba&)>;

// -- live lookup through GTK ---------------------------------------------------------------------------------------

inline GtkWidget* themeProbe() {
  static GtkWidget* w = nullptr;
  if (!w) w = g_object_ref_sink(gtk_label_new(""));
  return w;
}

inline bool gtkLookup(const char* name, Rgba& out) {
  GdkRGBA c;
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkStyleContext* sc = gtk_widget_get_style_context(themeProbe());
  gboolean ok = gtk_style_context_lookup_color(sc, name, &c);
  G_GNUC_END_IGNORE_DEPRECATIONS
  if (!ok) return false;
  out = Rgba{c.red, c.green, c.blue, c.alpha};
  return true;
}

// -- evaluating a stylesheet's @define-color lines ----------------------------------------------------------------------

struct CssDefs {
  std::map<std::string, std::string> raw;
};

namespace cssparse {

inline std::string trimWs(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

inline std::vector<std::string> splitArgs(const std::string& s) {  // top-level commas only
  std::vector<std::string> out;
  int depth = 0;
  std::string cur;
  for (char c : s) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == ',' && depth == 0) {
      out.push_back(trimWs(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(trimWs(cur));
  return out;
}

inline void load(const std::filesystem::path& file, CssDefs& defs, int depth = 0) {
  if (depth > 4) return;
  std::ifstream in(file);
  if (!in) return;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  for (size_t p = 0; (p = text.find("/*", p)) != std::string::npos;) {  // strip comments
    size_t e = text.find("*/", p + 2);
    text.erase(p, e == std::string::npos ? std::string::npos : e + 2 - p);
  }
  size_t i = 0;
  while (i < text.size()) {
    size_t at = text.find('@', i);
    if (at == std::string::npos) break;
    if (text.compare(at, 13, "@define-color") == 0) {
      size_t ns = text.find_first_not_of(" \t\r\n", at + 13);
      if (ns == std::string::npos) break;
      size_t ne = text.find_first_of(" \t\r\n", ns);
      size_t semi = text.find(';', ne == std::string::npos ? ns : ne);
      if (ne == std::string::npos || semi == std::string::npos) break;
      defs.raw[text.substr(ns, ne - ns)] = trimWs(text.substr(ne, semi - ne));
      i = semi + 1;
    } else if (text.compare(at, 7, "@import") == 0) {
      size_t semi = text.find(';', at);
      if (semi == std::string::npos) break;
      std::string arg = text.substr(at + 7, semi - at - 7);
      size_t q1 = arg.find_first_of("\"'");
      if (q1 != std::string::npos) {
        size_t q2 = arg.find(arg[q1], q1 + 1);
        if (q2 != std::string::npos) {
          std::string rel = arg.substr(q1 + 1, q2 - q1 - 1);
          if (rel.rfind("file://", 0) == 0) rel = rel.substr(7);
          if (!rel.empty() && rel.rfind("resource:", 0) != 0) {
            std::filesystem::path f = rel;
            if (f.is_relative()) f = file.parent_path() / f;
            load(f, defs, depth + 1);
          }
        }
      }
      i = semi + 1;
    } else {
      i = at + 1;
    }
  }
}

inline bool eval(const std::string& exprIn, const CssDefs& d, Rgba& out, int depth = 0) {
  if (depth > 16) return false;
  std::string e = trimWs(exprIn);
  if (e.empty()) return false;
  if (e[0] == '@') {
    auto it = d.raw.find(e.substr(1));
    return it != d.raw.end() && eval(it->second, d, out, depth + 1);
  }
  size_t lp = e.find('(');
  if (lp != std::string::npos && e.back() == ')') {
    std::string fn = lower(trimWs(e.substr(0, lp)));
    std::string inner = e.substr(lp + 1, e.size() - lp - 2);
    auto args = splitArgs(inner);
    if (fn == "alpha" && args.size() == 2) {
      if (!eval(args[0], d, out, depth + 1)) return false;
      out.a *= std::atof(args[1].c_str());
      return true;
    }
    if (fn == "shade" && args.size() == 2) {
      if (!eval(args[0], d, out, depth + 1)) return false;
      out = colorx::shade(out, std::atof(args[1].c_str()));
      return true;
    }
    if (fn == "darker" || fn == "lighter") {
      if (!eval(inner, d, out, depth + 1)) return false;
      out = colorx::shade(out, fn == "darker" ? 0.7 : 1.3);
      return true;
    }
    if (fn == "mix" && args.size() == 3) {
      Rgba a, b;
      if (!eval(args[0], d, a, depth + 1) || !eval(args[1], d, b, depth + 1)) return false;
      out = colorx::mix(a, b, std::atof(args[2].c_str()));
      return true;
    }
  }
  GdkRGBA g;
  if (!gdk_rgba_parse(&g, e.c_str())) return false;  // hex, rgb(), rgba(), named colours
  out = Rgba{g.red, g.green, g.blue, g.alpha};
  return true;
}

}  // namespace cssparse

// -- palette from a colour source ------------------------------------------------------------------------------------

inline bool firstOf(const ColorSource& src, std::initializer_list<const char*> names, Rgba& out) {
  for (const char* n : names)
    if (src(n, out)) return true;
  return false;
}

// Builds a palette out of whatever the source defines; anything it doesn't define is derived from what it does, so a
// minimal theme (just a background, a foreground and an accent) still ends up coherent. False if it has no
// recognisable background/foreground at all.
inline bool paletteFrom(const ColorSource& src, const std::string& accentOverride, Palette& out) {
  using namespace colorx;
  Rgba bg, fg;
  if (!firstOf(src, {"window_bg_color", "theme_bg_color", "bg_color", "base_color"}, bg)) return false;
  if (!firstOf(src, {"window_fg_color", "theme_fg_color", "fg_color", "text_color"}, fg)) return false;
  bg = opaque(bg);
  fg = opaque(fg);
  Palette p;
  p.bg = bg;
  p.fg = fg;
  p.dark = lum(bg) < 0.22;
  const Rgba white = rgbHex("#ffffff"), black = rgbHex("#000000");

  Rgba v;
  if (firstOf(src, {"sidebar_bg_color"}, v)) p.side = opaque(v);
  else p.side = p.dark ? mix(bg, black, 0.22) : mix(bg, fg, 0.045);

  if (p.dark) {
    Rgba c;
    if (firstOf(src, {"card_bg_color", "popover_bg_color"}, c) && lum(c) > lum(bg) + 0.004) p.card = opaque(c);
    else p.card = mix(bg, white, 0.065);
  } else {
    Rgba c;
    if (firstOf(src, {"view_bg_color", "card_bg_color", "theme_base_color"}, c) && lum(c) >= lum(bg) - 0.002) p.card = opaque(c);
    else p.card = mix(bg, white, 0.55);
  }
  p.fg2 = mix(fg, bg, 0.38);
  p.fg3 = mix(fg, bg, 0.62);
  p.line = withAlpha(fg, p.dark ? 0.10 : 0.09);
  p.hover = withAlpha(fg, p.dark ? 0.05 : 0.045);

  Rgba accent;
  if (!accentOverride.empty()) accent = rgbHex(accentOverride);
  else if (!firstOf(src, {"accent_bg_color", "theme_selected_bg_color", "selected_bg_color", "accent_color"}, accent))
    accent = rgbHex(p.dark ? "#5b9cff" : "#1f7cf3");
  accent = ensureVisible(opaque(accent), bg, fg, 1.8);
  p.accent = accent;
  Rgba onA;
  if (firstOf(src, {"accent_fg_color", "theme_selected_fg_color", "selected_fg_color"}, onA) && std::fabs(lum(onA) - lum(accent)) > 0.3)
    p.onAccent = opaque(onA);
  else p.onAccent = onColor(accent);
  p.select = withAlpha(accent, p.dark ? 0.26 : 0.16);

  // Semantic colours (icons, deadlines): the theme's own where it has them, otherwise derived from its accent.
  Hsl ah = toHsl(accent);
  double sat = std::clamp(ah.s, 0.45, 0.78);
  auto pick = [&](std::initializer_list<const char*> names, Rgba fallback) {
    Rgba c;
    if (firstOf(src, names, c)) fallback = c;
    return ensureVisible(opaque(fallback), bg, fg, 1.9);
  };
  p.red = pick({"error_color", "destructive_color", "destructive_bg_color", "red_3", "red", "maroon"},
               rgbHex(p.dark ? "#ff6259" : "#f0463c"));
  p.yellow = pick({"warning_color", "yellow_3", "yellow", "gold"}, rgbHex(p.dark ? "#ffd23f" : "#f5b800"));
  p.green = pick({"success_color", "green_3", "green"}, rgbHex(p.dark ? "#44d16a" : "#2fb350"));
  p.teal = pick({"teal", "teal_3", "cyan", "sky", "sapphire"}, fromHsl(187, sat, p.dark ? 0.58 : 0.42));
  p.tan = pick({"peach", "orange_3", "orange", "tan", "sand"}, fromHsl(38, std::min(sat, 0.6), p.dark ? 0.66 : 0.54));
  p.onRed = onColor(p.red);
  p.toast = p.dark ? mix(fg, white, 0.1) : mix(fg, black, 0.2);
  p.onToast = onColor(p.toast);
  out = p;
  return true;
}

inline std::string expandHome(std::string p) {
  if (!p.empty() && p[0] == '~') {
    const char* h = std::getenv("HOME");
    if (h) p = std::string(h) + p.substr(1);
  }
  return p;
}

// STRIDE_THEME_CSS wins; otherwise `theme_css=` in the config file next to the database.
inline std::string themeCssPath() {
  if (const char* e = std::getenv("STRIDE_THEME_CSS"))
    if (*e) return expandHome(e);
  try {
    Config cfg((dataDir() / "config").string());
    if (auto v = cfg.get("theme_css"))
      if (!v->empty()) return expandHome(*v);
  } catch (...) {
  }
  return "";
}

inline std::string accentOverride() {
  const char* a = std::getenv("STRIDE_ACCENT");
  return a ? a : "";
}

// Modification time of the stylesheet being followed (0 when none), so an edit to it is picked up live.
inline long themeCssStamp() {
  std::string path = themeCssPath();
  if (path.empty()) return 0;
  std::error_code ec;
  auto t = std::filesystem::last_write_time(path, ec);
  return ec ? -1 : (long)t.time_since_epoch().count();
}

// The one entry point both windows use. Order: forced light/dark -> an explicitly named stylesheet -> the live GTK
// theme -> the built-in palette matching the desktop's dark preference.
inline Palette resolvePalette() {
  std::string accent = accentOverride();
  std::string mode = "auto";
  if (const char* env = std::getenv("STRIDE_THEME")) mode = lower(env);
  if (mode == "dark") return makePalette(true, accent);
  if (mode == "light") return makePalette(false, accent);

  Palette p;
  std::string cssPath = themeCssPath();
  if (!cssPath.empty()) {
    auto defs = std::make_shared<CssDefs>();
    cssparse::load(cssPath, *defs);
    ColorSource src = [defs](const char* n, Rgba& o) {
      auto it = defs->raw.find(n);
      return it != defs->raw.end() && cssparse::eval(it->second, *defs, o);
    };
    if (paletteFrom(src, accent, p)) return p;
    std::fprintf(stderr, "stride: %s defines no usable window colours; following the active GTK theme instead\n", cssPath.c_str());
  }
  if (paletteFrom(gtkLookup, accent, p)) return p;

  // A theme that names none of the usual colours: fall back to the desktop's light/dark preference.
  bool dark = true;
  if (GtkSettings* st = gtk_settings_get_default()) {
    gboolean preferDark = FALSE;
    char* name = nullptr;
    g_object_get(st, "gtk-application-prefer-dark-theme", &preferDark, "gtk-theme-name", &name, nullptr);
    std::string n = lower(name ? name : "");
    g_free(name);
    dark = preferDark || n.find("dark") != std::string::npos;
  }
  return makePalette(dark, accent);
}

inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
  return s;
}

inline std::string buildCss(const Palette& p) {
  static const char* tpl = R"CSS(
window.stride { background-color: {{bg}}; color: {{fg}}; }
window.stride * { outline: none; }

/* ---------- sidebar ---------- */
.sidebar { background-color: {{side}}; border-right: 1px solid {{line}}; }
.sb-row { padding: 6px 10px; margin: 1px 10px; border-radius: 9px; transition: background-color 110ms ease-out; }
.sb-row:hover { background-color: {{hover}}; }
.sb-row.current { background-color: {{select}}; }
.sb-row.cursor { box-shadow: inset 0 0 0 1.5px {{accent}}; }
.sb-label { font-size: 14px; }
.sb-row.current .sb-label { font-weight: 600; }
.sb-count { font-size: 12px; color: {{fg3}}; font-feature-settings: "tnum"; }
.sb-row.indent { margin-left: 24px; }
.sb-area { font-size: 11.5px; font-weight: 700; color: {{fg3}}; margin: 16px 20px 4px 20px; letter-spacing: 0.4px; }
.sb-gap { min-height: 10px; }

/* ---------- page header ---------- */
.page-title { font-size: 28px; font-weight: 800; letter-spacing: -0.3px; }
.page-crumb { font-size: 12.5px; color: {{fg3}}; }
.page-notes { font-size: 14px; color: {{fg2}}; }
.page-empty { font-size: 14px; color: {{fg3}}; }
.page-empty-title { font-size: 16px; font-weight: 600; color: {{fg2}}; }

/* ---------- rows ---------- */
.task-row { padding: 7px 12px 7px 10px; margin: 0 14px; border-radius: 10px;
            transition: background-color 110ms ease-out, opacity 240ms ease-out; }
.task-row:hover { background-color: {{hover}}; }
.task-row.selected { background-color: {{select}}; }
.task-row.leaving { opacity: 0; }
.task-title { font-size: 15px; transition: color 200ms ease-out; }
.task-row.done .task-title { color: {{fg3}}; }
.task-row.someday .task-title { color: {{fg2}}; }
.task-row.project .task-title { font-weight: 600; }
.task-sub { font-size: 12px; color: {{fg3}}; }
.meta { font-size: 12px; color: {{fg2}}; font-feature-settings: "tnum"; }
.meta.faint { color: {{fg3}}; }
.meta.due { color: {{red}}; font-weight: 600; }
.chip { font-size: 11.5px; color: {{fg2}}; background-color: {{line}}; border-radius: 6px; padding: 1px 7px; }
.row-new { animation: row-flash 1100ms ease-out; }
@keyframes row-flash { from { background-color: {{accentA30}}; } to { background-color: transparent; } }
.row-back { animation: row-flash 900ms ease-out; }

.section { font-size: 13px; font-weight: 700; color: {{fg2}}; margin: 22px 24px 4px 24px; padding-bottom: 5px;
           border-bottom: 1px solid {{line}}; }
.section.heading { font-size: 16px; color: {{accent}}; margin-top: 26px; }
.section.first { margin-top: 6px; }
.more-row { font-size: 13px; color: {{fg3}}; }

/* ---------- inline editor ---------- */
.editor { background-color: {{card}}; border-radius: 13px; margin: 8px 12px; padding: 12px 16px 12px 14px;
          box-shadow: 0 10px 34px rgba(0,0,0,0.30), 0 0 0 1px {{line}}; animation: editor-in 190ms cubic-bezier(.2,.9,.25,1); }
@keyframes editor-in { from { opacity: 0; transform: translateY(-6px) scale(0.985); } to { opacity: 1; transform: none; } }
entry.bare, entry.bare > text, textview.bare, textview.bare > text { background: none; background-color: transparent;
    border: none; box-shadow: none; outline: none; padding: 0; min-height: 0; color: {{fg}}; caret-color: {{accent}}; }
entry.bare selection, entry.bare > text selection, textview.bare > text selection { background-color: {{accentA35}}; color: {{fg}}; }
entry.bare placeholder, entry.bare > text > placeholder { color: {{fg3}}; }
.editor-title { font-size: 16.5px; font-weight: 500; }
textview.bare { font-size: 13.5px; }
textview.bare > text { color: {{fg2}}; }
.field-cap { font-size: 10px; font-weight: 700; letter-spacing: 0.6px; color: {{fg3}}; }
.field { background-color: {{hover}}; border-radius: 8px; padding: 5px 9px; border: 1px solid transparent;
         transition: border-color 120ms ease-out, background-color 120ms ease-out; }
.field:focus-within { border-color: {{accent}}; background-color: {{accentA10}}; }
.field.invalid { border-color: {{red}}; }
.field entry.bare { font-size: 13px; }
.hint { font-size: 12px; color: {{fg3}}; }
.hint.error { color: {{red}}; }

/* ---------- footer ---------- */
.footer { padding: 6px 14px 10px 14px; }
.foot-btn { padding: 6px 10px; border-radius: 9px; color: {{fg2}}; transition: background-color 110ms ease-out, color 110ms; }
.foot-btn:hover { background-color: {{hover}}; color: {{fg}}; }
.foot-btn.disabled { opacity: 0.4; }
.new-label { font-size: 13.5px; font-weight: 600; color: {{fg2}}; }
.foot-btn:hover .new-label { color: {{fg}}; }

/* ---------- toast ---------- */
.toast { background-color: {{toast}}; color: {{onToast}}; border-radius: 999px; padding: 9px 18px;
         font-size: 13px; box-shadow: 0 8px 28px rgba(0,0,0,0.30), 0 0 0 1px {{line}};
         opacity: 0; transform: translateY(10px); transition: opacity 170ms ease-out, transform 200ms cubic-bezier(.2,.9,.25,1); }
.toast.show { opacity: 1; transform: none; }
.toast.error { background-color: {{red}}; color: {{onRed}}; }
.toast-key { font-family: monospace; font-size: 11.5px; background-color: {{onToastA14}}; border-radius: 5px; padding: 0 6px; }

/* ---------- modals ---------- */
.scrim { background-color: {{scrim}}; animation: scrim-in 140ms ease-out; }
@keyframes scrim-in { from { opacity: 0; } to { opacity: 1; } }
.modal { background-color: {{card}}; border-radius: 15px; box-shadow: 0 28px 70px rgba(0,0,0,0.42), 0 0 0 1px {{line}};
         animation: modal-in 170ms cubic-bezier(.2,.9,.25,1.05); }
@keyframes modal-in { from { opacity: 0; transform: translateY(-8px) scale(0.975); } to { opacity: 1; transform: none; } }
.modal-title { font-size: 16px; font-weight: 700; }
.modal-body { font-size: 13.5px; color: {{fg2}}; }
.modal-mono { font-family: monospace; font-size: 12px; color: {{fg2}}; }
.pal-entry { font-size: 18px; }
.pal-sep { background-color: {{line}}; min-height: 1px; }
.pal-row { padding: 7px 12px; margin: 0 8px; border-radius: 9px; }
.pal-row.sel { background-color: {{accent}}; }
.pal-row.sel .pal-label { color: {{onAccent}}; }
.pal-row.sel .pal-sub, .pal-row.sel .pal-kind { color: {{onAccentA72}}; }
.pal-label { font-size: 14.5px; }
.pal-sub { font-size: 12.5px; color: {{fg3}}; }
.pal-kind { font-size: 11.5px; color: {{fg3}}; }
.pal-empty { font-size: 14px; color: {{fg3}}; padding: 18px; }
.keycap { font-family: monospace; font-size: 11.5px; padding: 1px 6px; border-radius: 5px; color: {{fg2}};
          background-color: {{hover}}; border: 1px solid {{line}}; border-bottom-width: 2px; }
.btn { padding: 8px 16px; border-radius: 10px; font-size: 13.5px; font-weight: 600; background-color: {{hover}};
       transition: background-color 110ms ease-out; }
.btn:hover { background-color: {{line}}; }
.btn.primary { background-color: {{accent}}; color: {{onAccent}}; }
.btn.primary:hover { background-color: {{accentHover}}; }
.btn.primary .keycap { background-color: {{onAccentA18}}; border-color: {{onAccentA25}}; color: {{onAccent}}; }
.btn.danger { background-color: {{red}}; color: {{onRed}}; }
.btn-note { font-size: 12px; color: {{fg3}}; }
.stat { font-size: 13px; color: {{fg2}}; }
.stat.good { color: {{green}}; }
.help-group { font-size: 11px; font-weight: 700; letter-spacing: 0.6px; color: {{fg3}}; margin-top: 10px; }
.help-text { font-size: 13.5px; }

/* ---------- sidebar footer ---------- */
.sb-foot { padding: 6px 10px 12px 10px; border-top: 1px solid {{line}}; }
.sb-new { padding: 6px 10px; border-radius: 9px; color: {{fg2}}; transition: background-color 110ms ease-out, color 110ms; }
.sb-new:hover { background-color: {{hover}}; color: {{fg}}; }
.sb-new-label { font-size: 13.5px; font-weight: 600; }

/* ---------- grouping, logged to-dos ---------- */
.group-area { font-size: 15px; font-weight: 700; margin: 24px 24px 2px 24px; padding-bottom: 6px; border-bottom: 1px solid {{line}}; }
.group-proj { font-size: 13.5px; font-weight: 600; color: {{fg2}}; margin: 14px 24px 0 24px; padding: 0 0 2px 0; }
.group-area.first, .group-proj.first { margin-top: 6px; }
.log-toggle { padding: 7px 12px 7px 10px; margin: 8px 14px 0 14px; border-radius: 10px; color: {{fg3}};
              transition: background-color 110ms ease-out, color 110ms ease-out; }
.log-toggle:hover { color: {{fg2}}; }
.log-toggle.selected { background-color: {{select}}; color: {{fg2}}; }
.log-toggle-label { font-size: 13px; font-weight: 600; }
.meta.logged { color: {{fg3}}; }
.task-row.logged .task-title { color: {{fg3}}; }

/* ---------- checklist ---------- */
.cl-row { padding: 1px 0; }
.cl-row entry.bare, .cl-row entry.bare > text { font-size: 13.5px; }
.cl-row.done entry.bare, .cl-row.done entry.bare > text { color: {{fg3}}; text-decoration: line-through; }
.cl-add { font-size: 12.5px; color: {{fg3}}; padding: 4px 0 2px 0; transition: color 110ms ease-out; }
.cl-add:hover { color: {{fg2}}; }
.cl-sep { background-color: {{line}}; min-height: 1px; margin-left: 26px; }

/* ---------- project header ---------- */
.page-notes link, .page-notes link:hover { color: {{accent}}; }
.hdr-edit { color: {{fg3}}; font-size: 12px; }
textview.md-ro, textview.md-ro > text { background: none; background-color: transparent; color: {{fg2}}; font-size: 14px; }
)CSS";
  std::string css = tpl;
  Rgba a = p.accent;
  Rgba hov = a;
  hov.r = std::min(1.0, a.r * 1.1 + 0.03);
  hov.g = std::min(1.0, a.g * 1.1 + 0.03);
  hov.b = std::min(1.0, a.b * 1.1 + 0.03);
  const std::pair<const char*, std::string> map[] = {
      {"{{bg}}", cssColor(p.bg)},           {"{{side}}", cssColor(p.side)},
      {"{{card}}", cssColor(p.card)},       {"{{fg}}", cssColor(p.fg)},
      {"{{fg2}}", cssColor(p.fg2)},         {"{{fg3}}", cssColor(p.fg3)},
      {"{{line}}", cssColor(p.line)},       {"{{hover}}", cssColor(p.hover)},
      {"{{select}}", cssColor(p.select)},   {"{{accent}}", cssColor(p.accent)},
      {"{{accentHover}}", cssColor(hov)},   {"{{accentA10}}", cssColor(withAlpha(a, 0.10))},
      {"{{accentA30}}", cssColor(withAlpha(a, 0.30))}, {"{{accentA35}}", cssColor(withAlpha(a, 0.35))},
      {"{{red}}", cssColor(p.red)},         {"{{green}}", cssColor(p.green)},
      {"{{onRed}}", cssColor(p.onRed)},     {"{{onAccent}}", cssColor(p.onAccent)},
      {"{{onAccentA72}}", cssColor(withAlpha(p.onAccent, 0.72))}, {"{{onAccentA18}}", cssColor(withAlpha(p.onAccent, 0.18))},
      {"{{onAccentA25}}", cssColor(withAlpha(p.onAccent, 0.25))},
      {"{{toast}}", cssColor(withAlpha(p.toast, 0.97))}, {"{{onToast}}", cssColor(p.onToast)},
      {"{{onToastA14}}", cssColor(withAlpha(p.onToast, 0.14))},
      {"{{scrim}}", p.dark ? "rgba(0,0,0,0.46)" : "rgba(20,22,30,0.28)"},
  };
  for (auto& [k, v] : map) css = replaceAll(css, k, v);
  return css;
}
