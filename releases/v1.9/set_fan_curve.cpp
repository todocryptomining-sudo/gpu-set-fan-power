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
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#include <cmath>

#include "nvml_min.h"
#include "gpu_temps.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_FAN_SPEED         100
#define DEFAULT_MAX_TEMP       85
#define DEFAULT_MIN_SPEED      40
#define DEFAULT_MAX_SPEED     100
#define DEFAULT_POWER_LIMIT     0
#define DEFAULT_PERSISTENCE     1
#define DEFAULT_CRITICAL_TEMP  90

#define MAX_TEMP_MIN           50
#define MAX_TEMP_MAX          100
#define MIN_SPEED_MIN           0
#define MIN_SPEED_MAX          80
#define MAX_SPEED_MIN          20
#define MAX_SPEED_MAX         100
#define CRITICAL_TEMP_MIN      50
#define CRITICAL_TEMP_MAX     110

#define FAN_UPDATE_MIN_MS      250
#define FAN_UPDATE_MAX_MS    60000
#define FAN_THRESHOLD_MIN       0
#define FAN_THRESHOLD_MAX      25
#define SENSOR_INTERVAL_MIN_MS 250
#define SENSOR_INTERVAL_MAX_MS 60000
#define SENSOR_TIMEOUT_MIN_MS 1000
#define SENSOR_TIMEOUT_MAX_MS 300000
#define SENSOR_TEMP_MIN_MIN     1
#define SENSOR_TEMP_MIN_MAX    60
#define SENSOR_TEMP_MAX_MIN    60
#define SENSOR_TEMP_MAX_MAX   150

#define CONFIG_FILE_NAME       "set-fan-config"
#define RAMP_DELTA             40
#define POLICY_REASSERT_S      60
#define SUMMARY_INTERVAL_S     30
#define CRITICAL_HYSTERESIS     5
#define FAN_ZERO_ALERT_CYCLES   5
#define CRITICAL_FAN_SPEED      85
#define FAN_WRITE_REASSERT_MS 60000
#define POWER_CHECK_INTERVAL_MS 30000
#define RECOVERY_PROBE_INTERVAL_MS 60000
#define MAX_RECOVERY_ATTEMPTS   3
#define NVML_FAIL_STREAK_LIMIT  3
#define SENSOR_FAIL_STREAK_LIMIT 3
#define STATUS_DIR             "/run/set-fan-curve"
#define STATUS_FILE            STATUS_DIR "/temps"
#define FANS_FILE              STATUS_DIR "/fans"
#define STATE_FILE             STATUS_DIR "/state"
#define FAN_STATE_FILE         STATUS_DIR "/fanstate"
#define MANUAL_FILE            STATUS_DIR "/manual"

static constexpr int TEMP_SENSOR_GPU = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────
enum class GpuRuntimeState {
    INITIALIZING,
    NORMAL,
    SENSOR_ERROR,
    NVML_ERROR,
    GPU_LOST,
    RECOVERING,
    DISABLED
};

enum class PowerLimitMode {
    STARTUP,
    MONITOR,
    ENFORCE
};

static const char* stateName(GpuRuntimeState state) {
    switch (state) {
        case GpuRuntimeState::INITIALIZING: return "INITIALIZING";
        case GpuRuntimeState::NORMAL:       return "NORMAL";
        case GpuRuntimeState::SENSOR_ERROR: return "SENSOR_ERROR";
        case GpuRuntimeState::NVML_ERROR:   return "NVML_ERROR";
        case GpuRuntimeState::GPU_LOST:     return "GPU_LOST";
        case GpuRuntimeState::RECOVERING:   return "RECOVERING";
        case GpuRuntimeState::DISABLED:     return "DISABLED";
    }
    return "UNKNOWN";
}

static const char* powerModeName(PowerLimitMode mode) {
    switch (mode) {
        case PowerLimitMode::STARTUP: return "startup";
        case PowerLimitMode::MONITOR: return "monitor";
        case PowerLimitMode::ENFORCE: return "enforce";
    }
    return "monitor";
}

struct TempReading {
    bool ok = false;
    bool stale = false;
    int  temp = -1;
};

struct GpuState {
    nvmlDevice_t device = nullptr;
    std::string  pci_bus_id;
    std::string  uuid;
    std::string  name;
    bool         valid = false;
    /* Se habilita solo después de una lectura PCI validada; nunca se asume
     * que BAR0/MMIO sea seguro al construir el estado. */
    bool         mmio_allowed = false;

    GpuRuntimeState state = GpuRuntimeState::INITIALIZING;
    std::chrono::steady_clock::time_point state_since = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_recovery_probe{};
    int          recovery_attempts = 0;
    bool         auto_restored = false;

    bool         core_available = false;
    int          core_temp_c = 0;
    std::chrono::steady_clock::time_point core_last_ok{};
    int          core_fail_streak = 0;

    bool         vram_available = false;
    bool         vram_ever_seen = false;
    int          vram_temp_c = 0;
    std::chrono::steady_clock::time_point vram_last_ok{};
    int          vram_fail_streak = 0;

    bool         hotspot_available = false;
    bool         hotspot_ever_seen = false;
    int          hotspot_temp_c = 0;
    std::chrono::steady_clock::time_point hotspot_last_ok{};
    int          hotspot_fail_streak = 0;

    bool         pci_profile_known = false;
    gpu_temps_status_t pci_status = GPU_TEMPS_INVALID;
    int          pci_fail_streak = 0;
    bool         pci_error_logged = false;

    std::chrono::steady_clock::time_point vram_boost_since{};
    int          vram_boost_speed = 0;
    bool         critical_latched = false;
    bool         effective_ok = false;
    bool         effective_stale = false;
    int          effective_temp_c = -1;
    std::chrono::steady_clock::time_point effective_last_ok{};
    int          sensor_fail_streak = 0;

    unsigned int fan_count = 0;
    bool         fan_count_fallback = false;
    int          fan_speed[GPU_TEMPS_MAX_FANS];
    bool         fan_read_ok[GPU_TEMPS_MAX_FANS];
    int          fan_read_fail_streak[GPU_TEMPS_MAX_FANS];
    int          fan_zero_streak[GPU_TEMPS_MAX_FANS];
    bool         fan_zero_alerted[GPU_TEMPS_MAX_FANS];
    int          fan_target[GPU_TEMPS_MAX_FANS];
    int          fan_desired[GPU_TEMPS_MAX_FANS];
    int          fan_applied[GPU_TEMPS_MAX_FANS];
    nvmlReturn_t fan_last_write_error[GPU_TEMPS_MAX_FANS];
    std::chrono::steady_clock::time_point fan_last_write_time[GPU_TEMPS_MAX_FANS];
    bool         fan_resync_needed[GPU_TEMPS_MAX_FANS];
    bool         fan_policy_manual = false;
    bool         fan_policy_warned = false;
    bool         all_fans_zero_alerted = false;

    unsigned int power_limit_requested_mw = 0;
    unsigned int power_limit_applied_mw = 0;
    unsigned int power_limit_observed_mw = 0;
    bool         power_limit_supported = true;
    std::chrono::steady_clock::time_point power_last_check{};
    bool         power_limit_warned = false;

    int          nvml_fail_streak = 0;
    nvmlReturn_t last_nvml_error = NVML_SUCCESS;
    std::chrono::steady_clock::time_point last_nvml_error_log{};
    std::chrono::steady_clock::time_point last_summary{};

