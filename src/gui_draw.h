#pragma once
// Cairo drawing for the GTK4 front end: easing curves, the vector icon set (a Things-inspired language of simple
// geometric glyphs on a 20x20 grid) and the animated checkbox. Header-only; included by gui.cpp alone.
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>

#include "gui_theme.h"

namespace ease {
inline double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline double outQuad(double x) { x = clamp01(x); return 1 - (1 - x) * (1 - x); }
inline double outCubic(double x) { x = clamp01(x); double u = 1 - x; return 1 - u * u * u; }
inline double inOutCubic(double x) {
  x = clamp01(x);
  return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}
inline double inQuad(double x) { x = clamp01(x); return x * x; }
// Overshoots past 1 then settles; s controls how far.
inline double outBack(double x, double s = 1.70158) {
  x = clamp01(x);
  double c3 = s + 1, u = x - 1;
  return 1 + c3 * u * u * u + s * u * u;
}
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
}  // namespace ease

enum class Icon {
  Inbox, Today, Upcoming, Anytime, Someday, Logbook, Area, Project, Flag, Note, Checklist, Tag, Plus, Search, Sync,
  Calendar, Help, Deadlines, Archive
};

namespace draw {

inline void arrowHead(cairo_t* cr, double x, double y, double ang, double size) {
  cairo_move_to(cr, x + std::cos(ang + 2.5) * size, y + std::sin(ang + 2.5) * size);
  cairo_line_to(cr, x, y);
  cairo_line_to(cr, x + std::cos(ang - 2.5) * size, y + std::sin(ang - 2.5) * size);
  cairo_stroke(cr);
}

inline void roundRect(cairo_t* cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

// Draws `icon` into a size x size box with colour `c`. `param` is icon-specific: Project = fraction complete
// (0..1), Sync = rotation in radians.
inline void icon(cairo_t* cr, Icon ic, double size, Rgba c, double param = 0) {
  cairo_save(cr);
  double k = size / 20.0;
  cairo_scale(cr, k, k);
  cairo_set_line_width(cr, 1.6);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  setSource(cr, c);
  switch (ic) {
    case Icon::Inbox: {
      cairo_move_to(cr, 3, 11.5);
      cairo_line_to(cr, 5.4, 4.8);
      cairo_line_to(cr, 14.6, 4.8);
      cairo_line_to(cr, 17, 11.5);
      cairo_line_to(cr, 17, 15);
      cairo_curve_to(cr, 17, 15.8, 16.4, 16.4, 15.6, 16.4);
      cairo_line_to(cr, 4.4, 16.4);
      cairo_curve_to(cr, 3.6, 16.4, 3, 15.8, 3, 15);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 11.5);
      cairo_line_to(cr, 7.2, 11.5);
      cairo_curve_to(cr, 7.8, 13.6, 8.8, 14, 10, 14);
      cairo_curve_to(cr, 11.2, 14, 12.2, 13.6, 12.8, 11.5);
      cairo_line_to(cr, 17, 11.5);
      cairo_stroke(cr);
      break;
    }
    case Icon::Today: {  // filled star
      double cx = 10, cy = 10.6, ro = 8, ri = 3.4;
      for (int i = 0; i < 10; ++i) {
        double r = (i % 2 == 0) ? ro : ri, a = -M_PI / 2 + i * M_PI / 5;
        double x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
      }
      cairo_close_path(cr);
      cairo_set_line_width(cr, 1.2);
      cairo_fill_preserve(cr);
      cairo_stroke(cr);
      break;
    }
    case Icon::Upcoming:
    case Icon::Calendar: {
      roundRect(cr, 3, 4.6, 14, 12.4, 2.6);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 8.6);
      cairo_line_to(cr, 17, 8.6);
      cairo_stroke(cr);
      cairo_move_to(cr, 6.8, 2.8);
      cairo_line_to(cr, 6.8, 5.8);
      cairo_move_to(cr, 13.2, 2.8);
      cairo_line_to(cr, 13.2, 5.8);
      cairo_stroke(cr);
      break;
    }
    case Icon::Anytime: {  // stacked layers
      cairo_move_to(cr, 10, 3.2);
      cairo_line_to(cr, 17, 6.6);
      cairo_line_to(cr, 10, 10);
      cairo_line_to(cr, 3, 6.6);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 10);
      cairo_line_to(cr, 10, 13.4);
      cairo_line_to(cr, 17, 10);
      cairo_stroke(cr);
      cairo_move_to(cr, 3, 13.4);
      cairo_line_to(cr, 10, 16.8);
      cairo_line_to(cr, 17, 13.4);
      cairo_stroke(cr);
      break;
    }
    case Icon::Someday:
    case Icon::Archive: {  // archive box
      roundRect(cr, 2.8, 3.8, 14.4, 4.4, 1.4);
      cairo_stroke(cr);
      cairo_move_to(cr, 4.4, 8.2);
      cairo_line_to(cr, 4.4, 15.2);
      cairo_curve_to(cr, 4.4, 16, 5, 16.4, 5.8, 16.4);
      cairo_line_to(cr, 14.2, 16.4);
      cairo_curve_to(cr, 15, 16.4, 15.6, 16, 15.6, 15.2);
      cairo_line_to(cr, 15.6, 8.2);
      cairo_stroke(cr);
      cairo_move_to(cr, 8, 11.4);
      cairo_line_to(cr, 12, 11.4);
      cairo_stroke(cr);
      break;
    }
    case Icon::Logbook: {  // filled rounded tile with a check
      roundRect(cr, 3.4, 2.8, 13.2, 14.4, 2.8);
      cairo_fill(cr);
      setSource(cr, colorx::onColor(c), 0.96);
      cairo_set_line_width(cr, 1.9);
      cairo_move_to(cr, 7, 10.2);
      cairo_line_to(cr, 9.2, 12.4);
      cairo_line_to(cr, 13.2, 7.6);
      cairo_stroke(cr);
      break;
    }
    case Icon::Area: {
      roundRect(cr, 3.6, 3.6, 12.8, 12.8, 3.4);
      cairo_stroke(cr);
      break;
    }
    case Icon::Project: {  // pie: ring + filled wedge of `param` (0..1) clockwise from 12 o'clock
      cairo_arc(cr, 10, 10, 6.6, 0, 2 * M_PI);
      cairo_stroke(cr);
      double f = ease::clamp01(param);
      if (f >= 0.999) {
        cairo_arc(cr, 10, 10, 6.6, 0, 2 * M_PI);
        cairo_fill(cr);
      } else if (f > 0.001) {
        cairo_move_to(cr, 10, 10);
        cairo_arc(cr, 10, 10, 6.6, -M_PI / 2, -M_PI / 2 + f * 2 * M_PI);
        cairo_close_path(cr);
        cairo_fill(cr);
      }
      break;
    }
    case Icon::Flag:
    case Icon::Deadlines: {
      cairo_move_to(cr, 5, 17);
      cairo_line_to(cr, 5, 3.4);
      cairo_stroke(cr);
      cairo_move_to(cr, 5, 4.2);
      cairo_curve_to(cr, 8, 2.6, 11, 6.2, 15.4, 4.4);
      cairo_line_to(cr, 15.4, 11.2);
      cairo_curve_to(cr, 11, 13, 8, 9.4, 5, 11);
      cairo_fill_preserve(cr);
      cairo_set_line_width(cr, 1.2);
      cairo_stroke(cr);
      break;
    }
    case Icon::Note: {
      roundRect(cr, 4.4, 3.2, 11.2, 13.6, 2.4);
      cairo_stroke(cr);
      for (double y : {7.4, 10.2, 13.0}) {
        cairo_move_to(cr, 7.2, y);
        cairo_line_to(cr, y > 12 ? 10.4 : 12.8, y);
      }
      cairo_set_line_width(cr, 1.3);
      cairo_stroke(cr);
      break;
    }
    case Icon::Checklist: {
      cairo_set_line_width(cr, 1.5);
      cairo_move_to(cr, 3, 6);
      cairo_line_to(cr, 4.4, 7.4);
      cairo_line_to(cr, 6.8, 4.4);
      cairo_move_to(cr, 3, 13.6);
      cairo_line_to(cr, 4.4, 15);
      cairo_line_to(cr, 6.8, 12);
      cairo_move_to(cr, 9.6, 6);
      cairo_line_to(cr, 17, 6);
      cairo_move_to(cr, 9.6, 13.6);
      cairo_line_to(cr, 17, 13.6);
      cairo_stroke(cr);
      break;
    }
    case Icon::Tag: {
      cairo_move_to(cr, 3.4, 4.4);
      cairo_line_to(cr, 10, 3.6);
      cairo_line_to(cr, 17, 10.6);
      cairo_line_to(cr, 10.6, 17);
      cairo_line_to(cr, 3.6, 10);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_arc(cr, 7, 7.2, 1.1, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    }
    case Icon::Plus: {
      cairo_set_line_width(cr, 2);
      cairo_move_to(cr, 10, 4.4);
      cairo_line_to(cr, 10, 15.6);
      cairo_move_to(cr, 4.4, 10);
      cairo_line_to(cr, 15.6, 10);
      cairo_stroke(cr);
      break;
    }
    case Icon::Search: {
      cairo_arc(cr, 8.8, 8.8, 5, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 12.6, 12.6);
      cairo_line_to(cr, 16.6, 16.6);
      cairo_stroke(cr);
      break;
    }
    case Icon::Sync: {
      cairo_translate(cr, 10, 10);
      cairo_rotate(cr, param);
      double r = 6.4;
      cairo_arc(cr, 0, 0, r, 0.35, M_PI - 0.55);
      cairo_stroke(cr);
      arrowHead(cr, r * std::cos(M_PI - 0.55), r * std::sin(M_PI - 0.55), M_PI - 0.55 + M_PI / 2, 3.2);
      cairo_arc(cr, 0, 0, r, M_PI + 0.35, 2 * M_PI - 0.55);
      cairo_stroke(cr);
      arrowHead(cr, r * std::cos(2 * M_PI - 0.55), r * std::sin(2 * M_PI - 0.55), 2 * M_PI - 0.55 + M_PI / 2, 3.2);
      break;
    }
    case Icon::Help: {
      cairo_arc(cr, 10, 10, 7.2, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_set_line_width(cr, 1.5);
      cairo_move_to(cr, 7.6, 8);
      cairo_curve_to(cr, 7.6, 4.8, 12.4, 4.8, 12.4, 8);
      cairo_curve_to(cr, 12.4, 9.6, 10, 9.6, 10, 11.6);
      cairo_stroke(cr);
      cairo_arc(cr, 10, 14.2, 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      break;
    }
  }
  cairo_restore(cr);
}

inline Rgba colorFor(Icon ic) {
  switch (ic) {
    case Icon::Inbox: return gPal.accent;
    case Icon::Today: return gPal.yellow;
    case Icon::Upcoming: case Icon::Calendar: return gPal.red;
    case Icon::Anytime: return gPal.teal;
    case Icon::Someday: case Icon::Archive: return gPal.tan;
    case Icon::Logbook: return gPal.green;
    case Icon::Deadlines: return gPal.red;
    default: return gPal.fg2;
  }
}

// -- the checkbox ---------------------------------------------------------------------------------------------
//
// Timeline (milliseconds), completing:
//    0-110  press   -- the ring squeezes to 86%
//  110-400  release -- springs back with a small overshoot while the fill grows from the centre
//  170-410  stroke  -- the check mark is drawn along its path
//  130-650  pulse   -- one soft ring expands off the edge and fades. No particles, no confetti.
// Un-completing plays a quicker reversal (240ms). A finished/idle box is just fill 0 or 1.
constexpr double kCompleteMs = 660;
constexpr double kReopenMs = 260;

struct CheckPose {
  double scale = 1, fill = 0, stroke = 0, pulse = -1;  // pulse: 0..1 progress, <0 = none
  bool muted = false;  // a settled, finished box is drawn in grey rather than the accent
};

inline CheckPose checkPose(bool done, double tMs, bool animating, bool toDone) {
  CheckPose p;
  if (!animating) {
    p.fill = done ? 1 : 0;
    p.stroke = done ? 1 : 0;
    return p;
  }
  if (toDone) {
    double t = tMs;
    if (t < 110) p.scale = 1 - 0.14 * ease::outQuad(t / 110);
    else p.scale = 0.86 + 0.17 * ease::outBack((t - 110) / 300, 2.4);
    p.fill = ease::outCubic((t - 100) / 270);
    p.stroke = ease::outCubic((t - 170) / 240);
    double u = (t - 130) / 520;
    p.pulse = (u >= 0 && u <= 1) ? u : -1;
  } else {
    double t = tMs;
    p.fill = 1 - ease::inOutCubic(t / 240);
    p.stroke = 1 - ease::inQuad(t / 150);
    p.scale = 1 - 0.07 * std::sin(ease::clamp01(t / 240) * M_PI);
  }
  return p;
}

// To-dos are rounded squares; projects (and checklist items) are circles, so the kind of a row reads at a glance.
enum class Shape { Square, Circle };

// `pie` (projects only, 0..1): the share of the project that is already done, drawn as a wedge inside the ring.
inline void checkbox(cairo_t* cr, double w, double h, const CheckPose& p, bool hot, Shape shape, double pie = -1) {
  double cx = w / 2, cy = h / 2, R = 8.2;
  const double corner = 4.9;
  bool sq = shape == Shape::Square;
  auto outline = [&](double r) {
    if (sq) roundRect(cr, -r, -r, 2 * r, 2 * r, std::min(corner, r));
    else cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
  };
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, p.scale, p.scale);

  Rgba acc = p.muted ? gPal.fg3 : gPal.accent;
  Rgba ring = hot ? gPal.accent : gPal.fg3;
  ring = Rgba{ease::lerp(ring.r, acc.r, p.fill), ease::lerp(ring.g, acc.g, p.fill), ease::lerp(ring.b, acc.b, p.fill), 1};
  cairo_set_line_width(cr, 1.6);
  outline(R - 0.8);
  setSource(cr, ring);
  cairo_stroke(cr);

  if (!sq && pie > 0.001 && p.fill < 0.999) {  // progress pie, inset from the ring
    double f = ease::clamp01(pie);
    setSource(cr, gPal.accent, 0.78 * (1 - p.fill));
    if (f >= 0.999) {
      cairo_arc(cr, 0, 0, R - 3.4, 0, 2 * M_PI);
    } else {
      cairo_move_to(cr, 0, 0);
      cairo_arc(cr, 0, 0, R - 3.4, -M_PI / 2, -M_PI / 2 + f * 2 * M_PI);
      cairo_close_path(cr);
    }
    cairo_fill(cr);
  }

  if (p.fill > 0.001) {  // fill grows from the centre
    outline((R - 0.4) * p.fill);
    setSource(cr, acc);
    cairo_fill(cr);
  }
  if (p.stroke > 0.001) {  // check mark, stroked progressively along its two segments
    const double ax = -3.7, ay = 0.4, bx = -1.1, by = 3.0, ex = 3.9, ey = -3.1;
    double l1 = std::hypot(bx - ax, by - ay), l2 = std::hypot(ex - bx, ey - by), total = l1 + l2;
    double d = p.stroke * total;
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    setSource(cr, p.muted ? colorx::onColor(acc) : gPal.onAccent);
    cairo_move_to(cr, ax, ay);
    if (d <= l1) {
      cairo_line_to(cr, ax + (bx - ax) * d / l1, ay + (by - ay) * d / l1);
    } else {
      cairo_line_to(cr, bx, by);
      double f = (d - l1) / l2;
      cairo_line_to(cr, bx + (ex - bx) * f, by + (ey - by) * f);
    }
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  if (p.pulse >= 0) {  // one expanding, fading ring
    double u = ease::outCubic(p.pulse);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_set_line_width(cr, ease::lerp(2.2, 0.6, u));
    setSource(cr, gPal.accent, 0.42 * (1 - p.pulse));
    outline(R + 0.2 + 6 * u);
    cairo_stroke(cr);
    cairo_restore(cr);
  }
}

// A checklist item's circle: smaller and lighter than a to-do's box. `fill` (0..1) animates the tick.
inline void checklistCircle(cairo_t* cr, double w, double h, double fill, bool hot) {
  double cx = w / 2, cy = h / 2, R = 6.4;
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  Rgba ring = hot ? gPal.accent : gPal.fg3;
  ring = Rgba{ease::lerp(ring.r, gPal.accent.r, fill), ease::lerp(ring.g, gPal.accent.g, fill),
              ease::lerp(ring.b, gPal.accent.b, fill), 1};
  cairo_set_line_width(cr, 1.4);
  cairo_arc(cr, 0, 0, R - 0.7, 0, 2 * M_PI);
  setSource(cr, ring);
  cairo_stroke(cr);
  if (fill > 0.001) {
    cairo_arc(cr, 0, 0, (R - 0.3) * fill, 0, 2 * M_PI);
    setSource(cr, gPal.accent);
    cairo_fill(cr);
    double k = ease::clamp01((fill - 0.35) / 0.65);
    if (k > 0) {
      cairo_set_line_width(cr, 1.6);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      setSource(cr, gPal.onAccent, k);
      cairo_move_to(cr, -2.6, 0.2);
      cairo_line_to(cr, -0.8, 2.0);
      cairo_line_to(cr, 2.8, -2.2);
      cairo_stroke(cr);
    }
  }
  cairo_restore(cr);
}

}  // namespace draw
