// Closed-loop steppers on step/dir, one RMT channel per line (the P4 has four).
//
// Every control cycle the rig commands a position for each line. The steps needed to
// reach it go out over the next millisecond, evenly spaced, as one RMT transmission:
// a low gap and a high pulse per step, padded to exactly 1 ms. The direction pin is set
// first; each chunk starts with at least half a step period low, which covers the
// drive's direction setup time. Up to CONFIG_LEOTA_MAX_STEP_KHZ steps per millisecond go
// out; any more wait for the next cycle, so a step is never lost, only late.
//
// The drives close their own position loop; the controller counts the steps it has sent
// as the line's position. A drive that can't follow raises its alarm output.
//
// Untested on hardware: Phase 1 on the bench checks the pulses on a logic analyzer
// while the screen and network are busy (console `steps`, `timing`).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "drives.hpp"
#include "sdkconfig.h"

#ifdef CONFIG_LEOTA_DRIVES_STEPDIR

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

namespace fw {

namespace {

const char* TAG = "stepdir";

constexpr std::uint32_t kTicksPerSecond = 10'000'000;  // RMT resolution: 0.1 us
constexpr std::uint32_t kChunkTicks = 10'000;          // one control cycle, 1 ms
constexpr int kMaxSteps = CONFIG_LEOTA_MAX_STEP_KHZ;   // per chunk
constexpr int kBuffers = 4;                            // chunks in flight per line

class StepDirDrives : public Drives {
public:
    StepDirDrives(BoardIo& io, LoadCells& cells) : io_(io), cells_(cells) {
        steps_per_mm_ = CONFIG_LEOTA_STEPS_PER_REV / (CONFIG_LEOTA_DRUM_UM_PER_REV / 1000.0);
        pulse_ticks_ = static_cast<std::uint16_t>(
            std::max(5, CONFIG_LEOTA_STEP_PULSE_NS / 100));
        const int step[leota::kLines] = {CONFIG_LEOTA_STEP_PIN_A, CONFIG_LEOTA_STEP_PIN_B, CONFIG_LEOTA_STEP_PIN_C,
                                         CONFIG_LEOTA_STEP_PIN_D};
        const int dir[leota::kLines] = {CONFIG_LEOTA_DIR_PIN_A, CONFIG_LEOTA_DIR_PIN_B, CONFIG_LEOTA_DIR_PIN_C,
                                        CONFIG_LEOTA_DIR_PIN_D};
        for (int i = 0; i < leota::kLines; ++i) {
            Line& l = lines_[i];
            l.dir_pin = dir[i];
            if (step[i] < 0 || dir[i] < 0) {
                ESP_LOGW(TAG, "Line %c has no step or direction pin: it won't move.", 'A' + i);
                continue;
            }
            gpio_config_t c = {};
            c.pin_bit_mask = 1ULL << dir[i];
            c.mode = GPIO_MODE_OUTPUT;
            ESP_ERROR_CHECK(gpio_config(&c));
            rmt_tx_channel_config_t ch = {};
            ch.gpio_num = static_cast<gpio_num_t>(step[i]);
            ch.clk_src = RMT_CLK_SRC_DEFAULT;
            ch.resolution_hz = kTicksPerSecond;
            ch.mem_block_symbols = 48;
            ch.trans_queue_depth = kBuffers;
            ESP_ERROR_CHECK(rmt_new_tx_channel(&ch, &l.channel));
            rmt_copy_encoder_config_t enc = {};
            ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc, &l.encoder));  // one each: encoders keep state
            ESP_ERROR_CHECK(rmt_enable(l.channel));
        }
        ESP_LOGI(TAG, "%.3f steps per mm, at most %d kHz, %u ns pulses.", steps_per_mm_, kMaxSteps,
                 static_cast<unsigned>(pulse_ticks_) * 100);
    }

    const char* kind() const override { return "step/dir"; }

    void set_enabled(bool on) override { io_.set_drives_enabled(on); }
    void set_brakes_released(bool on) override { io_.set_brakes_released(on); }
    bool estop_open() override { return io_.estop_open(); }

    void command(const std::array<double, leota::kLines>& mm) override {
        if (!io_.drives_enabled()) return;  // nothing may move: the drives aren't holding
        for (int i = 0; i < leota::kLines; ++i) send(i, mm[i]);
    }