    GpuState() {
        for (int f = 0; f < GPU_TEMPS_MAX_FANS; f++) {
            fan_speed[f] = -1;
            fan_read_ok[f] = false;
            fan_read_fail_streak[f] = 0;
            fan_zero_streak[f] = 0;
            fan_zero_alerted[f] = false;
            fan_target[f] = -1;
            fan_desired[f] = -1;
            fan_applied[f] = -1;
            fan_last_write_error[f] = NVML_SUCCESS;
            fan_resync_needed[f] = true;
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
    unsigned int power_limit = DEFAULT_POWER_LIMIT;
    int          persistence_mode = DEFAULT_PERSISTENCE;
    int          critical_temp = DEFAULT_CRITICAL_TEMP;
    bool         use_vram_temp = true;
    bool         use_hotspot_temp = false;
    bool         safety_all_sensors = false;

    unsigned int fan_update_interval_ms = 2000;
    int          fan_change_threshold = 2;
    unsigned int sensor_interval_ms = 2000;
    unsigned int sensor_timeout_ms = 6000;
    int          sensor_min_temp = 1;
    int          sensor_max_temp = 125;

    PowerLimitMode power_limit_mode = PowerLimitMode::ENFORCE;
    bool           power_limit_mode_set = false;
    bool           stop_on_gpu_lost = true;
    bool           auto_recover_gpu = false;
    bool           restore_auto_on_sensor_failure = true;
    bool           log_nvml_errors = true;
    bool           log_pcie_errors = true;

    std::string sensorList() const {
        std::string s = "core";
        if (use_vram_temp)    s += " + VRAM";
        if (use_hotspot_temp) s += " + hotspot";
        return s;
    }

    std::string lockSensors() const {
        if (safety_all_sensors) {
            return "core + VRAM + hotspot (todos los disponibles)";
        }
        return sensorList() + " (los mismos que la curva)";
    }

    void print() const {
        std::cout << "[config] Cargada:" << std::endl;
        std::cout << "  max_temp                    = " << max_temp << " °C" << std::endl;
        std::cout << "  min_speed                   = " << min_speed << " % (suelo)" << std::endl;
        std::cout << "  max_speed                   = " << max_speed << " % (tope normal; el bloqueo crítico lo ignora)" << std::endl;
        std::cout << "  critical_temp               = " << critical_temp << " °C (bloqueo con latch)" << std::endl;
        std::cout << "  power_limit                 = "
                  << (power_limit ? std::to_string(power_limit) + " W" : "desactivado") << std::endl;
        std::cout << "  power_limit_mode            = " << powerModeName(power_limit_mode)
                  << (power_limit_mode_set ? "" : " (clave no presente: compatibilidad v1.7)") << std::endl;
        std::cout << "  persistence                 = " << (persistence_mode ? "activado" : "desactivado") << std::endl;
        std::cout << "  use_vram_temp               = " << (use_vram_temp ? "true (PCI directo)" : "false") << std::endl;
        std::cout << "  use_hotspot_temp            = " << (use_hotspot_temp ? "true (PCI directo)" : "false") << std::endl;
        std::cout << "  safety_all_sensors          = " << (safety_all_sensors ? "true (bloqueo con todos)" : "false (bloqueo con los de la curva)") << std::endl;
        std::cout << "  fan_update_interval_ms      = " << fan_update_interval_ms << std::endl;
        std::cout << "  fan_change_threshold        = " << fan_change_threshold << " %" << std::endl;
        std::cout << "  sensor_interval_ms          = " << sensor_interval_ms << std::endl;
        std::cout << "  sensor_timeout_ms           = " << sensor_timeout_ms << std::endl;
        std::cout << "  sensor_min_temp / max_temp  = " << sensor_min_temp << " / " << sensor_max_temp << " °C" << std::endl;
        std::cout << "  stop_on_gpu_lost            = " << (stop_on_gpu_lost ? "true" : "false") << std::endl;
        std::cout << "  auto_recover_gpu            = " << (auto_recover_gpu ? "true" : "false") << std::endl;
        std::cout << "  restore_auto_on_sensor_failure = " << (restore_auto_on_sensor_failure ? "true" : "false") << std::endl;
        std::cout << "  log_nvml_errors             = " << (log_nvml_errors ? "true" : "false") << std::endl;
        std::cout << "  log_pcie_errors             = " << (log_pcie_errors ? "true" : "false") << std::endl;
        std::cout << "  curve sensors               = " << sensorList() << std::endl;
        std::cout << "  lock sensors                = " << lockSensors() << std::endl;
        std::cout << "  ramp_start                  = " << (max_temp - RAMP_DELTA) << " °C" << std::endl;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────────────────────────────────────
static unsigned int g_device_count = 0;
static volatile std::sig_atomic_t g_shutdown = 0;
static volatile std::sig_atomic_t g_shutdown_signal = 0;
static Config g_config;
static std::vector<GpuState> g_gpu_states;
static std::string g_stop_reason = "señal de terminación";
static bool g_control_started = false;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string formatPciBusId(const nvmlPciInfo_t& pci) {
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

static long long msSince(const std::chrono::steady_clock::time_point& t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t).count();
}

static bool plausibleTemp(int value) {
    return value >= g_config.sensor_min_temp && value <= g_config.sensor_max_temp;
}

static std::string tempOrNA(bool available, int value) {
    return available ? (std::to_string(value) + " °C") : "N/A";
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
        "power_limit_mode",
        "persistence",
        "use_vram_temp",
        "use_hotspot_temp",
        "safety_all_sensors",
        "fan_update_interval_ms",
        "fan_change_threshold",
        "sensor_interval_ms",
        "sensor_timeout_ms",
        "sensor_min_temp",
        "sensor_max_temp",
        "stop_on_gpu_lost",
        "auto_recover_gpu",
        "restore_auto_on_sensor_failure",
        "log_nvml_errors",
        "log_pcie_errors",
    };

    for (const char* candidate : keys) {
        if (key == candidate) return true;
    }
    return false;
}

static std::string lineTag(int line) {
    return line > 0 ? ("línea " + std::to_string(line) + ": ") : std::string();
}

static bool parsePowerMode(const std::string& value, PowerLimitMode& out) {
    if (value == "startup") { out = PowerLimitMode::STARTUP; return true; }
    if (value == "monitor") { out = PowerLimitMode::MONITOR; return true; }
    if (value == "enforce") { out = PowerLimitMode::ENFORCE; return true; }
    return false;
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

    if (readInt("fan_update_interval_ms", FAN_UPDATE_MIN_MS,
                FAN_UPDATE_MAX_MS, v))
        config.fan_update_interval_ms = static_cast<unsigned int>(v);
    if (readInt("fan_change_threshold", FAN_THRESHOLD_MIN,
                FAN_THRESHOLD_MAX, v))
        config.fan_change_threshold = v;
    if (readInt("sensor_interval_ms", SENSOR_INTERVAL_MIN_MS,
                SENSOR_INTERVAL_MAX_MS, v))
        config.sensor_interval_ms = static_cast<unsigned int>(v);
    if (readInt("sensor_timeout_ms", SENSOR_TIMEOUT_MIN_MS,
                SENSOR_TIMEOUT_MAX_MS, v))
        config.sensor_timeout_ms = static_cast<unsigned int>(v);
    if (readInt("sensor_min_temp", SENSOR_TEMP_MIN_MIN,
                SENSOR_TEMP_MIN_MAX, v))
        config.sensor_min_temp = v;
    if (readInt("sensor_max_temp", SENSOR_TEMP_MAX_MIN,
                SENSOR_TEMP_MAX_MAX, v))
        config.sensor_max_temp = v;

    if (readInt("stop_on_gpu_lost", 0, 1, v))
        config.stop_on_gpu_lost = (v != 0);
    if (readInt("auto_recover_gpu", 0, 1, v))
        config.auto_recover_gpu = (v != 0);
    if (readInt("restore_auto_on_sensor_failure", 0, 1, v))
        config.restore_auto_on_sensor_failure = (v != 0);
    if (readInt("log_nvml_errors", 0, 1, v))
        config.log_nvml_errors = (v != 0);
    if (readInt("log_pcie_errors", 0, 1, v))
        config.log_pcie_errors = (v != 0);

    auto mode_it = values.find("power_limit_mode");
    if (mode_it != values.end()) {
        config.power_limit_mode_set = true;
        PowerLimitMode mode;
        if (!parsePowerMode(mode_it->second.value, mode)) {
            diag.errors.push_back(lineTag(mode_it->second.line) +
                "power_limit_mode='" + mode_it->second.value +
                "' no es válido (startup, monitor o enforce)");
        } else {
            config.power_limit_mode = mode;
        }
    } else if (config.power_limit > 0) {
        diag.infos.push_back(
            "power_limit_mode no está en el archivo: se usa 'enforce' para "
            "conservar el comportamiento de v1.7. Para Vast.ai se recomienda "
            "power_limit_mode=monitor");
    } else {
        config.power_limit_mode = PowerLimitMode::MONITOR;
        diag.infos.push_back(
            "power_limit=0: no se aplicará ningún límite de potencia; "
            "power_limit_mode=monitor no tiene efecto");
    }

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

    if (config.sensor_min_temp >= config.sensor_max_temp) {
        diag.errors.push_back("sensor_min_temp (" +
            std::to_string(config.sensor_min_temp) +
            ") debe ser menor que sensor_max_temp (" +
            std::to_string(config.sensor_max_temp) + ")");
    }

    if (config.sensor_timeout_ms < config.sensor_interval_ms) {
        diag.errors.push_back("sensor_timeout_ms (" +
            std::to_string(config.sensor_timeout_ms) +
            ") no puede ser menor que sensor_interval_ms (" +
            std::to_string(config.sensor_interval_ms) +
            "): los sensores caducarían antes de la siguiente muestra");
    } else if (config.sensor_timeout_ms < config.sensor_interval_ms * 2) {
        diag.warnings.push_back(
            "sensor_timeout_ms es menos del doble de sensor_interval_ms; "
            "un solo fallo transitorio puede marcar el sensor como obsoleto");
    }

    if (config.fan_update_interval_ms < config.sensor_interval_ms) {
        diag.infos.push_back(
            "fan_update_interval_ms < sensor_interval_ms: se reutiliza la "
            "última temperatura y el umbral evita escrituras redundantes");
    }

    if (!config.restore_auto_on_sensor_failure) {
        diag.warnings.push_back(
            "restore_auto_on_sensor_failure=0: si los sensores fallan se "
            "fijará max_speed en lugar de devolver el control automático; "
            "los ventiladores pueden quedar en una velocidad fija");
    }

    if (config.auto_recover_gpu) {
        diag.infos.push_back(
            "auto_recover_gpu=1: solo se hacen sondeos NVML seguros; nunca se "
            "ejecuta nvidia-smi --gpu-reset, reboot ni se reinicia Docker/Vast.ai");
    }

    if (config.stop_on_gpu_lost) {
        diag.infos.push_back(
            "stop_on_gpu_lost=1: el servicio se detiene si no queda ninguna "
            "GPU controlable; con otras GPUs sanas sigue funcionando");
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
    std::cout << "  max_temp                    = " << config.max_temp
              << " °C  (rango " << MAX_TEMP_MIN << "-" << MAX_TEMP_MAX << ")" << std::endl;
    std::cout << "  min_speed                   = " << config.min_speed
              << " %   (rango " << MIN_SPEED_MIN << "-" << MIN_SPEED_MAX
              << ", suelo)" << std::endl;
    std::cout << "  max_speed                   = " << config.max_speed
              << " %   (rango " << MAX_SPEED_MIN << "-" << MAX_SPEED_MAX
              << ", tope normal)" << std::endl;
    std::cout << "  critical_temp               = " << config.critical_temp
              << " °C  (rango " << CRITICAL_TEMP_MIN << "-" << CRITICAL_TEMP_MAX
              << ", bloqueo)" << std::endl;
    std::cout << "  power_limit                 = "
              << (config.power_limit ? std::to_string(config.power_limit) + " W"
                                     : "desactivado") << std::endl;
    std::cout << "  power_limit_mode            = "
              << powerModeName(config.power_limit_mode)
              << (config.power_limit_mode_set ? "" : " (clave no presente)") << std::endl;
    std::cout << "  persistence                 = "
              << (config.persistence_mode ? "activado" : "desactivado") << std::endl;
    std::cout << "  use_vram_temp               = " << (config.use_vram_temp ? 1 : 0) << std::endl;
    std::cout << "  use_hotspot_temp            = " << (config.use_hotspot_temp ? 1 : 0) << std::endl;
    std::cout << "  safety_all_sensors          = " << (config.safety_all_sensors ? 1 : 0) << std::endl;
    std::cout << "  fan_update_interval_ms      = " << config.fan_update_interval_ms << std::endl;
    std::cout << "  fan_change_threshold        = " << config.fan_change_threshold << " %" << std::endl;
    std::cout << "  sensor_interval_ms          = " << config.sensor_interval_ms << std::endl;
    std::cout << "  sensor_timeout_ms           = " << config.sensor_timeout_ms << std::endl;
    std::cout << "  sensor_min_temp             = " << config.sensor_min_temp << " °C" << std::endl;
    std::cout << "  sensor_max_temp             = " << config.sensor_max_temp << " °C" << std::endl;
    std::cout << "  stop_on_gpu_lost            = " << (config.stop_on_gpu_lost ? 1 : 0) << std::endl;
    std::cout << "  auto_recover_gpu            = " << (config.auto_recover_gpu ? 1 : 0) << std::endl;
    std::cout << "  restore_auto_on_sensor_failure = " << (config.restore_auto_on_sensor_failure ? 1 : 0) << std::endl;
    std::cout << "  log_nvml_errors             = " << (config.log_nvml_errors ? 1 : 0) << std::endl;
    std::cout << "  log_pcie_errors             = " << (config.log_pcie_errors ? 1 : 0) << std::endl;
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

static void signalHandler(int signal_number) {
    g_shutdown = 1;
    g_shutdown_signal = signal_number;
}

// ─────────────────────────────────────────────────────────────────────────────
// NVML error handling and per-GPU state machine
// ─────────────────────────────────────────────────────────────────────────────
static bool nvmlIsLost(nvmlReturn_t result) {
    return result == NVML_ERROR_GPU_IS_LOST ||
           result == NVML_ERROR_RESET_REQUIRED;
}

static bool anyOtherHealthyGpu(unsigned int failed_index) {
    for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
        if (i == failed_index) continue;
        const GpuState& other = g_gpu_states[i];
        if (!other.valid) continue;
        if (other.state == GpuRuntimeState::GPU_LOST ||
            other.state == GpuRuntimeState::DISABLED ||
            other.state == GpuRuntimeState::RECOVERING) {
            continue;
        }
        return true;
    }
    return false;
}

static void transitionGpu(GpuState& gs, unsigned int gpu_index,
                          GpuRuntimeState next, const std::string& reason) {
    if (gs.state == next) return;

    std::cout << "[state] GPU " << gpu_index << " [" << gs.pci_bus_id << "] "
              << stateName(gs.state) << " -> " << stateName(next)
              << ": " << reason << std::endl;

    gs.state = next;
    gs.state_since = std::chrono::steady_clock::now();

    if (next == GpuRuntimeState::GPU_LOST ||
        next == GpuRuntimeState::DISABLED) {
        gs.mmio_allowed = false;
        gs.core_available = false;
        gs.vram_available = false;
        gs.hotspot_available = false;
        gs.effective_ok = false;
        bool closed = gpu_temps_close(gs.pci_bus_id.c_str());
        if (!closed && g_config.log_pcie_errors) {
            std::cerr << "[warn] GPU " << gpu_index
                      << ": munmap/close falló al cerrar MMIO (se continúa sin "
                         "tocar la GPU)" << std::endl;
        }
    }

    if (next == GpuRuntimeState::NORMAL) {
        /* NORMAL no convierte un MMIO inválido en válido. Solo se habilita
         * tras una lectura PCI que haya confirmado perfil, BAR0 y mapeo. */
        gs.mmio_allowed = (gs.pci_status == GPU_TEMPS_OK);
        gs.auto_restored = false;
        gs.sensor_fail_streak = 0;
        gs.nvml_fail_streak = 0;
        for (unsigned int f = 0; f < gs.fan_count; f++) {
            gs.fan_resync_needed[f] = true;
        }
    }

    if (next == GpuRuntimeState::GPU_LOST) {
        std::cerr << "[alert] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: GPU perdida. Motivo: " << reason
                  << ". Se suspenden escrituras NVML y lecturas "
                     "MMIO; no se reinicia el servidor ni Docker/Vast.ai."
                  << std::endl;
        if (g_config.stop_on_gpu_lost && g_control_started &&
            !anyOtherHealthyGpu(gpu_index)) {
            g_stop_reason = "GPU perdida y no queda ninguna GPU controlable";
            g_shutdown = 1;
        }
    }
}

static void noteNvmlError(GpuState& gs, unsigned int gpu_index,
                          nvmlReturn_t result, const std::string& what,
                          int fan = -1) {
    if (result == NVML_SUCCESS) return;

    gs.last_nvml_error = result;
    gs.nvml_fail_streak++;

    if (fan >= 0 && fan < static_cast<int>(gs.fan_count)) {
        gs.fan_resync_needed[fan] = true;
    }

    if (nvmlIsLost(result)) {
        transitionGpu(gs, gpu_index, GpuRuntimeState::GPU_LOST,
                      what + ": " + nvmlErrorString(result));
        return;
    }

    if (g_config.log_nvml_errors) {
        long long since = msSince(gs.last_nvml_error_log);
        if (gs.nvml_fail_streak == 1 || since >= 30000) {
            std::cerr << "[error] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "] " << what << " falló: " << nvmlErrorString(result)
                      << " (fallos consecutivos: " << gs.nvml_fail_streak << ")"
                      << std::endl;
            gs.last_nvml_error_log = std::chrono::steady_clock::now();
        }
    }

    if (gs.nvml_fail_streak >= NVML_FAIL_STREAK_LIMIT &&
        (gs.state == GpuRuntimeState::NORMAL ||
         gs.state == GpuRuntimeState::INITIALIZING)) {
        transitionGpu(gs, gpu_index, GpuRuntimeState::NVML_ERROR,
                      "fallos NVML consecutivos: " + what);
    }
}

static void noteNvmlSuccess(GpuState& gs) {
    gs.nvml_fail_streak = 0;
    gs.last_nvml_error = NVML_SUCCESS;

    if (gs.state == GpuRuntimeState::NVML_ERROR) {
        bool writes_ok = true;
        for (unsigned int f = 0; f < gs.fan_count; f++) {
            if (gs.fan_last_write_error[f] != NVML_SUCCESS) {
                writes_ok = false;
                break;
            }
        }
        if (writes_ok && gs.sensor_fail_streak == 0) {
            unsigned int index = static_cast<unsigned int>(&gs - g_gpu_states.data());
            transitionGpu(gs, index, GpuRuntimeState::NORMAL,
                          "NVML responde de nuevo");
        }
    }
}

static unsigned int getFanCount(nvmlDevice_t device, bool* fallback) {
    unsigned int count = 0;
    *fallback = false;
    if (nvmlDeviceGetNumFans(device, &count) != NVML_SUCCESS || count == 0) {
        count = 1;
        *fallback = true;
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
        } else if (nvmlIsLost(r)) {
            noteNvmlError(gs, gpu_index, r, "SetFanControlPolicy(MANUAL)");
            return;
        } else if (!gs.fan_policy_warned) {
            if (g_config.log_nvml_errors) {
                std::cerr << "[warn] GPU " << gpu_index << " fan " << f
                          << ": cannot set manual fan policy ("
                          << nvmlErrorString(r)
                          << ") — usando solo SetFanSpeed directo." << std::endl;
            }
            gs.fan_policy_warned = true;
        }
    }

    if (any_ok) gs.fan_policy_manual = true;
}

static void readFanSpeeds(GpuState& gs, unsigned int gpu_index) {
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED) {
        return;
    }

    for (unsigned int f = 0; f < gs.fan_count; f++) {
        unsigned int speed = 0;
        nvmlReturn_t r = nvmlDeviceGetFanSpeed_v2(gs.device, f, &speed);

        if (r == NVML_SUCCESS) {
            gs.fan_speed[f] = static_cast<int>(speed);
            gs.fan_read_ok[f] = true;
            gs.fan_read_fail_streak[f] = 0;
        } else {
            gs.fan_speed[f] = -1;
            gs.fan_read_ok[f] = false;
            gs.fan_read_fail_streak[f]++;

            if (nvmlIsLost(r)) {
                noteNvmlError(gs, gpu_index, r, "GetFanSpeed_v2");
                return;
            }
            if (g_config.log_nvml_errors &&
                gs.fan_read_fail_streak[f] == 1) {
                std::cerr << "[warn] GPU " << gpu_index << " fan " << f
                          << ": GetFanSpeed_v2 falló: " << nvmlErrorString(r)
                          << " (la velocidad reportada queda como desconocida)"
                          << std::endl;
            }
        }
    }
}

/* Decides whether a fan write is necessary. `fan_update_interval_ms` limita la
 * frecuencia de escritura; los aumentos hacia la zona crítica la saltan porque
 * la temperatura puede subir más rápido que el siguiente ciclo. */
static bool fanNeedsUpdate(const GpuState& gs, unsigned int fan, int desired,
                           bool force) {
    if (force || gs.fan_resync_needed[fan]) return true;
    if (gs.fan_applied[fan] < 0) return true;

    int applied = gs.fan_applied[fan];

    if (desired > applied && desired >= CRITICAL_FAN_SPEED) return true;

    long long since = msSince(gs.fan_last_write_time[fan]);
    if (since < static_cast<long long>(g_config.fan_update_interval_ms)) {
        return false;
    }

    int threshold = g_config.fan_change_threshold;

    if (desired > applied) {
        if (desired - applied >= threshold) return true;
    } else if (applied > desired) {
        /* Un punto extra de histéresis al bajar evita oscilaciones. */
        if (applied - desired > threshold) return true;
    }

    if (since >= FAN_WRITE_REASSERT_MS) return true;
    return false;
}

static void applyFanSpeed(GpuState& gs, unsigned int gpu_index,
                          unsigned int fan, int desired, bool force) {
    if (fan >= gs.fan_count) return;
    if (desired < 0) desired = 0;
    if (desired > MAX_FAN_SPEED) desired = MAX_FAN_SPEED;

    gs.fan_target[fan] = desired;

    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED ||
        gs.state == GpuRuntimeState::SENSOR_ERROR ||
        gs.state == GpuRuntimeState::RECOVERING) {
        return;
    }

