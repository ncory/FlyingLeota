#include "leota/scmp.hpp"

#include <cmath>
#include <optional>

#include "ArduinoJson.h"

namespace leota {

namespace {

const char* refusal_name(Refusal r) {
    switch (r) {
        case Refusal::None: return "";
        case Refusal::BadMessage: return "bad_message";
        case Refusal::NoAxis: return "no_axis";
        case Refusal::Disarmed: return "disarmed";
        case Refusal::EStop: return "estop";
        case Refusal::NotHomed: return "not_homed";
        case Refusal::OutOfRange: return "out_of_range";
        case Refusal::Busy: return "busy";
        case Refusal::Fault: return "fault";
        case Refusal::Weather: return "weather";
    }
    return "bad_message";
}

const char* axis_names[kAxes] = {"x", "y", "z"};

double round_to(double v, double step) { return std::round(v / step) * step; }

const char* state_name(RigState s) {
    switch (s) {
        case RigState::Disarmed:
        case RigState::Arming: return "disarmed";
        case RigState::Idle: return "idle";
        case RigState::Homing: return "homing";
        case RigState::Moving: return "moving";
        case RigState::Stopping: return "stopping";
        case RigState::Fault:
        case RigState::EStop: return "fault";
    }
    return "fault";
}

// An error a handler raises: the SCMP error name and a plain sentence.
struct Err {
    const char* error;
    std::string detail;
};

struct Req {
    JsonObjectConst msg;
    JsonObject out;   // fields to add to a successful ack
    Err* err;         // set when refused
    Rig& rig;
    double now;

