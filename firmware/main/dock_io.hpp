// The dock's charger on the controller's side: a switch output for the inductive
// transmitter's 24 V, and a TI INA226 power monitor in its feed (on its own I²C bus).
// The core's DockCharger decides when it's on and what its power means.
#pragma once

namespace fw {

class DockIo {
public:
    void start();
    bool fitted() const { return fitted_; }
    void set_power(bool on);
    bool power() const { return on_; }
    double watts() const { return watts_; }  // from the INA226, about 10 times a second
    bool reading() const { return ok_; }     // the INA226 is answering

private:
    static void task(void* self);
    void run();

    bool fitted_ = false;
    volatile bool on_ = false, ok_ = false;
    volatile double watts_ = 0.0;
    void* dev_ = nullptr;  // i2c_master_dev_handle_t
};

}  // namespace fw