    /* Tras un error NVML no martillemos el dispositivo: reintento lento. */
    if (gs.state == GpuRuntimeState::NVML_ERROR &&
        msSince(gs.fan_last_write_time[fan]) < 5000) {
        return;
    }

    if (!fanNeedsUpdate(gs, fan, desired, force)) return;

    gs.fan_desired[fan] = desired;
    gs.fan_last_write_time[fan] = std::chrono::steady_clock::now();

    nvmlReturn_t r = nvmlDeviceSetFanSpeed_v2(
        gs.device, fan, static_cast<unsigned int>(desired));

    if (r == NVML_SUCCESS) {
        gs.fan_applied[fan] = desired;
        gs.fan_resync_needed[fan] = false;

        if (gs.fan_last_write_error[fan] != NVML_SUCCESS) {
            std::cout << "[info] GPU " << gpu_index << " fan " << fan
                      << ": escrituras NVML recuperadas (aplicado "
                      << desired << " %)." << std::endl;
        }
        gs.fan_last_write_error[fan] = NVML_SUCCESS;
        noteNvmlSuccess(gs);
    } else {
        gs.fan_last_write_error[fan] = r;
        gs.fan_resync_needed[fan] = true;
        noteNvmlError(gs, gpu_index, r,
                      "SetFanSpeed_v2(" + std::to_string(desired) + "%)", fan);
    }
}

