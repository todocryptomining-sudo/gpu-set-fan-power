#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>

#include "nvml_min.h"
#include "gpu_temps.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_FAN_SPEED         100
#define DEFAULT_MAX_TEMP       85
#define DEFAULT_MIN_SPEED      40
#define DEFAULT_MAX_SPEED     100   // normal cap; critical lock ignores it
#define DEFAULT_POWER_LIMIT     0    // 0 = disabled / use GPU default
#define DEFAULT_PERSISTENCE     1
#define DEFAULT_CRITICAL_TEMP  90    // Absolute safety lock

#define MAX_TEMP_MIN           50
#define MAX_TEMP_MAX          100
#define MIN_SPEED_MIN           0
#define MIN_SPEED_MAX          80
#define MAX_SPEED_MIN          20
#define MAX_SPEED_MAX         100
#define CRITICAL_TEMP_MIN      50
#define CRITICAL_TEMP_MAX     110    // 105/107 allowed for hot GDDR6X hotspots

#define CONFIG_FILE_NAME       "set-fan-config"
#define RAMP_DELTA             40    // °C below max_temp where ramp starts
#define REASSERT_INTERVAL_S    10    // Re-assert power limit every N seconds
#define POLICY_REASSERT_S      60    // Re-assert manual fan policy every N seconds
#define SUMMARY_INTERVAL_S     30    // Log temperature summary every N seconds
#define CRITICAL_HYSTERESIS     5    // °C below critical before releasing the lock
#define FAN_ZERO_ALERT_CYCLES   5    // 5 loops × 2 s = 10 s at 0 % before alerting
#define STATUS_DIR             "/run/set-fan-curve"
#define STATUS_FILE            STATUS_DIR "/temps"
#define FANS_FILE              STATUS_DIR "/fans"
#define MANUAL_FILE            STATUS_DIR "/manual"   // "<speed 0-100> <pid>"

// NVML_TEMPERATURE_GPU == 0
static constexpr int TEMP_SENSOR_GPU = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Per-GPU state
// ─────────────────────────────────────────────────────────────────────────────
struct GpuState {
    nvmlDevice_t device = nullptr;
    std::string  pci_bus_id;              // e.g. "0000:01:00.0"
    bool         valid = false;
    bool         vram_available = false;
    bool         hotspot_available = false;
    int          vram_temp_c = 0;
    int          hotspot_temp_c = 0;
    bool         critical_latched = false;
    unsigned int applied_power_limit_mw = 0;

    unsigned int fan_count = 0;
    int          fan_speed[GPU_TEMPS_MAX_FANS];
    bool         fan_read_ok[GPU_TEMPS_MAX_FANS];
    int          fan_zero_streak[GPU_TEMPS_MAX_FANS];
    bool         fan_zero_alerted[GPU_TEMPS_MAX_FANS];
    nvmlReturn_t fan_last_write_error[GPU_TEMPS_MAX_FANS];
    bool         fan_policy_manual = false;
    bool         fan_policy_warned = false;
    bool         all_fans_zero_alerted = false;

