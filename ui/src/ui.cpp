// The touchscreen. Every screen is built once; update() refreshes the visible one and
// the bars around it. Positions are fixed for the 1024 × 600 panel: the top bar is 64 px,
// the tab bar 76 px, and each screen gets the 460 px between.

#include "leota/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <vector>

extern "C" {
LV_FONT_DECLARE(leota_inter_14)
LV_FONT_DECLARE(leota_inter_16)
LV_FONT_DECLARE(leota_inter_16_semi)
LV_FONT_DECLARE(leota_inter_20_bold)
LV_FONT_DECLARE(leota_inter_28_bold)
}

namespace leota {

namespace {

// Colours, as in the mockup.
constexpr uint32_t kBg = 0x0B0E13, kBar = 0x11151C, kCard = 0x131821, kLine = 0x232A35,
                   kRaised = 0x1A2230, kEdge = 0x3A475A, kChip = 0x18202B, kChipEdge = 0x2A3442,
                   kInk = 0xE8ECF2, kMuted = 0x9AA3B2, kSoft = 0xC5CDD9, kBlue = 0x6CB0FF,
                   kBlueBg = 0x1D3A5E, kAmber = 0xF5A524, kAmberInk = 0x1B1200, kRed = 0xC62F35,
                   kRedSoft = 0xFFA8AB, kGreen = 0x3DD68C, kGreenInk = 0x7BE3AE, kTab = 0x243042,
                   kGrey = 0x2B3646, kDark = 0x0E131A, kGreenBg = 0x12301F, kAmberBg = 0x3A2A0E,
                   kRedBg = 0x3A1A1C, kOff = 0x8B95A5, kAmberSoft = 0xF5C46B;

constexpr int kW = 1024, kTop = 64, kNav = 76, kBody = 600 - kTop - kNav;
const char* kMinus = "\xE2\x88\x92";  // −

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// A whole number of mm with a real minus sign and thousands commas: "−1,240".
std::string mm(double v) {
    const long n = std::lround(v);
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return (n < 0 ? std::string(kMinus) : std::string()) + out;
}

std::string signed_mm(double v) { return (std::lround(v) > 0 ? "+" : "") + mm(v); }

lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// A point for an lv_line, rounded to whole pixels (LVGL is built without float).
lv_point_precise_t pt(double x, double y) {
    return {static_cast<lv_value_precise_t>(std::lround(x)), static_cast<lv_value_precise_t>(std::lround(y))};
}

// A plain rectangle: no theme padding, scrolling or shadow.
lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg, int radius = 0,
              uint32_t border = 0) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    if (border) {
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, hex(border), 0);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h) { return box(parent, x, y, w, h, kCard, 14, kLine); }

lv_obj_t* text(lv_obj_t* parent, int x, int y, const char* t, const lv_font_t* font = &leota_inter_16,
               uint32_t color = kInk, int width = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_obj_set_pos(l, x, y);
    if (width > 0) {
        lv_obj_set_width(l, width);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(l, t);
    return l;
}

enum class Kind { Plain, Amber, Red, Blue, Grey };

// A button with a centred label. Returns the button; its label is child 0.
lv_obj_t* button(lv_obj_t* parent, int x, int y, int w, int h, const char* t, Kind k = Kind::Plain,
                 const lv_font_t* font = &leota_inter_16_semi) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    uint32_t bg = kRaised, fg = kInk, edge = kEdge;
    if (k == Kind::Amber) { bg = kAmber; fg = kAmberInk; edge = 0; }
    if (k == Kind::Red) { bg = kRed; fg = 0xFFFFFF; edge = 0; }
    if (k == Kind::Blue) { bg = kBlueBg; fg = 0xFFFFFF; edge = kBlue; }
    if (k == Kind::Grey) { bg = kGrey; fg = 0xFFFFFF; edge = 0; }
    lv_obj_set_style_bg_color(b, hex(bg), 0);
    lv_obj_set_style_border_width(b, edge ? 1 : 0, 0);
    lv_obj_set_style_border_color(b, hex(edge), 0);
    lv_obj_set_style_bg_color(b, hex(0x151B24), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, hex(kOff), LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, hex(kChipEdge), LV_STATE_DISABLED);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(fg), 0);
    lv_obj_set_style_text_color(l, hex(kOff), LV_STATE_DISABLED);
    lv_label_set_text(l, t);
    lv_obj_center(l);
    return b;
}

lv_obj_t* dot(lv_obj_t* parent, int x, int y, int size, uint32_t color) {
    lv_obj_t* d = box(parent, x, y, size, size, color, LV_RADIUS_CIRCLE);
    return d;
}

struct Chip {
    lv_obj_t* box = nullptr;
    lv_obj_t* dot = nullptr;
    lv_obj_t* label = nullptr;
};

// A horizontal bar with an indicator.
lv_obj_t* bar(lv_obj_t* parent, int x, int y, int w, int h, int max) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, max);
    lv_obj_set_style_bg_color(b, hex(kDark), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, hex(kChipEdge), 0);
    lv_obj_set_style_radius(b, std::min(w, h) / 2, 0);
    lv_obj_set_style_radius(b, std::min(w, h) / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, hex(kBlue), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(b, 0, 0);
    return b;
}

// A top view of the gazebo: posts, workspace, dock, the lines and the ball.
struct MapView {
    lv_obj_t* root = nullptr;
    lv_obj_t* ball = nullptr;
    std::array<lv_obj_t*, kLines> lines{};
    std::array<std::array<lv_point_precise_t, 2>, kLines> pts{};
    std::array<std::array<lv_point_precise_t, 2>, 4> ws_pts{};
    double scale = 0.1, half = 1156.0;
    int size = 250, ball_r = 18;