/* Alert when one fan reports 0 % while another fan on the same GPU is
   running, or when every fan reports 0 % with a high setpoint. NVML reports a
   percentage; it is NOT a measurement of RPM. */
static void checkFanAlerts(GpuState& gs, unsigned int gpu_index,
                           int desired) {
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
                      << ": NVML informa 0 % mientras otro ventilador gira "
                      << "(objetivo " << desired << " %, último aplicado "
                      << gs.fan_applied[f] << " %). Esto NO demuestra 0 RPM: "
                      << "revisa política manual, índice de ventilador, cable "
                      << "y controlador de la tarjeta." << std::endl;
            gs.fan_zero_alerted[f] = true;
        }
    }

    int setpoint = std::max(desired, gs.fan_applied[0]);
    if (all_zero && setpoint >= 50) {
        if (!gs.all_fans_zero_alerted) {
            std::cerr << "[alert] GPU " << gpu_index
                      << ": todos los ventiladores reportan 0 % con objetivo "
                      << desired << " %. NVML no está midiendo RPM; comprueba "
                      << "política, alimentación y controlador de la tarjeta."
                      << std::endl;
            gs.all_fans_zero_alerted = true;
        }
    } else if (!all_zero) {
        gs.all_fans_zero_alerted = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Fan curve
// ─────────────────────────────────────────────────────────────────────────────
static unsigned int getFanSpeed(int temp, int max_temp,
                                unsigned int min_speed, unsigned int max_speed) {
    if (max_speed < min_speed) max_speed = min_speed;

    int ramp_start = max_temp - RAMP_DELTA;
    unsigned int curve;

    if (temp <= ramp_start) {
        curve = min_speed;
    } else if (temp >= max_temp) {
        curve = max_speed;
    } else {
        double ratio = static_cast<double>(temp - ramp_start) /
                       static_cast<double>(RAMP_DELTA);
        curve = min_speed + static_cast<unsigned int>(ratio * (max_speed - min_speed) + 0.5);
        if (curve > max_speed) curve = max_speed;
    }

    return std::max(min_speed, std::min(curve, max_speed));
}

// ─────────────────────────────────────────────────────────────────────────────
// Sensor sampling, staleness and critical lock
// ─────────────────────────────────────────────────────────────────────────────
static bool sensorFresh(bool available,
                        const std::chrono::steady_clock::time_point& last_ok,
                        unsigned int timeout_ms) {
    if (!available) return false;
    if (last_ok.time_since_epoch().count() == 0) return false;
    return msSince(last_ok) <= static_cast<long long>(timeout_ms);
}

static bool readCoreTemp(GpuState& gs, unsigned int gpu_index) {
    unsigned int value = 0;
    nvmlReturn_t r = nvmlDeviceGetTemperature(
        gs.device, static_cast<nvmlTemperatureSensors_t>(TEMP_SENSOR_GPU), &value);

    if (r == NVML_SUCCESS) {
        if (plausibleTemp(static_cast<int>(value))) {
            gs.core_temp_c = static_cast<int>(value);
            gs.core_available = true;
            gs.core_last_ok = std::chrono::steady_clock::now();
            gs.core_fail_streak = 0;
            noteNvmlSuccess(gs);
            return true;
        }

        gs.core_fail_streak++;
        if (g_config.log_nvml_errors && gs.core_fail_streak == 1) {
            std::cerr << "[warn] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "]: temperatura de core fuera de plausibilidad ("
                      << value << " °C; rango "
                      << g_config.sensor_min_temp << "-"
                      << g_config.sensor_max_temp << ")" << std::endl;
        }
    } else {
        gs.core_fail_streak++;
        if (nvmlIsLost(r)) {
            noteNvmlError(gs, gpu_index, r, "GetTemperature(core)");
            return false;
        }
        if (g_config.log_nvml_errors &&
            (gs.core_fail_streak == 1 || msSince(gs.last_nvml_error_log) >= 30000)) {
            std::cerr << "[warn] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "]: no se pudo leer el core: " << nvmlErrorString(r)
                      << " (fallos consecutivos: " << gs.core_fail_streak << ")"
                      << std::endl;
            gs.last_nvml_error_log = std::chrono::steady_clock::now();
        }
    }

    if (msSince(gs.core_last_ok) > static_cast<long long>(g_config.sensor_timeout_ms)) {
        gs.core_available = false;
    }
    return false;
}

static void updatePciTemps(GpuState& gs, unsigned int gpu_index) {
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED ||
        gs.state == GpuRuntimeState::RECOVERING) {
        return;
    }

    gpu_temps_t temps;
    gpu_temps_status_t status = gpu_temps_read_ex(
        gs.pci_bus_id.c_str(), &temps);

    gs.pci_status = status;
    gs.pci_profile_known = temps.profile_known;

    if (status == GPU_TEMPS_OK) {
        gs.mmio_allowed = true;
        gs.pci_fail_streak = 0;
        gs.pci_error_logged = false;

        auto now = std::chrono::steady_clock::now();

        if (temps.vram_available && plausibleTemp(temps.vram_temp_c)) {
            gs.vram_temp_c = temps.vram_temp_c;
            gs.vram_available = true;
            gs.vram_ever_seen = true;
            gs.vram_last_ok = now;
            gs.vram_fail_streak = 0;
        } else {
            gs.vram_fail_streak++;
            if (gs.vram_ever_seen &&
                msSince(gs.vram_last_ok) > static_cast<long long>(g_config.sensor_timeout_ms)) {
                gs.vram_available = false;
            }
        }

        if (temps.hotspot_available && plausibleTemp(temps.hotspot_temp_c)) {
            gs.hotspot_temp_c = temps.hotspot_temp_c;
            gs.hotspot_available = true;
            gs.hotspot_ever_seen = true;
            gs.hotspot_last_ok = now;
            gs.hotspot_fail_streak = 0;
        } else {
            gs.hotspot_fail_streak++;
            if (gs.hotspot_ever_seen &&
                msSince(gs.hotspot_last_ok) > static_cast<long long>(g_config.sensor_timeout_ms)) {
                gs.hotspot_available = false;
            }
        }
    } else {
        gs.mmio_allowed = false;
        gs.pci_fail_streak++;

        if (g_config.log_pcie_errors && !gs.pci_error_logged) {
            std::cerr << "[warn] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "]: lectura PCI VRAM/hotspot no disponible: "
                      << gpu_temps_status_string(status);
            if (temps.device_id) {
                std::cerr << " (device 0x" << std::hex << temps.device_id
                          << std::dec << ")";
            }
            std::cerr << std::endl;
            gs.pci_error_logged = true;
        }

        if (gs.vram_ever_seen &&
            msSince(gs.vram_last_ok) > static_cast<long long>(g_config.sensor_timeout_ms)) {
            gs.vram_available = false;
        }
        if (gs.hotspot_ever_seen &&
            msSince(gs.hotspot_last_ok) > static_cast<long long>(g_config.sensor_timeout_ms)) {
            gs.hotspot_available = false;
        }
    }
}

static void considerSensor(TempReading& reading, bool available, int value,
                           const std::chrono::steady_clock::time_point& last_ok,
                           unsigned int timeout_ms) {
    if (!sensorFresh(available, last_ok, timeout_ms)) return;
    if (!plausibleTemp(value)) return;

    if (!reading.ok || value > reading.temp) {
        reading.ok = true;
        reading.temp = value;
    }

    if (msSince(last_ok) > static_cast<long long>(g_config.sensor_interval_ms * 2ULL)) {
        reading.stale = true;
    }
}

static TempReading computeCurveTemp(const GpuState& gs) {
    TempReading reading;

    considerSensor(reading, gs.core_available, gs.core_temp_c,
                   gs.core_last_ok, g_config.sensor_timeout_ms);

    if (g_config.use_vram_temp) {
        considerSensor(reading, gs.vram_available, gs.vram_temp_c,
                       gs.vram_last_ok, g_config.sensor_timeout_ms);
    }
    if (g_config.use_hotspot_temp) {
        considerSensor(reading, gs.hotspot_available, gs.hotspot_temp_c,
                       gs.hotspot_last_ok, g_config.sensor_timeout_ms);
    }

    return reading;
}

