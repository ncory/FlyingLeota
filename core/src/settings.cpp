#include "leota/settings.hpp"

#include <cmath>
#include <cstdio>

#include "ArduinoJson.h"

namespace leota {

namespace {

constexpr int kSettingsVersion = 1;
const char* kMinus = "\xE2\x88\x92";  // −

// Pulley positions: one getter and setter per line and coordinate.
template <int L, int C>
double anchor_get(const Settings& s) {
    const Vec3& a = s.rig.lines[L].anchor;
    return C == 0 ? a.x : C == 1 ? a.y : a.z;
}
template <int L, int C>
void anchor_set(Settings& s, double v) {
    Vec3& a = s.rig.lines[L].anchor;
    (C == 0 ? a.x : C == 1 ? a.y : a.z) = v;
}

// The dock carries the table under it, the bottom of the workspace and, in two-axis
// mode, the depth plane.
void move_dock(Settings& s, const Vec3& to) {
    RigConfig& c = s.rig;
    const Vec3 d = to - c.dock;
    c.dock = to;
    c.table.lo += d;
    c.table.hi += d;
    c.axes[2].min = std::floor(c.dock.z);
    c.wind.amber_box.lo.z = c.axes[2].min;
    c.fixed_y = c.dock.y;
}

bool three_axis(const Settings& s) { return !s.rig.two_axis; }
bool wind_fitted(const Settings& s) { return s.rig.wind.enabled; }
bool static_address(const Settings& s) { return !s.net.dhcp; }

using K = Field::Kind;

const char* kPulleyHelp =
    "Where the line leaves its pulley, from the middle of the floor: x to the audience's right, "
    "y upstage, z up from the deck. Needs homing again.";
const char* kNetHelp = "Takes effect when the controller restarts.";

#define PULLEY(L, NAME)                                                                              \
    Field{"pulley." NAME ".x", "Measurements", "Pulley " NAME " across (x)", "mm", K::Number, -6000, 6000, 0, \
          kPulleyHelp, anchor_get<L, 0>, anchor_set<L, 0>, nullptr},                                    \
    Field{"pulley." NAME ".y", "Measurements", "Pulley " NAME " depth (y)", "mm", K::Number, -6000, 6000, 0,  \
          kPulleyHelp, anchor_get<L, 1>, anchor_set<L, 1>, nullptr},                                    \
    Field{"pulley." NAME ".z", "Measurements", "Pulley " NAME " height (z)", "mm", K::Number, 500, 10000, 0,  \
          kPulleyHelp, anchor_get<L, 2>, anchor_set<L, 2>, nullptr}

const std::vector<Field>& make_fields() {
    static const std::vector<Field> fields = {
        // -- Flight
        {"flight.two_axis", "Flight", "Two axes, depth fixed", "", K::Toggle, 0, 1, 0,
         "The ball flies only across and up and down, in the depth plane through the dock. Needs homing again.",
         [](const Settings& s) { return s.rig.two_axis ? 1.0 : 0.0; },
         [](Settings& s, double v) { s.rig.two_axis = v != 0; s.rig.fixed_y = s.rig.dock.y; }, nullptr},
        {"workspace.x_min", "Flight", "Left edge (x)", "mm", K::Number, -6000, 6000, 0,
         "The furthest the ball's centre may go to the audience's left.",
         [](const Settings& s) { return s.rig.axes[0].min; }, [](Settings& s, double v) { s.rig.axes[0].min = v; }, nullptr},
        {"workspace.x_max", "Flight", "Right edge (x)", "mm", K::Number, -6000, 6000, 0,
         "The furthest the ball's centre may go to the audience's right.",
         [](const Settings& s) { return s.rig.axes[0].max; }, [](Settings& s, double v) { s.rig.axes[0].max = v; }, nullptr},
        {"workspace.y_min", "Flight", "Front edge (y)", "mm", K::Number, -6000, 6000, 0,
         "The closest the ball's centre may come to the audience.",
         [](const Settings& s) { return s.rig.axes[1].min; }, [](Settings& s, double v) { s.rig.axes[1].min = v; },
         three_axis},
        {"workspace.y_max", "Flight", "Back edge (y)", "mm", K::Number, -6000, 6000, 0,
         "The furthest upstage the ball's centre may go.",
         [](const Settings& s) { return s.rig.axes[1].max; }, [](Settings& s, double v) { s.rig.axes[1].max = v; },
         three_axis},
        {"workspace.z_max", "Flight", "Highest (z)", "mm", K::Number, 0, 10000, 0,
         "The highest the ball's centre may go. Keep the ball's top well below the pulleys. The lowest is the dock.",
         [](const Settings& s) { return s.rig.axes[2].max; }, [](Settings& s, double v) { s.rig.axes[2].max = v; }, nullptr},
        {"speed.across", "Flight", "Top speed across", "mm/s", K::Number, 10, 1500, 0,
         "The fastest the ball moves across and in depth.",
         [](const Settings& s) { return s.rig.axes[0].max_velocity; },
         [](Settings& s, double v) { s.rig.axes[0].max_velocity = s.rig.axes[1].max_velocity = v; }, nullptr},
        {"speed.up", "Flight", "Top speed up and down", "mm/s", K::Number, 10, 1500, 0,
         "The fastest the ball moves up and down.",
         [](const Settings& s) { return s.rig.axes[2].max_velocity; },
         [](Settings& s, double v) { s.rig.axes[2].max_velocity = v; }, nullptr},
        {"accel.across", "Flight", "Acceleration across", "mm/s\xC2\xB2", K::Number, 50, 5000, 0,
         "How quickly the ball speeds up and slows down across and in depth.",
         [](const Settings& s) { return s.rig.axes[0].max_accel; },
         [](Settings& s, double v) { s.rig.axes[0].max_accel = s.rig.axes[1].max_accel = v; }, nullptr},
        {"accel.up", "Flight", "Acceleration up and down", "mm/s\xC2\xB2", K::Number, 50, 5000, 0,
         "How quickly the ball speeds up and slows down going up and down.",
         [](const Settings& s) { return s.rig.axes[2].max_accel; },
         [](Settings& s, double v) { s.rig.axes[2].max_accel = v; }, nullptr},
        {"smoothing", "Flight", "Smoothing", "s", K::Number, 0, 0.5, 2,
         "Rounds off the start and end of every move. Longer is gentler, and adds this much to each move.",
         [](const Settings& s) { return s.rig.smoothing; }, [](Settings& s, double v) { s.rig.smoothing = v; }, nullptr},

        // -- Lines and safety
        {"tension.slack", "Lines and safety", "Slack below", "N", K::Number, 2, 40, 1,
         "A line pulling less than this is slack, and the rig stops. Moves keep every line 3 N above it.",
         [](const Settings& s) { return s.rig.tension_min; },
         [](Settings& s, double v) { s.rig.tension_min = v; s.rig.plan_tension_min = v + 3.0; }, nullptr},
        {"tension.overload", "Lines and safety", "Overload above", "N", K::Number, 20, 500, 0,
         "A line pulling more than this is overloaded, and the rig stops. Moves plan to stay under 80% of it.",
         [](const Settings& s) { return s.rig.tension_max; },
         [](Settings& s, double v) { s.rig.tension_max = v; s.rig.plan_tension_max = 0.8 * v; }, nullptr},
        {"tension.mismatch", "Lines and safety", "Mismatch fault at", "N", K::Number, 2, 100, 1,
         "How far a line's tension may stray from what the model expects before the rig stops.",
         [](const Settings& s) { return s.rig.mismatch_limit; },
         [](Settings& s, double v) { s.rig.mismatch_limit = v; }, nullptr},
        {"heartbeat", "Lines and safety", "Heartbeat timeout", "s", K::Number, 0.1, 5, 1,
         "If Show Controller goes quiet for this long while remote control is on, the rig stops.",
         [](const Settings& s) { return s.rig.heartbeat_timeout; },
         [](Settings& s, double v) { s.rig.heartbeat_timeout = v; }, nullptr},
        {"remote_arm", "Lines and safety", "Show Controller may arm", "", K::Toggle, 0, 1, 0,
         "Off: only this screen can arm the rig. Show Controller can always disarm and stop it.",
         [](const Settings& s) { return s.rig.remote_arm ? 1.0 : 0.0; },
         [](Settings& s, double v) { s.rig.remote_arm = v != 0; }, nullptr},

        // -- Wind
        {"wind.enabled", "Wind", "Wind sensor fitted", "", K::Toggle, 0, 1, 0,
         "Off only for tests indoors: with no sensor, the wind rules are off.",
         [](const Settings& s) { return s.rig.wind.enabled ? 1.0 : 0.0; },
         [](Settings& s, double v) { s.rig.wind.enabled = v != 0; }, nullptr},
        {"wind.amber", "Wind", "Amber at gust", "m/s", K::Number, 1, 40, 1,
         "From this gust, moves slow down and stay low and central.",
         [](const Settings& s) { return s.rig.wind.amber; }, [](Settings& s, double v) { s.rig.wind.amber = v; },
         wind_fitted},
        {"wind.red", "Wind", "Red at gust", "m/s", K::Number, 1, 40, 1,
         "From this gust, the ball parks on the dock and the rig disarms.",
         [](const Settings& s) { return s.rig.wind.red; }, [](Settings& s, double v) { s.rig.wind.red = v; }, wind_fitted},
        {"wind.hold", "Wind", "Green again after", "min", K::Number, 1, 120, 0,
         "After Red, the wind must stay Green this long before the rig will arm.",
         [](const Settings& s) { return s.rig.wind.hold / 60.0; },
         [](Settings& s, double v) { s.rig.wind.hold = v * 60.0; }, wind_fitted},
        {"wind.amber_speed", "Wind", "Speed at Amber", "%", K::Number, 10, 100, 0,
         "At Amber, moves go at most this share of their top speed.",
         [](const Settings& s) { return s.rig.wind.amber_speed * 100.0; },
         [](Settings& s, double v) { s.rig.wind.amber_speed = v / 100.0; }, wind_fitted},
        {"wind.swing", "Wind", "Swing alarm", "N", K::Number, 0.5, 30, 1,
         "A line's tension swinging by this much while the ball hangs still counts as Red; half of it as Amber.",
         [](const Settings& s) { return s.rig.wind.swing_red; },
         [](Settings& s, double v) { s.rig.wind.swing_red = v; s.rig.wind.swing_amber = v / 2.0; }, nullptr},

        // -- Measurements
        PULLEY(0, "A"), PULLEY(1, "B"), PULLEY(2, "C"), PULLEY(3, "D"),
        {"dock.x", "Measurements", "Dock across (x)", "mm", K::Number, -3000, 3000, 0,
         "The middle of the dock's cup, from the middle of the floor. Needs homing again.",
         [](const Settings& s) { return s.rig.dock.x; },
         [](Settings& s, double v) { move_dock(s, {v, s.rig.dock.y, s.rig.dock.z}); }, nullptr},
        {"dock.y", "Measurements", "Dock depth (y)", "mm", K::Number, -3000, 3000, 0,
         "The middle of the dock's cup, from the middle of the floor. Needs homing again.",
         [](const Settings& s) { return s.rig.dock.y; },
         [](Settings& s, double v) { move_dock(s, {s.rig.dock.x, v, s.rig.dock.z}); }, nullptr},
        {"dock.height", "Measurements", "Dock top height", "mm", K::Number, 0, 4000, 0,
         "How high the top of the dock's pedestal is off the deck. Needs homing again.",
         [](const Settings& s) { return s.rig.dock.z - s.rig.ball_radius; },
         [](Settings& s, double v) { move_dock(s, {s.rig.dock.x, s.rig.dock.y, v + s.rig.ball_radius}); }, nullptr},
        {"mass", "Measurements", "Head weight", "kg", K::Number, 0.5, 30, 2,
         "Everything that flies: the ball, head, battery and fittings.",
         [](const Settings& s) { return s.rig.mass; }, [](Settings& s, double v) { s.rig.mass = v; }, nullptr},

        // -- Network
        {"net.dhcp", "Network", "Address from DHCP", "", K::Toggle, 0, 1, 0, kNetHelp,
         [](const Settings& s) { return s.net.dhcp ? 1.0 : 0.0; }, [](Settings& s, double v) { s.net.dhcp = v != 0; },
         nullptr},
        {"net.address", "Network", "Address", "", K::Address, 0, 4294967295.0, 0, kNetHelp,
         [](const Settings& s) { return static_cast<double>(s.net.address); },
         [](Settings& s, double v) { s.net.address = static_cast<std::uint32_t>(v); }, static_address},
        {"net.netmask", "Network", "Netmask", "", K::Address, 0, 4294967295.0, 0, kNetHelp,
         [](const Settings& s) { return static_cast<double>(s.net.netmask); },
         [](Settings& s, double v) { s.net.netmask = static_cast<std::uint32_t>(v); }, static_address},
        {"net.gateway", "Network", "Gateway", "", K::Address, 0, 4294967295.0, 0, kNetHelp,
         [](const Settings& s) { return static_cast<double>(s.net.gateway); },
         [](Settings& s, double v) { s.net.gateway = static_cast<std::uint32_t>(v); }, static_address},
        {"net.port", "Network", "SCMP port", "", K::Number, 1, 65535, 0,
         "The UDP port Show Controller sends to. Takes effect when the controller restarts.",
         [](const Settings& s) { return static_cast<double>(s.net.scmp_port); },
         [](Settings& s, double v) { s.net.scmp_port = static_cast<int>(v); }, nullptr},
    };
    return fields;
}

#undef PULLEY

double round_to(double v, int decimals) {
    const double k = std::pow(10.0, decimals);
    return std::round(v * k) / k;
}

bool pin_ok(const std::string& pin) {
    if (pin.size() < 4 || pin.size() > 8) return false;
    for (char c : pin)
        if (c < '0' || c > '9') return false;
    return true;
}

bool rig_part_differs(const Settings& a, const Settings& b) {
    for (const Field& f : settings_fields())
        if (std::string(f.group) != "Network" && f.get(a) != f.get(b)) return true;
    for (int i = 0; i < kAxes; ++i) {
        const AxisLimits &x = a.rig.axes[i], &y = b.rig.axes[i];
        if (x.min != y.min || x.max != y.max || x.max_velocity != y.max_velocity || x.max_accel != y.max_accel)
            return true;
    }
    return false;
}

}  // namespace

std::string address_text(std::uint32_t a) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (a >> 24) & 255u, (a >> 16) & 255u, (a >> 8) & 255u, a & 255u);
    return buf;
}

