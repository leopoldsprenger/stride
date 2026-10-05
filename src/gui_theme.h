#pragma once
// Palette + stylesheet for the GTK4 front end. Everything visual flows from one Palette so the cairo-drawn bits
// (icons, the animated checkbox) and the CSS-styled bits can never drift apart. Light/dark follows the system
// unless forced, and re-applies live when the system flips.
#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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
  Rgba red, yellow, teal, tan, green;
};
inline Palette gPal;  // the active palette; read by the cairo draw functions

inline Palette makePalette(bool dark, const std::string& accentHex) {
  Palette p;
  p.dark = dark;
  Rgba accent = rgbHex(accentHex.empty() ? (dark ? "#5b9cff" : "#1f7cf3") : accentHex);
  p.accent = accent;
  p.onAccent = rgbHex("#ffffff");
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
  return p;
}

// "auto" (default) follows the desktop; STRIDE_THEME=light|dark forces it. Order of evidence: explicit override,
// GTK 4.20's color-scheme property, GSettings (GNOME/portal-backed setups), the prefer-dark flag, then a
// theme-name sniff. If nothing says "dark", it's light.
inline bool detectDark() {
  if (const char* env = std::getenv("STRIDE_THEME")) {
    std::string e = env;
    if (e == "dark") return true;
    if (e == "light") return false;
  }
  GtkSettings* st = gtk_settings_get_default();
  if (!st) return true;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(st), "gtk-interface-color-scheme")) {
    int v = 0;
    g_object_get(st, "gtk-interface-color-scheme", &v, nullptr);
    if (v == 2) return true;   // GTK_INTERFACE_COLOR_SCHEME_DARK
    if (v == 3) return false;  // ..._LIGHT
  }
  if (GSettingsSchemaSource* src = g_settings_schema_source_get_default()) {
    if (GSettingsSchema* sch = g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE)) {
      bool hasKey = g_settings_schema_has_key(sch, "color-scheme");
      g_settings_schema_unref(sch);
      if (hasKey) {
        GSettings* gs = g_settings_new("org.gnome.desktop.interface");
        char* cs = g_settings_get_string(gs, "color-scheme");
        std::string v = cs ? cs : "";
        g_free(cs);
        g_object_unref(gs);
        if (v == "prefer-dark") return true;
        if (v == "prefer-light") return false;
      }
    }
  }
  gboolean preferDark = FALSE;
  g_object_get(st, "gtk-application-prefer-dark-theme", &preferDark, nullptr);
  if (preferDark) return true;
  char* name = nullptr;
  g_object_get(st, "gtk-theme-name", &name, nullptr);
  std::string n = name ? name : "";
  g_free(name);
  for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
  return n.find("dark") != std::string::npos;
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
.toast { background-color: rgba(34,35,39,0.96); color: #f2f2f5; border-radius: 999px; padding: 9px 18px;
         font-size: 13px; box-shadow: 0 8px 28px rgba(0,0,0,0.38), 0 0 0 1px rgba(255,255,255,0.06);
         opacity: 0; transform: translateY(10px); transition: opacity 170ms ease-out, transform 200ms cubic-bezier(.2,.9,.25,1); }
.toast.show { opacity: 1; transform: none; }
.toast.error { background-color: {{red}}; color: #fff; }
.toast-key { font-family: monospace; font-size: 11.5px; background-color: rgba(255,255,255,0.14); border-radius: 5px; padding: 0 6px; }

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
.pal-row.sel .pal-label { color: #fff; }
.pal-row.sel .pal-sub, .pal-row.sel .pal-kind { color: rgba(255,255,255,0.72); }
.pal-label { font-size: 14.5px; }
.pal-sub { font-size: 12.5px; color: {{fg3}}; }
.pal-kind { font-size: 11.5px; color: {{fg3}}; }
.pal-empty { font-size: 14px; color: {{fg3}}; padding: 18px; }
.keycap { font-family: monospace; font-size: 11.5px; padding: 1px 6px; border-radius: 5px; color: {{fg2}};
          background-color: {{hover}}; border: 1px solid {{line}}; border-bottom-width: 2px; }
.btn { padding: 8px 16px; border-radius: 10px; font-size: 13.5px; font-weight: 600; background-color: {{hover}};
       transition: background-color 110ms ease-out; }
.btn:hover { background-color: {{line}}; }
.btn.primary { background-color: {{accent}}; color: #fff; }
.btn.primary:hover { background-color: {{accentHover}}; }
.btn.primary .keycap { background-color: rgba(255,255,255,0.18); border-color: rgba(255,255,255,0.25); color: #fff; }
.btn.danger { background-color: {{red}}; color: #fff; }
.btn-note { font-size: 12px; color: {{fg3}}; }
.stat { font-size: 13px; color: {{fg2}}; }
.stat.good { color: {{green}}; }
.help-group { font-size: 11px; font-weight: 700; letter-spacing: 0.6px; color: {{fg3}}; margin-top: 10px; }
.help-text { font-size: 13.5px; }
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
      {"{{scrim}}", p.dark ? "rgba(0,0,0,0.46)" : "rgba(20,22,30,0.28)"},
  };
  for (auto& [k, v] : map) css = replaceAll(css, k, v);
  return css;
}
