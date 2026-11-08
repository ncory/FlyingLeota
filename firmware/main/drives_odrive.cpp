// ODrive S1s over CAN, with ODrive's "CANSimple" protocol: 11-bit IDs of node_id << 5 | command.
//
// Each control cycle sends Set_Input_Pos to each line's ODrive (position control,
// passthrough input). Each ODrive sends its heartbeat (state, errors) and encoder
// estimates by itself; set those cyclic messages in the ODrive's own configuration
// (10 ms is plenty). Arming puts each into closed-loop control; disarming, idle. The
// brakes are separate, on the brake relay, as with the steppers.
//
// Untested on hardware: Phase 1 checks the link with one ODrive (console `drives`).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "drives.hpp"
#include "sdkconfig.h"

#ifdef CONFIG_LEOTA_DRIVES_ODRIVE

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"

namespace fw {

namespace {

const char* TAG = "odrive";

enum Cmd : std::uint32_t {
    kHeartbeat = 0x01,
    kSetAxisState = 0x07,
    kEncoderEstimates = 0x09,
    kSetControllerMode = 0x0B,
    kSetInputPos = 0x0C,
    kClearErrors = 0x18,
};
constexpr std::uint32_t kStateIdle = 1, kStateClosedLoop = 8;
constexpr std::uint32_t kControlPosition = 3, kInputPassthrough = 1;
constexpr std::int64_t kStaleUs = 100'000;  // no heartbeat for 100 ms: treat as a fault
constexpr int kTxQueue = 32;                // the driver's queue of frames to send
constexpr int kTxSlots = 2 * kTxQueue;      // their buffers: a slot is reused only long after it went

class OdriveDrives : public Drives {
public:
    OdriveDrives(BoardIo& io, LoadCells& cells) : io_(io), cells_(cells) {
        mm_per_turn_ = CONFIG_LEOTA_ODRIVE_UM_PER_TURN / 1000.0;
        if (CONFIG_LEOTA_CAN_TX_PIN < 0 || CONFIG_LEOTA_CAN_RX_PIN < 0) {
            ESP_LOGE(TAG, "No CAN pins set: the drives can't be reached.");
            return;
        }
        twai_onchip_node_config_t cfg = {};
        cfg.io_cfg.tx = static_cast<gpio_num_t>(CONFIG_LEOTA_CAN_TX_PIN);
        cfg.io_cfg.rx = static_cast<gpio_num_t>(CONFIG_LEOTA_CAN_RX_PIN);
        cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
        cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
        cfg.bit_timing.bitrate = CONFIG_LEOTA_CAN_BITRATE;
        cfg.tx_queue_depth = kTxQueue;
        cfg.fail_retry_cnt = 3;
        ESP_ERROR_CHECK(twai_new_node_onchip(&cfg, &node_));
        twai_event_callbacks_t cbs = {};
        cbs.on_rx_done = on_rx;
        ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_, &cbs, this));
        ESP_ERROR_CHECK(twai_node_enable(node_));
        for (int i = 0; i < leota::kLines; ++i)
            send(i, kSetControllerMode, u32pair(kControlPosition, kInputPassthrough), 8);
        ESP_LOGI(TAG, "CAN at %d bit/s, nodes %d to %d, %.3f mm per turn.", CONFIG_LEOTA_CAN_BITRATE,
                 CONFIG_LEOTA_ODRIVE_NODE_BASE, CONFIG_LEOTA_ODRIVE_NODE_BASE + 3, mm_per_turn_);
    }

    const char* kind() const override { return "ODrive"; }

    void set_enabled(bool on) override {
        io_.set_drives_enabled(on);
        for (int i = 0; i < leota::kLines; ++i) {
            if (on) send(i, kClearErrors, {}, 1);
            std::uint8_t d[8] = {};
            const std::uint32_t state = on ? kStateClosedLoop : kStateIdle;
            std::memcpy(d, &state, 4);
            send(i, kSetAxisState, d, 4);
        }
    }
    void set_brakes_released(bool on) override { io_.set_brakes_released(on); }
    bool estop_open() override { return io_.estop_open(); }

    void command(const std::array<double, leota::kLines>& mm) override {
        if (!io_.drives_enabled()) return;
        for (int i = 0; i < leota::kLines; ++i) {
            const float turns = static_cast<float>(line_sign(i) * mm[i] / mm_per_turn_);
            std::uint8_t d[8] = {};
            std::memcpy(d, &turns, 4);  // then velocity and torque feed-forward: 0
            send(i, kSetInputPos, d, 8);
        }
    }