bool parse_address(const std::string& text, std::uint32_t& out) {
    std::uint32_t v = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        unsigned n = 0;
        int digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            n = n * 10 + static_cast<unsigned>(text[i] - '0');
            if (++digits > 3 || n > 255) return false;
            ++i;
        }
        v = (v << 8) | n;
        if (++parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = v;
    return true;
}

const std::vector<Field>& settings_fields() { return make_fields(); }

const Field* find_field(const std::string& key) {
    for (const Field& f : settings_fields())
        if (key == f.key) return &f;
    return nullptr;
}

std::string field_value_text(const Field& f, double v) {
    if (f.kind == Field::Kind::Toggle) return v != 0 ? "On" : "Off";
    if (f.kind == Field::Kind::Address) return address_text(static_cast<std::uint32_t>(v));
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.*f", f.decimals, std::fabs(round_to(v, f.decimals)));
    return (round_to(v, f.decimals) < 0 ? std::string(kMinus) : std::string()) + buf;
}

std::string field_text(const Field& f, const Settings& s) {
    std::string t = field_value_text(f, f.get(s));
    if (f.unit[0]) t += std::string(std::string(f.unit) == "%" ? "" : " ") + f.unit;
    return t;
}

bool field_allows(const Field& f, double v, std::string* why) {
    if (!std::isfinite(v)) {
        if (why) *why = "That isn't a number.";
        return false;
    }
    if (f.kind == Field::Kind::Toggle) return true;
    if (f.kind == Field::Kind::Address) {
        if (v < 0 || v > 4294967295.0 || v != std::floor(v)) {
            if (why) *why = "That isn't an address.";
            return false;
        }
        return true;
    }
    const double eps = 0.5 * std::pow(10.0, -f.decimals);
    if (v < f.min - eps || v > f.max + eps) {
        if (why) {
            std::string unit = f.unit[0] ? std::string(" ") + f.unit : "";
            *why = "It must be from " + field_value_text(f, f.min) + " to " + field_value_text(f, f.max) + unit + ".";
        }
        return false;
    }
    return true;
}

