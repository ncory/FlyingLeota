// Setup's settings in flash: one JSON blob in NVS (namespace "leota", key "settings").
#pragma once

#include "leota/settings.hpp"

namespace fw {

class NvsStore : public leota::SettingsStore {
public:
    bool read(std::string& json) override;
    bool write(const std::string& json) override;
    bool erase();  // back to the standard settings at the next start
};

}  // namespace fw