    std::array<leota::LineFeedback, leota::kLines> feedback() override {
        std::array<leota::LineFeedback, leota::kLines> fb{};
        const bool cells_stale = cells_.fitted() && cells_.stale_ms() > 250;
        for (int i = 0; i < leota::kLines; ++i) {
            fb[i].position = lines_[i].sent / steps_per_mm_;
            fb[i].tension = cells_.newtons(i);
            // A load cell that stops answering is as bad as a drive fault: stop.
            fb[i].alarm = io_.alarm(i) || (cells_stale && cells_.fitted(i));
            fb[i].ready = io_.drives_enabled() && !fb[i].alarm;
        }
        return fb;
    }

    bool can_bench() const override { return true; }
    void bench_command(int line, double mm) override { send(line, mm); }

    std::string describe() override {
        std::string s;
        char buf[160];
        for (int i = 0; i < leota::kLines; ++i) {
            const Line& l = lines_[i];
            std::snprintf(buf, sizeof buf, "%c: %s, %lld steps sent (%.2f mm), %lu chunks, %lu late%s\n", 'A' + i,
                          l.channel ? "fitted" : "no pins", static_cast<long long>(l.sent), l.sent / steps_per_mm_,
                          static_cast<unsigned long>(l.chunks), static_cast<unsigned long>(l.late),
                          io_.alarm(i) ? ", ALARM" : "");
            s += buf;
        }
        return s;
    }

private:
    struct Line {
        rmt_channel_handle_t channel = nullptr;
        rmt_encoder_handle_t encoder = nullptr;
        int dir_pin = -1;
        std::int64_t sent = 0;  // steps, in the line's own direction
        int dir = 0;
        int next_buffer = 0;
        rmt_symbol_word_t buf[kBuffers][kMaxSteps + 1];
        std::uint32_t chunks = 0, late = 0;
    };

    void send(int i, double mm) {
        Line& l = lines_[i];
        if (!l.channel) return;
        const std::int64_t want = std::llround(mm * steps_per_mm_);
        std::int64_t n = want - l.sent;
        if (n == 0) return;
        const int dir = n > 0 ? 1 : -1;
        int count = static_cast<int>(std::min<std::int64_t>(std::llabs(n), kMaxSteps));
        if (dir != l.dir) {
            gpio_set_level(static_cast<gpio_num_t>(l.dir_pin), (dir * line_sign(i)) > 0 ? 1 : 0);
            l.dir = dir;
        }
        rmt_symbol_word_t* s = l.buf[l.next_buffer];
        const std::uint32_t period = kChunkTicks / count;
        std::uint32_t t = 0;
        for (int k = 0; k < count; ++k) {
            const std::uint32_t start = period / 2 + k * period;  // the pulse's rising edge
            s[k].level0 = 0;
            s[k].duration0 = static_cast<std::uint16_t>(start - t);
            s[k].level1 = 1;
            s[k].duration1 = pulse_ticks_;
            t = start + pulse_ticks_;
        }
        // Pad to exactly 1 ms so chunks sent back to back keep an even rhythm.
        const std::uint32_t pad = kChunkTicks - t;
        int symbols = count;
        if (pad >= 2) {
            s[count].level0 = 0;
            s[count].duration0 = static_cast<std::uint16_t>(pad / 2);
            s[count].level1 = 0;
            s[count].duration1 = static_cast<std::uint16_t>(pad - pad / 2);
            ++symbols;
        }
        rmt_transmit_config_t tx = {};
        tx.loop_count = 0;
        tx.flags.eot_level = 0;
        tx.flags.queue_nonblocking = 1;  // never hold up the motion task
        if (rmt_transmit(l.channel, l.encoder, s, symbols * sizeof(rmt_symbol_word_t), &tx) != ESP_OK) {
            ++l.late;  // the queue is full: these steps go out next cycle
            return;
        }
        l.next_buffer = (l.next_buffer + 1) % kBuffers;
        l.sent += dir * count;
        ++l.chunks;
    }

    BoardIo& io_;
    LoadCells& cells_;
    double steps_per_mm_ = 1.0;
    std::uint16_t pulse_ticks_ = 25;
    std::array<Line, leota::kLines> lines_{};
};

}  // namespace

std::unique_ptr<Drives> make_drives(const leota::RigConfig&, BoardIo& io, LoadCells& cells) {
    return std::make_unique<StepDirDrives>(io, cells);
}

}  // namespace fw

#endif