Settings settings_for(const RigConfig& cfg) {
    Settings s;
    s.rig = cfg;
    return s;
}

std::string check_settings(const Settings& s) {
    std::string out;
    auto say = [&out](const std::string& t) { out += t + "\n"; };
    for (const Field& f : settings_fields()) {
        std::string why;
        if (!field_allows(f, f.get(s), &why)) say(std::string(f.label) + ": " + why);
    }
    const RigConfig& c = s.rig;
    out += check_config(c);
    if (c.wind.enabled && c.wind.amber >= c.wind.red) say("The wind's Amber gust must be below its Red gust.");
    if (c.dock.x < c.axes[0].min || c.dock.x > c.axes[0].max ||
        (!c.two_axis && (c.dock.y < c.axes[1].min || c.dock.y > c.axes[1].max)))
        say("The dock must be inside the workspace's edges.");
    if (c.axes[2].max < c.dock.z + 100.0) say("The highest point must be at least 100 mm above the ball on the dock.");
    if (!pin_ok(s.pin)) say("The PIN must be 4 to 8 digits.");
    if (!s.net.dhcp) {
        const std::uint32_t m = s.net.netmask, a = s.net.address, g = s.net.gateway;
        const bool mask_ok = m != 0 && ((~m + 1) & ~m) == 0;  // ones, then zeros
        if (!mask_ok) say("The netmask isn't one: it should look like 255.255.255.0.");
        else {
            if ((a & ~m) == 0 || (a & ~m) == ~m) say("That address can't be a device's: try another last number.");
            if ((g & m) != (a & m)) say("The gateway must be on the same network as the address.");
        }
    }
    return out;
}

