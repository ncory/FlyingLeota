#include "app.hpp"

#include <cmath>
#include <cstdio>

#include "ArduinoJson.h"
#include "web_page.hpp"

namespace leota_sim {

using namespace leota;

namespace {

constexpr double kPi = 3.14159265358979;
const char* kAxisNames[kAxes] = {"x", "y", "z"};

std::string clock_text(double s) {
    char buf[32];
    const int m = static_cast<int>(s / 60);
    std::snprintf(buf, sizeof buf, "%02d:%04.1f", m, s - m * 60);
    return buf;
}

const char* state_text(RigState s) {
    switch (s) {
        case RigState::Disarmed: return "DISARMED";
        case RigState::Arming: return "ARMING";
        case RigState::Idle: return "ARMED";
        case RigState::Homing: return "HOMING";
        case RigState::Moving: return "MOVING";
        case RigState::Stopping: return "STOPPING";
        case RigState::Fault: return "FAULT";
        case RigState::EStop: return "E-STOP";
    }
    return "?";
}

void vec(JsonObject o, const Vec3& v) {
    o["x"] = std::round(v.x * 10) / 10;
    o["y"] = std::round(v.y * 10) / 10;
    o["z"] = std::round(v.z * 10) / 10;
}

const char* refusal_text(const Result& r) { return r ? "ok" : r.detail.c_str(); }

}  // namespace

SimApp::SimApp(const Options& opt) : opt_(opt) { build(); }

void SimApp::build() {
    cfg_ = gazebo_config();
    cfg_.two_axis = opt_.two_axis;
    truth_ = cfg_;
    truth_.lines[0].anchor += Vec3{opt_.pulley_error, -opt_.pulley_error, 0};
    plant_ = std::make_unique<Plant>(truth_);
    rig_ = std::make_unique<Rig>(cfg_, *plant_);
    scmp_ = std::make_unique<ScmpServer>(*rig_, "flying-leota-sim 0.1.0");
    snag_ = 0.0;
    last_message_.clear();
}

double SimApp::wind_now(double now) const {
    if (opt_.wind <= 0) return 0.0;
    const double g = opt_.gusty;
    const double wave = 0.45 * std::sin(2 * kPi * now / 17.0) + 0.3 * std::sin(2 * kPi * now / 5.3 + 1.0) +
                        0.15 * std::sin(2 * kPi * now / 2.1 + 2.0);
    return std::max(0.0, opt_.wind * (1.0 + g * wave));
}

void SimApp::log(double now, const std::string& what) {
    log_.push_front(clock_text(now - started_) + "  " + what);
    while (log_.size() > 40) log_.pop_back();
}

std::string SimApp::datagram(const std::string& msg, const Address& from, double now) {
    const std::string reply = scmp_->handle(msg, now);
    const std::string& t = scmp_->last_type();
    if (t == "hello" || t == "heartbeat") {
        if (!(status_to_ == from)) log(now, "Show Controller at " + from.text());
        status_to_ = from;
    }
    last_heard_ = now;
    if (t != "heartbeat" && t != "status") {
        ++commands_;
        JsonDocument in, ack;
        deserializeJson(in, msg);
        deserializeJson(ack, reply);
        std::string what = "SCMP " + (t.empty() ? std::string("?") : t);
        if (in["axis"].is<const char*>()) what += std::string(" ") + in["axis"].as<const char*>();
        if (!in["target"].isNull()) what += " to " + std::to_string(static_cast<long>(in["target"].as<double>()));
        if (ack["ok"] == true) {
            what += ": ok";
            if (!ack["late"].isNull()) what += ", " + std::to_string(ack["late"].as<double>()).substr(0, 4) + " s late";
        } else {
            what += std::string(": refused, ") + (ack["error"].as<const char*>() ? ack["error"].as<const char*>() : "?");
        }
        log(now, what);
    }
    return reply;
}

std::string SimApp::step(double now) {
    if (started_ == 0.0) started_ = now;
    if (now >= next_wind_) {
        next_wind_ = now + 0.5;
        if (sensor_on_) rig_->wind_sample(wind_now(now), now);
    }
    // The wind blows in through the open front (toward +y) and pushes on the ball.
    const double v = opt_.push ? wind_now(now) : 0.0;
    const double area = kPi * cfg_.ball_radius * cfg_.ball_radius / 1e6;
    plant_->push({0.0, 0.5 * 1.2 * v * v * 0.47 * area - snag_, 0.0});
    rig_->update(now);
    if (rig_->message() != last_message_) {
        last_message_ = rig_->message();
        log(now, last_message_);
    }
    if (status_to_.set && scmp_->status_due(now)) return scmp_->status();
    return "";
}

std::string SimApp::state_json(double now) {
    JsonDocument d;
    const Rig& r = *rig_;
    d["state"] = state_text(r.state());
    d["armed"] = r.armed();
    d["homed"] = r.homed();
    d["remote"] = r.remote();
    d["estop"] = r.estop();
    d["parking"] = r.parking();
    d["message"] = r.message();
    JsonArray faults = d["faults"].to<JsonArray>();
    for (std::uint32_t bit = 1; bit <= faults::kAll; bit <<= 1)
        if (r.faults() & bit) faults.add(faults::name(bit));
    vec(d["position"].to<JsonObject>(), r.position());
    vec(d["ball"].to<JsonObject>(), plant_->ball());
    d["resting"] = plant_->resting();
    d["fell"] = plant_->fell();
    JsonArray targets = d["targets"].to<JsonArray>();
    for (int a = 0; a < kAxes; ++a) {
        if (r.has_target(a)) targets.add(r.target(a));
        else targets.add(nullptr);
    }
    const auto lines = r.lines();
    const auto real = plant_->tensions();
    JsonArray ls = d["lines"].to<JsonArray>();
    const Vec3 ball = plant_->ball(), rot = plant_->rotation(), model = r.lean();
    const double deg = 180.0 / kPi;
    JsonArray rv = d["rotation"].to<JsonArray>();
    rv.add(rot.x);
    rv.add(rot.y);
    rv.add(rot.z);
    d["tilt"] = std::round(std::hypot(rot.x, rot.y) * deg * 100) / 100;
    d["turn"] = std::round(rot.z * deg * 100) / 100;
    d["model_tilt"] = std::round(std::hypot(model.x, model.y) * deg * 100) / 100;
    for (int i = 0; i < kLines; ++i) {
        JsonObject o = ls.add<JsonObject>();
        o["name"] = std::string(1, lines[i].name);
        o["tension"] = std::round(lines[i].tension * 10) / 10;
        o["expected"] = std::round(lines[i].expected * 10) / 10;
        o["trim"] = std::round(lines[i].trim * 100) / 100;
        o["swing"] = std::round(lines[i].swing * 100) / 100;
        o["real"] = std::round(real[i] * 10) / 10;
        o["cut"] = plant_->line_cut(i);
        o["alarm"] = plant_->alarm(i);
        vec(o["anchor"].to<JsonObject>(), truth_.lines[i].anchor);
        vec(o["attach"].to<JsonObject>(), ball + rotate(rot, truth_.lines[i].attach));
    }
    const WindMonitor& w = r.wind();
    JsonObject wind = d["wind"].to<JsonObject>();
    wind["speed"] = std::round(w.speed() * 10) / 10;
    wind["gust"] = std::round(w.gust() * 10) / 10;
    wind["average"] = std::round(w.average() * 10) / 10;
    wind["level"] = wind_level_name(w.level());
    wind["calm_left"] = std::round(w.calm_left());
    wind["sensor"] = sensor_on_;
    wind["base"] = opt_.wind;
    wind["gusty"] = opt_.gusty;
    wind["push"] = opt_.push;
    JsonObject sc = d["show_controller"].to<JsonObject>();
    sc["address"] = status_to_.set ? status_to_.text() : "";
    sc["heard"] = last_heard_ < 0 ? -1.0 : std::round((now - last_heard_) * 10) / 10;
    sc["commands"] = commands_;
    sc["scmp_port"] = opt_.scmp_port;
    d["snag"] = snag_;
    JsonObject c = d["config"].to<JsonObject>();
    c["ball_radius"] = cfg_.ball_radius;
    vec(c["dock"].to<JsonObject>(), cfg_.dock);
    JsonObject table = c["table"].to<JsonObject>();
    vec(table["lo"].to<JsonObject>(), cfg_.table.lo);
    vec(table["hi"].to<JsonObject>(), cfg_.table.hi);
    const Box ws = workspace(cfg_);
    JsonObject box = c["workspace"].to<JsonObject>();
    vec(box["lo"].to<JsonObject>(), ws.lo);
    vec(box["hi"].to<JsonObject>(), ws.hi);
    c["two_axis"] = cfg_.two_axis;
    c["tension_max"] = cfg_.tension_max;
    c["tension_min"] = cfg_.tension_min;
    JsonArray lg = d["log"].to<JsonArray>();
    for (const std::string& s : log_) lg.add(s);
    std::string out;
    serializeJson(d, out);
    return out;
}

std::string SimApp::action(const std::string& body, double now) {
    JsonDocument in;
    if (deserializeJson(in, body)) return R"({"ok":false,"detail":"bad JSON"})";
    const std::string what = in["do"] | "";
    Rig& r = *rig_;
    Result res;
    std::string said;
    if (what == "arm") { res = r.arm(now); said = "Screen: arm"; }
    else if (what == "disarm") { res = r.disarm(now); said = "Screen: disarm"; }
    else if (what == "home") { res = r.home(now); said = "Screen: home"; }
    else if (what == "park") { res = r.park(now); said = "Screen: park"; }
    else if (what == "stop") { res = r.stop(std::nullopt, now); said = "Screen: STOP"; }
    else if (what == "remote") { r.set_remote(in["on"] | true, now); }
    else if (what == "jog") {
        res = r.jog(in["axis"] | 0, in["velocity"] | 0.0, now);
    } else if (what == "goto") {
        const double dur = in["duration"] | 3.0;
        said = "Screen: go to";
        for (int a = 0; a < kAxes; ++a) {
            if (!r.axis_present(a) || in[kAxisNames[a]].isNull()) continue;
            res = r.move(a, in[kAxisNames[a]].as<double>(), std::nullopt, dur, std::nullopt, now);
            if (!res) break;
        }
    } else if (what == "estop") {
        const bool on = in["on"] | true;
        plant_->set_estop(on);
        said = on ? "E-stop pressed" : "E-stop released and reset";
    } else if (what == "wind") {
        if (!in["speed"].isNull()) opt_.wind = in["speed"].as<double>();
        if (!in["gusty"].isNull()) opt_.gusty = in["gusty"].as<double>();
        if (!in["push"].isNull()) opt_.push = in["push"].as<bool>();
        if (!in["sensor"].isNull()) {
            sensor_on_ = in["sensor"].as<bool>();
            said = sensor_on_ ? "Wind sensor reconnected" : "Wind sensor unplugged";
        }
    } else if (what == "snag") {
        snag_ = in["force"] | 0.0;
        said = snag_ > 0 ? "Something snags the ball" : "The snag lets go";
    } else if (what == "cut" || what == "mend") {
        const int i = in["line"] | 0;
        if (i >= 0 && i < kLines) {
            what == "cut" ? plant_->cut_line(i) : plant_->mend_line(i);
            said = std::string(what == "cut" ? "Line " : "Mended line ") + cfg_.lines[i].name + (what == "cut" ? " cut" : "");
        }
    } else if (what == "alarm") {
        const int i = in["line"] | 0;
        const bool on = in["on"] | true;
        if (i >= 0 && i < kLines) {
            plant_->set_alarm(i, on);
            said = std::string("Drive ") + cfg_.lines[i].name + (on ? " alarm on" : " alarm cleared");
        }
    } else if (what == "noise") {
        plant_->tension_noise(in["n"] | 0.0);
    } else if (what == "reset") {
        build();
        said = "Simulator reset: ball on the dock, lines slack";
    } else {
        return R"({"ok":false,"detail":"unknown action"})";
    }
    if (!said.empty()) log(now, said + (res ? "" : std::string(": ") + refusal_text(res)));
    JsonDocument out;
    out["ok"] = static_cast<bool>(res);
    if (!res) out["detail"] = res.detail;
    std::string s;
    serializeJson(out, s);
    return s;
}

HttpResponse SimApp::http(const HttpRequest& req, double now) {
    HttpResponse res;
    if (req.path == "/" || req.path == "/index.html") {
        res.type = "text/html; charset=utf-8";
        res.body = kWebPage;
    } else if (req.path == "/api/state") {
        res.type = "application/json";
        res.body = state_json(now);
    } else if (req.path == "/api/action") {
        if (req.method != "POST") {
            res.status = 405;
            res.body = "POST only";
        } else {
            res.type = "application/json";
            res.body = action(req.body, now);
        }
    } else {
        res.status = 404;
        res.body = "Not here.";
    }
    return res;
}

}  // namespace leota_sim
