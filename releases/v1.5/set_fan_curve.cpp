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
#define DEFAULT_POWER_LIMIT     0    // 0 = disabled / use GPU default
#define DEFAULT_PERSISTENCE     1
#define DEFAULT_CRITICAL_TEMP  90    // Absolute safety lock
#define CONFIG_FILE_NAME       "set-fan-config"
#define RAMP_DELTA             40    // °C below max_temp where ramp starts
#define REASSERT_INTERVAL_S    10    // Re-assert power limit every N seconds
#define SUMMARY_INTERVAL_S     30    // Log temperature summary every N seconds
#define CRITICAL_HYSTERESIS     5    // °C below critical before releasing the lock
#define STATUS_DIR             "/run/set-fan-curve"
#define STATUS_FILE            STATUS_DIR "/temps"
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
};

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct Config {
    int          max_temp = DEFAULT_MAX_TEMP;
    unsigned int min_speed = DEFAULT_MIN_SPEED;
    unsigned int power_limit = DEFAULT_POWER_LIMIT;   // watts, 0 = disabled
    int          persistence_mode = DEFAULT_PERSISTENCE;
    int          critical_temp = DEFAULT_CRITICAL_TEMP;
    bool         use_vram_temp = true;
    bool         use_hotspot_temp = false;
    bool         safety_all_sensors = false;  // false = same sensors as the curve

    void print() const {
        std::cout << "[config] Loaded:" << std::endl;
        std::cout << "  max_temp           = " << max_temp << " °C" << std::endl;
        std::cout << "  min_speed          = " << min_speed << " %" << std::endl;
        std::cout << "  critical_temp      = " << critical_temp << " °C (hard safety lock)" << std::endl;
        std::cout << "  power_limit        = "
                  << (power_limit ? std::to_string(power_limit) + " W" : "disabled") << std::endl;
        std::cout << "  persistence        = " << (persistence_mode ? "enabled" : "disabled") << std::endl;
        std::cout << "  use_vram_temp      = " << (use_vram_temp ? "true (PCI direct read)" : "false") << std::endl;
        std::cout << "  use_hotspot_temp   = " << (use_hotspot_temp ? "true (PCI direct read)" : "false") << std::endl;
        std::cout << "  safety_all_sensors = " << (safety_all_sensors ? "true (core + VRAM + hotspot)" : "false (same as curve)") << std::endl;
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
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fan curve: linear ramp from (max_temp - RAMP_DELTA) to max_temp
// ─────────────────────────────────────────────────────────────────────────────
static unsigned int getFanSpeed(int temp, int max_temp, unsigned int min_speed) {
    int ramp_start = max_temp - RAMP_DELTA;

    if (temp <= ramp_start) return min_speed;
    if (temp >= max_temp)   return MAX_FAN_SPEED;

    double ratio = static_cast<double>(temp - ramp_start) /
                   static_cast<double>(RAMP_DELTA);

    unsigned int speed = static_cast<unsigned int>(
        min_speed + ratio * (MAX_FAN_SPEED - min_speed) + 0.5
    );

    return std::max(min_speed, std::min(speed, static_cast<unsigned int>(MAX_FAN_SPEED)));
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

/* Publica las temperaturas para que `vergpu` pueda mostrarlas sin root. */
static void writeStatusFile() {
    std::vector<const char*> ids;
    std::vector<int> vram;
    std::vector<int> hotspot;

    for (const auto& gs : g_gpu_states) {
        if (!gs.valid) continue;
        ids.push_back(gs.pci_bus_id.c_str());
        vram.push_back(gs.vram_available ? gs.vram_temp_c : -1);
        hotspot.push_back(gs.hotspot_available ? gs.hotspot_temp_c : -1);
    }

    if (ids.empty()) return;

    mkdir(STATUS_DIR, 0755);  // may already exist (systemd RuntimeDirectory)
    gpu_temps_status_write(STATUS_FILE, ids.data(), vram.data(),
                           hotspot.data(), static_cast<int>(ids.size()));
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
// Config parsing (INI-style, ~/set-fan-config)
// ─────────────────────────────────────────────────────────────────────────────
static bool loadConfig(const std::string& configPath, Config& config) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cout << "[info] Config file not found: " << configPath
                  << " (using safe defaults)" << std::endl;
        return false;
    }

    std::string line;
    std::unordered_map<std::string, std::string> values;

    while (std::getline(file, line)) {
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
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        if (!key.empty()) values[key] = value;
    }

    int v = 0;

    if (values.count("max_temp") && parse_int(values["max_temp"], v)) {
        if (v >= 50 && v <= 95) config.max_temp = v;
        else std::cerr << "[warn] max_temp=" << v
                       << " out of range (50-95), using " << config.max_temp << std::endl;
    }

    if (values.count("min_speed") && parse_int(values["min_speed"], v)) {
        if (v >= 0 && v <= 80) config.min_speed = static_cast<unsigned int>(v);
        else std::cerr << "[warn] min_speed=" << v
                       << " out of range (0-80), using " << config.min_speed << std::endl;
    }

    if (values.count("power_limit") && parse_int(values["power_limit"], v)) {
        if (v >= 0 && v <= 2000) config.power_limit = static_cast<unsigned int>(v);
        else std::cerr << "[warn] power_limit must be between 0 and 2000 W, using "
                       << config.power_limit << std::endl;
    }

    if (values.count("persistence") && parse_int(values["persistence"], v)) {
        config.persistence_mode = (v != 0) ? 1 : 0;
    }

    if (values.count("use_vram_temp") && parse_int(values["use_vram_temp"], v)) {
        config.use_vram_temp = (v != 0);
    }

    if (values.count("use_hotspot_temp") && parse_int(values["use_hotspot_temp"], v)) {
        config.use_hotspot_temp = (v != 0);
    }

    if (values.count("safety_all_sensors") && parse_int(values["safety_all_sensors"], v)) {
        config.safety_all_sensors = (v != 0);
    }

    if (values.count("critical_temp") && parse_int(values["critical_temp"], v)) {
        if (v > config.max_temp && v <= 100) {
            config.critical_temp = v;
        } else if (v <= config.max_temp) {
            std::cerr << "[warn] critical_temp (" << v << ") <= max_temp ("
                      << config.max_temp << ") — using max_temp + 5" << std::endl;
            config.critical_temp = config.max_temp + 5;
        } else {
            std::cerr << "[warn] critical_temp=" << v
                      << " out of range, using " << config.critical_temp << std::endl;
        }
    }

    return true;
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
    loadConfig(configPath, g_config);

    // Safety: critical_temp must always be above max_temp. If the user raised
    // max_temp but left the default critical_temp, adjust it instead of
    // refusing to start.
    if (g_config.critical_temp <= g_config.max_temp) {
        int adjusted = std::min(g_config.max_temp + 5, 100);
        std::cerr << "[warn] critical_temp (" << g_config.critical_temp
                  << " °C) <= max_temp (" << g_config.max_temp
                  << " °C) — adjusting critical_temp to " << adjusted << " °C" << std::endl;
        g_config.critical_temp = adjusted;
    }

    g_config.print();

    // Optional backward-compatible CLI override: set_fan_curve <max_temp> <min_speed>
    if (argc >= 3) {
        g_config.max_temp  = std::atoi(argv[1]);
        g_config.min_speed = static_cast<unsigned int>(std::atoi(argv[2]));
        std::cout << "[info] Command-line override: max_temp=" << g_config.max_temp
                  << ", min_speed=" << g_config.min_speed << std::endl;
    }

    if (g_config.max_temp < 50 || g_config.max_temp > 95) {
        std::cerr << "[error] max_temp must be between 50 and 95." << std::endl;
        return 1;
    }
    if (g_config.min_speed > 80) {
        std::cerr << "[error] min_speed must be between 0 and 80." << std::endl;
        return 1;
    }
    if (g_config.critical_temp <= g_config.max_temp) {
        std::cerr << "[error] critical_temp must be greater than max_temp." << std::endl;
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
              << " °C, sensors: "
              << (g_config.safety_all_sensors ? "core+VRAM+hotspot" : "same as curve")
              << ")" << std::endl;
    std::cout << "[info] Min fan speed          : " << g_config.min_speed << " %" << std::endl;
    std::cout << "[info] Ramp starts at         : " << (g_config.max_temp - RAMP_DELTA) << " °C" << std::endl;
    std::cout << "[info] Use VRAM temp (PCI)    : " << (g_config.use_vram_temp ? "yes" : "no") << std::endl;
    std::cout << "[info] Use Hotspot temp (PCI) : " << (g_config.use_hotspot_temp ? "yes" : "no") << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    // Initial status publish
    updatePciTemps();
    writeStatusFile();

    auto lastReassert = std::chrono::steady_clock::now();
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
                // Safety always wins, even during a manual fan test.
                desired = MAX_FAN_SPEED;
            } else if (manual >= 0) {
                desired = static_cast<unsigned int>(manual);
            } else {
                desired = getFanSpeed(temp, g_config.max_temp, g_config.min_speed);
            }

            unsigned int fans = getFanCount(gs.device);
            for (unsigned int fan = 0; fan < fans; fan++) {
                nvmlDeviceSetFanSpeed_v2(gs.device, fan, desired);
            }
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

                unsigned int fan_speed = 0;
                nvmlDeviceGetFanSpeed_v2(gs.device, 0, &fan_speed);

                std::cout << "[temp] GPU " << i << " [" << gs.pci_bus_id << "]"
                          << " core=" << core << " °C"
                          << " vram=";
                if (gs.vram_available) std::cout << gs.vram_temp_c << " °C";
                else                   std::cout << "N/A";
                std::cout << " hotspot=";
                if (gs.hotspot_available) std::cout << gs.hotspot_temp_c << " °C";
                else                      std::cout << "N/A";
                std::cout << " fan=" << fan_speed << " %"
                          << (gs.critical_latched ? " [CRITICAL LOCK]"
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

        unsigned int fans = getFanCount(gs.device);
        for (unsigned int fan = 0; fan < fans; fan++) {
            nvmlReturn_t r = nvmlDeviceSetDefaultFanSpeed_v2(gs.device, fan);
            if (r != NVML_SUCCESS) {
                std::cerr << "[warn] Could not restore automatic fan " << fan
                          << " on GPU [" << gs.pci_bus_id << "]: "
                          << nvmlErrorString(r) << std::endl;
            }
        }
    }

    std::cout << "[info] Done." << std::endl;

    nvmlShutdown();
    return 0;
}
