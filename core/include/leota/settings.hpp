// The settings a person may change on site, from the touchscreen's Setup screen: how the
// rig flies, its safety limits, the wind rules, where the pulleys and the dock are, the
// network and the Setup PIN. They are kept as JSON in the controller's flash (NVS on the
// ESP32-P4, a file in the PC simulator) through a SettingsStore.
//
// One table (settings_fields) lists every setting with its label, unit and range. The
// JSON, the checks, the change log and the screen's editor all come from it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "leota/config.hpp"
#include "leota/rig.hpp"

namespace leota {

struct Network {
    bool dhcp = true;
    std::uint32_t address = 0xC0A80132;  // 192.168.1.50, when not from DHCP
    std::uint32_t netmask = 0xFFFFFF00;  // 255.255.255.0
    std::uint32_t gateway = 0xC0A80101;  // 192.168.1.1
    int scmp_port = 7720;
};

struct Settings {
    RigConfig rig;
    Network net;
    std::string pin = "1234";  // unlocks Setup
};

std::string address_text(std::uint32_t a);                    // "192.168.1.50"
bool parse_address(const std::string& text, std::uint32_t& out);

struct Field {
    enum class Kind { Number, Toggle, Address };
    const char* key;    // "wind.red": the name in the JSON
    const char* group;  // "Wind": the Setup screen's group
    const char* label;  // "Red at gust"
    const char* unit;   // "m/s", or ""
    Kind kind;
    double min, max;    // allowed values (Number)
    int decimals;       // shown and typed to this many places
    const char* help;   // one sentence for the editor
    double (*get)(const Settings&);
    void (*set)(Settings&, double);
    bool (*shown)(const Settings&);  // nullptr: always
};

// Every setting, in the order the screen lists them.
const std::vector<Field>& settings_fields();
const Field* find_field(const std::string& key);
// The value as the screen shows it: "11.0 m/s", "On", "192.168.1.50".
std::string field_text(const Field& f, const Settings& s);
std::string field_value_text(const Field& f, double v);  // without the unit
// Whether a value is allowed for a field; if not, `why` says so in a sentence.
bool field_allows(const Field& f, double v, std::string* why = nullptr);

// Settings from a rig config, with the network and PIN at their defaults.
Settings settings_for(const RigConfig& cfg);

// Problems with a whole set of settings, one sentence a line. Empty when fine.
std::string check_settings(const Settings& s);

// What differs, a sentence each: "Red at gust: 11.0 → 12.0 m/s."
std::vector<std::string> settings_changes(const Settings& from, const Settings& to);

// The JSON kept in flash: {"version": 1, "settings": {"wind.red": 11, ...}, "pin": "1234"}.
std::string settings_to_json(const Settings& s);
// Reads stored JSON over `s`. A value that is missing, wrong or out of range keeps the
// value already in `s`. Returns what was wrong, one sentence a line; empty when all is well.
std::string settings_from_json(const std::string& json, Settings& s);

// Where the settings are kept. The firmware writes NVS; the simulator a file.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual bool read(std::string& json) = 0;   // false: nothing stored yet
    virtual bool write(const std::string& json) = 0;
};

// Holds the current settings, applies changes to the rig and keeps them in the store.
class SettingsKeeper {
public:
    // `store` may be nullptr: then changes last until the controller restarts.
    SettingsKeeper(const Settings& defaults, SettingsStore* store = nullptr);

    // Reads the store. Call before making the Rig, and give it settings().rig.
    // Returns what was wrong with what was stored ("" when fine or nothing stored).
    std::string load();
    const Settings& settings() const { return current_; }
    const Settings& defaults() const { return defaults_; }

    // Checks the new settings, gives them to the rig and stores them. Refused while the
    // rig is armed. The rig decides whether it needs homing again.
    Result change(const Settings& next, Rig& rig);
    // After SCMP `config` changed the rig's limits: keeps them too.
    bool adopt(const RigConfig& cfg);
    bool stored_ok() const { return stored_ok_; }  // the last write worked (or no store)
    bool has_store() const { return store_ != nullptr; }
    int revision() const { return revision_; }  // goes up with every change

private:
    bool save();

    Settings defaults_, current_;
    SettingsStore* store_;
    bool stored_ok_ = true;
    int revision_ = 0;
};

}  // namespace leota
