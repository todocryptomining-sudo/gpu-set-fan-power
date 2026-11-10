#include <iostream>
#include "nvml_min.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <ctime>
#include "gpu_temps.h"

// ─────────────────────────────────────────────────────────────────────────────
// VRAM / Hotspot temperature via direct PCI register read (same as gddr6.c / nvml_direct_access.c)
// NVML does not expose GDDR6X memory temperature on RTX 3090 with driver 615+.
// We read BAR0 + 0xE2A8 (VRAM) and BAR0 + 0x2046c (Hotspot) directly from /dev/mem.
// ─────────────────────────────────────────────────────────────────────────────
namespace TempSensor {
    constexpr int GPU  = 0;  // NVML_TEMPERATURE_GPU
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
#define REFRESH_MS       1000
#define MAX_GPUS         32
#define BAR_WIDTH        20
#define STATUS_FILE      "/run/set-fan-curve/temps"
#define FANS_FILE        "/run/set-fan-curve/fans"

volatile bool g_running = true;
bool g_show_processes = true;
bool g_show_bar = true;
bool g_color = true;
int g_refresh_ms = REFRESH_MS;

// ─────────────────────────────────────────────────────────────────────────────
// Colors
// ─────────────────────────────────────────────────────────────────────────────
namespace Color {
    const char* RESET   = "\033[0m";
    const char* BOLD    = "\033[1m";
    const char* DIM     = "\033[2m";
    const char* RED     = "\033[31m";
    const char* GREEN   = "\033[32m";
    const char* YELLOW  = "\033[33m";
    const char* BLUE    = "\033[34m";
    const char* MAGENTA = "\033[35m";
    const char* CYAN    = "\033[36m";
    const char* WHITE   = "\033[37m";
    const char* GRAY    = "\033[90m";
    const char* BG_RED  = "\033[41m";
    const char* BG_GREEN = "\033[42m";

    static void disable() {
        RESET = BOLD = DIM = RED = GREEN = YELLOW = BLUE = "";
        MAGENTA = CYAN = WHITE = GRAY = BG_RED = BG_GREEN = "";
    }