static TempReading computeLockTemp(const GpuState& gs) {
    if (!g_config.safety_all_sensors) return computeCurveTemp(gs);

    TempReading reading;
    considerSensor(reading, gs.core_available, gs.core_temp_c,
                   gs.core_last_ok, g_config.sensor_timeout_ms);
    considerSensor(reading, gs.vram_available, gs.vram_temp_c,
                   gs.vram_last_ok, g_config.sensor_timeout_ms);
    considerSensor(reading, gs.hotspot_available, gs.hotspot_temp_c,
                   gs.hotspot_last_ok, g_config.sensor_timeout_ms);
    return reading;
}

static void updateCriticalLatch(GpuState& gs, unsigned int gpu_index,
                                const TempReading& lock) {
    if (!lock.ok) return;

    if (lock.temp >= g_config.critical_temp) {
        if (!gs.critical_latched) {
            std::cerr << "[alert] GPU " << gpu_index << ": CRITICAL temp "
                      << lock.temp << " °C >= " << g_config.critical_temp
                      << " °C (core="
                      << tempOrNA(gs.core_available, gs.core_temp_c)
                      << ", VRAM=" << tempOrNA(gs.vram_available, gs.vram_temp_c)
                      << ", hotspot="
                      << tempOrNA(gs.hotspot_available, gs.hotspot_temp_c)
                      << ") — ventiladores fijados al 100 %" << std::endl;
        }
        gs.critical_latched = true;
    } else if (gs.critical_latched &&
               lock.temp <= (g_config.critical_temp - CRITICAL_HYSTERESIS)) {
        std::cerr << "[info] GPU " << gpu_index << ": temp " << lock.temp
                  << " °C, liberando el bloqueo crítico." << std::endl;
        gs.critical_latched = false;
    }
}

static bool restoreAutoControl(GpuState& gs, unsigned int gpu_index,
                               const char* reason) {
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED) {
        return false;
    }

    bool ok = true;
    for (unsigned int f = 0; f < gs.fan_count; f++) {
        nvmlReturn_t r = nvmlDeviceSetDefaultFanSpeed_v2(gs.device, f);
        if (r != NVML_SUCCESS) {
            ok = false;
            if (nvmlIsLost(r)) {
                noteNvmlError(gs, gpu_index, r, "SetDefaultFanSpeed_v2");
                return false;
            }
            if (g_config.log_nvml_errors) {
                std::cerr << "[warn] GPU " << gpu_index << " fan " << f
                          << ": no se pudo restaurar AUTO: "
                          << nvmlErrorString(r) << std::endl;
            }
        } else {
            gs.fan_applied[f] = -1;
            gs.fan_last_write_error[f] = NVML_SUCCESS;
            gs.fan_resync_needed[f] = true;
        }

        if (gs.fan_policy_manual) {
            nvmlReturn_t p = nvmlDeviceSetFanControlPolicy(
                gs.device, f, NVML_FAN_POLICY_AUTO);
            if (p != NVML_SUCCESS) {
                ok = false;
                if (nvmlIsLost(p)) {
                    noteNvmlError(gs, gpu_index, p, "SetFanControlPolicy(AUTO)");
                    return false;
                }
                if (g_config.log_nvml_errors) {
                    std::cerr << "[warn] GPU " << gpu_index << " fan " << f
                              << ": no se pudo restaurar la política AUTO: "
                              << nvmlErrorString(p) << std::endl;
                }
            }
        }
    }

    gs.auto_restored = true;
    std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
              << "]: control automático restaurado (" << reason << ")."
              << std::endl;
    return ok;
}

static void setSafeFanSpeed(GpuState& gs, unsigned int gpu_index,
                            unsigned int speed, const char* reason) {
    for (unsigned int f = 0; f < gs.fan_count; f++) {
        nvmlReturn_t r = nvmlDeviceSetFanSpeed_v2(gs.device, f, speed);
        if (r == NVML_SUCCESS) {
            gs.fan_desired[f] = static_cast<int>(speed);
            gs.fan_applied[f] = static_cast<int>(speed);
            gs.fan_last_write_error[f] = NVML_SUCCESS;
        } else {
            gs.fan_last_write_error[f] = r;
            if (nvmlIsLost(r)) {
                noteNvmlError(gs, gpu_index, r, "SetFanSpeed_v2(safe)", f);
                return;
            }
        }
    }
    std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
              << "]: velocidad segura " << speed << " % (" << reason << ")."
              << std::endl;
}

static void enterSensorFailure(GpuState& gs, unsigned int gpu_index,
                               const std::string& reason) {
    if (gs.state == GpuRuntimeState::SENSOR_ERROR) return;

    transitionGpu(gs, gpu_index, GpuRuntimeState::SENSOR_ERROR, reason);

    if (g_config.restore_auto_on_sensor_failure) {
        restoreAutoControl(gs, gpu_index, "fallo de sensores");
    } else {
        setSafeFanSpeed(gs, gpu_index, g_config.max_speed,
                        "fallo de sensores; restore_auto_on_sensor_failure=0");
    }
}

static void evaluateSensorHealth(GpuState& gs, unsigned int gpu_index,
                                 const TempReading& curve) {
    bool required_lost = false;

    if (g_config.use_vram_temp && gs.vram_ever_seen && !gs.vram_available) {
        required_lost = true;
    }
    if (g_config.use_hotspot_temp && gs.hotspot_ever_seen &&
        !gs.hotspot_available) {
        required_lost = true;
    }

    if (curve.ok) {
        gs.sensor_fail_streak = 0;
        if (gs.state == GpuRuntimeState::SENSOR_ERROR) {
            transitionGpu(gs, gpu_index, GpuRuntimeState::NORMAL,
                          "sensores disponibles de nuevo");
            setManualFanPolicy(gs, gpu_index);
            for (unsigned int f = 0; f < gs.fan_count; f++) {
                gs.fan_resync_needed[f] = true;
            }
        } else if (gs.state == GpuRuntimeState::INITIALIZING) {
            transitionGpu(gs, gpu_index, GpuRuntimeState::NORMAL,
                          "primera lectura de sensores válida");
        }
        return;
    }

    gs.sensor_fail_streak++;
    if ((gs.sensor_fail_streak >= SENSOR_FAIL_STREAK_LIMIT || required_lost) &&
        (gs.state == GpuRuntimeState::NORMAL ||
         gs.state == GpuRuntimeState::INITIALIZING ||
         gs.state == GpuRuntimeState::NVML_ERROR)) {
        enterSensorFailure(gs, gpu_index,
            "no hay ninguna temperatura válida (" +
            std::to_string(gs.sensor_fail_streak) + " fallos)");
    }
}

static void sampleGpu(GpuState& gs, unsigned int gpu_index) {
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED ||
        gs.state == GpuRuntimeState::RECOVERING) {
        return;
    }

    readCoreTemp(gs, gpu_index);
    updatePciTemps(gs, gpu_index);
    readFanSpeeds(gs, gpu_index);

    TempReading curve = computeCurveTemp(gs);
    TempReading lock = computeLockTemp(gs);

    gs.effective_ok = curve.ok;
    gs.effective_temp_c = curve.ok ? curve.temp : -1;
    gs.effective_stale = curve.stale;
    if (curve.ok) {
        gs.effective_last_ok = std::chrono::steady_clock::now();
    }

    if (curve.ok) updateCriticalLatch(gs, gpu_index, lock);
    evaluateSensorHealth(gs, gpu_index, curve);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fan control
// ─────────────────────────────────────────────────────────────────────────────
static void controlGpu(GpuState& gs, unsigned int gpu_index, int manual,
                       bool manual_changed) {
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED ||
        gs.state == GpuRuntimeState::SENSOR_ERROR ||
        gs.state == GpuRuntimeState::RECOVERING ||
        !gs.valid) {
        return;
    }

    if (!gs.effective_ok || gs.effective_temp_c < 0) return;
    if (msSince(gs.effective_last_ok) >
        static_cast<long long>(g_config.sensor_timeout_ms)) {
        return;
    }

    int desired;
    if (gs.critical_latched) {
        desired = MAX_FAN_SPEED;
    } else if (manual >= 0) {
        desired = manual;
    } else {
        desired = static_cast<int>(getFanSpeed(
            gs.effective_temp_c, g_config.max_temp,
            g_config.min_speed, g_config.max_speed));

        // VRAM debe permanecer caliente 30 s después de aplicar el máximo.
        // Subir 1 punto cada 2 s; liberar al bajar 5 °C o caducar el sensor.
        bool fans_at_max = gs.fan_count > 0;
        for (unsigned int f = 0; f < gs.fan_count; ++f) {
            if (gs.fan_applied[f] < static_cast<int>(g_config.max_speed))
                fans_at_max = false;
        }
        bool fresh_vram = sensorFresh(gs.vram_available, gs.vram_last_ok,
                                      g_config.sensor_timeout_ms) &&
                          gs.vram_fail_streak == 0;
        auto now = std::chrono::steady_clock::now();
        if (!fresh_vram || gs.vram_temp_c <= g_config.max_temp - 5) {
            gs.vram_boost_since = {};
            gs.vram_boost_speed = 0;
        } else if (gs.vram_temp_c >= g_config.max_temp && fans_at_max) {
            if (gs.vram_boost_since == std::chrono::steady_clock::time_point{})
                gs.vram_boost_since = now;
            long long elapsed = msSince(gs.vram_boost_since);
            if (elapsed >= 30000) {
                gs.vram_boost_speed = std::min(100, static_cast<int>(g_config.max_speed)
                    + 1 + static_cast<int>((elapsed - 30000) / 2000));
            }
        } else if (gs.vram_boost_speed == 0) {
            gs.vram_boost_since = {};
        }
        desired = std::max(desired, gs.vram_boost_speed);
    }

    bool force = gs.critical_latched || manual_changed;
    for (unsigned int f = 0; f < gs.fan_count; f++) {
        applyFanSpeed(gs, gpu_index, f, desired, force);
    }

    checkFanAlerts(gs, gpu_index, desired);
}

