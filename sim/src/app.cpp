#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

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

// Settings in a file, written whole and renamed into place.
class FileStore : public SettingsStore {
public:
    explicit FileStore(std::string path) : path_(std::move(path)) {}
    bool read(std::string& out) override {
        std::ifstream f(path_);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }
    bool write(const std::string& json) override {
        const std::string tmp = path_ + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!(f << json)) return false;
        }
        return std::rename(tmp.c_str(), path_.c_str()) == 0;
    }

private:
    std::string path_;
};

}  // namespace

SimApp::SimApp(const Options& opt) : opt_(opt) {
    RigConfig defaults = gazebo_config();
    defaults.two_axis = opt_.two_axis;
    defaults.fixed_y = defaults.dock.y;
    if (!opt_.settings_file.empty()) store_ = std::make_unique<FileStore>(opt_.settings_file);
    keeper_ = std::make_unique<SettingsKeeper>(settings_for(defaults), store_.get());
    const std::string said = keeper_->load();
    if (!said.empty()) std::fprintf(stderr, "Settings in %s:\n%s", opt_.settings_file.c_str(), said.c_str());
    build();
}

void SimApp::build() {
#ifdef LEOTA_SIM_UI
    ui_.reset();  // it holds the old rig
#endif
    truth_ = gazebo_config();
    truth_.lines[0].anchor += Vec3{opt_.pulley_error, -opt_.pulley_error, 0};
    if (opt_.real_mass > 0) truth_.mass = opt_.real_mass;
    plant_ = std::make_unique<Plant>(truth_);
    plant_->fit_dock(true);  // the dock's inductive charger, with the ball's battery below
    if (opt_.sensor_error)  // drives and load cells that don't read quite true
        plant_->set_sensors({{{1.015, 2.5, 0.96}, {0.99, -1.8, 1.03}, {1.02, 3.1, 0.98}, {0.985, -2.2, 1.05}}});
    rig_ = std::make_unique<Rig>(keeper_->settings().rig, *plant_);
    scmp_ = std::make_unique<ScmpServer>(*rig_, "flying-leota-sim 0.1.0");
    scmp_->on_config_set = [this] { keeper_->adopt(rig_->config()); };
    snag_ = 0.0;
    last_message_.clear();
#ifdef LEOTA_SIM_UI
    if (!screen_) screen_ = std::make_unique<Screen>();
    ui_ = std::make_unique<Ui>(*rig_, screen_->display(), keeper_.get());
#endif
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
        if (!(status_to_ == from)) {
            log(now, "Show Controller at " + from.text());
#ifdef LEOTA_SIM_UI
            ui_->log(LogKind::Network, "Show Controller connected from " + from.text() + ".");
#endif
        }
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
#ifdef LEOTA_SIM_UI
        ui_->log(LogKind::Show, what);
#endif
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
    const double area = kPi * truth_.ball_radius * truth_.ball_radius / 1e6;
    plant_->push({0.0, 0.5 * 1.2 * v * v * 0.47 * area - snag_, 0.0});
    rig_->update(now);
    // The ball's battery, 60 Wh. Away from the dock the head uses about 10 W. On a working
    // charger, the charger runs the head and puts about 5 W more into the battery (20 W in,
    // 75% of it through), until it's full.
    if (last_step_ > 0) {
        const double dt = now - last_step_;
        const bool charging = plant_->dock_on() && plant_->resting() && !plant_->metal_on_pad();
        const double net = charging ? 5.0 : -10.0;
        ball_charge_ = std::min(100.0, std::max(0.0, ball_charge_ + net * dt / 3600.0 / 60.0 * 100.0));
        plant_->set_ball_full(ball_charge_ >= 99.5);
    }
    last_step_ = now;
    if (rig_->message() != last_message_) {
        last_message_ = rig_->message();
        log(now, last_message_);
    }
#ifdef LEOTA_SIM_UI
    if (now >= next_ui_) {
        next_ui_ = now + 0.05;
        UiLink link;
        link.heard = last_heard_ < 0 ? -1.0 : now - last_heard_;
        link.connected = link.heard >= 0 && link.heard < 1.5;
        link.address = status_to_.set ? status_to_.text() : "";
        link.commands = commands_;
        link.own_address = opt_.host + ":" + std::to_string(opt_.scmp_port);
        link.scmp_port = opt_.scmp_port;
        char clock[8];
        const std::time_t wall = std::time(nullptr);
        std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&wall));
        ui_->update(now, link, clock);
    }
    screen_->run();
