// The touchscreen. Every screen is built once; update() refreshes the visible one and
// the bars around it. Positions are fixed for the 1024 × 600 panel: the top bar is 64 px,
// the tab bar 76 px, and each screen gets the 460 px between.

#include "leota/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "leota/calibration.hpp"
#include "leota/line_test.hpp"

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

// A number with a real minus sign: "−1.80".
std::string num(double v, int decimals) {
    char buf[40];
    const double k = std::pow(10.0, decimals);
    const double r = std::round(v * k) / k;
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(r));
    return (r < 0 ? std::string(kMinus) : std::string()) + buf;
}

std::string lower_first(std::string s) {
    if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z' && !(s.size() > 1 && s[1] >= 'A' && s[1] <= 'Z')) s[0] = static_cast<char>(s[0] - 'A' + 'a');
    return s;
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
    lv_obj_set_style_text_color(b, hex(fg), 0);  // the label takes its colour from the button
    // Disabled: dark, outlined, grey text, whatever the kind. (Without the filter setting,
    // the theme washes it pale grey, which looks more pressable, not less.)
    lv_obj_set_style_color_filter_opa(b, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(b, hex(0x10151C), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(b, 1, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, hex(kChipEdge), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, hex(0x5E6878), LV_STATE_DISABLED);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
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
    std::array<lv_obj_t*, 6> sys_value{};

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
    SettingsKeeper* keeper = nullptr;
    std::unique_ptr<SettingsKeeper> own_keeper;
    Settings draft, base;  // being edited; and as it was when editing began
    int seen_revision = -1;
    lv_obj_t *locked = nullptr, *unlocked = nullptr, *pin_dots = nullptr;
    std::string entered;
    int group = 0;
    std::vector<lv_obj_t*> group_buttons;
    lv_obj_t *set_title = nullptr, *set_status = nullptr, *set_save = nullptr, *set_undo = nullptr, *set_rows = nullptr;
    std::vector<std::unique_ptr<std::pair<Impl*, std::function<void()>>>> row_actions;

    // The number pad
    enum class EditMode { Field, PinNew, PinAgain };
    lv_obj_t *editor = nullptr, *ed_title = nullptr, *ed_help = nullptr, *ed_value = nullptr, *ed_note = nullptr;
    std::map<std::string, lv_obj_t*> ed_keys;
    const Field* ed_field = nullptr;
    EditMode ed_mode = EditMode::Field;
    std::string ed_text, ed_pin;
    bool ed_fresh = true;
    std::function<void(double)> ed_done;  // for a value that isn't a setting
    Field ed_temp{};
    std::string ed_temp_label, ed_temp_help, ed_temp_unit;

    // The calibration check
    CalibrationRun cal;
    CalibrationRun::Phase cal_seen = CalibrationRun::Phase::Off;
    lv_obj_t *cal_dialog = nullptr, *cal_ready = nullptr, *cal_ready_dot = nullptr, *cal_bar = nullptr,
             *cal_msg = nullptr, *cal_start = nullptr, *cal_stop = nullptr, *cal_use = nullptr, *cal_close = nullptr,
             *cal_summary = nullptr;
    std::array<lv_obj_t*, 5> cal_found{}, cal_off{};
    std::array<lv_obj_t*, kLines> sensor_now{};

    // The line test
    LineTest lt;
    lv_obj_t *lt_dialog = nullptr, *lt_title = nullptr, *lt_instr = nullptr, *lt_msg = nullptr, *lt_weight = nullptr,
             *lt_primary = nullptr, *lt_skip = nullptr, *lt_close = nullptr, *lt_live = nullptr, *lt_live_note = nullptr;
    std::array<lv_obj_t*, 3> lt_dot{}, lt_step{}, lt_before{}, lt_found{};

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

    Impl(Rig& r, lv_display_t* d, SettingsKeeper* k) : rig(r), disp(d), keeper(k) {
        if (!keeper) {
            own_keeper = std::make_unique<SettingsKeeper>(settings_for(rig.config()));
            keeper = own_keeper.get();
        }
        draft = base = keeper->settings();
        seen_revision = keeper->revision();
    }
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
    // Every touch handler runs under the host's lock (see Ui::set_lock): on the controller,
    // LVGL calls them from its own task while the motion task runs the rig.
    std::function<void()> lock_fn, unlock_fn;
    static void dispatch(lv_event_t* e) {
        auto* a = static_cast<std::pair<Impl*, Action>*>(lv_event_get_user_data(e));
        Impl* self = a->first;
        if (self->lock_fn) self->lock_fn();
        a->second();
        if (self->unlock_fn) self->unlock_fn();
    }
    std::vector<std::unique_ptr<std::pair<Impl*, Action>>> actions;
    void on(lv_obj_t* o, lv_event_code_t code, Action a) {
        actions.push_back(std::make_unique<std::pair<Impl*, Action>>(this, std::move(a)));
        lv_obj_add_event_cb(o, dispatch, code, actions.back().get());
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
        const char* keys[6] = {"E-stop chain", "Drives", "Brakes", "Homed", "Faults", "Dock"};
        for (int i = 0; i < 6; ++i) {
            text(sys, 16, 47 + i * 30, keys[i], &leota_inter_14, kMuted);
            sys_value[i] = text(sys, 110, 46 + i * 30, "", &leota_inter_16_semi);
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
        const DockCharger& dock = rig.dock();
        std::string d = "No charger";
        uint32_t dc = kMuted;
        if (dock.fitted()) {
            switch (dock.state()) {
                case DockState::Charging: d = fmt("Charging %.0f W", dock.watts()); dc = kGreenInk; break;
                case DockState::Charged: d = "Charged"; dc = kGreenInk; break;
                case DockState::Off: d = "Off"; dc = kInk; break;
                case DockState::Empty: d = "Empty"; dc = kMuted; break;
                case DockState::Fault: d = "FAULT"; dc = kRedSoft; break;
            }
        }
        set(sys_value[5], d);
        set_text_color(sys_value[5], dc);
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
        set_text_color(arm_btn, armed ? 0xFFFFFF : kAmberInk);
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
        lv_obj_set_style_bg_color(dialog_check, hex(kAmber), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(dialog_check, hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(dialog_check, hex(kAmberInk), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
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
    // Behind a PIN. Changes go into a draft; Save checks them, gives them to the rig and
    // stores them. Numbers are typed on a keypad; Measurements also has the calibration check.
    static constexpr int kGroups = 7;
    const char* group_names[kGroups] = {"Flight", "Lines and safety", "Wind", "Measurements", "Sensors", "Network", "PIN"};

    void build_setup(lv_obj_t* p) {
        locked = box(p, 0, 0, kW, kBody, kBg);
        text(locked, 120, 120, "Setup is locked", &leota_inter_28_bold);
        text(locked, 120, 168, "These settings change how the rig flies and when it stops. Enter the PIN to change them.",
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
        for (int g = 0; g < kGroups; ++g) {
            lv_obj_t* b = button(unlocked, 16, 14 + g * 54, 200, 48, group_names[g]);
            name("setup.group." + std::to_string(g), b);
            group_buttons.push_back(b);
            on_click(b, [this, g] {
                group = g;
                fill_group();
            });
        }
        lv_obj_t* lock = button(unlocked, 16, 398, 200, 48, "Lock");
        name("setup.lock", lock);
        on_click(lock, [this] {
            draft = keeper->settings();
            show(unlocked, false);
            show(locked, true);
        });
        lv_obj_t* c = card(unlocked, 232, 14, 776, 432);
        set_title = text(c, 20, 14, "", &leota_inter_20_bold);
        set_status = text(c, 20, 44, "", &leota_inter_14, kMuted);
        name("setup.status", set_status);
        lv_obj_set_width(set_status, 420);
        lv_label_set_long_mode(set_status, LV_LABEL_LONG_DOT);
        set_undo = button(c, 464, 12, 140, 48, "Undo changes");
        name("setup.undo", set_undo);
        on_click(set_undo, [this] {
            draft = keeper->settings();
            fill_group();
        });
        set_save = button(c, 616, 12, 140, 48, "Save", Kind::Amber);
        name("setup.save", set_save);
        on_click(set_save, [this] { save_settings(); });
        set_rows = box(c, 0, 70, 776, 360, kCard);
        show(unlocked, false);
        build_editor();
        build_calibration();
        build_line_test();
    }

    void key(int i) {
        static const char digits[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'C', '0', 'B'};
        const std::string& pin = keeper->settings().pin;
        const char k = digits[i];
        if (k == 'C') entered.clear();
        else if (k == 'B') { if (!entered.empty()) entered.pop_back(); }
        else if (entered.size() < pin.size()) entered += k;
        if (entered.size() == pin.size()) {
            if (entered == pin) {
                draft = base = keeper->settings();
                seen_revision = keeper->revision();
                show(locked, false);
                show(unlocked, true);
                fill_group();
            } else {
                say("That PIN isn't right.");
            }
            entered.clear();
        }
        std::string dots;
        for (size_t n = 0; n < pin.size(); ++n) dots += n < entered.size() ? "\xE2\x80\xA2 " : "\xC2\xB7 ";
        set(pin_dots, dots);
    }

    bool changed(const Field& f) const { return field_text(f, draft) != field_text(f, keeper->settings()); }

    // Rebuilds the open group from the draft.
    void fill_group() {
        for (auto it = named.begin(); it != named.end();) {
            const bool row = it->first.rfind("setup.field.", 0) == 0 || it->first == "setup.calibrate" ||
                             it->first.rfind("setup.test.", 0) == 0;
            it = row ? named.erase(it) : std::next(it);
        }
        lv_obj_clean(set_rows);
        row_actions.clear();  // their widgets are gone
        sensor_now = {};
        shown.clear();        // a new label may reuse an old one's address
        for (int g = 0; g < kGroups; ++g) {
            set_color(group_buttons[g], g == group ? kBlueBg : kRaised);
            lv_obj_set_style_border_color(group_buttons[g], hex(g == group ? kBlue : kEdge), 0);
        }
        set(set_title, group_names[group]);
        if (group == 3) build_measurements();
        else if (group == 4) build_sensors();
        else if (group == 6) build_pin();
        else build_list(group_names[group]);
        refresh_setup();
    }

    void click_row(lv_obj_t* o, Action a) {
        row_actions.push_back(std::make_unique<std::pair<Impl*, Action>>(this, std::move(a)));
        lv_obj_add_event_cb(o, dispatch, LV_EVENT_CLICKED, row_actions.back().get());
    }

    void tap_field(const Field& f) {
        if (f.kind == Field::Kind::Toggle) {
            f.set(draft, f.get(draft) != 0 ? 0.0 : 1.0);
            fill_group();
        } else {
            open_editor(&f);
        }
    }

    void build_list(const char* g) {
        lv_obj_t* list = box(set_rows, 0, 0, 776, 360, kCard);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_left(list, 20, 0);
        lv_obj_set_style_pad_bottom(list, 12, 0);
        lv_obj_set_style_pad_row(list, 6, 0);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
        lv_obj_set_style_pad_right(list, 4, LV_PART_SCROLLBAR);
        lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(list, hex(kEdge), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
        for (const Field& f : settings_fields()) {
            if (std::string(f.group) != g || (f.shown && !f.shown(draft))) continue;
            const bool ch = changed(f);
            lv_obj_t* r = box(list, 0, 0, 736, 50, kChip, 8, ch ? kAmber : kChipEdge);
            lv_obj_set_style_bg_color(r, hex(kRaised), LV_STATE_PRESSED);
            text(r, 14, 15, f.label, &leota_inter_16, kSoft);
            const bool toggle = f.kind == Field::Kind::Toggle;
            uint32_t col = ch ? kAmberSoft : kInk;
            if (toggle && !ch) col = f.get(draft) != 0 ? kGreenInk : kMuted;
            lv_obj_t* v = text(r, 330, 14, field_text(f, draft).c_str(), &leota_inter_16_semi, col);
            lv_obj_set_width(v, 340);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            text(r, 694, 15, toggle ? "" : LV_SYMBOL_RIGHT, &leota_inter_16_semi, kMuted);
            name(std::string("setup.field.") + f.key, r);
            click_row(r, [this, &f] { tap_field(f); });
        }
        if (std::string(g) == "Network")
            text(list, 0, 0, "Address changes take effect when the controller restarts.", &leota_inter_14, kMuted);
    }

    void build_measurements() {
        lv_obj_t* m = box(set_rows, 0, 0, 776, 360, kCard);
        const char* heads[3] = {"Across (x)", "Depth (y)", "Height (z)"};
        for (int c = 0; c < 3; ++c) {
            lv_obj_t* h = text(m, 190 + c * 162, 4, heads[c], &leota_inter_14, kMuted);
            lv_obj_set_width(h, 150);
            lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        }
        auto cell = [&](int x, int y, int w, const char* key) {
            const Field& f = *find_field(key);
            const bool ch = changed(f);
            lv_obj_t* b = box(m, x, y, w, 44, kChip, 8, ch ? kAmber : kChipEdge);
            lv_obj_set_style_bg_color(b, hex(kRaised), LV_STATE_PRESSED);
            const double v = f.get(draft);
            const std::string t = std::string(f.unit) == "mm" ? mm(v) : field_text(f, draft);
            lv_obj_t* l = text(b, 0, 12, t.c_str(), &leota_inter_16_semi, ch ? kAmberSoft : kInk);
            lv_obj_set_width(l, w);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            name(std::string("setup.field.") + key, b);
            click_row(b, [this, &f] { tap_field(f); });
        };
        const char* rows[5] = {"Pulley A", "Pulley B", "Pulley C", "Pulley D", "Dock (top)"};
        const char* keys[5][3] = {{"pulley.A.x", "pulley.A.y", "pulley.A.z"}, {"pulley.B.x", "pulley.B.y", "pulley.B.z"},
                                  {"pulley.C.x", "pulley.C.y", "pulley.C.z"}, {"pulley.D.x", "pulley.D.y", "pulley.D.z"},
                                  {"dock.x", "dock.y", "dock.height"}};
        for (int r = 0; r < 5; ++r) {
            text(m, 20, 38 + r * 52, rows[r], &leota_inter_16, kSoft);
            for (int c = 0; c < 3; ++c) cell(190 + c * 162, 26 + r * 52, 150, keys[r][c]);
        }
        text(m, 20, 304, "Head weight", &leota_inter_16, kSoft);
        cell(190, 292, 150, "mass");
        lv_obj_t* check = button(m, 352, 292, 404, 44, "Check by flying" "\xE2\x80\xA6", Kind::Blue);
        name("setup.calibrate", check);
        click_row(check, [this] { open_calibration(); });
    }

    void build_sensors() {
        lv_obj_t* m = box(set_rows, 0, 0, 776, 360, kCard);
        const char* heads[4] = {"Load cell zero", "Load cell scale", "Drum", "Reads now"};
        for (int c = 0; c < 4; ++c) {
            lv_obj_t* h = text(m, 100 + c * 146, 4, heads[c], &leota_inter_14, kMuted);
            lv_obj_set_width(h, 136);
            lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        }
        const char* kinds[3] = {"zero", "scale", "drum"};
        for (int r = 0; r < kLines; ++r) {
            const char name[2] = {static_cast<char>('A' + r), 0};
            text(m, 20, 38 + r * 52, (std::string("Line ") + name).c_str(), &leota_inter_16, kSoft);
            for (int c = 0; c < 3; ++c) {
                const Field& f = *find_field(std::string("sensor.") + name + "." + kinds[c]);
                const bool ch = changed(f);
                lv_obj_t* b = box(m, 100 + c * 146, 26 + r * 52, 136, 44, kChip, 8, ch ? kAmber : kChipEdge);
                lv_obj_set_style_bg_color(b, hex(kRaised), LV_STATE_PRESSED);
                lv_obj_t* l = text(b, 0, 12, field_text(f, draft).c_str(), &leota_inter_16_semi, ch ? kAmberSoft : kInk);
                lv_obj_set_width(l, 136);
                lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
                name_row(std::string("setup.field.") + f.key, b);
                click_row(b, [this, &f] { tap_field(f); });
            }
            sensor_now[r] = text(m, 100 + 3 * 146, 38 + r * 52, "", &leota_inter_16, kSoft);
            lv_obj_set_width(sensor_now[r], 136);
            lv_obj_set_style_text_align(sensor_now[r], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t* t = button(m, 690, 26 + r * 52, 66, 44, "Test", Kind::Blue);
            name_row(std::string("setup.test.") + name, t);
            click_row(t, [this, r] { open_line_test(r); });
        }
        text(m, 20, 240,
             "Test each line once it's rigged, and after changing a load cell or drum: you'll need a weighed "
             "2 to 10 kg weight and a tape measure. Load cells drift with the cold, so test the zero again on a "
             "cold night.",
             &leota_inter_14, kMuted, 736);
    }

    void name_row(const std::string& n, lv_obj_t* o) { name(n, o); }

    void build_pin() {
        lv_obj_t* m = box(set_rows, 0, 0, 776, 360, kCard);
        text(m, 20, 6, "The PIN unlocks this screen. Choose 4 to 8 digits, and tell the people who need it.",
             &leota_inter_16, kSoft, 700);
        lv_obj_t* r = box(m, 20, 70, 736, 50, kChip, 8, draft.pin != keeper->settings().pin ? kAmber : kChipEdge);
        lv_obj_set_style_bg_color(r, hex(kRaised), LV_STATE_PRESSED);
        text(r, 14, 15, "PIN", &leota_inter_16, kSoft);
        std::string dots;
        for (size_t i = 0; i < draft.pin.size(); ++i) dots += "\xE2\x80\xA2";
        lv_obj_t* v = text(r, 330, 14, (dots + "   Change").c_str(), &leota_inter_16_semi);
        lv_obj_set_width(v, 340);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        text(r, 694, 15, LV_SYMBOL_RIGHT, &leota_inter_16_semi, kMuted);
        name("setup.field.pin", r);
        click_row(r, [this] { open_pin_editor(); });
    }

    void refresh_setup() {
        if (lv_obj_has_flag(unlocked, LV_OBJ_FLAG_HIDDEN)) return;
        if (group == 4) {
            const auto ls = rig.lines();
            for (int i = 0; i < kLines; ++i)
                if (sensor_now[i]) set(sensor_now[i], num(ls[i].tension, 1) + " N");
        }
        if (keeper->revision() != seen_revision) {
            // Changed elsewhere (SCMP set new limits): take it, unless there are edits here.
            seen_revision = keeper->revision();
            if (settings_changes(base, draft).empty()) {
                draft = keeper->settings();
                base = draft;
                fill_group();
                return;
            }
        }
        const auto changes = settings_changes(keeper->settings(), draft);
        const bool dirty = !changes.empty();
        std::string s;
        if (dirty) {
            s = changes.size() == 1 ? "1 change not saved." : fmt("%d changes not saved.", static_cast<int>(changes.size()));
            Settings live = keeper->settings();
            live.net = draft.net;
            live.pin = draft.pin;
            if (rig.armed() && !settings_changes(live, draft).empty()) s += " Disarm to save.";
        } else if (!keeper->has_store()) {
            s = "Saved until the controller restarts.";
        } else {
            s = keeper->stored_ok() ? "Saved. Kept in the controller's flash." : "Not stored: changes last until a restart.";
        }
        set(set_status, s);
        set_text_color(set_status, dirty ? kAmberSoft : kMuted);
        enable(set_save, dirty);
        enable(set_undo, dirty);
    }

    void save_settings() {
        const auto changes = settings_changes(keeper->settings(), draft);
        if (changes.empty()) return;
        const Result r = keeper->change(draft, rig);
        if (!r) {
            say(r.detail);
            return;
        }
        if (changes.size() <= 4) {
            for (const std::string& c : changes) add_log(LogKind::Safety, "Setup: " + c);
        } else {
            std::string labels;
            for (const Field& f : settings_fields())
                if (field_text(f, base) != field_text(f, draft)) labels += (labels.empty() ? "" : ", ") + std::string(f.label);
            if (base.pin != draft.pin) labels += (labels.empty() ? "" : ", ") + std::string("PIN");
            add_log(LogKind::Safety, fmt("Setup: %d settings changed: ", static_cast<int>(changes.size())) + labels + ".");
        }
        if (!r.detail.empty()) say(r.detail);
        draft = keeper->settings();
        base = draft;
        seen_revision = keeper->revision();
        fill_group();
    }

    // -- the number pad ------------------------------------------------------------------
    void build_editor() {
        editor = box(lv_display_get_layer_top(disp), 0, 0, kW, 600 - kNav, 0x05070A);
        lv_obj_set_style_bg_opa(editor, LV_OPA_80, 0);
        lv_obj_add_flag(editor, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* c = box(editor, 162, 8, 700, 508, 0x151B24, 16, kEdge);
        ed_title = text(c, 24, 18, "", &leota_inter_28_bold);
        name("edit.title", ed_title);
        name("editor", editor);
        ed_help = text(c, 24, 60, "", &leota_inter_16, kSoft, 652);
        lv_obj_t* vb = box(c, 24, 112, 652, 64, kDark, 10, kChipEdge);
        ed_value = text(vb, 16, 16, "", &leota_inter_28_bold);
        name("edit.value", ed_value);
        ed_note = text(c, 24, 186, "", &leota_inter_16, kMuted, 652);
        name("edit.note", ed_note);
        struct K { const char* label; const char* id; int col, row, span; };
        const K keys[] = {{"7", "7", 0, 0, 1}, {"8", "8", 1, 0, 1}, {"9", "9", 2, 0, 1}, {"Delete", "delete", 3, 0, 1},
                          {"4", "4", 0, 1, 1}, {"5", "5", 1, 1, 1}, {"6", "6", 2, 1, 1}, {"Clear", "clear", 3, 1, 1},
                          {"1", "1", 0, 2, 1}, {"2", "2", 1, 2, 1}, {"3", "3", 2, 2, 1}, {kMinus, "minus", 3, 2, 1},
                          {"0", "0", 0, 3, 2}, {".", "dot", 2, 3, 1}};
        for (const K& k : keys) {
            const bool word = std::string(k.label).size() > 3;
            lv_obj_t* b = button(c, 24 + k.col * 108, 222 + k.row * 70, 100 * k.span + 8 * (k.span - 1), 62, k.label,
                                 Kind::Plain, word ? &leota_inter_16_semi : &leota_inter_28_bold);
            const std::string id = k.id;
            name("edit.key." + id, b);
            ed_keys[id] = b;
            on_click(b, [this, id] { edit_key(id); });
        }
        lv_obj_t* set_btn = button(c, 468, 222, 208, 132, "Set", Kind::Blue, &leota_inter_20_bold);
        name("edit.set", set_btn);
        on_click(set_btn, [this] { edit_set(); });
        lv_obj_t* cancel = button(c, 468, 362, 208, 132, "Cancel");
        name("edit.cancel", cancel);
        on_click(cancel, [this] { show(editor, false); });
        show(editor, false);
    }

    static std::string plain_number(double v, int decimals) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        std::string s = buf;
        if (s == "-0" || s.rfind("-0.", 0) == 0) {
            bool zero = true;
            for (char ch : s) zero = zero && (ch == '-' || ch == '0' || ch == '.');
            if (zero) s.erase(0, 1);
        }
        return s;
    }

    void open_editor(const Field* f) {
        ed_done = nullptr;
        open_editor_for(f, f->get(draft));
    }

    // The number pad for a value that isn't a setting: `done` gets it once it's in range.
    void open_number(const std::string& title, const std::string& help, const std::string& unit, double min,
                     double max, int decimals, double value, std::function<void(double)> done) {
        ed_temp_label = title;
        ed_temp_help = help;
        ed_temp_unit = unit;
        ed_temp = Field{ed_temp_label.c_str(), "", ed_temp_label.c_str(), ed_temp_unit.c_str(), Field::Kind::Number,
                        min, max, decimals, ed_temp_help.c_str(), nullptr, nullptr, nullptr};
        open_editor_for(&ed_temp, value);
        ed_done = std::move(done);
    }

    void open_editor_for(const Field* f, double value) {
        ed_field = f;
        ed_mode = EditMode::Field;
        ed_text = f->kind == Field::Kind::Address ? address_text(static_cast<std::uint32_t>(value))
                                                  : plain_number(value, f->decimals);
        ed_fresh = true;
        set(ed_title, f->label);
        set(ed_help, f->help);
        if (f->kind == Field::Kind::Address) {
            set(ed_note, "Four numbers from 0 to 255, with dots between.");
        } else {
            const std::string unit = f->unit[0] ? std::string(" ") + f->unit : "";
            set(ed_note, "From " + field_value_text(*f, f->min) + " to " + field_value_text(*f, f->max) + unit + ".");
        }
        set_text_color(ed_note, kMuted);
        enable(ed_keys["dot"], f->kind == Field::Kind::Address || f->decimals > 0);
        enable(ed_keys["minus"], f->kind == Field::Kind::Number && f->min < 0);
        show_edit_text();
        show(editor, true);
        lv_obj_move_foreground(editor);
    }

    void open_pin_editor() {
        ed_field = nullptr;
        ed_mode = EditMode::PinNew;
        ed_text.clear();
        ed_fresh = false;
        set(ed_title, "New PIN");
        set(ed_help, "4 to 8 digits. You'll enter it twice, and it takes effect when you save.");
        set(ed_note, "");
        enable(ed_keys["dot"], false);
        enable(ed_keys["minus"], false);
        show_edit_text();
        show(editor, true);
        lv_obj_move_foreground(editor);
    }

    void show_edit_text() {
        std::string t;
        for (char ch : ed_text) t += ch == '-' ? std::string(kMinus) : std::string(1, ch);
        if (ed_field && ed_field->unit[0] && !t.empty()) t += std::string(" ") + ed_field->unit;
        set(ed_value, t.empty() ? " " : t);
        set_text_color(ed_value, ed_fresh ? kSoft : kInk);
    }

    void edit_key(const std::string& id) {
        if (ed_fresh && id != "delete") ed_text.clear();
        ed_fresh = false;
        if (id == "delete") { if (!ed_text.empty()) ed_text.pop_back(); }
        else if (id == "clear") ed_text.clear();
        else if (id == "minus") {
            if (!ed_text.empty() && ed_text[0] == '-') ed_text.erase(0, 1);
            else ed_text.insert(0, "-");
        } else if (id == "dot") {
            const bool address = ed_field && ed_field->kind == Field::Kind::Address;
            if (address ? std::count(ed_text.begin(), ed_text.end(), '.') < 3 : ed_text.find('.') == std::string::npos)
                ed_text += ed_text.empty() || ed_text == "-" ? (address ? "" : "0.") : ".";
        } else if (ed_text.size() < 16) {
            ed_text += id;
        }
        show_edit_text();
    }

    void edit_error(const std::string& why) {
        set(ed_note, why);
        set_text_color(ed_note, kRedSoft);
    }

    void edit_set() {
        if (ed_mode != EditMode::Field) {
            const bool digits = ed_text.size() >= 4 && ed_text.size() <= 8 &&
                                std::all_of(ed_text.begin(), ed_text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            if (ed_mode == EditMode::PinNew) {
                if (!digits) return edit_error("The PIN must be 4 to 8 digits.");
                ed_pin = ed_text;
                ed_text.clear();
                ed_mode = EditMode::PinAgain;
                set(ed_title, "New PIN again");
                set(ed_note, "");
                show_edit_text();
                return;
            }
            if (ed_text != ed_pin) {
                ed_text.clear();
                ed_mode = EditMode::PinNew;
                set(ed_title, "New PIN");
                show_edit_text();
                return edit_error("Those didn't match. Enter the new PIN again.");
            }
            draft.pin = ed_pin;
            show(editor, false);
            fill_group();
            return;
        }
        const Field& f = *ed_field;
        double v = 0.0;
        if (f.kind == Field::Kind::Address) {
            std::uint32_t a = 0;
            if (!parse_address(ed_text, a)) return edit_error("That isn't an address: four numbers from 0 to 255, with dots.");
            v = a;
        } else {
            char* end = nullptr;
            v = std::strtod(ed_text.c_str(), &end);
            if (ed_text.empty() || ed_text == "-" || !end || *end) return edit_error("Type a number.");
            const double k = std::pow(10.0, f.decimals);
            v = std::round(v * k) / k;
        }
        std::string why;
        if (!field_allows(f, v, &why)) return edit_error(why);
        show(editor, false);
        if (ed_done) {
            const auto done = std::move(ed_done);
            ed_done = nullptr;
            done(v);
            return;
        }
        f.set(draft, v);
        fill_group();
    }

    // -- the calibration check -----------------------------------------------------------
    void build_calibration() {
        cal_dialog = box(lv_display_get_layer_top(disp), 0, 0, kW, 600 - kNav, 0x05070A);
        name("cal.dialog", cal_dialog);
        lv_obj_set_style_bg_opa(cal_dialog, LV_OPA_80, 0);
        lv_obj_add_flag(cal_dialog, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* c = box(cal_dialog, 16, 8, 992, 508, 0x151B24, 16, kEdge);
        text(c, 24, 18, "Check the measurements", &leota_inter_28_bold);
        text(c, 24, 62,
             "The ball flies to nine points around the middle and hangs still at each while every line is measured. "
             "From that the controller works out where the pulleys really are and what the head weighs. It takes about "
             "a minute, then parks the ball on the dock and disarms.",
             &leota_inter_16, kSoft, 440);
        lv_obj_t* rb = box(c, 24, 214, 440, 44, kDark, 10, kChipEdge);
        cal_ready_dot = dot(rb, 14, 17, 10, kOff);
        cal_ready = text(rb, 34, 12, "", &leota_inter_16_semi);
        lv_obj_set_width(cal_ready, 396);
        lv_label_set_long_mode(cal_ready, LV_LABEL_LONG_DOT);
        name("cal.ready", cal_ready);
        text(c, 24, 266, "The ball moves on its own: keep everyone clear. STOP and the E-stops work as always.",
             &leota_inter_14, kMuted, 440);
        cal_bar = bar(c, 24, 312, 440, 14, 100);
        cal_msg = text(c, 24, 334, "", &leota_inter_16, kInk, 440);
        name("cal.message", cal_msg);
        cal_start = button(c, 24, 372, 212, 56, "Start", Kind::Amber, &leota_inter_20_bold);
        name("cal.start", cal_start);
        on_click(cal_start, [this] {
            const Result r = cal.start(rig, now);
            result(r);
            if (r) add_log(LogKind::Motion, "Calibration check started at the screen.");
        });
        cal_stop = button(c, 252, 372, 212, 56, "Stop", Kind::Red, &leota_inter_20_bold);
        name("cal.stop", cal_stop);
        on_click(cal_stop, [this] { cal.cancel(rig, now); });
        cal_use = button(c, 24, 438, 212, 56, "Use these", Kind::Blue);
        name("cal.use", cal_use);
        on_click(cal_use, [this] { use_calibration(); });
        cal_close = button(c, 252, 438, 212, 56, "Close");
        name("cal.close", cal_close);
        on_click(cal_close, [this] { show(cal_dialog, false); });

        lv_obj_t* r = box(c, 496, 18, 472, 476, kCard, 12, kLine);
        text(r, 18, 14, "What it found", &leota_inter_20_bold);
        const char* heads[3] = {"", "Found at (x, y, z)", "Off by"};
        const int cx[3] = {18, 110, 350};
        for (int i = 0; i < 3; ++i) text(r, cx[i], 52, heads[i], &leota_inter_14, kMuted);
        const char* rows[5] = {"Pulley A", "Pulley B", "Pulley C", "Pulley D", "Head"};
        for (int i = 0; i < 5; ++i) {
            text(r, 18, 80 + i * 34, rows[i], &leota_inter_16, kSoft);
            cal_found[i] = text(r, 110, 80 + i * 34, "", &leota_inter_16_semi);
            cal_off[i] = text(r, 350, 80 + i * 34, "", &leota_inter_16_semi);
            lv_obj_set_width(cal_off[i], 104);
            lv_obj_set_style_text_align(cal_off[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
        cal_summary = text(r, 18, 260, "", &leota_inter_16, kInk, 436);
        name("cal.summary", cal_summary);
        show(cal_dialog, false);
    }

    void open_calibration() {
        show(cal_dialog, true);
        refresh_calibration();
    }

    void refresh_calibration() {
        const bool running = cal.running();
        const std::string why = CalibrationRun::not_ready(rig);
        const bool again = cal.phase() == CalibrationRun::Phase::Done || cal.phase() == CalibrationRun::Phase::Failed;
        set(cal_ready, running ? "Running." : why.empty() ? (again ? "Ready to run it again." : "Ready to start.")
                                : again ? "To run it again, " + lower_first(why) : why);
        set_color(cal_ready_dot, running ? kAmber : why.empty() ? kGreen : kOff);
        enable(cal_start, !running && why.empty());
        enable(cal_stop, running);
        enable(cal_close, !running);
        const bool have = cal.phase() == CalibrationRun::Phase::Done && cal.fit().mass > 0.0;
        enable(cal_use, have);
        bar_value(cal_bar, cal.points() ? 100 * cal.measured() / cal.points() : 0);
        set(cal_msg, cal.phase() == CalibrationRun::Phase::Off ? "" : cal.message());
        set_text_color(cal_msg, cal.phase() == CalibrationRun::Phase::Failed ? kRedSoft : kInk);
        const CalFit& f = cal.fit();
        for (int i = 0; i < 5; ++i) {
            if (!have) {
                set(cal_found[i], "\xE2\x80\x94");
                set(cal_off[i], "");
                continue;
            }
            if (i < kLines) {
                const Vec3& a = f.anchors[i];
                set(cal_found[i], mm(a.x) + ", " + mm(a.y) + ", " + mm(a.z));
                set(cal_off[i], mm(f.moved[i]) + " mm");
                set_text_color(cal_off[i], f.moved[i] >= 30 ? kAmberSoft : f.moved[i] >= 10 ? kInk : kGreenInk);
            } else {
                set(cal_found[i], fmt("%.2f kg", f.mass));
                const double d = f.mass - rig.config().mass;
                set(cal_off[i], std::fabs(d) < 0.005 ? "0 kg" : fmt("%s%.2f kg", d > 0 ? "+" : kMinus, std::fabs(d)));
                set_text_color(cal_off[i], std::fabs(d) >= 0.05 ? kAmberSoft : kGreenInk);
            }
        }
        std::string summary = cal.phase() == CalibrationRun::Phase::Done ? f.summary : "";
        for (size_t at = 0; (at = summary.find(". ", at)) != std::string::npos;) summary.replace(at, 2, ".\n");
        set(cal_summary, summary);
    }

    void use_calibration() {
        const CalFit& f = cal.fit();
        const char* names[kLines] = {"A", "B", "C", "D"};
        const char* axes[3] = {"x", "y", "z"};
        for (int i = 0; i < kLines; ++i) {
            const double v[3] = {f.anchors[i].x, f.anchors[i].y, f.anchors[i].z};
            for (int k = 0; k < 3; ++k)
                find_field(std::string("pulley.") + names[i] + "." + axes[k])->set(draft, std::round(v[k]));
        }
        find_field("mass")->set(draft, std::round(f.mass * 100.0) / 100.0);
        show(cal_dialog, false);
        group = 3;
        fill_group();
    }

    // -- the line test -------------------------------------------------------------------
    void build_line_test() {
        lt_dialog = box(lv_display_get_layer_top(disp), 0, 0, kW, 600 - kNav, 0x05070A);
        name("lt.dialog", lt_dialog);
        lv_obj_set_style_bg_opa(lt_dialog, LV_OPA_80, 0);
        lv_obj_add_flag(lt_dialog, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* c = box(lt_dialog, 16, 8, 992, 508, 0x151B24, 16, kEdge);
        lt_title = text(c, 24, 18, "", &leota_inter_28_bold);
        const char* steps[3] = {"1. Zero, with nothing on the line", "2. Scale, with a known weight",
                                "3. Drum, raising the weight 500 mm"};
        for (int i = 0; i < 3; ++i) {
            lt_dot[i] = dot(c, 24, 70 + i * 32, 12, kOff);
            lt_step[i] = text(c, 46, 66 + i * 32, steps[i], &leota_inter_16, kSoft);
        }
        lv_obj_t* ib = box(c, 24, 170, 444, 150, kDark, 10, kChipEdge);
        lt_instr = text(ib, 16, 14, "", &leota_inter_16, kInk, 412);
        name("lt.instructions", lt_instr);
        lt_msg = text(c, 24, 328, "", &leota_inter_14, kMuted, 444);
        name("lt.message", lt_msg);
        lt_weight = button(c, 24, 372, 212, 56, "", Kind::Plain);
        name("lt.weight", lt_weight);
        on_click(lt_weight, [this] {
            open_number("Test weight", "What the weight hanging from the line weighs, from a scale.", "kg", 0.5, 30, 2,
                        lt.weight_kg(), [this](double v) { lt.set_weight(v); });
        });
        lt_primary = button(c, 256, 372, 212, 56, "", Kind::Blue, &leota_inter_16_semi);
        name("lt.primary", lt_primary);
        on_click(lt_primary, [this] { line_test_primary(); });
        lt_skip = button(c, 24, 438, 212, 56, "Finish here");
        name("lt.skip", lt_skip);
        on_click(lt_skip, [this] { lt.finish(); });
        lt_close = button(c, 256, 438, 212, 56, "Cancel");
        name("lt.close", lt_close);
        on_click(lt_close, [this] {
            lt.cancel(rig, now);
            show(lt_dialog, false);
        });

        lv_obj_t* r = box(c, 496, 18, 472, 476, kCard, 12, kLine);
        text(r, 18, 14, "The load cell reads now", &leota_inter_16, kMuted);
        lt_live = text(r, 18, 40, "", &leota_inter_28_bold);
        name("lt.live", lt_live);
        lt_live_note = text(r, 18, 80, "", &leota_inter_14, kMuted, 436);
        const char* heads[3] = {"", "Now", "Found"};
        const int cx[3] = {18, 200, 330};
        for (int i = 0; i < 3; ++i) text(r, cx[i], 130, heads[i], &leota_inter_14, kMuted);
        const char* rows[3] = {"Load cell zero", "Load cell scale", "Drum"};
        for (int i = 0; i < 3; ++i) {
            text(r, 18, 158 + i * 36, rows[i], &leota_inter_16, kSoft);
            lt_before[i] = text(r, 200, 158 + i * 36, "", &leota_inter_16_semi);
            lt_found[i] = text(r, 330, 158 + i * 36, "", &leota_inter_16_semi);
            name(std::string("lt.found.") + std::to_string(i), lt_found[i]);
        }
        text(r, 18, 290,
             "The zero and scale take effect at once when saved. A new drum means homing again, and so does "
             "raising the weight: hook the line back onto the ball first.",
             &leota_inter_14, kMuted, 436);
        show(lt_dialog, false);
    }

    void open_line_test(int line) {
        const Result r = lt.start(rig, line);
        if (!r) {
            say(r.detail);
            return;
        }
        show(lt_dialog, true);
        refresh_line_test();
    }

    void line_test_primary() {
        using S = LineTest::Step;
        Result r;
        switch (lt.step()) {
            case S::Empty:
            case S::Weight: r = lt.measure(rig, now); break;
            case S::Raise: r = lt.raise(rig, now); break;
            case S::Rise:
                open_number("How far did it go up?",
                            "From your mark to where the weight is now, to the nearest millimetre.", "mm", 1, 2000, 0,
                            lt.move_mm, [this](double v) {
                                const Result d = lt.rise(v);
                                if (!d) say(d.detail);
                            });
                return;
            case S::Done: use_line_test(); return;
            case S::Off: return;
        }
        if (!r) say(r.detail);
    }

    void use_line_test() {
        const char name[2] = {static_cast<char>('A' + lt.line()), 0};
        const LineSensors& f = lt.result();
        find_field(std::string("sensor.") + name + ".zero")->set(draft, std::round(f.load_zero * 100.0) / 100.0);
        find_field(std::string("sensor.") + name + ".scale")->set(draft, std::round(f.load_scale * 1e4) / 1e4);
        find_field(std::string("sensor.") + name + ".drum")->set(draft, std::round(f.drum * 1e4) / 1e4);
        add_log(LogKind::Safety, std::string("Line test on line ") + name +
                                     fmt(": zero %.2f N, scale %.4f, drum %.4f.", f.load_zero, f.load_scale, f.drum));
        lt.cancel(rig, now);
        show(lt_dialog, false);
        group = 4;
        fill_group();
    }

    void refresh_line_test() {
        using S = LineTest::Step;
        const S st = lt.step();
        const char name[2] = {static_cast<char>('A' + lt.line()), 0};
        const std::string L = name;
        set(lt_title, "Test line " + L);
        const int at = st == S::Empty ? 0 : st == S::Weight ? 1 : st == S::Off ? -1 : st == S::Done ? 3 : 2;
        for (int i = 0; i < 3; ++i) {
            set_color(lt_dot[i], i < at ? kGreen : i == at ? kBlue : kOff);
            set_text_color(lt_step[i], i == at ? kInk : i < at ? kGreenInk : kMuted);
        }
        std::string instr, primary;
        bool can = !lt.busy();
        switch (st) {
            case S::Empty:
                instr = "Unhook line " + L + " from the ball and let it hang free, with nothing on it. When it's still, "
                        "measure.";
                primary = "Measure zero";
                break;
            case S::Weight:
                instr = "Hang a weighed weight from line " + L +
                        ": 2 to 10 kg, clear of the floor and still. Check its weight on the left, then measure.";
                primary = "Measure weight";
                break;
            case S::Raise: {
                instr = "Mark the bottom of the weight on something fixed beside it. The winch will raise it " +
                        fmt("%.0f", lt.move_mm) + " mm on its own: keep hands clear of the line and pulley.";
                const std::string why = !rig.armed() ? "Arm the rig first, at Controls."
                                        : rig.remote() ? "Turn remote control off first, at Controls." : "";
                if (!why.empty() && !lt.busy()) instr += " " + why;
                can = can && why.empty();
                primary = fmt("Raise %.0f mm", lt.move_mm);
                break;
            }
            case S::Rise:
                instr = "Measure how far the weight went up, from your mark to its bottom now.";
                primary = "Enter the rise";
                break;
            case S::Done:
                instr = "Done. Use these to put them in Sensors, then Save. Hook line " + L +
                        " back onto the ball and home before flying.";
                primary = "Use these";
                break;
            case S::Off: break;
        }
        if (lt.busy()) primary = st == S::Raise ? "Raising" "\xE2\x80\xA6" : "Measuring" "\xE2\x80\xA6";
        set(lt_instr, instr);
        set(lv_obj_get_child(lt_primary, 0), primary);
        enable(lt_primary, can);
        set(lt_msg, lt.message());
        show(lt_weight, st == S::Weight);
        set(lv_obj_get_child(lt_weight, 0), fmt("Weight: %.2f kg", lt.weight_kg()));
        enable(lt_skip, !lt.busy() && (st == S::Weight || st == S::Raise || st == S::Rise));
        set(lv_obj_get_child(lt_close, 0), st == S::Done ? "Close" : "Cancel");
        const double raw = lt.reading(rig);
        const LineSensors& now_ = rig.config().sensors[lt.line()];
        set(lt_live, num(raw, 2) + " N");
        set(lt_live_note, "as it reads; " + num((raw - now_.load_zero) * now_.load_scale, 2) +
                              " N with the corrections in use now");
        const LineSensors &b = lt.before(), &f = lt.result();
        const double bv[3] = {b.load_zero, b.load_scale, b.drum}, fv[3] = {f.load_zero, f.load_scale, f.drum};
        const bool found[3] = {at > 0, at > 1, st == S::Done && f.drum != b.drum};
        for (int i = 0; i < 3; ++i) {
            auto shown_ = [&](double v) { return i == 0 ? num(v, 2) + " N" : num(v, 4); };
            set(lt_before[i], shown_(bv[i]));
            set(lt_found[i], found[i] ? shown_(fv[i]) : std::string("\xE2\x80\x94"));
            set_text_color(lt_found[i], found[i] && std::fabs(fv[i] - bv[i]) > (i == 0 ? 0.05 : 0.0005) ? kAmberSoft : kInk);
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
            lv_obj_set_size(msg, 776, 20);  // one line, cut short with dots
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
        if (cal.running()) cal.update(rig, now);
        if (cal.phase() != cal_seen) {
            cal_seen = cal.phase();
            if (cal_seen == CalibrationRun::Phase::Done) add_log(LogKind::Motion, "Calibration check: " + cal.fit().summary);
            if (cal_seen == CalibrationRun::Phase::Failed) add_log(LogKind::Motion, "Calibration check: " + cal.message());
        }
        sample_wind();
        const bool estop = rig.estop();
        show(estop_page, estop);
        if (estop) {
            lv_obj_move_foreground(estop_page);
            show(editor, false);
            if (!cal.running()) show(cal_dialog, false);
        }
        if (!lv_obj_has_flag(cal_dialog, LV_OBJ_FLAG_HIDDEN)) refresh_calibration();
        if (lt.step() != LineTest::Step::Off) lt.update(rig, now);
        if (!lv_obj_has_flag(lt_dialog, LV_OBJ_FLAG_HIDDEN)) refresh_line_test();
        refresh_top();
        if (!estop) {
            switch (page) {
                case 0: refresh_status(); break;
                case 1: refresh_controls(); break;
                case 2: refresh_jog(); break;
                case 3: refresh_weather(); break;
                case 4: refresh_diagnostics(); break;
                case 5: refresh_setup(); break;
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

Ui::Ui(Rig& rig, lv_display_t* display, SettingsKeeper* settings) : d_(new Impl(rig, display, settings)) {
    d_->build();
}

Ui::~Ui() = default;

void Ui::update(double now, const UiLink& link, const std::string& clock) { d_->update(now, link, clock); }

void Ui::log(LogKind kind, const std::string& text) { d_->add_log(kind, text); }

void Ui::set_lock(std::function<void()> lock, std::function<void()> unlock) {
    d_->lock_fn = std::move(lock);
    d_->unlock_fn = std::move(unlock);
}

lv_obj_t* Ui::find(const std::string& name) const {
    auto it = d_->named.find(name);
    return it == d_->named.end() ? nullptr : it->second;
}

std::string Ui::page() const { return d_->page_names[d_->page]; }

}  // namespace leota