// ─────────────────────────────────────────────────────────────────────────────
// Power limit
// ─────────────────────────────────────────────────────────────────────────────
static bool setPowerLimit(GpuState& gs, unsigned int gpu_index,
                          unsigned int limit_mw) {
    nvmlReturn_t r = nvmlDeviceSetPowerManagementLimit(gs.device, limit_mw);
    if (r != NVML_SUCCESS) {
        if (r == NVML_ERROR_NOT_SUPPORTED) {
            gs.power_limit_supported = false;
            if (!gs.power_limit_warned) {
                std::cerr << "[warn] GPU " << gpu_index
                          << ": SetPowerManagementLimit no soportado; no se "
                             "insistirá con el límite de potencia." << std::endl;
                gs.power_limit_warned = true;
            }
            return false;
        }
        noteNvmlError(gs, gpu_index, r, "SetPowerManagementLimit");
        return false;
    }

    gs.power_limit_applied_mw = limit_mw;
    gs.power_limit_observed_mw = limit_mw;
    std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
              << "]: power limit aplicado a " << (limit_mw / 1000) << " W"
              << std::endl;
    noteNvmlSuccess(gs);
    return true;
}

static void initPowerLimit(GpuState& gs, unsigned int gpu_index) {
    if (g_config.power_limit == 0) {
        std::cout << "[info] GPU " << gpu_index
                  << ": power limit desactivado." << std::endl;
        return;
    }

    unsigned int min_limit = 0, max_limit = 0;
    nvmlReturn_t r = nvmlDeviceGetPowerManagementLimitConstraints(
        gs.device, &min_limit, &max_limit);
    if (r != NVML_SUCCESS) {
        if (r == NVML_ERROR_NOT_SUPPORTED) {
            gs.power_limit_supported = false;
            if (!gs.power_limit_warned) {
                std::cerr << "[warn] GPU " << gpu_index
                          << ": control de potencia no soportado por el driver "
                             "o la GPU virtual; se ignora power_limit."
                          << std::endl;
                gs.power_limit_warned = true;
            }
            return;
        }
        noteNvmlError(gs, gpu_index, r, "GetPowerManagementLimitConstraints");
        return;
    }

    unsigned int requested = g_config.power_limit * 1000u;
    if (requested < min_limit || requested > max_limit) {
        unsigned int clamped = (requested < min_limit) ? min_limit : max_limit;
        if (!gs.power_limit_warned) {
            std::cerr << "[warn] GPU " << gpu_index << ": power_limit "
                      << g_config.power_limit << " W fuera del rango ("
                      << (min_limit / 1000) << "-" << (max_limit / 1000)
                      << " W); se usará " << (clamped / 1000) << " W."
                      << std::endl;
            gs.power_limit_warned = true;
        }
        requested = clamped;
    }
    gs.power_limit_requested_mw = requested;

    unsigned int current = 0;
    if (nvmlDeviceGetPowerManagementLimit(gs.device, &current) == NVML_SUCCESS) {
        gs.power_limit_observed_mw = current;
    }

    if (g_config.power_limit_mode == PowerLimitMode::MONITOR) {
        std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: power_limit_mode=monitor; límite actual "
                  << (current / 1000) << " W (no se modificará)."
                  << std::endl;
        return;
    }

    if (g_config.power_limit_mode == PowerLimitMode::STARTUP) {
        std::cout << "[info] GPU " << gpu_index
                  << ": power_limit_mode=startup; se aplica una vez y no se "
                     "reafirma." << std::endl;
    }

    setPowerLimit(gs, gpu_index, requested);
}

static void checkPowerLimit(GpuState& gs, unsigned int gpu_index) {
    if (g_config.power_limit == 0 ||
        !gs.power_limit_supported ||
        g_config.power_limit_mode == PowerLimitMode::STARTUP ||
        gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED ||
        gs.state == GpuRuntimeState::RECOVERING) {
        return;
    }

    if (msSince(gs.power_last_check) < POWER_CHECK_INTERVAL_MS) return;
    gs.power_last_check = std::chrono::steady_clock::now();

    unsigned int current = 0;
    nvmlReturn_t r = nvmlDeviceGetPowerManagementLimit(gs.device, &current);
    if (r != NVML_SUCCESS) {
        noteNvmlError(gs, gpu_index, r, "GetPowerManagementLimit");
        return;
    }

    if (current != gs.power_limit_observed_mw) {
        std::cout << "[power] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: límite observado " << (current / 1000) << " W (antes "
                  << (gs.power_limit_observed_mw / 1000) << " W)."
                  << std::endl;
        gs.power_limit_observed_mw = current;
    }

    if (g_config.power_limit_mode != PowerLimitMode::ENFORCE) return;
    if (gs.power_limit_applied_mw == 0) {
        setPowerLimit(gs, gpu_index, gs.power_limit_requested_mw);
        return;
    }
    if (current != gs.power_limit_applied_mw) {
        std::cout << "[power] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: power limit cambiado a " << (current / 1000)
                  << " W; reafirmando "
                  << (gs.power_limit_applied_mw / 1000) << " W." << std::endl;
        setPowerLimit(gs, gpu_index, gs.power_limit_applied_mw);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Controlled recovery (never GPU reset, reboot, Docker or Vast.ai restarts)
// ─────────────────────────────────────────────────────────────────────────────
static bool findDeviceForRecovery(GpuState& gs, nvmlDevice_t* device) {
    *device = nullptr;

    if (!gs.uuid.empty()) {
        nvmlReturn_t r = nvmlDeviceGetHandleByUUID(gs.uuid.c_str(), device);
        if (r == NVML_SUCCESS && *device) return true;
    }

    unsigned int count = 0;
    if (nvmlDeviceGetCount(&count) != NVML_SUCCESS) return false;

    for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t candidate = nullptr;
        if (nvmlDeviceGetHandleByIndex(i, &candidate) != NVML_SUCCESS) continue;

        nvmlPciInfo_t pci;
        memset(&pci, 0, sizeof(pci));
        if (nvmlDeviceGetPciInfo_v3(candidate, &pci) != NVML_SUCCESS) continue;

        std::string bus = formatPciBusId(pci);
        if (bus != gs.pci_bus_id && gs.uuid.empty()) continue;

        if (!gs.uuid.empty()) {
            char uuid[NVML_DEVICE_UUID_BUFFER_SIZE] = {0};
            if (nvmlDeviceGetUUID(candidate, uuid, sizeof(uuid)) != NVML_SUCCESS) {
                continue;
            }
            if (uuid != gs.uuid) continue;
        }

        *device = candidate;
        return true;
    }

    return false;
}

static bool probeGpu(GpuState& gs, unsigned int gpu_index) {
    nvmlDevice_t device = nullptr;
    if (!findDeviceForRecovery(gs, &device)) {
        if (g_config.log_nvml_errors) {
            std::cerr << "[recover] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "]: no se encuentra el dispositivo por UUID/PCI."
                      << std::endl;
        }
        return false;
    }

    unsigned int temp = 0;
    nvmlReturn_t r = nvmlDeviceGetTemperature(
        device, static_cast<nvmlTemperatureSensors_t>(TEMP_SENSOR_GPU), &temp);
    if (r != NVML_SUCCESS) {
        if (g_config.log_nvml_errors) {
            std::cerr << "[recover] GPU " << gpu_index << ": sondeo NVML falló: "
                      << nvmlErrorString(r) << std::endl;
        }
        return false;
    }

    gs.device = device;
    gs.valid = true;
    transitionGpu(gs, gpu_index, GpuRuntimeState::RECOVERING,
                  "dispositivo responde a un sondeo NVML seguro");

    /* El recuento de ventiladores puede haber cambiado (driver recargado,
     * índice distinto). Se refresca antes de reutilizar los arrays. */
    gs.fan_count = getFanCount(device, &gs.fan_count_fallback);
    for (unsigned int f = 0; f < GPU_TEMPS_MAX_FANS; f++) {
        gs.fan_speed[f] = -1;
        gs.fan_read_ok[f] = false;
        gs.fan_read_fail_streak[f] = 0;
        gs.fan_zero_streak[f] = 0;
        gs.fan_zero_alerted[f] = false;
        gs.fan_target[f] = -1;
        gs.fan_desired[f] = -1;
        gs.fan_applied[f] = -1;
        gs.fan_last_write_error[f] = NVML_SUCCESS;
        gs.fan_resync_needed[f] = true;
    }
    gs.all_fans_zero_alerted = false;

    gpu_temps_reset(gs.pci_bus_id.c_str());
    /* La lectura PCI siguiente decide si MMIO queda permitido. No se habilita
     * por adelantado tras recuperar solo NVML. */
    gs.mmio_allowed = false;
    gs.pci_status = GPU_TEMPS_INVALID;
    gs.core_available = true;
    gs.core_temp_c = static_cast<int>(temp);
    gs.core_last_ok = std::chrono::steady_clock::now();
    gs.core_fail_streak = 0;
    gs.vram_available = false;
    gs.hotspot_available = false;
    gs.pci_error_logged = false;
    gs.pci_fail_streak = 0;
    gs.sensor_fail_streak = 0;
    gs.effective_ok = false;
    gs.fan_policy_warned = false;
    gs.fan_policy_manual = false;
    gs.power_limit_applied_mw = 0;
    gs.power_limit_supported = true;
    gs.power_limit_warned = false;

    setManualFanPolicy(gs, gpu_index);
    if (gs.state == GpuRuntimeState::GPU_LOST ||
        gs.state == GpuRuntimeState::DISABLED) {
        /* La política manual volvió a fallar con GPU_IS_LOST: no se debe
         * declarar la GPU sana ni tocar sus ventiladores. */
        return false;
    }
    for (unsigned int f = 0; f < gs.fan_count; f++) {
        gs.fan_resync_needed[f] = true;
    }

    transitionGpu(gs, gpu_index, GpuRuntimeState::NORMAL,
                  "recuperación controlada completada");
    return true;
}