std::vector<std::string> settings_changes(const Settings& from, const Settings& to) {
    std::vector<std::string> out;
    for (const Field& f : settings_fields()) {
        const std::string a = field_text(f, from), b = field_text(f, to);
        if (a != b) out.push_back(std::string(f.label) + ": " + a + " to " + b + ".");
    }
    if (from.pin != to.pin) out.push_back("The Setup PIN changed.");
    return out;
}

std::string settings_to_json(const Settings& s) {
    JsonDocument doc;
    doc["version"] = kSettingsVersion;
    JsonObject o = doc["settings"].to<JsonObject>();
    for (const Field& f : settings_fields()) {
        const double v = f.get(s);
        if (f.kind == Field::Kind::Toggle) o[f.key] = v != 0;
        else if (f.kind == Field::Kind::Address) o[f.key] = address_text(static_cast<std::uint32_t>(v));
        else o[f.key] = round_to(v, f.decimals + 2);
    }
    doc["pin"] = s.pin;
    std::string out;
    serializeJsonPretty(doc, out);
    return out;
}

std::string settings_from_json(const std::string& json, Settings& s) {
    JsonDocument doc;
    if (deserializeJson(doc, json) || !doc.is<JsonObject>()) return "The stored settings aren't readable.\n";
    std::string out;
    if (doc["version"].as<int>() != kSettingsVersion) out += "The stored settings are from another version.\n";
    JsonObjectConst o = doc["settings"].as<JsonObjectConst>();
    if (o.isNull()) return out + "The stored settings are empty.\n";
    for (const Field& f : settings_fields()) {
        JsonVariantConst v = o[f.key];
        if (v.isNull()) continue;
        double value = 0.0;
        bool ok = true;
        if (f.kind == Field::Kind::Toggle) {
            ok = v.is<bool>();
            value = v.as<bool>() ? 1.0 : 0.0;
        } else if (f.kind == Field::Kind::Address) {
            std::uint32_t a = 0;
            ok = v.is<const char*>() && parse_address(v.as<const char*>(), a);
            value = a;
        } else {
            ok = (v.is<double>() || v.is<long>()) && !v.is<bool>();
            value = v.as<double>();
        }
        std::string why;
        if (ok && !field_allows(f, value, &why)) ok = false;
        if (!ok) {
            out += std::string("Stored ") + f.label + " was wrong; kept " + field_text(f, s) + ".\n";
            continue;
        }
        f.set(s, value);
    }
    JsonVariantConst pin = doc["pin"];
    if (pin.is<const char*>() && pin_ok(pin.as<const char*>())) s.pin = pin.as<const char*>();
    else if (!pin.isNull()) out += "The stored PIN was wrong; kept the old one.\n";
    return out;
}