    void build(lv_obj_t* parent, int x, int y, int px, const RigConfig& cfg) {
        size = px;
        half = 0.0;
        for (const Line& l : cfg.lines) half = std::max({half, std::fabs(l.anchor.x), std::fabs(l.anchor.y)});
        scale = (px - 16.0) / (2.0 * half);
        ball_r = std::max(6, static_cast<int>(std::lround(cfg.ball_radius * scale)));
        root = box(parent, x, y, px, px, kDark, 6, kChipEdge);
        // The workspace, dashed.
        const Box ws = workspace(cfg);
        const lv_point_precise_t c[4] = {pt(map_x(ws.lo.x), map_y(ws.hi.y)), pt(map_x(ws.hi.x), map_y(ws.hi.y)),
                                         pt(map_x(ws.hi.x), map_y(ws.lo.y)), pt(map_x(ws.lo.x), map_y(ws.lo.y))};
        for (int i = 0; i < 4; ++i) {
            ws_pts[i] = {c[i], c[(i + 1) % 4]};
            lv_obj_t* l = lv_line_create(root);
            lv_line_set_points(l, ws_pts[i].data(), 2);
            lv_obj_set_style_line_color(l, hex(kEdge), 0);
            lv_obj_set_style_line_width(l, 1, 0);
            lv_obj_set_style_line_dash_width(l, 5, 0);
            lv_obj_set_style_line_dash_gap(l, 5, 0);
        }
        // The dock: a ring.
        const int dr = std::max(6, static_cast<int>(cfg.ball_radius * scale * 0.6));
        lv_obj_t* dock = box(root, static_cast<int>(map_x(cfg.dock.x)) - dr, static_cast<int>(map_y(cfg.dock.y)) - dr,
                             2 * dr, 2 * dr, kDark, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(dock, 2, 0);
        lv_obj_set_style_border_color(dock, hex(0x5B6B82), 0);
        for (int i = 0; i < kLines; ++i) {
            lines[i] = lv_line_create(root);
            lv_obj_set_style_line_color(lines[i], hex(kBlue), 0);
            lv_obj_set_style_line_width(lines[i], 2, 0);
            lv_obj_set_style_line_opa(lines[i], LV_OPA_70, 0);
        }
        ball = box(root, 0, 0, 2 * ball_r, 2 * ball_r, kBlueBg, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(ball, 2, 0);
        lv_obj_set_style_border_color(ball, hex(0x9CC4FF), 0);
        for (int i = 0; i < kLines; ++i) {
            const Line& l = cfg.lines[i];
            const int px_ = static_cast<int>(map_x(l.anchor.x)), py = static_cast<int>(map_y(l.anchor.y));
            box(root, px_ - 6, py - 6, 12, 12, 0x5B6B82, 2);
            const int lx = l.anchor.x < 0 ? px_ + 10 : px_ - 20, ly = l.anchor.y > 0 ? py + 8 : py - 24;
            char name[2] = {l.name, 0};
            text(root, lx, ly, name, &leota_inter_14, kMuted);
        }
        place({cfg.dock.x, cfg.dock.y, cfg.dock.z}, cfg);
    }
    float map_x(double x) const { return static_cast<float>(size / 2.0 + x * scale); }
    float map_y(double y) const { return static_cast<float>(size / 2.0 - y * scale); }
    void place(const Vec3& p, const RigConfig& cfg) {
        const float bx = map_x(p.x), by = map_y(p.y);
        lv_obj_set_pos(ball, static_cast<int>(bx) - ball_r, static_cast<int>(by) - ball_r);
        for (int i = 0; i < kLines; ++i) {
            const Line& l = cfg.lines[i];
            const float ax = map_x(l.anchor.x), ay = map_y(l.anchor.y);
            const lv_point_precise_t a = pt(ax, ay), e = pt(bx + (ax > bx ? 5.f : -5.f), by + (ay > by ? 5.f : -5.f));
            if (pts[i][1].x == e.x && pts[i][1].y == e.y && pts[i][0].x == a.x && pts[i][0].y == a.y) continue;
            pts[i] = {a, e};
            lv_line_set_points(lines[i], pts[i].data(), 2);
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------

struct Ui::Impl {
    Rig& rig;
    lv_display_t* disp;
    std::string pin;
    double now = 0.0;
    UiLink link;
    std::string clock;

    lv_obj_t* scr = nullptr;
    std::map<std::string, lv_obj_t*> named;
    std::map<lv_obj_t*, std::string> shown;  // last text set on each label
    std::vector<std::string> page_names = {"status", "controls", "jog", "weather", "diagnostics", "setup", "log"};
    std::vector<lv_obj_t*> pages, tabs;
    int page = 0;
    lv_obj_t* estop_page = nullptr;

    // Top bar
    lv_obj_t *badge = nullptr, *badge_text = nullptr, *subtitle = nullptr, *clock_label = nullptr;
    Chip remote_chip, link_chip, wind_chip;

    // Status
    MapView status_map;
    lv_obj_t *pos_sub = nullptr, *zbar = nullptr, *xyz[3] = {}, *map_note = nullptr;
    std::array<lv_obj_t*, kLines> line_bar{}, line_value{}, line_len{};
    lv_obj_t *wind_now = nullptr, *wind_gust = nullptr, *wind_level = nullptr, *wind_level_text = nullptr,
             *wind_level_dot = nullptr;
    std::array<lv_obj_t*, 5> sys_value{};

    // Controls
    lv_obj_t *key_note = nullptr, *remote_text = nullptr, *remote_btn = nullptr, *arm_title = nullptr,
             *arm_text = nullptr, *arm_btn = nullptr, *home_text = nullptr, *home_btn = nullptr,
             *park_btn = nullptr, *fault_text = nullptr, *fault_btn = nullptr, *weather_text = nullptr;
    lv_obj_t *dialog = nullptr, *dialog_check = nullptr, *dialog_arm = nullptr;
    std::array<lv_obj_t*, 3> dialog_checks{};

    // Jog
    MapView jog_map;
    lv_obj_t *jog_z = nullptr, *jog_note = nullptr, *jog_read[3] = {};
    std::vector<lv_obj_t*> jog_buttons, speed_buttons;
    int speed = 1;
    int jog_axis = -1;
    double jog_dir = 0.0, jog_last = -1.0;

    // Weather
    lv_obj_t *w_chip = nullptr, *w_chip_text = nullptr, *w_chip_dot = nullptr, *w_read[3] = {}, *w_swing = nullptr,
             *w_sensor = nullptr, *lock_text = nullptr, *lock_bar = nullptr, *lock_note = nullptr;
    std::array<lv_obj_t*, 30> w_bars{};
    std::array<lv_obj_t*, 3> level_rows{};
    std::array<std::array<lv_point_precise_t, 2>, 2> thresh_pts{};
    std::deque<double> minute_max{std::deque<double>(30, 0.0)};
    double minute_start = -1.0, next_sample = 0.0;
    int chart_h = 150;
    double chart_top = 14.0;

    // Diagnostics
    lv_obj_t* table = nullptr;
    std::array<lv_obj_t*, 6> safety_value{}, sensor_value{}, net_value{};

    // Setup
    lv_obj_t *locked = nullptr, *unlocked = nullptr, *pin_dots = nullptr;
    std::string entered;

    // Log
    struct Entry { LogKind kind; std::string clock, text; };
    std::deque<Entry> entries;
    lv_obj_t* log_list = nullptr;
    std::vector<lv_obj_t*> filter_buttons;
    int filter = -1;  // -1: all
    bool log_dirty = true;
    std::string last_message;
    RigState last_state = RigState::Disarmed;

    // A message shown for a few seconds over everything
    lv_obj_t *toast = nullptr, *toast_text = nullptr;
    double toast_until = 0.0;

    Impl(Rig& r, lv_display_t* d, std::string p) : rig(r), disp(d), pin(std::move(p)) {}
    ~Impl() {
        // The widgets' callbacks point into this object: take the widgets away first.
        lv_obj_clean(lv_display_get_layer_top(disp));
        if (scr) lv_obj_clean(scr);
    }

    // -- helpers -------------------------------------------------------------------------
    void set(lv_obj_t* label, const std::string& t) {
        auto it = shown.find(label);
        if (it != shown.end() && it->second == t) return;
        shown[label] = t;
        lv_label_set_text(label, t.c_str());
    }
    void set_color(lv_obj_t* o, uint32_t c, lv_style_selector_t sel = 0) {
        const lv_color_t want = hex(c);
        if (lv_color_eq(lv_obj_get_style_bg_color(o, sel), want)) return;
        lv_obj_set_style_bg_color(o, want, sel);
    }
    void set_text_color(lv_obj_t* o, uint32_t c) {
        const lv_color_t want = hex(c);
        if (lv_color_eq(lv_obj_get_style_text_color(o, 0), want)) return;
        lv_obj_set_style_text_color(o, want, 0);
    }
    void enable(lv_obj_t* b, bool on) {
        if (lv_obj_has_state(b, LV_STATE_DISABLED) == !on) return;
        lv_obj_set_state(b, LV_STATE_DISABLED, !on);
    }
    void show(lv_obj_t* o, bool on) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == !on) return;
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    void bar_value(lv_obj_t* b, int v) {
        if (lv_bar_get_value(b) != v) lv_bar_set_value(b, v, LV_ANIM_OFF);
    }
    void name(const std::string& n, lv_obj_t* o) { named[n] = o; }

    using Action = std::function<void()>;
    std::vector<std::unique_ptr<Action>> actions;
    void on(lv_obj_t* o, lv_event_code_t code, Action a) {
        actions.push_back(std::make_unique<Action>(std::move(a)));
        lv_obj_add_event_cb(o, [](lv_event_t* e) { (*static_cast<Action*>(lv_event_get_user_data(e)))(); }, code,
                            actions.back().get());
    }
    void on_click(lv_obj_t* o, Action a) { on(o, LV_EVENT_CLICKED, std::move(a)); }

    void say(const std::string& t) {
        set(toast_text, t);
        show(toast, true);
        lv_obj_move_foreground(toast);
        toast_until = now + 3.5;
    }
    void result(const Result& r) {
        if (!r) say(r.detail.empty() ? "Refused." : r.detail);
    }
    void add_log(LogKind k, const std::string& t) {
        entries.push_front({k, clock, t});
        while (entries.size() > 60) entries.pop_back();
        log_dirty = true;
    }

    // -- the frame -----------------------------------------------------------------------
    void build() {
        scr = lv_display_get_screen_active(disp);
        lv_obj_remove_style_all(scr);
        lv_obj_set_style_bg_color(scr, hex(kBg), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(scr, &leota_inter_16, 0);
        lv_obj_set_style_text_color(scr, hex(kInk), 0);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        build_top();
        for (size_t i = 0; i < page_names.size(); ++i) {
            lv_obj_t* p = box(scr, 0, kTop, kW, kBody, kBg);
            pages.push_back(p);
            name("page." + page_names[i], p);
        }
        build_status(pages[0]);
        build_controls(pages[1]);
        build_jog(pages[2]);
        build_weather(pages[3]);
        build_diagnostics(pages[4]);
        build_setup(pages[5]);
        build_log(pages[6]);
        estop_page = box(scr, 0, kTop, kW, kBody, kBg);
        name("page.estop", estop_page);
        build_estop(estop_page);
        build_nav();
        build_dialog();
        toast = box(lv_display_get_layer_top(disp), 212, 80, 600, 52, kRedBg, 10, kRed);
        toast_text = text(toast, 16, 15, "", &leota_inter_16, 0xFFD3D5, 568);
        show(toast, false);
        go(0);
    }

    void go(int p) {
        page = p;
        for (size_t i = 0; i < pages.size(); ++i) show(pages[i], static_cast<int>(i) == p);
        for (size_t i = 0; i < tabs.size(); ++i) {
            const bool on_ = static_cast<int>(i) == p;
            set_color(tabs[i], on_ ? kTab : kBar);
            set_text_color(lv_obj_get_child(tabs[i], 0), on_ ? 0xFFFFFF : 0xAEB7C5);
        }
        log_dirty = true;
    }

    void build_top() {
        lv_obj_t* top = box(scr, 0, 0, kW, kTop, kBar);
        lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top, 1, 0);
        lv_obj_set_style_border_color(top, hex(kLine), 0);
        badge = box(top, 16, 10, 150, 44, kGrey, 10);
        badge_text = text(badge, 0, 0, "DISARMED", &leota_inter_20_bold, kInk);
        lv_obj_center(badge_text);
        subtitle = text(top, 178, 22, "", &leota_inter_16, kSoft);
        lv_obj_set_width(subtitle, 300);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
        auto chip = [&](int x, int w) {
            Chip c;
            c.box = box(top, x, 14, w, 36, kChip, 18, kChipEdge);
            c.dot = dot(c.box, 12, 12, 10, kOff);
            c.label = text(c.box, 30, 8, "", &leota_inter_14, 0xD5DCE6);
            return c;
        };
        remote_chip = chip(490, 112);
        link_chip = chip(610, 166);
        wind_chip = chip(784, 128);
        clock_label = text(top, 924, 21, "", &leota_inter_16, kSoft);
        lv_obj_set_width(clock_label, 84);
        lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    void build_nav() {
        lv_obj_t* nav = box(scr, 0, 600 - kNav, kW, kNav, kBar);
        lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(nav, 1, 0);
        lv_obj_set_style_border_color(nav, hex(kLine), 0);
        const char* labels[] = {"Status", "Controls", "Jog", "Weather", "Diagnostics", "Setup", "Log"};
        for (int i = 0; i < 7; ++i) {
            lv_obj_t* t = button(nav, 12 + i * 120, 8, 112, 60, labels[i]);
            lv_obj_set_style_border_width(t, 0, 0);
            lv_obj_set_style_bg_color(t, hex(kBar), 0);
            tabs.push_back(t);
            name("tab." + page_names[i], t);
            on_click(t, [this, i] { go(i); });
        }
        lv_obj_t* stop = button(nav, 852, 8, 160, 60, "STOP", Kind::Red, &leota_inter_20_bold);
        name("stop", stop);
        on_click(stop, [this] {
            jog_axis = -1;
            result(rig.stop(std::nullopt, now));
            add_log(LogKind::Motion, "STOP pressed on the screen.");
        });
    }

    // -- Status --------------------------------------------------------------------------
    void build_status(lv_obj_t* p) {
        const RigConfig& cfg = rig.config();
        lv_obj_t* left = card(p, 16, 14, 468, 432);
        text(left, 16, 12, "Position", &leota_inter_20_bold);
        pos_sub = text(left, 260, 15, "", &leota_inter_14, kMuted);
        lv_obj_set_width(pos_sub, 192);
        lv_obj_set_style_text_align(pos_sub, LV_TEXT_ALIGN_RIGHT, 0);
        status_map.build(left, 16, 46, 250, cfg);
        lv_obj_t* aud = text(left, 16, 302, "AUDIENCE", &leota_inter_14, kMuted);
        lv_obj_set_width(aud, 250);
        lv_obj_set_style_text_align(aud, LV_TEXT_ALIGN_CENTER, 0);
        text(left, 282, 46, mm(cfg.axes[2].max).c_str(), &leota_inter_14, kMuted);
        zbar = bar(left, 290, 68, 36, 210, 1000);
        lv_obj_set_style_radius(zbar, 8, 0);
        lv_obj_set_style_radius(zbar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(zbar, hex(kBlueBg), LV_PART_INDICATOR);
        text(left, 282, 282, mm(cfg.axes[2].min).c_str(), &leota_inter_14, kMuted);
        const char* names[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) {
            text(left, 344, 48 + a * 70, names[a], &leota_inter_14, kMuted);
            xyz[a] = text(left, 344, 66 + a * 70, "", &leota_inter_28_bold);
        }
        text(left, 344, 258, "mm", &leota_inter_14, kMuted);
        lv_obj_t* rule = box(left, 16, 330, 436, 1, kLine);
        (void)rule;
        map_note = text(left, 16, 344, "", &leota_inter_14, kMuted, 436);

        lv_obj_t* lines = card(p, 498, 14, 510, 180);
        text(lines, 16, 12, "Line tension", &leota_inter_20_bold);
        text(lines, 330, 15, fmt("limits %.0f\xE2\x80\x93%.0f N", cfg.tension_min, cfg.tension_max).c_str(),
             &leota_inter_14, kMuted);
        const char* where[4] = {"back L", "back R", "front L", "front R"};
        for (int i = 0; i < kLines; ++i) {
            const int y = 48 + i * 31;
            text(lines, 16, y, fmt("%c", cfg.lines[i].name).c_str(), &leota_inter_16_semi);
            text(lines, 36, y + 1, where[i], &leota_inter_14, kMuted);
            line_bar[i] = bar(lines, 110, y + 3, 230, 14, static_cast<int>(cfg.tension_max * 10));
            line_value[i] = text(lines, 350, y, "", &leota_inter_16);
            lv_obj_set_width(line_value[i], 70);
            lv_obj_set_style_text_align(line_value[i], LV_TEXT_ALIGN_RIGHT, 0);
            line_len[i] = text(lines, 430, y + 1, "", &leota_inter_14, kMuted);
            lv_obj_set_width(line_len[i], 66);
            lv_obj_set_style_text_align(line_len[i], LV_TEXT_ALIGN_RIGHT, 0);
        }

        lv_obj_t* wind = card(p, 498, 206, 249, 240);
        text(wind, 16, 12, "Wind", &leota_inter_20_bold);
        wind_now = text(wind, 16, 48, "", &leota_inter_28_bold);
        text(wind, 100, 58, "m/s now", &leota_inter_14, kMuted);
        wind_gust = text(wind, 16, 92, "", &leota_inter_14, kSoft, 217);
        wind_level = box(wind, 16, 178, 217, 44, kGreenBg, 10);
        wind_level_dot = dot(wind_level, 12, 17, 10, kGreen);
        wind_level_text = text(wind_level, 30, 12, "", &leota_inter_16_semi, kGreenInk);

        lv_obj_t* sys = card(p, 759, 206, 249, 240);
        text(sys, 16, 12, "System", &leota_inter_20_bold);
        const char* keys[5] = {"E-stop chain", "Drives", "Brakes", "Homed", "Faults"};
        for (int i = 0; i < 5; ++i) {
            text(sys, 16, 50 + i * 34, keys[i], &leota_inter_14, kMuted);
            sys_value[i] = text(sys, 110, 49 + i * 34, "", &leota_inter_16_semi);
            lv_obj_set_width(sys_value[i], 123);
            lv_obj_set_style_text_align(sys_value[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(sys_value[i], LV_LABEL_LONG_DOT);
        }
    }

    void refresh_status() {
        const RigConfig& cfg = rig.config();
        const Vec3 p = rig.position();
        status_map.place(p, cfg);
        set(pos_sub, std::string(cfg.two_axis ? "2-axis" : "3-axis") + " \xC2\xB7 " + (rig.homed() ? "homed" : "not homed"));
        const AxisLimits& z = cfg.axes[2];
        bar_value(zbar, static_cast<int>(std::clamp((p.z - z.min) / (z.max - z.min), 0.0, 1.0) * 1000));
        set(xyz[0], rig.homed() ? signed_mm(p.x) : "\xE2\x80\x94");
        set(xyz[1], rig.homed() ? signed_mm(p.y) : "\xE2\x80\x94");
        set(xyz[2], rig.homed() ? mm(p.z) : "\xE2\x80\x94");
        const Vec3 lean = rig.lean();
        set(map_note, fmt("Dashed: workspace.  Ring: dock.  Lean %.1f\xC2\xB0", std::hypot(lean.x, lean.y) * 57.2958));
        const auto ls = rig.lines();
        for (int i = 0; i < kLines; ++i) {
            bar_value(line_bar[i], static_cast<int>(std::clamp(ls[i].tension, 0.0, cfg.tension_max) * 10));
            const uint32_t c = ls[i].tension < cfg.tension_min && rig.homed() ? kAmber
                               : ls[i].tension > cfg.tension_max                ? 0xE5484D
                                                                                : kBlue;
            set_color(line_bar[i], c, LV_PART_INDICATOR);
            set(line_value[i], fmt("%.1f N", ls[i].tension));
            set(line_len[i], ls[i].length > 0 ? fmt("%.2f m", ls[i].length / 1000) : std::string(""));
        }
        const WindMonitor& w = rig.wind();
        set(wind_now, cfg.wind.enabled ? fmt("%.1f", w.speed()) : std::string("\xE2\x80\x94"));
        set(wind_gust, cfg.wind.enabled ? fmt("Gust %.1f m/s \xC2\xB7 last 10 min", w.gust()) : std::string("No wind sensor fitted."));
        level_chip(wind_level, wind_level_dot, wind_level_text);
        const bool chain_ok = !rig.estop();
        set(sys_value[0], chain_ok ? "Closed" : "OPEN");
        set_text_color(sys_value[0], chain_ok ? kGreenInk : kRedSoft);
        set(sys_value[1], rig.drives_on() ? "On" : "Off");
        set_text_color(sys_value[1], rig.drives_on() ? kGreenInk : kInk);
        set(sys_value[2], rig.brakes_released() ? "Released" : "Holding");
        set_text_color(sys_value[2], rig.brakes_released() ? kAmberSoft : kInk);
        set(sys_value[3], rig.homed() ? "Yes" : "No");
        set_text_color(sys_value[3], rig.homed() ? kGreenInk : kAmberSoft);
        set(sys_value[4], fault_names().empty() ? "None" : fault_names());
        set_text_color(sys_value[4], rig.faults() ? kRedSoft : kGreenInk);
    }

    std::string fault_names() const {
        std::string s;
        for (std::uint32_t bit = 1; bit <= faults::kAll; bit <<= 1)
            if (rig.faults() & bit) s += (s.empty() ? "" : ", ") + std::string(faults::name(bit));
        return s;
    }

    void level_chip(lv_obj_t* chip, lv_obj_t* d, lv_obj_t* label) {
        const RigConfig& cfg = rig.config();
        if (!cfg.wind.enabled) {
            set_color(chip, kChip);
            set_color(d, kOff);
            set_text_color(label, kSoft);
            set(label, "No wind sensor");
            return;
        }
        const WindMonitor& w = rig.wind();
        switch (w.level()) {
            case WindLevel::Green:
                set_color(chip, kGreenBg); set_color(d, kGreen); set_text_color(label, kGreenInk);
                set(label, "Green: full workspace");
                break;
            case WindLevel::Amber:
                set_color(chip, kAmberBg); set_color(d, kAmber); set_text_color(label, kAmberSoft);
                set(label, "Amber: low and slow");
                break;
            case WindLevel::Red:
                set_color(chip, kRedBg); set_color(d, 0xE5484D); set_text_color(label, kRedSoft);
                set(label, !w.ever_read() ? "Waiting for sensor" : w.holding() ? "Red: hold" : "Red");
                break;
        }
    }

    // -- Controls ------------------------------------------------------------------------
    lv_obj_t* tile(lv_obj_t* p, int i, const char* title, lv_obj_t** title_out = nullptr) {
        const int w = 322, h = 184, x = 16 + (i % 3) * (w + 13), y = 66 + (i / 3) * (h + 12);
        lv_obj_t* t = card(p, x, y, w, h);
        lv_obj_t* l = text(t, 14, 12, title, &leota_inter_20_bold);
        if (title_out) *title_out = l;
        return t;
    }

    void build_controls(lv_obj_t* p) {
        lv_obj_t* note = box(p, 16, 14, 992, 40, kCard, 10, kLine);
        key_note = text(note, 14, 10, "", &leota_inter_14, kSoft, 960);

        lv_obj_t* t0 = tile(p, 0, "Remote control");
        remote_text = text(t0, 14, 42, "", &leota_inter_14, kMuted, 294);
        remote_btn = button(t0, 14, 118, 294, 52, "");
        name("controls.remote", remote_btn);
        on_click(remote_btn, [this] {
            rig.set_remote(!rig.remote(), now);
            add_log(LogKind::Safety, rig.remote() ? "Remote control turned on at the screen." : "Remote control turned off at the screen.");
        });

        lv_obj_t* t1 = tile(p, 1, "Arm", &arm_title);
        arm_text = text(t1, 14, 42, "", &leota_inter_14, kMuted, 294);
        arm_btn = button(t1, 14, 118, 294, 52, "", Kind::Amber, &leota_inter_20_bold);
        name("controls.arm", arm_btn);
        on_click(arm_btn, [this] {
            if (rig.armed()) {
                result(rig.disarm(now));
                add_log(LogKind::Safety, "Disarmed at the screen.");
            } else {
                open_dialog();
            }
        });

        lv_obj_t* t2 = tile(p, 2, "Home");
        home_text = text(t2, 14, 42, "", &leota_inter_14, kMuted, 294);
        home_btn = button(t2, 14, 118, 294, 52, "Home at dock");
        name("controls.home", home_btn);
        on_click(home_btn, [this] { result(rig.home(now)); });

        lv_obj_t* t3 = tile(p, 3, "Park");
        text(t3, 14, 42, "Flies back over the pedestal and sets the ball down. Use before shutdown or in bad weather.",
             &leota_inter_14, kMuted, 294);
        park_btn = button(t3, 14, 118, 294, 52, "Park at dock");
        name("controls.park", park_btn);
        on_click(park_btn, [this] { result(rig.park(now)); });

        lv_obj_t* t4 = tile(p, 4, "Clear fault");
        fault_text = text(t4, 14, 42, "", &leota_inter_14, kMuted, 294);
        fault_btn = button(t4, 14, 118, 294, 52, "Clear fault");
        name("controls.clear", fault_btn);
        on_click(fault_btn, [this] { result(rig.clear_faults(now)); });

        lv_obj_t* t5 = tile(p, 5, "Weather");
        weather_text = text(t5, 14, 42, "", &leota_inter_14, kMuted, 294);
        lv_obj_t* wb = button(t5, 14, 118, 294, 52, "Weather details");
        on_click(wb, [this] { go(3); });
    }

    void refresh_controls() {
        const bool armed = rig.armed();
        set(key_note, rig.remote()
                          ? "Remote control is ON: Show Controller can arm, home and move the rig. A key switch, when fitted, overrides this screen."
                          : "Remote control is OFF: the network can only watch and stop. Jog and these controls work.");
        set(remote_text, rig.remote() ? "On: Show Controller can arm, home and move the rig. Stop works from anywhere."
                                      : "Off: the network can only watch and stop. Jog and these controls work.");
        set(lv_obj_get_child(remote_btn, 0), rig.remote() ? "Turn remote off" : "Turn remote on");
        set_color(remote_btn, rig.remote() ? kRaised : kBlueBg);
        set(arm_title, armed ? "Disarm" : "Arm");
        set(arm_text, armed ? "Stops smoothly, puts the brakes on and turns the drives off."
                            : "Turns the drives on, waits for them to hold, then releases the brakes. Asks you to confirm first.");
        set(lv_obj_get_child(arm_btn, 0), armed ? "DISARM" : "ARM\xE2\x80\xA6");
        set_color(arm_btn, armed ? kGrey : kAmber);
        set_text_color(lv_obj_get_child(arm_btn, 0), armed ? 0xFFFFFF : kAmberInk);
        const bool idle = rig.state() == RigState::Idle;
        enable(home_btn, armed && idle);
        set(home_text, std::string("Reels the lines in at the dock until tight, then lifts 30 mm and checks. ") +
                           (!armed ? "Arm first." : rig.homed() ? "Park first if flying." : "Ready."));
        enable(park_btn, armed && rig.homed() && !rig.parking());
        const bool faulted = rig.state() == RigState::Fault;
        enable(fault_btn, faulted);
        set(fault_text, faulted ? "Faults: " + fault_names() + ". Clears them once their cause is gone; the rig stays disarmed."
                                : "No faults now. Clears a fault once its cause is gone; the rig stays disarmed.");
        const WindMonitor& w = rig.wind();
        set(weather_text, !rig.config().wind.enabled ? "No wind sensor fitted."
                          : w.holding() ? fmt("Red hold: arming waits about %.0f more minutes.", std::ceil(w.calm_left() / 60))
                                        : fmt("%s now. At Red the ball parks, and arming waits until it has been Green for %.0f minutes.",
                                              w.level() == WindLevel::Green ? "Green" : w.level() == WindLevel::Amber ? "Amber" : "Red",
                                              rig.config().wind.hold / 60));
    }

    void build_dialog() {
        dialog = box(lv_display_get_layer_top(disp), 0, 0, kW, 600, 0x05070A);
        lv_obj_set_style_bg_opa(dialog, LV_OPA_80, 0);
        lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);  // nothing behind it can be touched
        lv_obj_t* c = box(dialog, 212, 76, 600, 448, 0x151B24, 16, kEdge);
        text(c, 24, 20, "Arm the rig?", &leota_inter_28_bold);
        text(c, 24, 64, "The drives turn on and the brakes release. With remote on, Show Controller can move the ball as soon as this finishes.",
             &leota_inter_16, kSoft, 552);
        const char* keys[3] = {"E-stop chain", "Wind", "Drives"};
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* row = box(c, 24, 130 + i * 42, 552, 36, kDark, 8);
            text(row, 12, 8, keys[i], &leota_inter_16, kMuted);
            dialog_checks[i] = text(row, 300, 8, "", &leota_inter_16_semi);
            lv_obj_set_width(dialog_checks[i], 240);
            lv_obj_set_style_text_align(dialog_checks[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
        dialog_check = lv_checkbox_create(c);
        lv_obj_set_pos(dialog_check, 24, 266);
        lv_obj_set_width(dialog_check, 552);
        lv_checkbox_set_text(dialog_check, "I've checked that nobody is under the ball and the lines are free.");
        lv_obj_set_style_text_font(dialog_check, &leota_inter_16_semi, 0);
        lv_obj_set_style_text_color(dialog_check, hex(kInk), 0);
        lv_obj_set_style_pad_all(dialog_check, 10, 0);
        lv_obj_set_style_bg_color(dialog_check, hex(kRaised), 0);
        lv_obj_set_style_bg_opa(dialog_check, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dialog_check, 10, 0);
        lv_obj_set_style_border_width(dialog_check, 1, 0);
        lv_obj_set_style_border_color(dialog_check, hex(kEdge), 0);
        lv_obj_set_style_border_color(dialog_check, hex(kAmber), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(dialog_check, hex(kAmber), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(dialog_check, hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(dialog_check, hex(kAmberInk), LV_PART_INDICATOR | LV_STATE_CHECKED);
        name("dialog.check", dialog_check);
        on(dialog_check, LV_EVENT_VALUE_CHANGED, [this] { enable(dialog_arm, lv_obj_has_state(dialog_check, LV_STATE_CHECKED)); });
        lv_obj_t* cancel = button(c, 24, 366, 268, 60, "Cancel");
        name("dialog.cancel", cancel);
        on_click(cancel, [this] { show(dialog, false); });
        dialog_arm = button(c, 308, 366, 268, 60, "ARM", Kind::Amber, &leota_inter_20_bold);
        name("dialog.arm", dialog_arm);
        on_click(dialog_arm, [this] {
            if (!lv_obj_has_state(dialog_check, LV_STATE_CHECKED)) return;
            show(dialog, false);
            const Result r = rig.arm(now);
            result(r);
            if (r) add_log(LogKind::Safety, "Armed at the screen, after checking the area.");
        });
        show(dialog, false);
    }

    void open_dialog() {
        lv_obj_remove_state(dialog_check, LV_STATE_CHECKED);
        enable(dialog_arm, false);
        const WindMonitor& w = rig.wind();
        set(dialog_checks[0], rig.estop() ? "OPEN" : "Closed");
        set_text_color(dialog_checks[0], rig.estop() ? kRedSoft : kGreenInk);
        const char* lv = !rig.config().wind.enabled ? "No sensor" : w.level() == WindLevel::Green ? "Green"
                         : w.level() == WindLevel::Amber ? "Amber" : "Red";
        set(dialog_checks[1], lv);
        set_text_color(dialog_checks[1], !rig.config().wind.enabled || w.level() == WindLevel::Green ? kGreenInk
                                         : w.level() == WindLevel::Amber ? kAmberSoft : kRedSoft);
        set(dialog_checks[2], rig.faults() & faults::DriveAlarm ? "Alarm" : "Ready");
        set_text_color(dialog_checks[2], rig.faults() & faults::DriveAlarm ? kRedSoft : kGreenInk);
        show(dialog, true);
        lv_obj_move_foreground(dialog);
    }

    // -- Jog -----------------------------------------------------------------------------
    lv_obj_t* jog_button(lv_obj_t* p, int x, int y, int w, int h, const char* t, int axis, double dir, const char* id) {
        lv_obj_t* b = button(p, x, y, w, h, t, Kind::Plain, &leota_inter_20_bold);
        name(id, b);
        jog_buttons.push_back(b);
        on(b, LV_EVENT_PRESSED, [this, axis, dir] {
            jog_axis = axis;
            jog_dir = dir;
            jog_last = -1.0;
            send_jog();
        });
        auto stop_jog = [this, axis] {
            if (jog_axis != axis) return;
            jog_axis = -1;
            rig.jog(axis, 0.0, now);
        };
        on(b, LV_EVENT_RELEASED, stop_jog);
        on(b, LV_EVENT_PRESS_LOST, stop_jog);
        return b;
    }

    double jog_speed() const {
        static const double speeds[3] = {30.0, 100.0, 250.0};
        return speeds[speed];
    }

    void send_jog() {
        if (jog_axis < 0) return;
        if (rig.remote()) {
            say("Turn remote control off to jog.");
            jog_axis = -1;
            return;
        }
        jog_last = now;
        const Result r = rig.jog(jog_axis, jog_dir * jog_speed(), now);
        if (!r) {
            jog_axis = -1;
            result(r);
        }
    }

    void build_jog(lv_obj_t* p) {
        const RigConfig& cfg = rig.config();
        lv_obj_t* xy = card(p, 16, 14, 404, 432);
        text(xy, 16, 12, "Left, right, toward, away", &leota_inter_20_bold);
        jog_button(xy, 142, 44, 120, 52, LV_SYMBOL_UP, 1, +1, "jog.away");
        jog_map.build(xy, 86, 104, 232, cfg);
        jog_button(xy, 20, 160, 56, 120, LV_SYMBOL_LEFT, 0, -1, "jog.left");
        jog_button(xy, 328, 160, 56, 120, LV_SYMBOL_RIGHT, 0, +1, "jog.right");
        jog_button(xy, 142, 344, 120, 52, LV_SYMBOL_DOWN, 1, -1, "jog.toward");

        lv_obj_t* zc = card(p, 432, 14, 160, 432);
        text(zc, 16, 12, "Height", &leota_inter_20_bold);
        jog_button(zc, 14, 44, 132, 60, "Up", 2, +1, "jog.up");
        jog_z = bar(zc, 34, 116, 40, 232, 1000);
        lv_obj_set_style_radius(jog_z, 8, 0);
        lv_obj_set_style_radius(jog_z, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(jog_z, hex(kBlueBg), LV_PART_INDICATOR);
        text(zc, 84, 116, mm(cfg.axes[2].max).c_str(), &leota_inter_14, kMuted);
        text(zc, 84, 330, mm(cfg.axes[2].min).c_str(), &leota_inter_14, kMuted);
        jog_button(zc, 14, 360, 132, 60, "Down", 2, -1, "jog.down");

        lv_obj_t* rc = card(p, 604, 14, 404, 432);
        const char* names[3] = {"X mm", "Y mm", "Z mm"};
        for (int a = 0; a < 3; ++a) {
            lv_obj_t* b = box(rc, 14 + a * 128, 14, 120, 66, kDark, 10);
            text(b, 10, 8, names[a], &leota_inter_14, kMuted);
            jog_read[a] = text(b, 10, 28, "", &leota_inter_20_bold);
        }
        text(rc, 14, 96, "Jog speed", &leota_inter_14, kMuted);
        const char* sp[3] = {"Slow", "Medium", "Fast"};
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* b = button(rc, 14 + i * 128, 118, 120, 52, sp[i]);
            speed_buttons.push_back(b);
            on_click(b, [this, i] { speed = i; });
        }
        lv_obj_t* dock = button(rc, 14, 186, 184, 56, "Go to dock");
        name("jog.dock", dock);
        on_click(dock, [this] { result(rig.park(now)); });
        lv_obj_t* service = button(rc, 206, 186, 184, 56, "Service position");
        on_click(service, [this] {
            const RigConfig& c = rig.config();
            const double y = c.two_axis ? c.fixed_y : std::max(c.axes[1].min, -400.0);
            const double target[3] = {0.0, y, std::clamp(1100.0, c.axes[2].min, c.axes[2].max)};
            for (int a = 0; a < 3; ++a) {
                if (!rig.axis_present(a)) continue;
                const Result r = rig.move(a, target[a], std::nullopt, 3.0, std::nullopt, now);
                if (!r) { result(r); break; }
            }
        });
        jog_note = text(rc, 14, 262, "", &leota_inter_14, kMuted, 376);
        text(rc, 14, 330, "Hold an arrow to move; let go to stop. Moves stay inside the workspace and off the table. Jog works only with remote control off.",
             &leota_inter_14, kMuted, 376);
    }

    void refresh_jog() {
        const RigConfig& cfg = rig.config();
        const Vec3 p = rig.position();
        jog_map.place(p, cfg);
        const AxisLimits& z = cfg.axes[2];
        bar_value(jog_z, static_cast<int>(std::clamp((p.z - z.min) / (z.max - z.min), 0.0, 1.0) * 1000));
        set(jog_read[0], rig.homed() ? signed_mm(p.x) : "\xE2\x80\x94");
        set(jog_read[1], rig.homed() ? signed_mm(p.y) : "\xE2\x80\x94");
        set(jog_read[2], rig.homed() ? mm(p.z) : "\xE2\x80\x94");
        for (int i = 0; i < 3; ++i) {
            set_color(speed_buttons[i], i == speed ? kBlueBg : kRaised);
            lv_obj_set_style_border_color(speed_buttons[i], hex(i == speed ? kBlue : kEdge), 0);
        }
        const bool can = !rig.remote() && rig.armed() && rig.homed();
        for (lv_obj_t* b : jog_buttons) enable(b, can);
        set(jog_note, rig.remote() ? "Remote control is on. Turn it off on the Controls screen to jog here."
                      : !rig.armed() ? "Arm the rig to jog."
                      : !rig.homed() ? "Home at the dock to jog."
                                     : fmt("Ready. %s: %.0f mm/s.", speed == 0 ? "Slow" : speed == 1 ? "Medium" : "Fast", jog_speed()));
        set_text_color(jog_note, can ? kGreenInk : kAmberSoft);
        if (cfg.two_axis) {
            enable(named["jog.away"], false);
            enable(named["jog.toward"], false);
        }
    }

    // -- Weather -------------------------------------------------------------------------
    void build_weather(lv_obj_t* p) {
        lv_obj_t* left = card(p, 16, 14, 600, 432);
        text(left, 16, 12, "Wind at the gazebo opening", &leota_inter_20_bold);
        w_chip = box(left, 344, 10, 240, 36, kGreenBg, 10);
        w_chip_dot = dot(w_chip, 12, 13, 10, kGreen);
        w_chip_text = text(w_chip, 30, 8, "", &leota_inter_16_semi, kGreenInk);
        const char* keys[3] = {"Now", "Gust, 10 min", "Average, 2 min"};
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* b = box(left, 16 + i * 192, 56, 184, 64, kDark, 10);
            text(b, 12, 8, keys[i], &leota_inter_14, kMuted);
            w_read[i] = text(b, 12, 28, "", &leota_inter_20_bold);
        }
        const int cx = 16, cy = 134, cw = 500;
        lv_obj_t* chart = box(left, cx, cy, cw, chart_h, kDark, 8, kChipEdge);
        const int bw = (cw - 12 - 29 * 3) / 30;
        for (int i = 0; i < 30; ++i) {
            w_bars[i] = box(chart, 6 + i * (bw + 3), chart_h - 2, bw, 1, kBlue, 2);
        }
        const RigConfig& cfg = rig.config();
        const double levels[2] = {cfg.wind.amber, cfg.wind.red};
        const uint32_t cols[2] = {kAmber, 0xE5484D};
        for (int k = 0; k < 2; ++k) {
            const float y = static_cast<float>(chart_h - levels[k] / chart_top * chart_h);
            thresh_pts[k] = {pt(0, y), pt(cw, y)};
            lv_obj_t* l = lv_line_create(chart);
            lv_line_set_points(l, thresh_pts[k].data(), 2);
            lv_obj_set_style_line_color(l, hex(cols[k]), 0);
            lv_obj_set_style_line_width(l, 2, 0);
            lv_obj_set_style_line_dash_width(l, 6, 0);
            lv_obj_set_style_line_dash_gap(l, 4, 0);
            text(left, cx + cw + 8, cy + static_cast<int>(y) - 9,
                 fmt("%s %.0f", k ? "Red" : "Amber", levels[k]).c_str(), &leota_inter_14, k ? kRedSoft : kAmberSoft);
        }
        text(left, cx + cw + 8, cy + chart_h - 18, "0 m/s", &leota_inter_14, kMuted);
        text(left, 16, 292, "Strongest wind in each of the last 30 minutes, oldest on the left.", &leota_inter_14, kMuted, 568);
        lv_obj_t* rule = box(left, 16, 322, 568, 1, kLine);
        (void)rule;
        text(left, 16, 336, "Line swing, 2 s", &leota_inter_14, kMuted);
        w_swing = text(left, 200, 334, "", &leota_inter_16_semi);
        text(left, 16, 370, "Sensor", &leota_inter_14, kMuted);
        w_sensor = text(left, 200, 368, "", &leota_inter_16_semi);

        lv_obj_t* lv = card(p, 628, 14, 380, 236);
        text(lv, 16, 12, "Levels", &leota_inter_20_bold);
        const char* nm[3] = {"Green", "Amber", "Red"};
        const uint32_t dc[3] = {kGreen, kAmber, 0xE5484D};
        const std::string when[3] = {fmt("gust under %.0f m/s", cfg.wind.amber),
                                     fmt("gust %.0f\xE2\x80\x93%.0f m/s, or lines swing", cfg.wind.amber, cfg.wind.red),
                                     fmt("gust over %.0f m/s, or sensor lost", cfg.wind.red)};
        const char* does[3] = {"Full workspace and speed.", "Half speed. Low, central workspace.",
                               "Parks, disarms, won't arm. Also on a big swing."};
        for (int i = 0; i < 3; ++i) {
            level_rows[i] = box(lv, 12, 44 + i * 62, 356, 56, kDark, 10);
            lv_obj_set_style_border_width(level_rows[i], 2, 0);
            lv_obj_set_style_border_color(level_rows[i], hex(kDark), 0);
            dot(level_rows[i], 10, 12, 12, dc[i]);
            text(level_rows[i], 30, 6, (std::string(nm[i]) + "  ").c_str(), &leota_inter_16_semi);
            text(level_rows[i], 92, 8, when[i].c_str(), &leota_inter_14, kSoft, 256);
            text(level_rows[i], 30, 30, does[i], &leota_inter_14, kMuted);
        }

        lv_obj_t* lk = card(p, 628, 262, 380, 184);
        text(lk, 16, 12, "Lockout", &leota_inter_20_bold);
        lock_text = text(lk, 16, 44, "", &leota_inter_16_semi, kInk, 348);
        lock_bar = bar(lk, 16, 106, 348, 12, 1000);
        lv_obj_set_style_bg_color(lock_bar, hex(kGreen), LV_PART_INDICATOR);
        lock_note = text(lk, 16, 126, "", &leota_inter_14, kMuted, 348);
    }

    void sample_wind() {
        if (now < next_sample) return;
        next_sample = now + 1.0;
        if (minute_start < 0) minute_start = now;
        if (now - minute_start >= 60.0) {
            minute_start = now;
            minute_max.pop_front();
            minute_max.push_back(0.0);
        }
        minute_max.back() = std::max(minute_max.back(), rig.wind().speed());
    }

    void refresh_weather() {
        const RigConfig& cfg = rig.config();
        const WindMonitor& w = rig.wind();
        level_chip(w_chip, w_chip_dot, w_chip_text);
        set(w_read[0], fmt("%.1f m/s", w.speed()));
        set(w_read[1], fmt("%.1f m/s", w.gust()));
        set(w_read[2], fmt("%.1f m/s", w.average()));
        for (int i = 0; i < 30; ++i) {
            const double v = minute_max[i];
            const int h = std::max(1, static_cast<int>(std::min(v, chart_top) / chart_top * (chart_h - 4)));
            if (lv_obj_get_height(w_bars[i]) != h) {
                lv_obj_set_height(w_bars[i], h);
                lv_obj_set_y(w_bars[i], chart_h - 2 - h);
            }
            set_color(w_bars[i], v >= cfg.wind.red ? 0xE5484D : v >= cfg.wind.amber ? kAmber : kBlue);
        }
        std::string swing;
        for (const LineStatus& l : rig.lines()) swing += fmt("%c \xC2\xB1%.1f  ", l.name, l.swing);
        set(w_swing, swing + fmt("(alarm \xC2\xB1%.0f N)", cfg.wind.swing_red));
        set(w_sensor, !cfg.wind.enabled ? "None fitted" : !w.ever_read() ? "Waiting for the first reading"
                      : w.sensor_ok() ? "Reading" : "Not answering: treated as Red");
        set_text_color(w_sensor, w.sensor_ok() || !cfg.wind.enabled ? kGreenInk : kRedSoft);
        const int lvl = static_cast<int>(w.level());
        const uint32_t dc[3] = {kGreen, kAmber, 0xE5484D};
        for (int i = 0; i < 3; ++i) {
            lv_obj_set_style_border_color(level_rows[i], hex(i == lvl ? dc[i] : kDark), 0);
            set_color(level_rows[i], i == lvl ? kChip : kDark);
        }
        if (w.holding()) {
            set(lock_text, fmt("Arming refused until the wind has been Green for %.0f minutes.", cfg.wind.hold / 60));
            set_text_color(lock_text, kRedSoft);
            const double done = std::clamp(1.0 - w.calm_left() / cfg.wind.hold, 0.0, 1.0);
            bar_value(lock_bar, static_cast<int>(done * 1000));
            show(lock_bar, true);
            set(lock_note, fmt("Calm for %.0f of %.0f minutes. The count restarts if a gust passes %.0f m/s. A person must arm it again.",
                               (cfg.wind.hold - w.calm_left()) / 60, cfg.wind.hold / 60, cfg.wind.amber));
        } else {
            show(lock_bar, false);
            set(lock_text, w.level() == WindLevel::Amber ? "No lockout. The rig flies at half speed, low and central."
                           : w.level() == WindLevel::Red ? "Red: waiting for the wind sensor."
                                                         : "No lockout. Arming is allowed.");
            set_text_color(lock_text, w.level() == WindLevel::Green ? kGreenInk : kAmberSoft);
            set(lock_note, "At Red the ball parks on the dock and disarms.");
        }
    }

    // -- Diagnostics ---------------------------------------------------------------------
    lv_obj_t* kv_card(lv_obj_t* p, int x, const char* title, const char* const keys[], int n, std::array<lv_obj_t*, 6>& values) {
        lv_obj_t* c = card(p, x, 240, 322, 206);
        text(c, 16, 10, title, &leota_inter_20_bold);
        for (int i = 0; i < n; ++i) {
            text(c, 16, 42 + i * 26, keys[i], &leota_inter_14, kMuted);
            values[i] = text(c, 150, 41 + i * 26, "", &leota_inter_16_semi);
            lv_obj_set_width(values[i], 156);
            lv_obj_set_style_text_align(values[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
        }
        return c;
    }

    void build_diagnostics(lv_obj_t* p) {
        lv_obj_t* w = card(p, 16, 14, 992, 216);
        text(w, 16, 10, "Winches", &leota_inter_20_bold);
        table = lv_table_create(w);
        lv_obj_set_pos(table, 8, 36);
        lv_obj_set_size(table, 976, 176);
        lv_table_set_column_count(table, 7);
        lv_table_set_row_count(table, 5);
        const int widths[7] = {150, 130, 120, 130, 130, 160, 156};
        const char* head[7] = {"Winch", "Line out", "Tension", "Model says", "Swing, 2 s", "Trim", "Drive"};
        for (int c = 0; c < 7; ++c) {
            lv_table_set_column_width(table, c, widths[c]);
            lv_table_set_cell_value(table, 0, c, head[c]);
        }
        lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(table, 0, 0);
        lv_obj_set_style_pad_all(table, 0, 0);
        lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_text_color(table, hex(kInk), LV_PART_ITEMS);
        lv_obj_set_style_text_font(table, &leota_inter_16, LV_PART_ITEMS);
        lv_obj_set_style_pad_top(table, 3, LV_PART_ITEMS);
        lv_obj_set_style_pad_bottom(table, 3, LV_PART_ITEMS);
        lv_obj_set_style_pad_left(table, 8, LV_PART_ITEMS);
        lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_side(table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
        lv_obj_set_style_border_color(table, hex(kLine), LV_PART_ITEMS);
        lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE);

        static const char* const safety[6] = {"E-stop chain", "Drive power", "Brakes", "Remote control", "State", "Faults"};
        static const char* const sensor[6] = {"Now", "Gust, 10 min", "Average, 2 min", "Level", "Readings", "Hold left"};
        static const char* const net[6] = {"This controller", "SCMP port", "Show Controller", "Last heard", "Commands", "Link"};
        kv_card(p, 16, "Safety", safety, 6, safety_value);
        kv_card(p, 351, "Wind sensor", sensor, 6, sensor_value);
        kv_card(p, 686, "Network", net, 6, net_value);
    }

    void refresh_diagnostics() {
        const auto ls = rig.lines();
        const char* where[4] = {"back left", "back right", "front left", "front right"};
        for (int i = 0; i < kLines; ++i) {
            const LineStatus& l = ls[i];
            const std::string cells[7] = {fmt("%c  %s", l.name, where[i]),
                                          l.length > 0 ? fmt("%.3f m", l.length / 1000) : std::string("\xE2\x80\x94"),
                                          fmt("%.1f N", l.tension),
                                          rig.homed() ? fmt("%.1f N", l.expected) : std::string("\xE2\x80\x94"),
                                          fmt("\xC2\xB1%.1f N", l.swing),
                                          fmt("%+.2f mm", l.trim),
                                          l.alarm ? "ALARM" : l.ready ? "Ready" : "Off"};
            for (int c = 0; c < 7; ++c) {
                const char* old = lv_table_get_cell_value(table, i + 1, c);
                if (!old || cells[c] != old) lv_table_set_cell_value(table, i + 1, c, cells[c].c_str());
            }
        }
        static const char* states[] = {"Disarmed", "Arming", "Armed, idle", "Homing", "Moving", "Stopping", "Fault", "E-stop"};
        const std::string sv[6] = {rig.estop() ? "OPEN" : "Closed", rig.drives_on() ? "On" : "Off",
                                   rig.brakes_released() ? "Released" : "Holding", rig.remote() ? "On" : "Off",
                                   states[static_cast<int>(rig.state())], fault_names().empty() ? "None" : fault_names()};
        for (int i = 0; i < 6; ++i) set(safety_value[i], sv[i]);
        set_text_color(safety_value[0], rig.estop() ? kRedSoft : kGreenInk);
        set_text_color(safety_value[5], rig.faults() ? kRedSoft : kGreenInk);
        const WindMonitor& w = rig.wind();
        const bool fitted = rig.config().wind.enabled;
        const std::string wv[6] = {fmt("%.1f m/s", w.speed()), fmt("%.1f m/s", w.gust()), fmt("%.1f m/s", w.average()),
                                   !fitted ? "No sensor" : w.level() == WindLevel::Green ? "Green"
                                   : w.level() == WindLevel::Amber ? "Amber" : "Red",
                                   !fitted ? "\xE2\x80\x94" : w.sensor_ok() ? "Coming in" : "Missing",
                                   w.holding() ? fmt("%.0f min", std::ceil(w.calm_left() / 60)) : std::string("None")};
        for (int i = 0; i < 6; ++i) set(sensor_value[i], wv[i]);
        set_text_color(sensor_value[4], w.sensor_ok() || !fitted ? kGreenInk : kRedSoft);
        const std::string nv[6] = {link.own_address.empty() ? "\xE2\x80\x94" : link.own_address, fmt("UDP %d", link.scmp_port),
                                   link.address.empty() ? "Not seen yet" : link.address,
                                   link.heard < 0 ? "Never" : link.heard < 1 ? fmt("%.0f ms ago", link.heard * 1000) : fmt("%.1f s ago", link.heard),
                                   std::to_string(link.commands), link.connected ? "Connected" : "Not connected"};
        for (int i = 0; i < 6; ++i) set(net_value[i], nv[i]);
        set_text_color(net_value[5], link.connected ? kGreenInk : kAmberSoft);
    }

    // -- Setup ---------------------------------------------------------------------------
    void build_setup(lv_obj_t* p) {
        locked = box(p, 0, 0, kW, kBody, kBg);
        text(locked, 120, 120, "Setup is locked", &leota_inter_28_bold);
        text(locked, 120, 168, "These settings change how the rig flies and when it stops. Enter the PIN to see them.",
             &leota_inter_16, kMuted, 360);
        pin_dots = text(locked, 120, 250, "", &leota_inter_28_bold, kBlue);
        const char* keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0", "Back"};
        for (int i = 0; i < 12; ++i) {
            lv_obj_t* b = button(locked, 560 + (i % 3) * 112, 36 + (i / 3) * 96, 100, 84, keys[i], Kind::Plain,
                                 i == 9 || i == 11 ? &leota_inter_16_semi : &leota_inter_28_bold);
            name(std::string("setup.key.") + keys[i], b);
            on_click(b, [this, i] { key(i); });
        }
        unlocked = box(p, 0, 0, kW, kBody, kBg);
        show(unlocked, false);
    }

    void key(int i) {
        static const char digits[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'C', '0', 'B'};
        const char k = digits[i];
        if (k == 'C') entered.clear();
        else if (k == 'B') { if (!entered.empty()) entered.pop_back(); }
        else if (entered.size() < pin.size()) entered += k;
        if (entered.size() == pin.size()) {
            if (entered == pin) {
                fill_settings();
                show(locked, false);
                show(unlocked, true);
            } else {
                say("That PIN isn't right.");
            }
            entered.clear();
        }
        std::string dots;
        for (size_t n = 0; n < pin.size(); ++n) dots += n < entered.size() ? "\xE2\x80\xA2 " : "\xC2\xB7 ";
        set(pin_dots, dots);
    }

    void fill_settings() {
        lv_obj_clean(unlocked);
        const RigConfig& c = rig.config();
        text(unlocked, 16, 18, (std::string("Flight mode: ") + (c.two_axis ? "2 axes, depth fixed" : "3 axes")).c_str(),
             &leota_inter_20_bold);
        text(unlocked, 16, 48, "Changing settings here comes with settings stored in flash. For now they come from the config.",
             &leota_inter_14, kMuted, 700);
        lv_obj_t* lock = button(unlocked, 868, 14, 140, 52, "Lock");
        name("setup.lock", lock);
        on_click(lock, [this] {
            show(unlocked, false);
            show(locked, true);
        });
        auto range = [](const AxisLimits& a) { return mm(a.min) + " to " + mm(a.max) + " mm"; };
        const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> groups = {
            {"Flight", {{"Workspace x", range(c.axes[0])}, {"Workspace y", c.two_axis ? "fixed " + mm(c.fixed_y) : range(c.axes[1])},
                        {"Workspace z", range(c.axes[2])}, {"Top speed", fmt("%.0f mm/s", c.axes[2].max_velocity)},
                        {"Acceleration", fmt("%.0f mm/s\xC2\xB2", c.axes[2].max_accel)}, {"Smoothing", fmt("%.2f s", c.smoothing)}}},
            {"Lines and safety", {{"Slack below", fmt("%.0f N", c.tension_min)}, {"Overload above", fmt("%.0f N", c.tension_max)},
                                  {"Plans keep lines over", fmt("%.0f N", c.plan_tension_min)},
                                  {"Mismatch fault", fmt("%.0f N for %.1f s", c.mismatch_limit, c.mismatch_time)},
                                  {"Heartbeat timeout", fmt("%.1f s", c.heartbeat_timeout)}, {"Head weight", fmt("%.1f kg", c.mass)}}},
            {"Wind", {{"Amber at gust", fmt("%.1f m/s", c.wind.amber)}, {"Red at gust", fmt("%.1f m/s", c.wind.red)},
                      {"Green again after", fmt("%.0f min", c.wind.hold / 60)}, {"Swing alarm", fmt("\xC2\xB1%.1f N", c.wind.swing_red)},
                      {"Sensor missing after", fmt("%.0f s", c.wind.sensor_timeout)}, {"Amber speed", fmt("%.0f%%", c.wind.amber_speed * 100)}}}};
        for (size_t g = 0; g < groups.size(); ++g) {
            lv_obj_t* col = card(unlocked, 16 + static_cast<int>(g) * 335, 80, 322, 366);
            text(col, 14, 10, groups[g].first.c_str(), &leota_inter_20_bold);
            for (size_t r = 0; r < groups[g].second.size(); ++r) {
                lv_obj_t* row = box(col, 10, 44 + static_cast<int>(r) * 52, 302, 46, kChip, 8);
                text(row, 10, 14, groups[g].second[r].first.c_str(), &leota_inter_14, kMuted);
                lv_obj_t* v = text(row, 140, 13, groups[g].second[r].second.c_str(), &leota_inter_16_semi);
                lv_obj_set_width(v, 152);
                lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            }
        }
    }

    // -- Log -----------------------------------------------------------------------------
    void build_log(lv_obj_t* p) {
        const char* names[6] = {"All", "Safety", "Motion", "Wind", "Network", "Show"};
        for (int i = 0; i < 6; ++i) {
            lv_obj_t* b = button(p, 16 + i * 112, 14, 104, 44, names[i]);
            lv_obj_set_style_radius(b, 22, 0);
            filter_buttons.push_back(b);
            on_click(b, [this, i] {
                filter = i - 1;
                log_dirty = true;
            });
        }
        log_list = card(p, 16, 70, 992, 376);
        lv_obj_add_flag(log_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(log_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(log_list, 8, 0);
        lv_obj_set_style_pad_row(log_list, 0, 0);
    }

    void refresh_log() {
        for (int i = 0; i < 6; ++i) {
            set_color(filter_buttons[i], i - 1 == filter ? kBlueBg : kChip);
            lv_obj_set_style_border_color(filter_buttons[i], hex(i - 1 == filter ? kBlue : kChipEdge), 0);
        }
        if (!log_dirty) return;
        log_dirty = false;
        lv_obj_clean(log_list);
        static const char* kinds[5] = {"Safety", "Motion", "Wind", "Network", "Show"};
        static const uint32_t tag_bg[5] = {kRedBg, kBlueBg, kGreenBg, kGrey, kAmberBg};
        static const uint32_t tag_fg[5] = {kRedSoft, 0xBCD8FF, kGreenInk, 0xD5DCE6, kAmberSoft};
        int n = 0;
        for (const Entry& e : entries) {
            const int k = static_cast<int>(e.kind);
            if (filter >= 0 && k != filter) continue;
            lv_obj_t* row = box(log_list, 0, 0, 972, 40, kCard);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, hex(0x1E2530), 0);
            text(row, 8, 11, e.clock.c_str(), &leota_inter_14, kMuted);
            lv_obj_t* tag = box(row, 90, 8, 84, 24, tag_bg[k], 12);
            lv_obj_t* tl = text(tag, 0, 3, kinds[k], &leota_inter_14, tag_fg[k]);
            lv_obj_set_width(tl, 84);
            lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t* msg = text(row, 188, 10, e.text.c_str(), &leota_inter_16);
            lv_obj_set_width(msg, 776);
            lv_label_set_long_mode(msg, LV_LABEL_LONG_DOT);
            if (++n >= 40) break;
        }
        if (n == 0) text(log_list, 0, 0, "Nothing yet.", &leota_inter_16, kMuted);
    }

    // -- E-stop --------------------------------------------------------------------------
    void build_estop(lv_obj_t* p) {
        lv_obj_t* l = box(p, 16, 14, 540, 432, 0x1E1012, 14);
        lv_obj_set_style_border_width(l, 2, 0);
        lv_obj_set_style_border_color(l, hex(kRed), 0);
        text(l, 20, 20, LV_SYMBOL_WARNING "  Emergency stop", &leota_inter_28_bold);
        text(l, 20, 72, "The E-stop chain is open. Motor power is off and the brakes are holding the ball where it stopped.",
             &leota_inter_16, 0xF2D6D7, 500);
        text(l, 20, 150, "Show Controller has been told. Every axis reports the estop fault.", &leota_inter_14, 0xC9A9AB, 500);
        lv_obj_t* r = card(p, 568, 14, 440, 432);
        text(r, 20, 18, "To recover", &leota_inter_20_bold);
        text(r, 20, 56,
             "1. Check that the area under the ball is clear and the lines are free.\n\n"
             "2. Twist the pressed E-stop button to release it.\n\n"
             "3. Press the blue Reset button on the controller. This screen can't reset the chain.\n\n"
             "4. Arm again from Controls. If the ball moved while stopped, home first.",
             &leota_inter_16, kInk, 400);
        lv_obj_t* go_controls = button(r, 20, 356, 400, 56, "Go to Controls");
        on_click(go_controls, [this] { go(1); });
        show(p, false);
    }

    // -- top bar and state ---------------------------------------------------------------
    void refresh_top() {
        const RigState s = rig.state();
        std::string label = "DISARMED";
        uint32_t bg = kGrey, fg = kInk;
        switch (s) {
            case RigState::Disarmed: break;
            case RigState::Arming: label = "ARMING"; bg = kAmber; fg = kAmberInk; break;
            case RigState::Idle: label = "ARMED"; bg = kAmber; fg = kAmberInk; break;
            case RigState::Homing: label = "HOMING"; bg = kAmber; fg = kAmberInk; break;
            case RigState::Moving: label = rig.parking() ? "PARKING" : "MOVING"; bg = kAmber; fg = kAmberInk; break;
            case RigState::Stopping: label = "STOPPING"; bg = kAmber; fg = kAmberInk; break;
            case RigState::Fault: label = "FAULT"; bg = kRed; fg = 0xFFFFFF; break;
            case RigState::EStop: label = "E-STOP"; bg = kRed; fg = 0xFFFFFF; break;
        }
        set(badge_text, label);
        set_color(badge, bg);
        set_text_color(badge_text, fg);
        set(subtitle, rig.message());
        set(remote_chip.label, rig.remote() ? "Remote on" : "Local only");
        set_color(remote_chip.dot, rig.remote() ? kBlue : kOff);
        set(link_chip.label, link.connected ? (link.heard >= 0 ? fmt("Show link %.0f ms", link.heard * 1000) : "Show link")
                                            : "No Show Controller");
        set_color(link_chip.dot, link.connected ? kGreen : kOff);
        const WindMonitor& w = rig.wind();
        const bool fitted = rig.config().wind.enabled;
        set(wind_chip.label, !fitted ? "No wind sensor" : w.level() == WindLevel::Green ? "Wind green"
                             : w.level() == WindLevel::Amber ? "Wind amber" : "Wind red");
        set_color(wind_chip.dot, !fitted ? kOff : w.level() == WindLevel::Green ? kGreen
                                 : w.level() == WindLevel::Amber ? kAmber : 0xE5484D);
        set(clock_label, clock);
    }

    void update(double t, const UiLink& l, const std::string& c) {
        now = t;
        link = l;
        clock = c;
        if (rig.message() != last_message) {
            last_message = rig.message();
            const bool safety = rig.faults() || rig.estop() || rig.state() == RigState::Arming ||
                                rig.state() == RigState::Disarmed || rig.state() == RigState::Fault;
            const bool windy = last_message.find("wind") != std::string::npos || last_message.find("Wind") != std::string::npos;
            add_log(windy ? LogKind::Wind : safety ? LogKind::Safety : LogKind::Motion, last_message);
        }
        if (jog_axis >= 0 && now - jog_last >= 0.1) send_jog();
        sample_wind();
        const bool estop = rig.estop();
        show(estop_page, estop);
        if (estop) lv_obj_move_foreground(estop_page);
        refresh_top();
        if (!estop) {
            switch (page) {
                case 0: refresh_status(); break;
                case 1: refresh_controls(); break;
                case 2: refresh_jog(); break;
                case 3: refresh_weather(); break;
                case 4: refresh_diagnostics(); break;
                case 6: refresh_log(); break;
                default: break;
            }
        }
        if (toast_until > 0 && now > toast_until) {
            toast_until = 0;
            show(toast, false);
        }
    }
};

Ui::Ui(Rig& rig, lv_display_t* display, std::string pin) : d_(new Impl(rig, display, std::move(pin))) {
    d_->build();
}

Ui::~Ui() = default;

void Ui::update(double now, const UiLink& link, const std::string& clock) { d_->update(now, link, clock); }

void Ui::log(LogKind kind, const std::string& text) { d_->add_log(kind, text); }

lv_obj_t* Ui::find(const std::string& name) const {
    auto it = d_->named.find(name);
    return it == d_->named.end() ? nullptr : it->second;
}

std::string Ui::page() const { return d_->page_names[d_->page]; }

}  // namespace leota