static void maybeRecoverGpu(GpuState& gs, unsigned int gpu_index) {
    if (!g_config.auto_recover_gpu) return;
    if (gs.state != GpuRuntimeState::GPU_LOST &&
        gs.state != GpuRuntimeState::DISABLED) {
        return;
    }

    if (gs.recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
        if (gs.state != GpuRuntimeState::DISABLED) {
            transitionGpu(gs, gpu_index, GpuRuntimeState::DISABLED,
                          "agotados los intentos de recuperación; no se "
                          "reintenta automáticamente");
        }
        return;
    }

    if (msSince(gs.last_recovery_probe) < RECOVERY_PROBE_INTERVAL_MS) return;
    gs.last_recovery_probe = std::chrono::steady_clock::now();
    gs.recovery_attempts++;

    std::cout << "[recover] GPU " << gpu_index << " [" << gs.pci_bus_id
              << "]: intento " << gs.recovery_attempts << "/"
              << MAX_RECOVERY_ATTEMPTS << " (solo sondeo NVML seguro)"
              << std::endl;

    if (probeGpu(gs, gpu_index)) {
        gs.recovery_attempts = 0;
        return;
    }

    if (gs.recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
        transitionGpu(gs, gpu_index, GpuRuntimeState::DISABLED,
                      "recuperación no disponible tras "
                      + std::to_string(MAX_RECOVERY_ATTEMPTS) + " intentos");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Status files (shared with vergpu/gpustatus without root)
// ─────────────────────────────────────────────────────────────────────────────
static bool writeTextFileAtomic(const char* path, const std::string& content) {
    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    std::ofstream out(tmp, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << content;
    out.flush();
    if (!out.good()) {
        out.close();
        unlink(tmp);
        return false;
    }
    out.close();

    if (std::rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    chmod(path, 0644);
    return true;
}

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

    mkdir(STATUS_DIR, 0755);
    gpu_temps_status_write(STATUS_FILE, ids.data(), vram.data(),
                           hotspot.data(), static_cast<int>(ids.size()));
    gpu_fans_status_write(FANS_FILE, fans.data(), static_cast<int>(fans.size()));

    std::ostringstream state;
    std::ostringstream fanstate;
    for (const auto& gs : g_gpu_states) {
        if (!gs.valid) continue;
        state << gs.pci_bus_id << " " << stateName(gs.state) << " "
              << (gs.core_available ? gs.core_temp_c : -1) << " "
              << (gs.vram_available ? gs.vram_temp_c : -1) << " "
              << (gs.hotspot_available ? gs.hotspot_temp_c : -1) << " "
              << (gs.critical_latched ? 1 : 0) << " "
              << (gs.power_limit_observed_mw / 1000) << "\n";

        fanstate << gs.pci_bus_id << " " << gs.fan_count;
        for (unsigned int f = 0; f < gs.fan_count; f++) {
            fanstate << " " << gs.fan_target[f]
                     << " " << gs.fan_desired[f]
                     << " " << gs.fan_applied[f]
                     << " " << (gs.fan_read_ok[f] ? gs.fan_speed[f] : -1);
        }
        fanstate << "\n";
    }

    writeTextFileAtomic(STATE_FILE, state.str());
    writeTextFileAtomic(FAN_STATE_FILE, fanstate.str());
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
        std::cerr << "[warn] Archivo de control manual inválido — se elimina."
                  << std::endl;
        unlink(MANUAL_FILE);
        return -1;
    }

    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
        std::cout << "[manual] El proceso de prueba " << pid
                  << " ya no existe — se elimina el archivo y se reanuda la "
                     "curva." << std::endl;
        unlink(MANUAL_FILE);
        return -1;
    }

    return static_cast<int>(speed);
}

static void writeSummary() {
    for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
        const GpuState& gs = g_gpu_states[i];
        if (!gs.valid) continue;

        std::cout << "[temp] GPU " << i << " [" << gs.pci_bus_id << "] "
                  << stateName(gs.state) << " core="
                  << tempOrNA(gs.core_available, gs.core_temp_c)
                  << " vram=" << tempOrNA(gs.vram_available, gs.vram_temp_c)
                  << " hotspot="
                  << tempOrNA(gs.hotspot_available, gs.hotspot_temp_c)
                  << " fans=";
        for (unsigned int f = 0; f < gs.fan_count; f++) {
            if (f) std::cout << ",";
            std::cout << "fan" << f << "=";
            if (gs.fan_read_ok[f]) std::cout << gs.fan_speed[f] << "%";
            else                   std::cout << "?";
            std::cout << "(target=";
            if (gs.fan_target[f] >= 0) std::cout << gs.fan_target[f];
            else                       std::cout << "?";
            std::cout << ",applied=";
            if (gs.fan_applied[f] >= 0) std::cout << gs.fan_applied[f];
            else                        std::cout << "?";
            std::cout << ")";
        }
        std::cout << " power=";
        if (gs.power_limit_observed_mw) {
            std::cout << (gs.power_limit_observed_mw / 1000) << "W";
        } else {
            std::cout << "?";
        }
        if (gs.critical_latched) std::cout << " [CRITICAL LOCK]";
        if (gs.effective_stale) std::cout << " [STALE]";
        std::cout << std::endl;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Startup helpers, main loop and shutdown
// ─────────────────────────────────────────────────────────────────────────────
static bool setPersistenceMode(GpuState& gs, unsigned int gpu_index) {
    nvmlEnableState_t desired = g_config.persistence_mode
                                    ? NVML_FEATURE_ENABLED
                                    : NVML_FEATURE_DISABLED;

    nvmlReturn_t r = nvmlDeviceSetPersistenceMode(gs.device, desired);
    if (r != NVML_SUCCESS) {
        if (nvmlIsLost(r)) {
            noteNvmlError(gs, gpu_index, r, "SetPersistenceMode");
        } else if (g_config.log_nvml_errors) {
            std::cerr << "[warn] GPU " << gpu_index << " [" << gs.pci_bus_id
                      << "]: no se pudo cambiar el modo persistence: "
                      << nvmlErrorString(r) << " (no es crítico)." << std::endl;
        }
        return false;
    }

    std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
              << "]: persistence " << (g_config.persistence_mode ? "enabled" : "disabled")
              << std::endl;
    return true;
}

static std::string gpuName(nvmlDevice_t device) {
    char name[96] = {0};
    if (nvmlDeviceGetName(device, name, static_cast<unsigned int>(sizeof(name)))
            != NVML_SUCCESS) {
        return "NVIDIA GPU";
    }
    return std::string(name);
}

static std::string gpuUuid(nvmlDevice_t device) {
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE] = {0};
    if (nvmlDeviceGetUUID(device, uuid,
                          static_cast<unsigned int>(sizeof(uuid))) != NVML_SUCCESS) {
        return std::string();
    }
    return std::string(uuid);
}

/* Reads the initial PCI temperatures and leaves the timestamps set so the
 * sampler does not consider them stale on the first cycle. */
static void readInitialPciTemps(GpuState& gs, unsigned int gpu_index) {
    gpu_temps_t initial;
    gpu_temps_status_t status = gpu_temps_read_ex(
        gs.pci_bus_id.c_str(), &initial);

    gs.pci_status = status;
    gs.pci_profile_known = initial.profile_known;
    gs.mmio_allowed = (status == GPU_TEMPS_OK);

    auto now = std::chrono::steady_clock::now();

    if (status == GPU_TEMPS_OK) {
        if (initial.vram_available) {
            gs.vram_available = true;
            gs.vram_ever_seen = true;
            gs.vram_temp_c = initial.vram_temp_c;
            gs.vram_last_ok = now;
        }
        if (initial.hotspot_available) {
            gs.hotspot_available = true;
            gs.hotspot_ever_seen = true;
            gs.hotspot_temp_c = initial.hotspot_temp_c;
            gs.hotspot_last_ok = now;
        }

        std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: PCI VRAM="
                  << tempOrNA(gs.vram_available, gs.vram_temp_c)
                  << "  Hotspot="
                  << tempOrNA(gs.hotspot_available, gs.hotspot_temp_c);
        if (initial.device_id) {
            std::cout << "  (device 0x" << std::hex << initial.device_id
                      << std::dec << ")";
        }
        std::cout << std::endl;
        return;
    }

    if (g_config.log_pcie_errors) {
        std::cerr << "[warn] GPU " << gpu_index << " [" << gs.pci_bus_id
                  << "]: lectura PCI VRAM/hotspot no disponible: "
                  << gpu_temps_status_string(status);
        if (initial.device_id) {
            std::cerr << " (device 0x" << std::hex << initial.device_id
                      << std::dec << ")";
        }
        std::cerr << ". El control seguirá usando el core si está disponible."
                  << std::endl;
    }
    gs.pci_error_logged = true;
}

static void initializeGpu(GpuState& gs, unsigned int gpu_index) {
    nvmlPciInfo_t pci;
    memset(&pci, 0, sizeof(pci));

    nvmlReturn_t r = nvmlDeviceGetPciInfo_v3(gs.device, &pci);
    if (r != NVML_SUCCESS) {
        std::cerr << "[warn] GPU " << gpu_index
                  << ": no se pudo obtener la información PCI: "
                  << nvmlErrorString(r) << "; se omite." << std::endl;
        gs.valid = false;
        return;
    }

    gs.pci_bus_id = formatPciBusId(pci);
    gs.uuid = gpuUuid(gs.device);
    gs.name = gpuName(gs.device);
    gs.valid = true;

    gs.fan_count = getFanCount(gs.device, &gs.fan_count_fallback);

    std::cout << "[info] GPU " << gpu_index << " [" << gs.pci_bus_id << "] "
              << gs.name << ": " << gs.fan_count << " fan sensor(s)";
    if (gs.fan_count_fallback) {
        std::cout << " (GetNumFans no disponible: se sondea solo el fan 0)";
    }
    if (!gs.uuid.empty()) std::cout << ", uuid=" << gs.uuid;
    std::cout << std::endl;

    readInitialPciTemps(gs, gpu_index);

    if (g_config.persistence_mode) {
        setPersistenceMode(gs, gpu_index);
        if (gs.state == GpuRuntimeState::GPU_LOST) return;
    }

    if (g_config.power_limit > 0) {
        initPowerLimit(gs, gpu_index);
        if (gs.state == GpuRuntimeState::GPU_LOST) return;
    }

    setManualFanPolicy(gs, gpu_index);
    if (gs.state == GpuRuntimeState::GPU_LOST) return;

    for (unsigned int f = 0; f < gs.fan_count; f++) {
        gs.fan_resync_needed[f] = true;
    }
}

static void installSignalHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    /* Sin SA_RESTART: dormir debe volver cuanto antes tras la señal. */
    action.sa_flags = 0;

    if (sigaction(SIGTERM, &action, nullptr) != 0 ||
        sigaction(SIGINT, &action, nullptr) != 0) {
        std::cerr << "[warn] No se pudieron instalar todos los manejadores de "
                     "señal: " << std::strerror(errno) << std::endl;
    }
    signal(SIGPIPE, SIG_IGN);
}