#endif
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
        o["hung"] = plant_->hung(i);
        o["hang_kg"] = plant_->hang_kg(i);
        o["hang_height"] = std::round(plant_->hang_height(i) * 10) / 10;
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
    JsonObject dock = d["dock"].to<JsonObject>();
    dock["state"] = dock_state_name(r.dock().state());
    dock["watts"] = std::round(r.dock().watts() * 10) / 10;
    dock["on"] = plant_->dock_on();
    dock["metal"] = plant_->metal_on_pad();
    d["ball_charge"] = std::round(ball_charge_ * 10) / 10;
    JsonObject c = d["config"].to<JsonObject>();
    const RigConfig& cfg = r.config();
    c["ball_radius"] = truth_.ball_radius;
    vec(c["dock"].to<JsonObject>(), truth_.dock);
    JsonObject table = c["table"].to<JsonObject>();
    vec(table["lo"].to<JsonObject>(), truth_.table.lo);
    vec(table["hi"].to<JsonObject>(), truth_.table.hi);
    const Box ws = workspace(cfg);
    JsonObject box = c["workspace"].to<JsonObject>();
    vec(box["lo"].to<JsonObject>(), ws.lo);
    vec(box["hi"].to<JsonObject>(), ws.hi);
    c["two_axis"] = cfg.two_axis;
    c["tension_max"] = cfg.tension_max;
    c["tension_min"] = cfg.tension_min;
#ifdef LEOTA_SIM_UI
    c["touchscreen"] = true;
#endif
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
    } else if (what == "metal") {
        const bool on = in["on"] | true;
        plant_->set_metal_on_pad(on);
        said = on ? "Something metal is put on the dock's pad" : "The metal is taken off the dock's pad";
    } else if (what == "ball_charge") {
        ball_charge_ = std::min(100.0, std::max(0.0, in["percent"] | ball_charge_));
        said = "The ball's battery is set to " + std::to_string(static_cast<int>(ball_charge_)) + "%";
    } else if (what == "snag") {
        snag_ = in["force"] | 0.0;
        said = snag_ > 0 ? "Something snags the ball" : "The snag lets go";
    } else if (what == "cut" || what == "mend") {
        const int i = in["line"] | 0;
        if (i >= 0 && i < kLines) {
            what == "cut" ? plant_->cut_line(i) : plant_->mend_line(i);
            said = std::string(what == "cut" ? "Line " : "Mended line ") + truth_.lines[i].name + (what == "cut" ? " cut" : "");
        }
    } else if (what == "alarm") {
        const int i = in["line"] | 0;
        const bool on = in["on"] | true;
        if (i >= 0 && i < kLines) {
            plant_->set_alarm(i, on);
            said = std::string("Drive ") + truth_.lines[i].name + (on ? " alarm on" : " alarm cleared");
        }
    } else if (what == "hang" || what == "hook") {
        const int i = in["line"] | 0;
        if (i >= 0 && i < kLines) {
            const double kg = in["kg"] | 0.0;
            if (what == "hang") plant_->hang(i, kg);
            else plant_->hook(i);
            said = std::string("Line ") + truth_.lines[i].name +
                   (what == "hook" ? " hooked back onto the ball"
                                   : " unhooked" + (kg > 0 ? ", with " + std::to_string(kg).substr(0, 4) + " kg on it"
                                                           : std::string(", hanging free")));
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

HttpResponse SimApp::http(const HttpRequest& full, double now) {
    HttpResponse res;
    HttpRequest req = full;
    std::string query;
    if (const size_t q = req.path.find('?'); q != std::string::npos) {
        query = req.path.substr(q + 1);
        req.path.resize(q);
    }
#ifdef LEOTA_SIM_UI
    if (req.path == "/api/screen") {
        // ?after=N: only if the screen has changed since frame N.
        const unsigned after = query.rfind("after=", 0) == 0 ? static_cast<unsigned>(std::stoul(query.substr(6))) : ~0u;
        res.headers = "X-Frame: " + std::to_string(screen_->frame()) + "\r\n";
        if (after != ~0u && screen_->frame() <= after) {
            res.status = 204;
            return res;
        }
        res.type = "image/png";
        res.body = screen_->png();
        return res;
    }
    if (req.path == "/api/touch" && req.method == "POST") {
        JsonDocument in;
        if (!deserializeJson(in, req.body))
            screen_->touch(in["x"] | 0, in["y"] | 0, in["down"] | false);
        res.type = "application/json";
        res.body = R"({"ok":true})";
        return res;
    }
#else
    (void)query;
#endif
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