    GpuState() {
        for (int f = 0; f < GPU_TEMPS_MAX_FANS; f++) {
            fan_speed[f] = -1;
            fan_read_ok[f] = false;
            fan_zero_streak[f] = 0;
            fan_zero_alerted[f] = false;
            fan_last_write_error[f] = NVML_SUCCESS;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct Config {
    int          max_temp = DEFAULT_MAX_TEMP;
    unsigned int min_speed = DEFAULT_MIN_SPEED;
    unsigned int max_speed = DEFAULT_MAX_SPEED;
    unsigned int power_limit = DEFAULT_POWER_LIMIT;   // watts, 0 = disabled
    int          persistence_mode = DEFAULT_PERSISTENCE;
    int          critical_temp = DEFAULT_CRITICAL_TEMP;
    bool         use_vram_temp = true;
    bool         use_hotspot_temp = false;
    bool         safety_all_sensors = false;  // false = same sensors as the curve

    // Sensors that feed the normal curve (core is always read).
    std::string sensorList() const {
        std::string s = "core";
        if (use_vram_temp)    s += " + VRAM";
        if (use_hotspot_temp) s += " + hotspot";
        return s;
    }

    // Sensors that can trigger the critical safety lock.
    std::string lockSensors() const {
        if (safety_all_sensors) {
            return "core + VRAM + hotspot (safety_all_sensors=1)";
        }
        return sensorList() + " (same as curve)";
    }

    void print() const {
        std::cout << "[config] Loaded:" << std::endl;
        std::cout << "  max_temp           = " << max_temp << " °C" << std::endl;
        std::cout << "  min_speed          = " << min_speed << " % (floor)" << std::endl;
        std::cout << "  max_speed          = " << max_speed << " % (normal cap; critical lock ignores it)" << std::endl;
        std::cout << "  critical_temp      = " << critical_temp << " °C (hard safety lock)" << std::endl;
        std::cout << "  power_limit        = "
                  << (power_limit ? std::to_string(power_limit) + " W" : "disabled") << std::endl;
        std::cout << "  persistence        = " << (persistence_mode ? "enabled" : "disabled") << std::endl;
        std::cout << "  use_vram_temp      = " << (use_vram_temp ? "true (PCI direct read)" : "false") << std::endl;
        std::cout << "  use_hotspot_temp   = " << (use_hotspot_temp ? "true (PCI direct read)" : "false") << std::endl;
        std::cout << "  safety_all_sensors = " << (safety_all_sensors ? "true (core + VRAM + hotspot)" : "false (same as curve)") << std::endl;
        std::cout << "  curve sensors      = " << sensorList() << std::endl;
        std::cout << "  lock sensors       = " << lockSensors() << std::endl;
        std::cout << "  ramp_start         = " << (max_temp - RAMP_DELTA) << " °C" << std::endl;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────────────────────────────────────
static unsigned int            g_device_count = 0;
static volatile std::sig_atomic_t g_shutdown = 0;
static Config                  g_config;
static std::vector<GpuState>   g_gpu_states;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string formatPciBusId(const nvmlPciInfo_t& pci) {
    // NVIDIA GPUs are always PCI function 0. Do NOT use pci.busId:
    // some drivers return 8-digit domains, while sysfs uses 4.
    char bus[32] = {0};
    snprintf(bus, sizeof(bus), "%04x:%02x:%02x.0",
             pci.domain & 0xFFFFu, pci.bus & 0xFFu, pci.device & 0xFFu);
    return std::string(bus);
}

static bool parse_int(const std::string& s, int& out) {
    try {
        size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

static unsigned int getFanCount(nvmlDevice_t device) {
    unsigned int count = 0;
    if (nvmlDeviceGetNumFans(device, &count) != NVML_SUCCESS || count == 0) {
        count = 1;  // fall back to fan 0
    }
    if (count > GPU_TEMPS_MAX_FANS) count = GPU_TEMPS_MAX_FANS;
    return count;
}

/* Ask the driver for manual fan control on every fan. Some older drivers or
   virtualized GPUs do not implement the policy API; in that case the daemon
   keeps working with direct SetFanSpeed calls and logs one warning. */
static void setManualFanPolicy(GpuState& gs, unsigned int gpu_index) {
    bool any_ok = false;

    for (unsigned int f = 0; f < gs.fan_count; f++) {
        nvmlReturn_t r = nvmlDeviceSetFanControlPolicy(
            gs.device, f, NVML_FAN_POLICY_MANUAL);

        if (r == NVML_SUCCESS) {
            any_ok = true;
        } else if (!gs.fan_policy_warned) {
            std::cerr << "[warn] GPU " << gpu_index << " fan " << f
                      << ": cannot set manual fan policy ("
                      << nvmlErrorString(r)
                      << ") — using direct speed writes only." << std::endl;
            gs.fan_policy_warned = true;
        }
    }

    gs.fan_policy_manual = any_ok;
}

static void readFanSpeeds(GpuState& gs) {
    for (unsigned int f = 0; f < gs.fan_count; f++) {
        unsigned int speed = 0;
        nvmlReturn_t r = nvmlDeviceGetFanSpeed_v2(gs.device, f, &speed);

        if (r == NVML_SUCCESS) {
            gs.fan_speed[f] = static_cast<int>(speed);
            gs.fan_read_ok[f] = true;
        } else {
            gs.fan_speed[f] = -1;
            gs.fan_read_ok[f] = false;
        }
    }
}

static void applyFanSpeed(GpuState& gs, unsigned int gpu_index,
                          unsigned int fan, unsigned int desired) {
    nvmlReturn_t r = nvmlDeviceSetFanSpeed_v2(gs.device, fan, desired);

    if (r != NVML_SUCCESS) {
        if (gs.fan_last_write_error[fan] != r) {
            std::cerr << "[error] GPU " << gpu_index << " fan " << fan
                      << ": SetFanSpeed_v2(" << desired << "%) failed: "
                      << nvmlErrorString(r) << std::endl;
            gs.fan_last_write_error[fan] = r;
        }
    } else if (gs.fan_last_write_error[fan] != NVML_SUCCESS) {
        std::cout << "[info] GPU " << gpu_index << " fan " << fan
                  << ": fan writes recovered." << std::endl;
        gs.fan_last_write_error[fan] = NVML_SUCCESS;
    }
}

/* Alert when one fan reports 0 % while another fan on the same GPU is
   running, or when every fan reads 0 % with a high setpoint. The alert is
   emitted once per event and re-armed when the fan starts moving again. */
static void checkFanAlerts(GpuState& gs, unsigned int gpu_index,
                           unsigned int desired) {
    unsigned int working = 0;
    bool all_zero = (gs.fan_count > 0);

    for (unsigned int f = 0; f < gs.fan_count; f++) {
        if (!gs.fan_read_ok[f]) { all_zero = false; continue; }
        if (gs.fan_speed[f] > 0) { working++; all_zero = false; }
    }

    for (unsigned int f = 0; f < gs.fan_count; f++) {
        if (!gs.fan_read_ok[f]) {
            gs.fan_zero_streak[f] = 0;
            gs.fan_zero_alerted[f] = false;
            continue;
        }

        if (gs.fan_speed[f] == 0) {
            gs.fan_zero_streak[f]++;
        } else {
            gs.fan_zero_streak[f] = 0;
            gs.fan_zero_alerted[f] = false;
        }

        if (gs.fan_zero_streak[f] >= FAN_ZERO_ALERT_CYCLES &&
            !gs.fan_zero_alerted[f] && working > 0) {
            std::cerr << "[alert] GPU " << gpu_index << " fan " << f
                      << " stays at 0 % while another fan is running "
                      << "(setpoint " << desired << " %). Check the fan "
                      << "connector/cable or the card's fan controller."
                      << std::endl;
            gs.fan_zero_alerted[f] = true;
        }
    }

    if (all_zero && desired >= 50) {
        if (!gs.all_fans_zero_alerted) {
            std::cerr << "[alert] GPU " << gpu_index
                      << ": all fans read 0 % while setpoint is " << desired
                      << " %. Check the fans, power cables or the card's "
                      << "fan controller." << std::endl;
            gs.all_fans_zero_alerted = true;
        }
    } else if (!all_zero) {
        gs.all_fans_zero_alerted = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Fan curve
//
//   min_speed is only a floor. The curve itself goes from 0 % at
//   (max_temp - RAMP_DELTA) to max_speed at max_temp. max_speed caps normal
//   operation; the critical safety lock ignores it and forces 100 %.
// ─────────────────────────────────────────────────────────────────────────────
static unsigned int getFanSpeed(int temp, int max_temp,
                                unsigned int min_speed, unsigned int max_speed) {
    if (max_speed < min_speed) max_speed = min_speed;

    int ramp_start = max_temp - RAMP_DELTA;
    unsigned int curve;

    if (temp <= ramp_start) {
        curve = 0;
    } else if (temp >= max_temp) {
        curve = max_speed;
    } else {
        double ratio = static_cast<double>(temp - ramp_start) /
                       static_cast<double>(RAMP_DELTA);
        curve = static_cast<unsigned int>(ratio * max_speed + 0.5);
        if (curve > max_speed) curve = max_speed;
    }

    return std::max(min_speed, std::min(curve, max_speed));
}

// ─────────────────────────────────────────────────────────────────────────────
// PCI temperatures
// ─────────────────────────────────────────────────────────────────────────────
static void updatePciTemps() {
    for (auto& gs : g_gpu_states) {
        if (!gs.valid) continue;

        gpu_temps_t t;
        if (gpu_temps_read(gs.pci_bus_id.c_str(), &t)) {
            gs.vram_available    = t.vram_available;
            gs.vram_temp_c       = t.vram_temp_c;
            gs.hotspot_available = t.hotspot_available;
            gs.hotspot_temp_c    = t.hotspot_temp_c;
        }
        // On a transient read failure keep the last known value. Sensor
        // availability is only cleared if it was never detected.
    }
}

/* Publica las temperaturas y las velocidades por ventilador para que
   `vergpu` y los comandos puedan mostrarlas sin root. */
static void writeStatusFile() {
    std::vector<const char*> ids;
    std::vector<int> vram;
    std::vector<int> hotspot;
    std::vector<gpu_fans_entry_t> fans;

    for (const auto& gs : g_gpu_states) {
        if (!gs.valid) continue;
        ids.push_back(gs.pci_bus_id.c_str());
        vram.push_back(gs.vram_available ? gs.vram_temp_c : -1);
        hotspot.push_back(gs.hotspot_available ? gs.hotspot_temp_c : -1);

        gpu_fans_entry_t entry;
        entry.bus_id = gs.pci_bus_id.c_str();
        entry.count = static_cast<int>(gs.fan_count);
        for (int f = 0; f < GPU_TEMPS_MAX_FANS; f++) {
            entry.speed[f] = (f < entry.count && gs.fan_read_ok[f])
                                 ? gs.fan_speed[f] : -1;
        }
        fans.push_back(entry);
    }

    if (ids.empty()) return;

    mkdir(STATUS_DIR, 0755);  // may already exist (systemd RuntimeDirectory)
    gpu_temps_status_write(STATUS_FILE, ids.data(), vram.data(),
                           hotspot.data(), static_cast<int>(ids.size()));
    gpu_fans_status_write(FANS_FILE, fans.data(), static_cast<int>(fans.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Manual fan test (setfan command)
//
// /run/set-fan-curve/manual contains "<speed 0-100> <pid>". The daemon keeps
// the fans at <speed> while that PID is alive and stops doing so as soon as
// the file disappears or the process dies. This makes an SSH disconnect or a
// Ctrl+C restore the curve automatically, without leaving the GPU pinned.
// ─────────────────────────────────────────────────────────────────────────────
static int readManualSpeed() {
    std::ifstream file(MANUAL_FILE);
    if (!file.is_open()) return -1;

    long speed = -1;
    long pid   = -1;
    if (!(file >> speed >> pid) || speed < 0 || speed > 100 || pid <= 0) {
        std::cerr << "[warn] Invalid manual control file — removing it." << std::endl;
        unlink(MANUAL_FILE);
        return -1;
    }

    // EPERM means the process exists but belongs to another user: still alive.
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
        std::cout << "[manual] Test process " << pid
                  << " is gone — removing control file and resuming the curve."
                  << std::endl;
        unlink(MANUAL_FILE);
        return -1;
    }

    return static_cast<int>(speed);
}

// ─────────────────────────────────────────────────────────────────────────────
// getEffectiveTemperature
//   include_all_sensors = false → max(core, VRAM, hotspot) per config
//   include_all_sensors = true  → max of ALL available sensors
// ─────────────────────────────────────────────────────────────────────────────
static int getEffectiveTemperature(unsigned int gpu_index, bool include_all_sensors) {
    if (gpu_index >= g_gpu_states.size()) return -1;

    GpuState& gs = g_gpu_states[gpu_index];

    unsigned int core_temp = 0;
    nvmlReturn_t result = nvmlDeviceGetTemperature(
        gs.device,
        static_cast<nvmlTemperatureSensors_t>(TEMP_SENSOR_GPU),
        &core_temp);

    if (result != NVML_SUCCESS) {
        std::cerr << "[warn] Core temp read failed for GPU " << gpu_index
                  << ": " << nvmlErrorString(result) << std::endl;
        return -1;
    }

    int best = static_cast<int>(core_temp);

    if ((include_all_sensors || g_config.use_vram_temp) && gs.vram_available) {
        best = std::max(best, gs.vram_temp_c);
    }
    if ((include_all_sensors || g_config.use_hotspot_temp) && gs.hotspot_available) {
        best = std::max(best, gs.hotspot_temp_c);
    }

    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// Power limit: returns the limit actually applied (may be clamped)
// ─────────────────────────────────────────────────────────────────────────────
static bool setPowerLimit(nvmlDevice_t device, unsigned int watts,
                          unsigned int* applied_mw) {
    if (watts == 0) return true;  // disabled

    unsigned int min_limit = 0, max_limit = 0;
    nvmlReturn_t result = nvmlDeviceGetPowerManagementLimitConstraints(
        device, &min_limit, &max_limit);

    if (result != NVML_SUCCESS) {
        std::cerr << "[warn] Cannot read power limit constraints: "
                  << nvmlErrorString(result) << std::endl;
        std::cerr << "     (continuing without power limit)" << std::endl;
        return false;
    }

    unsigned int limit_mw = watts * 1000;

    if (limit_mw < min_limit || limit_mw > max_limit) {
        std::cerr << "[warn] Power limit " << watts << " W out of hardware range ("
                  << (min_limit / 1000) << " W - " << (max_limit / 1000)
                  << " W). Using " << (max_limit / 1000) << " W." << std::endl;
        limit_mw = (limit_mw < min_limit) ? min_limit : max_limit;
    }

    result = nvmlDeviceSetPowerManagementLimit(device, limit_mw);
    if (result != NVML_SUCCESS) {
        std::cerr << "[error] Failed to set power limit: "
                  << nvmlErrorString(result) << std::endl;
        return false;
    }

    if (applied_mw) *applied_mw = limit_mw;
    std::cout << "[info] Power limit set to " << (limit_mw / 1000) << " W" << std::endl;
    return true;
}

static bool setPersistenceMode(nvmlDevice_t device, int mode) {
    nvmlEnableState_t state = mode ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    nvmlReturn_t result = nvmlDeviceSetPersistenceMode(device, state);

    if (result != NVML_SUCCESS) {
        std::cerr << "[warn] Failed to set persistence mode: "
                  << nvmlErrorString(result) << std::endl;
        return false;
    }

    std::cout << "[info] Persistence mode " << (mode ? "enabled" : "disabled") << std::endl;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Config parsing and validation (INI-style, ~/set-fan-config)
// ─────────────────────────────────────────────────────────────────────────────

struct ConfigDiag {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> infos;

    bool ok() const { return errors.empty(); }
};

struct ConfigValue {
    std::string value;
    int line = 0;
};

static bool isKnownConfigKey(const std::string& key) {
    static const char* const keys[] = {
        "max_temp",
        "min_speed",
        "max_speed",
        "critical_temp",
        "power_limit",
        "persistence",
        "use_vram_temp",
        "use_hotspot_temp",
        "safety_all_sensors",
    };

    for (const char* candidate : keys) {
        if (key == candidate) return true;
    }
    return false;
}

static std::string lineTag(int line) {
    return line > 0 ? ("línea " + std::to_string(line) + ": ") : std::string();
}

static bool loadConfig(const std::string& configPath, Config& config,
                       ConfigDiag& diag) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        diag.warnings.push_back("no se encontró " + configPath +
                                " (usando valores por defecto)");
        return false;
    }

    std::unordered_map<std::string, ConfigValue> values;
    std::string line;
    int line_no = 0;
    bool first_line = true;

    while (std::getline(file, line)) {
        line_no++;

        if (first_line) {
            // Strip a UTF-8 BOM if an editor added one.
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            first_line = false;
        }

        size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            size_t end   = s.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) { s.clear(); return; }
            s = s.substr(start, end - start + 1);
        };

        trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            diag.errors.push_back(lineTag(line_no) +
                                  "falta '=' (revisa la línea)");
            continue;
        }

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);

        if (key.empty()) {
            diag.errors.push_back(lineTag(line_no) + "clave vacía antes de '='");
            continue;
        }
        if (!isKnownConfigKey(key)) {
            diag.errors.push_back(lineTag(line_no) + "clave desconocida '" +
                                  key + "' (¿typo?)");
            continue;
        }
        if (value.empty()) {
            diag.errors.push_back(lineTag(line_no) + "la clave '" + key +
                                  "' no tiene valor");
            continue;
        }
        if (values.count(key)) {
            diag.warnings.push_back(lineTag(line_no) + "clave duplicada '" +
                                    key + "'; se usa la última");
        }
        values[key] = ConfigValue{value, line_no};
    }

    if (values.empty()) {
        diag.errors.push_back("no hay ninguna clave válida en " + configPath);
        return false;
    }

    auto readInt = [&](const char* key, int lo, int hi, int& out) -> bool {
        auto it = values.find(key);
        if (it == values.end()) return false;

        int v = 0;
        if (!parse_int(it->second.value, v)) {
            diag.errors.push_back(lineTag(it->second.line) + key + "='" +
                                  it->second.value + "' no es un número");
            return false;
        }
        if (v < lo || v > hi) {
            diag.errors.push_back(lineTag(it->second.line) + key + "=" +
                                  std::to_string(v) + " fuera de rango (" +
                                  std::to_string(lo) + "-" +
                                  std::to_string(hi) + ")");
            return false;
        }
        out = v;
        return true;
    };

    int v = 0;

    if (readInt("max_temp", MAX_TEMP_MIN, MAX_TEMP_MAX, v))
        config.max_temp = v;
    if (readInt("min_speed", MIN_SPEED_MIN, MIN_SPEED_MAX, v))
        config.min_speed = static_cast<unsigned int>(v);
    if (readInt("max_speed", MAX_SPEED_MIN, MAX_SPEED_MAX, v))
        config.max_speed = static_cast<unsigned int>(v);
    if (readInt("critical_temp", CRITICAL_TEMP_MIN, CRITICAL_TEMP_MAX, v))
        config.critical_temp = v;
    if (readInt("power_limit", 0, 2000, v))
        config.power_limit = static_cast<unsigned int>(v);
    if (readInt("persistence", 0, 1, v))
        config.persistence_mode = v;
    if (readInt("use_vram_temp", 0, 1, v))
        config.use_vram_temp = (v != 0);
    if (readInt("use_hotspot_temp", 0, 1, v))
        config.use_hotspot_temp = (v != 0);
    if (readInt("safety_all_sensors", 0, 1, v))
        config.safety_all_sensors = (v != 0);

    if (config.max_speed < config.min_speed) {
        diag.warnings.push_back("max_speed (" +
            std::to_string(config.max_speed) + ") < min_speed (" +
            std::to_string(config.min_speed) +
            "); max_speed se elevará a " +
            std::to_string(config.min_speed));
        config.max_speed = config.min_speed;
    }

    if (config.critical_temp < config.max_temp) {
        diag.infos.push_back("critical_temp (" +
            std::to_string(config.critical_temp) + " °C) < max_temp (" +
            std::to_string(config.max_temp) +
            " °C); está permitido: el bloqueo fuerza 100 % antes de que la "
            "curva llegue a max_speed");
    }

    if (config.critical_temp > 100) {
        if (!config.use_vram_temp && !config.use_hotspot_temp &&
            !config.safety_all_sensors) {
            diag.warnings.push_back("critical_temp=" +
                std::to_string(config.critical_temp) +
                " °C con el bloqueo usando solo el core: >100 °C es para "
                "VRAM/hotspot, no para el core. Activa use_vram_temp o "
                "use_hotspot_temp, o baja critical_temp a 100 o menos");
        } else {
            diag.infos.push_back("critical_temp=" +
                std::to_string(config.critical_temp) +
                " °C (>100): válido para VRAM/hotspot; no lo uses como límite "
                "del core");
        }
    }

    return diag.ok();
}

static void printDiagnostics(const ConfigDiag& diag) {
    for (const auto& info : diag.infos) {
        std::cout << "[info] " << info << std::endl;
    }
    for (const auto& warning : diag.warnings) {
        std::cerr << "[warn] " << warning << std::endl;
    }
    for (const auto& error : diag.errors) {
        std::cerr << "[error] " << error << std::endl;
    }
}

static void printConfigReport(const std::string& path, const Config& config,
                              const ConfigDiag& diag) {
    std::cout << "─── Comprobación de configuración ──────────────────────────────" << std::endl;
    std::cout << "Archivo: " << path << std::endl;
    std::cout << std::endl;
    std::cout << "Valores efectivos:" << std::endl;
    std::cout << "  max_temp            = " << config.max_temp
              << " °C  (rango " << MAX_TEMP_MIN << "-" << MAX_TEMP_MAX << ")" << std::endl;
    std::cout << "  min_speed           = " << config.min_speed
              << " %   (rango " << MIN_SPEED_MIN << "-" << MIN_SPEED_MAX
              << ", suelo)" << std::endl;
    std::cout << "  max_speed           = " << config.max_speed
              << " %   (rango " << MAX_SPEED_MIN << "-" << MAX_SPEED_MAX
              << ", tope normal)" << std::endl;
    std::cout << "  critical_temp       = " << config.critical_temp
              << " °C  (rango " << CRITICAL_TEMP_MIN << "-" << CRITICAL_TEMP_MAX
              << ", bloqueo)" << std::endl;
    std::cout << "  power_limit         = "
              << (config.power_limit ? std::to_string(config.power_limit) + " W"
                                     : "desactivado") << std::endl;
    std::cout << "  persistence         = "
              << (config.persistence_mode ? "activado" : "desactivado") << std::endl;
    std::cout << "  use_vram_temp       = " << (config.use_vram_temp ? 1 : 0) << std::endl;
    std::cout << "  use_hotspot_temp    = " << (config.use_hotspot_temp ? 1 : 0) << std::endl;
    std::cout << "  safety_all_sensors  = " << (config.safety_all_sensors ? 1 : 0) << std::endl;
    std::cout << std::endl;
    std::cout << "Sensores de la curva         : " << config.sensorList() << std::endl;
    std::cout << "Sensores del bloqueo crítico : " << config.lockSensors() << std::endl;

    if (config.use_hotspot_temp || config.safety_all_sensors) {
        std::cout << "[info] El hotspot SÍ forma parte del bloqueo: con >= "
                  << config.critical_temp << " °C se fuerza 100 %." << std::endl;
    } else {
        std::cout << "[info] El hotspot NO forma parte del bloqueo: una lectura "
                  << "alta de hotspot no fuerza 100 % por sí sola." << std::endl;
    }

    std::cout << std::endl;
    printDiagnostics(diag);

    if (diag.ok()) {
        if (diag.warnings.empty()) {
            std::cout << "[ok] Configuración válida." << std::endl;
        } else {
            std::cout << "[ok] Configuración válida (" << diag.warnings.size()
                      << " aviso(s))." << std::endl;
        }
        std::cout << "     Aplicar con: restartgpu" << std::endl;
        return;
    }

    std::cout << "[FAIL] " << diag.errors.size()
              << " error(es). NO reinicies el servicio con este archivo."
              << std::endl;
    std::cout << "       Corrige con: editargpu" << std::endl;
}

static int checkConfig(const std::string& path) {
    Config cfg;
    ConfigDiag diag;

    if (access(path.c_str(), R_OK) != 0) {
        std::cerr << "[error] no se puede leer " << path << ": "
                  << std::strerror(errno) << std::endl;
        return 1;
    }

    loadConfig(path, cfg, diag);
    printConfigReport(path, cfg, diag);
    return diag.ok() ? 0 : 1;
}

static std::string getConfigPath() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) home = std::getenv("USERPROFILE");
    if (!home || !home[0]) return CONFIG_FILE_NAME;
    return std::string(home) + "/" + CONFIG_FILE_NAME;
}

static void signalHandler(int) {
    g_shutdown = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string configPath = getConfigPath();

    // Validation mode: no NVML, no root, no service restart.
    // Exit 0 when the file is valid, 1 when it contains errors.
    if (argc >= 2 && (std::strcmp(argv[1], "--check-config") == 0 ||
                      std::strcmp(argv[1], "--validate") == 0)) {
        if (argc >= 3) configPath = argv[2];
        return checkConfig(configPath);
    }

    ConfigDiag diag;
    loadConfig(configPath, g_config, diag);

    // Errors are non-fatal at runtime: fan control keeps working with safe
    // defaults. The scripts use --check-config to reject a bad config before
    // the service is restarted.
    printDiagnostics(diag);

    g_config.print();

    // Optional backward-compatible CLI override:
    //   set_fan_curve <max_temp> <min_speed> [max_speed]
    if (argc >= 3) {
        g_config.max_temp  = std::atoi(argv[1]);
        g_config.min_speed = static_cast<unsigned int>(std::atoi(argv[2]));
        if (argc >= 4) {
            g_config.max_speed = static_cast<unsigned int>(std::atoi(argv[3]));
        }
        std::cout << "[info] Command-line override: max_temp=" << g_config.max_temp
                  << ", min_speed=" << g_config.min_speed
                  << ", max_speed=" << g_config.max_speed << std::endl;
    }

    if (g_config.max_temp < MAX_TEMP_MIN || g_config.max_temp > MAX_TEMP_MAX) {
        std::cerr << "[error] max_temp must be between " << MAX_TEMP_MIN
                  << " and " << MAX_TEMP_MAX << "." << std::endl;
        return 1;
    }
    if (g_config.min_speed > MIN_SPEED_MAX) {
        std::cerr << "[error] min_speed must be between " << MIN_SPEED_MIN
                  << " and " << MIN_SPEED_MAX << "." << std::endl;
        return 1;
    }
    if (g_config.max_speed < MAX_SPEED_MIN || g_config.max_speed > MAX_SPEED_MAX) {
        std::cerr << "[error] max_speed must be between " << MAX_SPEED_MIN
                  << " and " << MAX_SPEED_MAX << "." << std::endl;
        return 1;
    }
    if (g_config.max_speed < g_config.min_speed) {
        std::cerr << "[warn] max_speed (" << g_config.max_speed
                  << ") < min_speed (" << g_config.min_speed
                  << ") — raising max_speed to " << g_config.min_speed << std::endl;
        g_config.max_speed = g_config.min_speed;
    }
    if (g_config.critical_temp < CRITICAL_TEMP_MIN ||
        g_config.critical_temp > CRITICAL_TEMP_MAX) {
        std::cerr << "[error] critical_temp must be between "
                  << CRITICAL_TEMP_MIN << " and " << CRITICAL_TEMP_MAX << "."
                  << std::endl;
        return 1;
    }

    // NVML init with retries (systemd may start before the driver is ready)
    nvmlReturn_t result;
    int retries = 0;
    const int max_retries = 60;

    do {
        result = nvmlInit();
        if (result == NVML_SUCCESS) break;
        std::cerr << "[info] Waiting for NVIDIA driver... ("
                  << retries + 1 << "/" << max_retries << ")" << std::endl;
        retries++;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (retries < max_retries);

    if (result != NVML_SUCCESS) {
        std::cerr << "[error] Failed to initialize NVML after " << max_retries
                  << "s: " << nvmlErrorString(result) << std::endl;
        std::cerr << "     Verify the nvidia driver is loaded." << std::endl;
        return 1;
    }

    result = nvmlDeviceGetCount(&g_device_count);
    if (result != NVML_SUCCESS) {
        std::cerr << "[error] Failed to query GPU count: "
                  << nvmlErrorString(result) << std::endl;
        nvmlShutdown();
        return 1;
    }

    if (g_device_count == 0) {
        std::cerr << "[error] No NVIDIA GPUs found after NVML init." << std::endl;
        nvmlShutdown();
        return 1;
    }

    g_gpu_states.resize(g_device_count);

    for (unsigned int i = 0; i < g_device_count; i++) {
        GpuState& gs = g_gpu_states[i];

        result = nvmlDeviceGetHandleByIndex(i, &gs.device);
        if (result != NVML_SUCCESS) {
            std::cerr << "[warn] GPU " << i << ": cannot get handle ("
                      << nvmlErrorString(result) << "), skipping." << std::endl;
            continue;
        }

        nvmlPciInfo_t pci;
        memset(&pci, 0, sizeof(pci));
        result = nvmlDeviceGetPciInfo_v3(gs.device, &pci);
        if (result != NVML_SUCCESS) {
            std::cerr << "[warn] GPU " << i << ": cannot get PCI info ("
                      << nvmlErrorString(result) << "), skipping." << std::endl;
            continue;
        }

        gs.pci_bus_id = formatPciBusId(pci);
        gs.valid = true;

        gs.fan_count = getFanCount(gs.device);
        setManualFanPolicy(gs, i);
        readFanSpeeds(gs);

        std::cout << "[info] GPU " << i << " [" << gs.pci_bus_id << "]: "
                  << gs.fan_count << " fan sensor(s)" << std::endl;

        gpu_temps_t initial;
        if (gpu_temps_read(gs.pci_bus_id.c_str(), &initial)) {
            gs.vram_available    = initial.vram_available;
            gs.vram_temp_c       = initial.vram_temp_c;
            gs.hotspot_available = initial.hotspot_available;
            gs.hotspot_temp_c    = initial.hotspot_temp_c;

            std::cout << "[info] GPU " << i << " [" << gs.pci_bus_id << "]: PCI VRAM=";
            if (initial.vram_available) std::cout << initial.vram_temp_c << " °C";
            else                        std::cout << "N/A";
            std::cout << "  Hotspot=";
            if (initial.hotspot_available) std::cout << initial.hotspot_temp_c << " °C";
            else                           std::cout << "N/A";
            std::cout << std::endl;
        } else {
            std::cout << "[warn] GPU " << i << " [" << gs.pci_bus_id
                      << "]: PCI temps unavailable. Check /dev/mem access "
                      << "(root) and kernel parameter iomem=relaxed." << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "[info] Found " << g_device_count << " GPU(s)" << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    // Startup: persistence + power limit
    for (auto& gs : g_gpu_states) {
        if (!gs.valid) continue;

        if (g_config.persistence_mode) {
            setPersistenceMode(gs.device, 1);
        }
        if (g_config.power_limit > 0) {
            setPowerLimit(gs.device, g_config.power_limit, &gs.applied_power_limit_mw);
        }
    }

    std::cout << std::string(72, '-') << std::endl;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Pick up a test that was running before a daemon restart. A stale file
    // (dead PID) is removed by readManualSpeed().
    int manual_last = readManualSpeed();
    if (manual_last >= 0) {
        std::cout << "[manual] Existing fan test detected: " << manual_last
                  << " % (PID alive) — applying it." << std::endl;
    }

    std::cout << "[info] Fan curve controller started." << std::endl;
    std::cout << "[info] Max temperature target : " << g_config.max_temp << " °C" << std::endl;
    std::cout << "[info] Critical safety lock   : " << g_config.critical_temp
              << " °C (latching, release at " << (g_config.critical_temp - CRITICAL_HYSTERESIS)
              << " °C)" << std::endl;
    std::cout << "[info] Min fan speed (floor)  : " << g_config.min_speed << " %" << std::endl;
    std::cout << "[info] Max fan speed (normal) : " << g_config.max_speed
              << " % (critical lock ignores it)" << std::endl;
    std::cout << "[info] Curve                  : "
              << (g_config.max_temp - RAMP_DELTA) << " °C → "
              << g_config.max_temp << " °C, 0 → " << g_config.max_speed << " %" << std::endl;
    std::cout << "[info] Use VRAM temp (PCI)    : " << (g_config.use_vram_temp ? "yes" : "no") << std::endl;
    std::cout << "[info] Use Hotspot temp (PCI) : " << (g_config.use_hotspot_temp ? "yes" : "no") << std::endl;
    std::cout << "[info] Curve sensors          : " << g_config.sensorList() << std::endl;
    std::cout << "[info] Critical lock sensors  : " << g_config.lockSensors() << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    // Initial status publish
    updatePciTemps();
    writeStatusFile();

    auto lastReassert = std::chrono::steady_clock::now();
    auto lastPolicyReassert = std::chrono::steady_clock::now();
    auto lastSummary  = std::chrono::steady_clock::now();

    while (!g_shutdown) {
        updatePciTemps();
        writeStatusFile();

        int manual = readManualSpeed();
        if (manual >= 0 && manual != manual_last) {
            std::cout << "[manual] Fan test active: " << manual << " % — "
                      << "run 'setfan auto' or press Ctrl+C in that terminal to stop."
                      << std::endl;
            manual_last = manual;
        } else if (manual < 0 && manual_last >= 0) {
            std::cout << "[manual] Fan test ended — curve control resumed." << std::endl;
            manual_last = -1;
        }

        auto now = std::chrono::steady_clock::now();

        for (unsigned int i = 0; i < g_device_count; i++) {
            GpuState& gs = g_gpu_states[i];
            if (!gs.valid) continue;

            int temp = getEffectiveTemperature(i, g_config.safety_all_sensors);
            if (temp < 0) continue;

            // ── Critical safety lock (latching) ─────────────────────────────
            if (temp >= g_config.critical_temp) {
                if (!gs.critical_latched) {
                    std::cerr << "[alert] GPU " << i << ": CRITICAL temp " << temp
                              << " °C >= " << g_config.critical_temp
                              << " °C (VRAM="
                              << (gs.vram_available ? std::to_string(gs.vram_temp_c) : "N/A")
                              << ", hotspot="
                              << (gs.hotspot_available ? std::to_string(gs.hotspot_temp_c) : "N/A")
                              << ") — fans locked to 100%" << std::endl;
                }
                gs.critical_latched = true;
            } else if (gs.critical_latched &&
                       temp <= (g_config.critical_temp - CRITICAL_HYSTERESIS)) {
                std::cerr << "[info] GPU " << i << ": temp " << temp
                          << " °C, releasing critical lock." << std::endl;
                gs.critical_latched = false;
            }

            unsigned int desired;
            if (gs.critical_latched) {
                // Safety always wins: ignore min_speed, max_speed and manual.
                desired = MAX_FAN_SPEED;
            } else if (manual >= 0) {
                // setfan is an explicit test; only the critical lock overrides it.
                desired = static_cast<unsigned int>(manual);
            } else {
                desired = getFanSpeed(temp, g_config.max_temp,
                                      g_config.min_speed, g_config.max_speed);
            }

            for (unsigned int fan = 0; fan < gs.fan_count; fan++) {
                applyFanSpeed(gs, i, fan, desired);
            }

            readFanSpeeds(gs);
            checkFanAlerts(gs, i, desired);
        }

        // ── Dominance: re-assert the power limit every 10 s ────────────────
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReassert).count()
                >= REASSERT_INTERVAL_S) {
            lastReassert = now;

            for (auto& gs : g_gpu_states) {
                if (!gs.valid || gs.applied_power_limit_mw == 0) continue;

                unsigned int current = 0;
                if (nvmlDeviceGetPowerManagementLimit(gs.device, &current) != NVML_SUCCESS)
                    continue;

                if (current != gs.applied_power_limit_mw) {
                    std::cout << "[dominance] GPU [" << gs.pci_bus_id
                              << "]: power limit changed to " << (current / 1000)
                              << " W — re-asserting "
                              << (gs.applied_power_limit_mw / 1000) << " W" << std::endl;
                    nvmlDeviceSetPowerManagementLimit(gs.device, gs.applied_power_limit_mw);
                }
            }
        }

        // ── Re-assert manual fan policy every 60 s ─────────────────────────
        // Some drivers reset the policy after a suspend/resume or a container
        // restart. Re-applying it keeps SetFanSpeed authoritative.
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPolicyReassert).count()
                >= POLICY_REASSERT_S) {
            lastPolicyReassert = now;

            for (auto& gs : g_gpu_states) {
                if (!gs.valid || !gs.fan_policy_manual) continue;
                for (unsigned int fan = 0; fan < gs.fan_count; fan++) {
                    nvmlDeviceSetFanControlPolicy(
                        gs.device, fan, NVML_FAN_POLICY_MANUAL);
                }
            }
        }

        // ── Periodic temperature summary (useful for journalctl) ──────────
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastSummary).count()
                >= SUMMARY_INTERVAL_S) {
            lastSummary = now;

            for (size_t i = 0; i < g_gpu_states.size(); i++) {
                const GpuState& gs = g_gpu_states[i];
                if (!gs.valid) continue;

                unsigned int core = 0;
                nvmlDeviceGetTemperature(gs.device,
                    static_cast<nvmlTemperatureSensors_t>(TEMP_SENSOR_GPU), &core);

                std::cout << "[temp] GPU " << i << " [" << gs.pci_bus_id << "]"
                          << " core=" << core << " °C"
                          << " vram=";
                if (gs.vram_available) std::cout << gs.vram_temp_c << " °C";
                else                   std::cout << "N/A";
                std::cout << " hotspot=";
                if (gs.hotspot_available) std::cout << gs.hotspot_temp_c << " °C";
                else                      std::cout << "N/A";
                std::cout << " fans=";
                for (unsigned int f = 0; f < gs.fan_count; f++) {
                    if (f) std::cout << ",";
                    std::cout << "fan" << f << "=";
                    if (gs.fan_read_ok[f]) std::cout << gs.fan_speed[f] << "%";
                    else                   std::cout << "?";
                }
                std::cout << (gs.critical_latched ? " [CRITICAL LOCK]"
                              : (manual >= 0 ? " [MANUAL]" : ""))
                          << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ── Shutdown: restore automatic fan control ────────────────────────────
    std::cout << "[info] Signal received. Restoring automatic fan control..." << std::endl;

    // A manual test cannot survive the daemon: remove its control file so a
    // later start follows the curve again. The setfan process notices this.
    if (unlink(MANUAL_FILE) == 0) {
        std::cout << "[manual] Manual control file removed." << std::endl;
    }

    for (auto& gs : g_gpu_states) {
        if (!gs.valid) continue;

        for (unsigned int fan = 0; fan < gs.fan_count; fan++) {
            nvmlReturn_t r = nvmlDeviceSetDefaultFanSpeed_v2(gs.device, fan);
            if (r != NVML_SUCCESS) {
                std::cerr << "[warn] Could not restore automatic fan " << fan
                          << " on GPU [" << gs.pci_bus_id << "]: "
                          << nvmlErrorString(r) << std::endl;
            }

            if (gs.fan_policy_manual) {
                r = nvmlDeviceSetFanControlPolicy(
                    gs.device, fan, NVML_FAN_POLICY_AUTO);
                if (r != NVML_SUCCESS) {
                    std::cerr << "[warn] Could not restore fan policy " << fan
                              << " on GPU [" << gs.pci_bus_id << "]: "
                              << nvmlErrorString(r) << std::endl;
                }
            }
        }
    }

    std::cout << "[info] Done." << std::endl;

    nvmlShutdown();
    return 0;
}