static const char* signalName(int sig) {
    if (sig == SIGTERM) return "SIGTERM";
    if (sig == SIGINT)  return "SIGINT";
    return "desconocida";
}

static void shutdownDaemon() {
    if (unlink(MANUAL_FILE) == 0) {
        std::cout << "[manual] Archivo de control manual eliminado." << std::endl;
    }

    for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
        GpuState& gs = g_gpu_states[i];
        if (!gs.valid) continue;
        if (gs.state == GpuRuntimeState::GPU_LOST ||
            gs.state == GpuRuntimeState::DISABLED) {
            continue;
        }
        if (!gs.auto_restored) {
            restoreAutoControl(gs, i, "parada del servicio");
        }
    }

    if (!gpu_temps_close_all() && g_config.log_pcie_errors) {
        std::cerr << "[warn] Algún munmap()/close() de MMIO falló durante la "
                     "parada." << std::endl;
    }

    unlink(STATUS_FILE);
    unlink(FANS_FILE);
    unlink(STATE_FILE);
    unlink(FAN_STATE_FILE);
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

    if (!diag.ok()) {
        std::cerr << "[error] La configuración contiene errores. El daemon "
                     "arranca con los valores por defecto seguros; corrige el "
                     "archivo y reinicia el servicio." << std::endl;
        g_config = Config();
    }

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
    }

    if (g_config.critical_temp < CRITICAL_TEMP_MIN ||
        g_config.critical_temp > CRITICAL_TEMP_MAX) {
        std::cerr << "[error] critical_temp must be between "
                  << CRITICAL_TEMP_MIN << " and " << CRITICAL_TEMP_MAX << "."
                  << std::endl;
        return 1;
    }

    installSignalHandlers();

    // NVML init with retries (systemd may start before the driver is ready).
    nvmlReturn_t result = NVML_SUCCESS;
    int retries = 0;
    const int max_retries = 60;

    while (!g_shutdown) {
        result = nvmlInit();
        if (result == NVML_SUCCESS) break;
        std::cerr << "[info] Waiting for NVIDIA driver... ("
                  << retries + 1 << "/" << max_retries << "): "
                  << nvmlErrorString(result) << std::endl;
        retries++;
        if (retries >= max_retries) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (g_shutdown) {
        std::cout << "[info] Parada solicitada durante el arranque ("
                  << signalName(static_cast<int>(g_shutdown_signal)) << ")."
                  << std::endl;
        return 0;
    }

    if (result != NVML_SUCCESS) {
        std::cerr << "[error] Failed to initialize NVML after " << max_retries
                  << " attempt(s): " << nvmlErrorString(result) << std::endl;
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
            gs.valid = false;
            continue;
        }

        initializeGpu(gs, i);
    }

    std::cout << std::endl;
    std::cout << "[info] Found " << g_device_count << " GPU(s)" << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    // First sample and first fan application. Any GPU that is not lost is
    // controlled with its own state; a failure on one does not stop the rest.
    int manual = readManualSpeed();
    if (manual >= 0) {
        std::cout << "[manual] Existing fan test detected: " << manual
                  << " % (PID alive) — applying it." << std::endl;
    }

    for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
        GpuState& gs = g_gpu_states[i];
        if (!gs.valid) continue;
        sampleGpu(gs, i);
        controlGpu(gs, i, manual, true);
    }

    unsigned int controllable = 0;
    for (const auto& gs : g_gpu_states) {
        if (gs.valid &&
            gs.state != GpuRuntimeState::GPU_LOST &&
            gs.state != GpuRuntimeState::DISABLED) {
            controllable++;
        }
    }

    writeStatusFile();
    writeSummary();

    std::cout << std::string(72, '-') << std::endl;
    std::cout << "[info] Fan curve controller started." << std::endl;
    std::cout << "[info] Max temperature target : " << g_config.max_temp << " °C" << std::endl;
    std::cout << "[info] Critical safety lock   : " << g_config.critical_temp
              << " °C (latching, release at "
              << (g_config.critical_temp - CRITICAL_HYSTERESIS) << " °C)" << std::endl;
    std::cout << "[info] Min fan speed (floor)  : " << g_config.min_speed << " %" << std::endl;
    std::cout << "[info] Max fan speed (normal) : " << g_config.max_speed
              << " % (critical lock ignores it)" << std::endl;
    std::cout << "[info] Curve                  : "
              << (g_config.max_temp - RAMP_DELTA) << " °C → "
              << g_config.max_temp << " °C, " << g_config.min_speed << " → " << g_config.max_speed << " %" << std::endl;
    std::cout << "[info] Use VRAM temp (PCI)    : "
              << (g_config.use_vram_temp ? "yes" : "no") << std::endl;
    std::cout << "[info] Use Hotspot temp (PCI) : "
              << (g_config.use_hotspot_temp ? "yes" : "no") << std::endl;
    std::cout << "[info] Curve sensors          : " << g_config.sensorList() << std::endl;
    std::cout << "[info] Critical lock sensors  : " << g_config.lockSensors() << std::endl;
    std::cout << "[info] Power limit mode       : "
              << powerModeName(g_config.power_limit_mode) << std::endl;
    std::cout << std::string(72, '-') << std::endl;

    if (controllable == 0) {
        std::cerr << "[error] Ninguna GPU es controlable en este momento." << std::endl;
        if (g_config.stop_on_gpu_lost) {
            std::cerr << "[error] stop_on_gpu_lost=1: el servicio se detiene. "
                         "Revisa el driver, la alimentación y el riser; no se "
                         "ejecuta ningún reset ni reboot automático." << std::endl;
            shutdownDaemon();
            nvmlShutdown();
            return 1;
        }
        std::cerr << "[warn] stop_on_gpu_lost=0: el servicio sigue a la espera "
                     "de que alguna GPU vuelva a responder." << std::endl;
    }

    g_control_started = true;

    // ── Main loop ─────────────────────────────────────────────────────────
    auto last_sample  = std::chrono::steady_clock::now();
    auto last_manual  = last_sample;
    auto last_status  = last_sample;
    auto last_summary = last_sample;
    auto last_policy  = last_sample;

    unsigned int tick_ms = std::min(g_config.sensor_interval_ms,
                                    g_config.fan_update_interval_ms);
    if (tick_ms < 200) tick_ms = 200;
    if (tick_ms > 1000) tick_ms = 1000;

    int manual_last = manual;

    while (!g_shutdown) {
        auto now = std::chrono::steady_clock::now();

        if (msSince(last_sample) >=
            static_cast<long long>(g_config.sensor_interval_ms)) {
            last_sample = now;
            for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
                sampleGpu(g_gpu_states[i], i);
            }
        }

        for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
            maybeRecoverGpu(g_gpu_states[i], i);
        }

        if (msSince(last_manual) >= 1000) {
            last_manual = now;
            int observed = readManualSpeed();
            if (observed != manual_last) {
                if (observed >= 0) {
                    std::cout << "[manual] Fan test active: " << observed << " % — "
                              << "run 'setfan auto' or press Ctrl+C in that "
                                 "terminal to stop." << std::endl;
                } else if (manual_last >= 0) {
                    std::cout << "[manual] Fan test ended — curve control resumed."
                              << std::endl;
                }
                manual_last = observed;
            }
        }

        bool manual_changed = false;
        if (manual_last != manual) {
            manual = manual_last;
            manual_changed = true;
        }

        for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
            controlGpu(g_gpu_states[i], i, manual, manual_changed);
        }

        for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
            checkPowerLimit(g_gpu_states[i], i);
        }

        if (msSince(last_policy) >= POLICY_REASSERT_S * 1000LL) {
            last_policy = now;
            for (unsigned int i = 0; i < g_gpu_states.size(); i++) {
                GpuState& gs = g_gpu_states[i];
                if (!gs.valid || !gs.fan_policy_manual) continue;
                if (gs.state == GpuRuntimeState::GPU_LOST ||
                    gs.state == GpuRuntimeState::DISABLED ||
                    gs.state == GpuRuntimeState::RECOVERING) {
                    continue;
                }
                setManualFanPolicy(gs, i);
            }
        }

        if (msSince(last_status) >= 1000) {
            last_status = now;
            writeStatusFile();
        }

        if (msSince(last_summary) >=
            static_cast<long long>(SUMMARY_INTERVAL_S) * 1000LL) {
            last_summary = now;
            writeSummary();
        }

        if (!g_shutdown) {
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
        }
    }

    const char* stop_signal = signalName(static_cast<int>(g_shutdown_signal));
    std::cout << "[info] Parada solicitada (" << stop_signal
              << "). Motivo: " << g_stop_reason << std::endl;

    shutdownDaemon();

    nvmlShutdown();
    std::cout << "[info] Servicio detenido." << std::endl;
    return 0;
}