    static const char* tempColor(int temp) {
        if (temp >= 85) return RED;
        if (temp >= 75) return YELLOW;
        if (temp >= 60) return GREEN;
        return CYAN;
    }
    static const char* usageColor(unsigned int u) {
        if (u >= 90) return RED;
        if (u >= 70) return YELLOW;
        return GREEN;
    }
    static const char* powerColor(unsigned int watts, unsigned int max_watts) {
        if (max_watts == 0) return WHITE;
        float ratio = float(watts) / max_watts;
        if (ratio >= 0.9) return RED;
        if (ratio >= 0.7) return YELLOW;
        return GREEN;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Terminal utils
// ─────────────────────────────────────────────────────────────────────────────
struct TermSize { int rows, cols; };

TermSize getTermSize() {
    TermSize ts = {24, 80};
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        ts.rows = w.ws_row;
        ts.cols = w.ws_col;
    }
    return ts;
}

void clearScreen() { std::cout << "\033[2J\033[H"; }
void hideCursor()  { std::cout << "\033[?25l"; }
void showCursor()  { std::cout << "\033[?25h"; }

std::string bar(float pct, int width = BAR_WIDTH) {
    if (!g_show_bar) return "";
    int filled = int(pct * width / 100.0f);
    std::string b = "[";
    for (int i = 0; i < width; i++) {
        b += (i < filled) ? "█" : "░";
    }
    b += "]";
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU Info
// ─────────────────────────────────────────────────────────────────────────────
struct GPUInfo {
    int index = -1;
    std::string name;
    std::string uuid;
    int temp_core = 0;
    int temp_vram = 0;
    int temp_hotspot = 0;
    unsigned int gpu_util = 0;
    unsigned int mem_util = 0;
    bool vram_sensor = false;
    bool hotspot_sensor = false;
    bool vram_via_daemon = false;     // true if value came from set-fan-curve
    bool hotspot_via_daemon = false;
    unsigned long long mem_total = 0;
    unsigned long long mem_used = 0;
    unsigned int power_draw = 0;
    unsigned int power_limit = 0;
    unsigned int clock_gpu = 0;
    unsigned int clock_mem = 0;
    unsigned int fan_count = 0;
    unsigned int fan_speeds[GPU_TEMPS_MAX_FANS] = {0};
    bool         fan_ok[GPU_TEMPS_MAX_FANS] = {false};
    bool         fan_via_daemon = false;
    int persistence = 0;
    std::string pci_bus;

    float mem_pct() const { return mem_total ? float(mem_used) * 100 / mem_total : 0; }
    float power_pct() const { return power_limit ? float(power_draw) * 100 / power_limit : 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Refresh GPU
// ─────────────────────────────────────────────────────────────────────────────
bool refreshGPU(GPUInfo& gpu) {
    nvmlDevice_t dev;
    if (nvmlDeviceGetHandleByIndex(gpu.index, &dev) != NVML_SUCCESS) return false;
    nvmlReturn_t r;
    auto err = [](nvmlReturn_t r, const char*) { if (r != NVML_SUCCESS) /* silent */; };

    char name_buf[NVML_DEVICE_NAME_BUFFER_SIZE] = {0};
    r = nvmlDeviceGetName(dev, name_buf, sizeof(name_buf));
    if (r == NVML_SUCCESS) gpu.name = name_buf;

    char uuid_buf[NVML_DEVICE_UUID_BUFFER_SIZE] = {0};
    r = nvmlDeviceGetUUID(dev, uuid_buf, sizeof(uuid_buf));
    if (r == NVML_SUCCESS) gpu.uuid = uuid_buf;

    nvmlPciInfo_t pci;
    memset(&pci, 0, sizeof(pci));
    r = nvmlDeviceGetPciInfo_v3(dev, &pci);
    if (r == NVML_SUCCESS) {
        char bus[32] = {0};
        // NVIDIA GPUs use function 0. Never use pci.busId here: some
        // drivers return an 8-digit domain that sysfs does not accept.
        snprintf(bus, sizeof(bus), "%04x:%02x:%02x.0",
                 pci.domain & 0xFFFFu, pci.bus & 0xFFu, pci.device & 0xFFu);
        gpu.pci_bus = bus;
    }

    // Temperatures
    unsigned int ct;
    r = nvmlDeviceGetTemperature(dev,
                                  static_cast<nvmlTemperatureSensors_t>(TempSensor::GPU), &ct);
    err(r, "core");
    gpu.temp_core = (r == NVML_SUCCESS) ? int(ct) : 0;

    // Read VRAM and Hotspot temps directly from PCI registers (same as gddr6.c / nvml_direct_access.c).
    // Direct /dev/mem access only works as root; when running as a normal user,
    // fall back to the status file published by the set-fan-curve daemon.
    gpu_temps_t pci_temps;
    bool pci_ok = gpu_temps_read(gpu.pci_bus.c_str(), &pci_temps);

    gpu.vram_sensor    = pci_ok && pci_temps.vram_available;
    gpu.temp_vram      = gpu.vram_sensor ? pci_temps.vram_temp_c : 0;
    gpu.hotspot_sensor = pci_ok && pci_temps.hotspot_available;
    gpu.temp_hotspot   = gpu.hotspot_sensor ? pci_temps.hotspot_temp_c : 0;
    gpu.vram_via_daemon    = false;
    gpu.hotspot_via_daemon = false;

    if (!gpu.vram_sensor || !gpu.hotspot_sensor) {
        int sv = -1, sh = -1;
        if (gpu_temps_status_read(STATUS_FILE, gpu.pci_bus.c_str(), &sv, &sh)) {
            if (!gpu.vram_sensor && sv > 0) {
                gpu.vram_sensor = true;
                gpu.temp_vram = sv;
                gpu.vram_via_daemon = true;
            }
            if (!gpu.hotspot_sensor && sh > 0) {
                gpu.hotspot_sensor = true;
                gpu.temp_hotspot = sh;
                gpu.hotspot_via_daemon = true;
            }
        }
    }

    // Utilization
    nvmlUtilization_t util;
    r = nvmlDeviceGetUtilizationRates(dev, &util);
    if (r == NVML_SUCCESS) {
        gpu.gpu_util = util.gpu;
        gpu.mem_util = util.memory;
    }

    // Memory
    nvmlMemory_t mem;
    r = nvmlDeviceGetMemoryInfo(dev, &mem);
    if (r == NVML_SUCCESS) {
        gpu.mem_total = mem.total;
        gpu.mem_used  = mem.used;
    }

    // Power
    r = nvmlDeviceGetPowerUsage(dev, &gpu.power_draw);
    err(r, "power_usage");
    r = nvmlDeviceGetPowerManagementLimit(dev, &gpu.power_limit);
    err(r, "power_limit");
    if (r != NVML_SUCCESS) gpu.power_limit = 0;

    // Clocks
    r = nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &gpu.clock_gpu);
    err(r, "clock_gpu");
    r = nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &gpu.clock_mem);
    err(r, "clock_mem");

    // Fans: show every sensor exposed by NVML, with the daemon status file
    // as a fallback for cards where NVML only reports fan 0.
    gpu.fan_count = 0;
    gpu.fan_via_daemon = false;
    for (unsigned int f = 0; f < GPU_TEMPS_MAX_FANS; f++) {
        gpu.fan_ok[f] = false;
        gpu.fan_speeds[f] = 0;
    }

    r = nvmlDeviceGetNumFans(dev, &gpu.fan_count);
    if (r != NVML_SUCCESS || gpu.fan_count > GPU_TEMPS_MAX_FANS) {
        gpu.fan_count = 0;
    }

    for (unsigned int f = 0; f < gpu.fan_count; f++) {
        unsigned int speed = 0;
        r = nvmlDeviceGetFanSpeed_v2(dev, f, &speed);
        if (r == NVML_SUCCESS) {
            gpu.fan_speeds[f] = speed;
            gpu.fan_ok[f] = true;
        }
    }

    gpu_fans_t daemon_fans;
    if (gpu_fans_status_read(FANS_FILE, gpu.pci_bus.c_str(), &daemon_fans)) {
        unsigned int daemon_count = static_cast<unsigned int>(daemon_fans.count);
        if (daemon_count > gpu.fan_count) {
            gpu.fan_count = daemon_count;
            gpu.fan_via_daemon = true;
        }
        for (unsigned int f = 0; f < gpu.fan_count; f++) {
            if (f < daemon_count && !gpu.fan_ok[f] && daemon_fans.speed[f] >= 0) {
                gpu.fan_speeds[f] =
                    static_cast<unsigned int>(daemon_fans.speed[f]);
                gpu.fan_ok[f] = true;
                gpu.fan_via_daemon = true;
            }
        }
    }

    // Persistence
    nvmlEnableState_t pers;
    r = nvmlDeviceGetPersistenceMode(dev, &pers);
    if (r == NVML_SUCCESS) gpu.persistence = (pers == NVML_FEATURE_ENABLED);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Print GPU
// ─────────────────────────────────────────────────────────────────────────────
void printGPU(const GPUInfo& gpu, const TermSize&) {
    // Header
    std::cout << Color::BOLD << Color::CYAN << "GPU " << gpu.index << ": " << gpu.name
              << Color::RESET << "  [" << gpu.pci_bus << "]" << std::endl;

    // Temps line
    std::cout << "  " << Color::BOLD << "Core:" << Color::RESET << " "
              << Color::tempColor(gpu.temp_core) << std::setw(3) << gpu.temp_core << " °C" << Color::RESET;
    if (gpu.vram_sensor) {
        std::cout << "  " << Color::BOLD << "VRAM:" << Color::RESET << " "
                  << Color::tempColor(gpu.temp_vram) << std::setw(3) << gpu.temp_vram << " °C" << Color::RESET
                  << Color::DIM
                  << (gpu.vram_via_daemon ? " (via daemon)" : " (PCI direct)")
                  << Color::RESET;
    } else {
        std::cout << "  " << Color::BOLD << "VRAM:" << Color::RESET << " "
                  << Color::GRAY << "N/A" << Color::RESET
                  << Color::DIM << " (no PCI sensor / daemon)" << Color::RESET;
    }
    if (gpu.hotspot_sensor) {
        std::cout << "  " << Color::BOLD << "Hotspot:" << Color::RESET << " "
                  << Color::tempColor(gpu.temp_hotspot) << std::setw(3) << gpu.temp_hotspot << " °C" << Color::RESET
                  << Color::DIM
                  << (gpu.hotspot_via_daemon ? " (via daemon)" : " (PCI direct)")
                  << Color::RESET;
    }
    std::cout << "  " << Color::BOLD
              << (gpu.fan_count > 1 ? "Fans:" : "Fan: ")
              << Color::RESET << " ";

    if (gpu.fan_count == 0) {
        std::cout << Color::GRAY << "N/A" << Color::RESET;
    } else if (gpu.fan_count == 1) {
        if (gpu.fan_ok[0]) {
            std::cout << std::setw(3) << gpu.fan_speeds[0] << " %";
        } else {
            std::cout << Color::GRAY << "  ?" << Color::RESET;
        }
    } else {
        bool zero = false, running = false;
        for (unsigned int f = 0; f < gpu.fan_count; f++) {
            if (f) std::cout << "  ";
            std::cout << "Fan" << f << " ";
            if (gpu.fan_ok[f]) {
                std::cout << std::setw(3) << gpu.fan_speeds[f] << " %";
                if (gpu.fan_speeds[f] == 0) zero = true;
                else running = true;
            } else {
                std::cout << Color::GRAY << "  ?" << Color::RESET;
            }
        }
        if (zero && running) {
            std::cout << "  " << Color::RED << "(!) fan at 0 %" << Color::RESET;
        }
    }

    if (gpu.fan_via_daemon) {
        std::cout << Color::DIM << " (daemon)" << Color::RESET;
    }
    std::cout << std::endl;

    // GPU usage bar
    std::cout << "  " << Color::BOLD << "GPU  " << Color::RESET << " "
              << Color::usageColor(gpu.gpu_util) << std::setw(3) << gpu.gpu_util << " %" << Color::RESET
              << " " << bar(gpu.gpu_util) << std::endl;

    // VRAM usage bar
    std::cout << "  " << Color::BOLD << "VRAM " << Color::RESET << " "
              << Color::usageColor(gpu.mem_util) << std::setw(3) << gpu.mem_util << " %" << Color::RESET
              << " " << bar(gpu.mem_util)
              << "  " << std::fixed << std::setprecision(1)
              << (gpu.mem_used / 1024.0 / 1024.0 / 1024.0)
              << " / " << (gpu.mem_total / 1024.0 / 1024.0 / 1024.0)
              << " GB" << std::endl;

    // Power
    std::cout << "  " << Color::BOLD << "PWR  " << Color::RESET << " "
              << Color::powerColor(gpu.power_draw, gpu.power_limit)
              << std::fixed << std::setprecision(1) << (gpu.power_draw / 1000.0) << " W" << Color::RESET;
    if (gpu.power_limit > 0) {
        std::cout << " / " << (gpu.power_limit / 1000.0) << " W"
                  << " (" << std::setprecision(0) << gpu.power_pct() << "%)";
    }
    std::cout << std::endl;

    // Clocks
    std::cout << "  " << Color::BOLD << "Clk  " << Color::RESET
              << "GPU: " << gpu.clock_gpu << " MHz"
              << "  Mem: " << gpu.clock_mem << " MHz" << std::endl;

    // Persistence
    std::cout << "  " << Color::BOLD << "Mode:" << Color::RESET
              << " Persistence: "
              << (gpu.persistence ? std::string(Color::GREEN) + "ON" + Color::RESET : std::string(Color::RED) + "OFF" + Color::RESET)
              << std::endl;

    std::cout << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Header
// ─────────────────────────────────────────────────────────────────────────────
void printHeader(unsigned int count) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&tt));

    std::cout << Color::BOLD << Color::BLUE
              << "╔══════════════════════════════════════════════════════════════════════╗" << Color::RESET << std::endl;
    std::cout << Color::BOLD << Color::BLUE
              << "║" << Color::RESET << "  " << Color::BOLD << Color::MAGENTA << "vergpu" << Color::RESET
              << " - GPU Monitor  |  " << time_str
              << "  |  GPUs: " << count
              << "  |  Refresh: " << g_refresh_ms << "ms"
              << std::string(std::max(0, 70 - (int)strlen(time_str) - 36), ' ')
              << Color::YELLOW << " q=quit  p=procs  b=bars  c=color  +/-=refresh"
              << Color::RESET << std::endl;
    std::cout << Color::BOLD << Color::BLUE
              << "╚══════════════════════════════════════════════════════════════════════╝" << Color::RESET << std::endl;
    std::cout << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal + input
// ─────────────────────────────────────────────────────────────────────────────
void sigHandler(int) { g_running = false; }

bool checkInput() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            switch (c) {
                case 'q': case 'Q': g_running = false; break;
                case 'p': case 'P': g_show_processes = !g_show_processes; break;
                case 'b': case 'B': g_show_bar = !g_show_bar; break;
                case 'c': case 'C': g_color = !g_color; if (!g_color) Color::disable(); break;
                case '+': case '=': g_refresh_ms = std::min(5000, g_refresh_ms + 500); break;
                case '-': case '_': g_refresh_ms = std::max(200, g_refresh_ms - 500); break;
            }
        }
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Usage
// ─────────────────────────────────────────────────────────────────────────────
void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -n, --no-processes   Hide process list\n"
              << "  -b, --no-bar         Hide usage bars\n"
              << "  -c, --no-color       Disable colors\n"
              << "  -i, --interval MS    Refresh interval (default: 1000ms)\n"
              << "  -1, --once           Print once and exit\n"
              << "  -h, --help           Show this help\n"
              << "\nInteractive keys:\n"
              << "  q=quit  p=processes  b=bars  c=color  +/-=refresh\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    bool once = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { printUsage(argv[0]); return 0; }
        else if (arg == "-n" || arg == "--no-processes") g_show_processes = false;
        else if (arg == "-b" || arg == "--no-bar") g_show_bar = false;
        else if (arg == "-c" || arg == "--no-color") { g_color = false; Color::disable(); }
        else if (arg == "-i" || arg == "--interval") {
            if (i + 1 < argc) g_refresh_ms = std::atoi(argv[++i]);
        }
        else if (arg == "-1" || arg == "--once") once = true;
        else { std::cerr << "Unknown option: " << arg << std::endl; return 1; }
    }