    bool fail(const char* e, std::string d) {
        *err = {e, std::move(d)};
        return false;
    }
    bool refused(const Result& r) {
        if (r) return true;
        return fail(refusal_name(r.error), r.detail);
    }
    // A number field. Absent: nullopt. `positive`: must be above 0.
    bool number(const char* key, bool positive, std::optional<double>& v) {
        JsonVariantConst x = msg[key];
        v.reset();
        if (x.isNull()) return true;
        if (x.is<bool>() || !(x.is<double>() || x.is<long>()))
            return fail("bad_message", std::string(key) + " must be a number");
        const double d = x.as<double>();
        if (!std::isfinite(d) || (positive && d <= 0))
            return fail("bad_message", std::string(key) + " must be a positive number");
        v = d;
        return true;
    }
    // The axis named in "axis". `optional`: none means every axis (-1).
    bool axis(bool optional, int& index) {
        JsonVariantConst a = msg["axis"];
        index = -1;
        if (a.isNull()) return optional ? true : fail("bad_message", "axis is missing");
        const char* name = a.as<const char*>();
        for (int i = 0; name && i < kAxes; ++i)
            if (std::string_view(name) == axis_names[i] && rig.axis_present(i)) {
                index = i;
                return true;
            }
        return fail("no_axis", std::string("no axis ") + (name ? name : "?"));
    }
};

void axis_info(const Rig& rig, int i, JsonObject o) {
    const AxisLimits& l = rig.limits(i);
    o["axis"] = axis_names[i];
    o["units"] = "mm";
    o["min"] = l.min;
    o["max"] = l.max;
    o["max_velocity"] = l.max_velocity;
    o["max_accel"] = l.max_accel;
    o["needs_homing"] = true;
}

void fill_status(const Rig& rig, JsonObject s) {
    s["t"] = "status";
    s["estop"] = rig.estop();
    s["armed"] = rig.armed();
    s["remote"] = rig.remote();
    const RigConfig& cfg = rig.config();
    if (cfg.wind.enabled) {
        const WindMonitor& w = rig.wind();
        JsonObject wind = s["wind"].to<JsonObject>();
        wind["speed"] = round_to(w.speed(), 0.1);
        wind["gust"] = round_to(w.gust(), 0.1);
        wind["average"] = round_to(w.average(), 0.1);
        wind["level"] = wind_level_name(w.level());
        if (w.holding()) wind["calm_left"] = std::round(w.calm_left());
    }
    JsonArray lines = s["lines"].to<JsonArray>();
    for (const LineStatus& l : rig.lines()) {
        JsonObject o = lines.add<JsonObject>();
        o["line"] = std::string(1, l.name);
        o["tension"] = round_to(l.tension, 0.1);
        o["length"] = round_to(l.length, 0.1);
        o["swing"] = round_to(l.swing, 0.1);
    }
    if (!rig.message().empty()) s["message"] = rig.message();

    const Vec3 p = rig.position(), v = rig.velocity();
    const double pos[kAxes] = {p.x, p.y, p.z}, vel[kAxes] = {v.x, v.y, v.z};
    JsonArray axes = s["axes"].to<JsonArray>();
    for (int i = 0; i < kAxes; ++i) {
        if (!rig.axis_present(i)) continue;
        JsonObject a = axes.add<JsonObject>();
        a["axis"] = axis_names[i];
        a["state"] = state_name(rig.state());
        a["position"] = round_to(pos[i], 0.001);
        a["velocity"] = round_to(vel[i], 0.001);
        if (rig.has_target(i)) a["target"] = round_to(rig.target(i), 0.001);
        else a["target"] = nullptr;
        a["homed"] = rig.homed();
        a["armed"] = rig.armed();
        JsonArray f = a["faults"].to<JsonArray>();
        for (std::uint32_t bit = 1; bit <= faults::kAll; bit <<= 1)
            if (rig.faults() & bit) f.add(faults::name(bit));
        JsonObject lim = a["limits"].to<JsonObject>();
        lim["low"] = rig.homed() && pos[i] <= rig.limits(i).min + 0.01;
        lim["high"] = rig.homed() && pos[i] >= rig.limits(i).max - 0.01;
    }
}

bool do_config(Req& q) {
    int ax;
    if (!q.axis(false, ax)) return false;
    static const char* keys[] = {"units", "min", "max", "max_velocity", "max_accel",
                                 "heartbeat_timeout", "needs_homing"};
    auto get = [&](std::string_view k, JsonObject values) {
        const AxisLimits& l = q.rig.limits(ax);
        if (k == "units") values[k] = "mm";
        else if (k == "min") values[k] = l.min;
        else if (k == "max") values[k] = l.max;
        else if (k == "max_velocity") values[k] = l.max_velocity;
        else if (k == "max_accel") values[k] = l.max_accel;
        else if (k == "heartbeat_timeout") values[k] = q.rig.config().heartbeat_timeout;
        else if (k == "needs_homing") values[k] = true;
        else return false;
        return true;
    };
    JsonVariantConst set = q.msg["set"];
    if (set.is<JsonObjectConst>()) {
        AxisLimits l = q.rig.limits(ax);
        std::optional<double> hb;
        for (JsonPairConst kv : set.as<JsonObjectConst>()) {
            std::string_view k = kv.key().c_str();
            if (k == "units" || k == "needs_homing")
                return q.fail("read_only", std::string(k) + " can't be set");
            JsonVariantConst v = kv.value();
            if (v.is<bool>() || !(v.is<double>() || v.is<long>()))
                return q.fail("bad_message", std::string(k) + " must be a number");
            const double d = v.as<double>();
            if (k == "min") l.min = d;
            else if (k == "max") l.max = d;
            else if (k == "max_velocity") l.max_velocity = d;
            else if (k == "max_accel") l.max_accel = d;
            else if (k == "heartbeat_timeout") hb = d;
            else return q.fail("read_only", std::string(k) + " can't be set");
        }
        if (hb && (*hb <= 0 || *hb > 5)) return q.fail("bad_message", "heartbeat_timeout must be 0 to 5 s");
        if (!q.refused(q.rig.set_limits(ax, l))) return false;
        if (hb) q.rig.set_heartbeat_timeout(*hb);
        JsonObject values = q.out["values"].to<JsonObject>();
        for (JsonPairConst kv : set.as<JsonObjectConst>()) get(kv.key().c_str(), values);
        return true;
    }
    JsonObject values = q.out["values"].to<JsonObject>();
    JsonVariantConst want = q.msg["get"];
    if (want.is<JsonArrayConst>()) {
        for (JsonVariantConst k : want.as<JsonArrayConst>()) {
            const char* name = k.as<const char*>();
            if (!name || !get(name, values)) return q.fail("bad_message", "unknown key");
        }
    } else {
        for (const char* k : keys) get(k, values);
    }
    return true;
}

bool dispatch(std::string_view t, Req& q, const std::string& firmware) {
    Rig& rig = q.rig;
    const bool changes = t == "arm" || t == "home" || t == "move" || t == "jog" ||
                         (t == "config" && q.msg["set"].is<JsonObjectConst>());
    if (changes && !rig.remote())
        return q.fail("local", "Remote control is off at the controller.");

    if (t == "hello") {
        if (!q.msg["scmp"].is<long>() || q.msg["scmp"].as<long>() != kScmpVersion)
            return q.fail("version", "this firmware speaks SCMP 1");
        JsonObject info = q.out["info"].to<JsonObject>();
        info["scmp"] = kScmpVersion;
        info["firmware"] = firmware;
        info["name"] = rig.config().name;
        info["heartbeat_timeout"] = rig.config().heartbeat_timeout;
        info["coupled"] = true;
        JsonArray axes = info["axes"].to<JsonArray>();
        for (int i = 0; i < kAxes; ++i)
            if (rig.axis_present(i)) axis_info(rig, i, axes.add<JsonObject>());
        return true;
    }
    if (t == "status") {
        fill_status(rig, q.out["status"].to<JsonObject>());
        return true;
    }
    int ax = -1;
    if (t == "arm" || t == "disarm" || t == "home" || t == "stop") {
        if (!q.axis(true, ax)) return false;  // coupled: any axis means all
        if (t == "arm" && !rig.config().remote_arm && !rig.armed())
            return q.fail("local", "This rig is set to be armed only at its own screen.");
        if (t == "arm") return q.refused(rig.arm(q.now));
        if (t == "disarm") return q.refused(rig.disarm(q.now));
        if (t == "home") return q.refused(rig.home(q.now));
        std::optional<double> decel;
        if (!q.number("decel", true, decel)) return false;
        return q.refused(rig.stop(decel, q.now));
    }
    if (t == "move") {
        std::optional<double> target, velocity, duration, accel;
        if (!q.axis(false, ax) || !q.number("target", false, target) ||
            !q.number("velocity", true, velocity) || !q.number("duration", true, duration) ||
            !q.number("accel", true, accel))
            return false;
        if (!target) return q.fail("bad_message", "target must be a number");
        Result r = rig.move(ax, *target, velocity, duration, accel, q.now);
        if (!q.refused(r)) return false;
        if (r.late > 0.0005) q.out["late"] = round_to(r.late, 0.001);
        return true;
    }
    if (t == "jog") {
        std::optional<double> v;
        if (!q.axis(false, ax) || !q.number("velocity", false, v)) return false;
        if (!v) return q.fail("bad_message", "velocity must be a number");
        return q.refused(rig.jog(ax, *v, q.now));
    }
    if (t == "config") return do_config(q);
    return q.fail("bad_message", "unknown command");
}

}  // namespace

ScmpServer::ScmpServer(Rig& rig, std::string firmware) : rig_(rig), firmware_(std::move(firmware)) {}

std::string ScmpServer::handle(std::string_view message, double now) {
    rig_.heard(now);
    JsonDocument in;
    std::string out;
    const DeserializationError e = deserializeJson(in, message.data(), message.size());
    last_type_.clear();
    if (e || !in.is<JsonObject>()) {
        JsonDocument ack;
        ack["t"] = "ack";
        ack["seq"] = nullptr;
        ack["ok"] = false;
        ack["error"] = "bad_message";
        serializeJson(ack, out);
        return out;
    }
    JsonObjectConst msg = in.as<JsonObjectConst>();
    const char* t = msg["t"].as<const char*>();
    const std::string_view type = t ? t : "";
    last_type_ = std::string(type);
    if (type == "heartbeat") return "";
    const bool int_seq = msg["seq"].is<long long>() && !msg["seq"].is<bool>();
    const long long seq = int_seq ? msg["seq"].as<long long>() : 0;
    if (type != "hello" && int_seq)
        for (const Answer& a : answered_)
            if (a.used && a.seq == seq) return a.ack;  // sent again: answer again, don't act twice

    JsonDocument extra;
    JsonObject fields = extra.to<JsonObject>();
    Err err{nullptr, {}};
    Req q{msg, fields, &err, rig_, now};
    const bool ok = dispatch(type, q, firmware_);

    JsonDocument ack;
    ack["t"] = "ack";
    if (msg["seq"].isNull()) ack["seq"] = nullptr;
    else ack["seq"] = msg["seq"];
    ack["ok"] = ok;
    if (ok) {
        for (JsonPair kv : fields) ack[kv.key()] = kv.value();
    } else {
        ack["error"] = err.error;
        if (!err.detail.empty()) ack["detail"] = err.detail;
    }
    serializeJson(ack, out);
    if (type == "hello" && ok)
        for (Answer& a : answered_) a.used = false;
    if (int_seq) {
        answered_[next_] = {true, seq, out};
        next_ = (next_ + 1) % static_cast<int>(answered_.size());
    }
    if (ok && type != "status") force_ = true;
    if (ok && type == "config" && msg["set"].is<JsonObjectConst>() && on_config_set) on_config_set();
    return out;
}

std::string ScmpServer::status() const {
    JsonDocument doc;
    fill_status(rig_, doc.to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string ScmpServer::signature() const {
    std::string s;
    s += rig_.estop() ? 'E' : '-';
    s += rig_.armed() ? 'A' : '-';
    s += rig_.remote() ? 'R' : '-';
    s += rig_.homed() ? 'H' : '-';
    s += static_cast<char>('0' + static_cast<int>(rig_.state()));
    s += static_cast<char>('0' + static_cast<int>(rig_.wind().level()));
    s += std::to_string(rig_.faults());
    return s;
}

bool ScmpServer::status_due(double now) {
    const std::string sig = signature();
    const double every = rig_.busy() ? 0.1 : 1.0;
    if (force_ || sig != last_sig_ || now - last_status_ >= every) {
        force_ = false;
        last_sig_ = sig;
        last_status_ = now;
        return true;
    }
    return false;
}

}  // namespace leota