    std::array<leota::LineFeedback, leota::kLines> feedback() override {
        std::array<leota::LineFeedback, leota::kLines> fb{};
        const std::int64_t now = esp_timer_get_time();
        const bool cells_stale = cells_.fitted() && cells_.stale_ms() > 250;
        portENTER_CRITICAL(&mux_);
        const auto nodes = nodes_;
        portEXIT_CRITICAL(&mux_);
        for (int i = 0; i < leota::kLines; ++i) {
            const Node& n = nodes[i];
            const bool fresh = n.heartbeat_us && now - n.heartbeat_us < kStaleUs;
            fb[i].position = line_sign(i) * n.turns * mm_per_turn_;
            fb[i].tension = cells_.newtons(i);
            fb[i].alarm = n.error != 0 || (io_.drives_enabled() && !fresh) || (cells_stale && cells_.fitted(i));
            fb[i].ready = fresh && n.state == kStateClosedLoop && !fb[i].alarm;
        }
        return fb;
    }

    std::string describe() override {
        std::string s;
        char buf[160];
        const std::int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&mux_);
        const auto nodes = nodes_;
        portEXIT_CRITICAL(&mux_);
        for (int i = 0; i < leota::kLines; ++i) {
            const Node& n = nodes[i];
            if (!n.heartbeat_us) {
                std::snprintf(buf, sizeof buf, "%c: node %d never heard\n", 'A' + i, node_id(i));
            } else {
                std::snprintf(buf, sizeof buf, "%c: node %d, state %lu, errors 0x%08lx, %.3f turns, heard %lld ms ago\n",
                              'A' + i, node_id(i), static_cast<unsigned long>(n.state),
                              static_cast<unsigned long>(n.error), n.turns,
                              static_cast<long long>((now - n.heartbeat_us) / 1000));
            }
            s += buf;
        }
        std::snprintf(buf, sizeof buf, "Frames that couldn't be queued: %lu\n", static_cast<unsigned long>(dropped_));
        return s + buf;
    }

private:
    struct Node {
        std::int64_t heartbeat_us = 0;
        std::uint32_t error = 0, state = 0;
        float turns = 0.0f;
    };
    struct Slot {
        twai_frame_t frame{};
        std::uint8_t data[8]{};
    };

    static int node_id(int line) { return CONFIG_LEOTA_ODRIVE_NODE_BASE + line; }
    static const std::uint8_t* u32pair(std::uint32_t a, std::uint32_t b) {
        static std::uint8_t d[8];
        std::memcpy(d, &a, 4);
        std::memcpy(d + 4, &b, 4);
        return d;
    }

    void send(int line, std::uint32_t cmd, const std::uint8_t* data, int len) {
        if (!node_) return;
        // The driver keeps a pointer to each frame until it has gone, so every frame
        // gets its own slot, and there are twice as many slots as the queue holds.
        Slot& s = slots_[next_slot_];
        next_slot_ = (next_slot_ + 1) % kTxSlots;
        std::memset(s.data, 0, sizeof s.data);
        if (data) std::memcpy(s.data, data, len);
        s.frame = {};
        s.frame.header.id = (static_cast<std::uint32_t>(node_id(line)) << 5) | cmd;
        s.frame.header.dlc = static_cast<std::uint16_t>(len);
        s.frame.buffer = s.data;
        s.frame.buffer_len = static_cast<size_t>(len);
        if (twai_node_transmit(node_, &s.frame, 0) != ESP_OK) ++dropped_;
    }

    static bool on_rx(twai_node_handle_t handle, const twai_rx_done_event_data_t*, void* ctx) {
        auto* self = static_cast<OdriveDrives*>(ctx);
        std::uint8_t data[8] = {};
        twai_frame_t f = {};
        f.buffer = data;
        f.buffer_len = sizeof data;
        if (twai_node_receive_from_isr(handle, &f) != ESP_OK) return false;
        const int line = static_cast<int>(f.header.id >> 5) - CONFIG_LEOTA_ODRIVE_NODE_BASE;
        if (line < 0 || line >= leota::kLines) return false;
        const std::uint32_t cmd = f.header.id & 0x1F;
        portENTER_CRITICAL_ISR(&self->mux_);
        Node& n = self->nodes_[line];
        if (cmd == kHeartbeat) {
            std::memcpy(&n.error, data, 4);
            n.state = data[4];
            n.heartbeat_us = esp_timer_get_time();
        } else if (cmd == kEncoderEstimates) {
            std::memcpy(&n.turns, data, 4);
        }
        portEXIT_CRITICAL_ISR(&self->mux_);
        return false;
    }

    BoardIo& io_;
    LoadCells& cells_;
    twai_node_handle_t node_ = nullptr;
    double mm_per_turn_ = 1.0;
    std::array<Node, leota::kLines> nodes_{};
    std::array<Slot, kTxSlots> slots_{};
    int next_slot_ = 0;
    std::uint32_t dropped_ = 0;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace

std::unique_ptr<Drives> make_drives(const leota::RigConfig&, BoardIo& io, LoadCells& cells) {
    return std::make_unique<OdriveDrives>(io, cells);
}

}  // namespace fw

#endif