    nvmlReturn_t r = nvmlInit();
    if (r != NVML_SUCCESS) {
        std::cerr << "[error] NVML init failed: " << nvmlErrorString(r) << std::endl;
        return 1;
    }

    unsigned int count = 0;
    r = nvmlDeviceGetCount(&count);
    if (r != NVML_SUCCESS || count == 0) {
        std::cerr << "[error] No GPUs found" << std::endl;
        nvmlShutdown();
        return 1;
    }
    count = std::min(count, (unsigned int)MAX_GPUS);

    std::vector<GPUInfo> gpus(count);
    for (unsigned int i = 0; i < count; i++) gpus[i].index = i;

    // Verify PCI temp reading works for each GPU (or find the daemon fallback)
    for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t dev;
        if (nvmlDeviceGetHandleByIndex(i, &dev) != NVML_SUCCESS) continue;

        nvmlPciInfo_t pci;
        memset(&pci, 0, sizeof(pci));
        char bus[32] = {0};
        if (nvmlDeviceGetPciInfo_v3(dev, &pci) == NVML_SUCCESS) {
            snprintf(bus, sizeof(bus), "%04x:%02x:%02x.0",
                     pci.domain & 0xFFFFu, pci.bus & 0xFFu, pci.device & 0xFFu);
        }