// -- SettingsKeeper ----------------------------------------------------------------------

SettingsKeeper::SettingsKeeper(const Settings& defaults, SettingsStore* store)
    : defaults_(defaults), current_(defaults), store_(store) {}

std::string SettingsKeeper::load() {
    current_ = defaults_;
    std::string json;
    if (!store_ || !store_->read(json)) return "";
    Settings s = defaults_;
    std::string out = settings_from_json(json, s);
    const std::string bad = check_settings(s);
    if (!bad.empty()) return out + bad + "Using the standard settings instead.\n";
    current_ = s;
    ++revision_;
    return out;
}

Result SettingsKeeper::change(const Settings& next, Rig& rig) {
    const std::string bad = check_settings(next);
    if (!bad.empty()) return {Refusal::BadMessage, bad.substr(0, bad.find('\n'))};
    Settings live = current_;
    live.rig = rig.config();
    if (rig_part_differs(live, next)) {
        Result r = rig.reconfigure(next.rig);
        if (!r) return r;
    }
    current_ = next;
    // A Result with no error but a detail: it worked, with a warning.
    if (!save()) return {Refusal::None, "Changed, but the settings couldn't be stored: they last until a restart."};
    return {};
}

bool SettingsKeeper::adopt(const RigConfig& cfg) {
    current_.rig.axes = cfg.axes;
    current_.rig.heartbeat_timeout = cfg.heartbeat_timeout;
    return save();
}

bool SettingsKeeper::save() {
    ++revision_;
    if (!store_) return stored_ok_ = true;
    return stored_ok_ = store_->write(settings_to_json(current_));
}

}  // namespace leota