        gpu_temps_t test_temps;
        if (gpu_temps_read(bus, &test_temps) &&
            (test_temps.vram_available || test_temps.hotspot_available)) {
            std::cout << "[info] GPU " << i << ": PCI VRAM="
                      << (test_temps.vram_available ? std::to_string(test_temps.vram_temp_c) : "N/A")
                      << "°C  Hotspot="
                      << (test_temps.hotspot_available ? std::to_string(test_temps.hotspot_temp_c) : "N/A")
                      << "°C (direct PCI register read)" << std::endl;
        } else {
            int sv = -1, sh = -1;
            if (gpu_temps_status_read(STATUS_FILE, bus, &sv, &sh) && (sv > 0 || sh > 0)) {
                std::cout << "[info] GPU " << i << ": temps via set-fan-curve daemon"
                          << " (VRAM=" << (sv > 0 ? std::to_string(sv) : "N/A")
                          << "°C, Hotspot=" << (sh > 0 ? std::to_string(sh) : "N/A")
                          << "°C)" << std::endl;
            } else {
                std::cout << "[warn] GPU " << i << ": PCI temps unavailable."
                          << " Run as root for direct /dev/mem reads, or make sure"
                          << " set-fan-curve service is running." << std::endl;
            }
        }
    }

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);
    if (!once) hideCursor();

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    do {
        if (!once) clearScreen();
        TermSize ts = getTermSize();
        printHeader(count);

        for (unsigned int i = 0; i < count; i++) {
            if (refreshGPU(gpus[i])) {
                printGPU(gpus[i], ts);
            }
        }

        if (once) break;

        std::cout << Color::DIM << "Controls: q=quit  p=processes  b=bars  c=color  +/-=refresh" << Color::RESET << std::endl;

        auto start = std::chrono::steady_clock::now();
        while (g_running) {
            checkInput();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= g_refresh_ms) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (g_running);

    showCursor();
    if (!once) clearScreen();
    nvmlShutdown();
    return 0;
}