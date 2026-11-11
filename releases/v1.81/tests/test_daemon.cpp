// Tests del daemon v1.81.
//
// El daemon se incluye en esta unidad de traducción para poder probar sus
// funciones internas (curva, umbral, sensores, estados y potencia) sin tocar
// hardware. main() se renombra a daemon_main; las funciones NVML y de MMIO se
// sustituyen por mocks.
//
// Compilar y ejecutar:  make test

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../nvml_min.h"
#include "../gpu_temps.h"

#define main daemon_main
#include "../set_fan_curve.cpp"
#undef main

// ─────────────────────────────────────────────────────────────────────────────
// Mocks de gpu_temps (MMIO)
// ─────────────────────────────────────────────────────────────────────────────
static gpu_temps_status_t g_mock_temps_status = GPU_TEMPS_OK;
static bool g_mock_vram_available = true;
static int  g_mock_vram_temp = 70;
static bool g_mock_hotspot_available = true;
static int  g_mock_hotspot_temp = 80;
static int  g_mock_close_calls = 0;
static int  g_mock_close_all_calls = 0;
static int  g_mock_reset_calls = 0;

extern "C" {

gpu_temps_status_t gpu_temps_read_ex(const char* pci_bus_id, gpu_temps_t* out) {
    (void)pci_bus_id;
    if (!out) return GPU_TEMPS_INVALID;

    memset(out, 0, sizeof(*out));
    out->vram_temp_c = -1;
    out->hotspot_temp_c = -1;
    out->status = g_mock_temps_status;

    if (g_mock_temps_status != GPU_TEMPS_OK) return g_mock_temps_status;

    out->vendor_id = 0x10DE;
    out->device_id = 0x2204;
    out->vram_offset = 0x0000E2A8u;
    out->hotspot_offset = 0x0002046Cu;
    out->profile_known = true;
    out->vram_available = g_mock_vram_available;
    out->hotspot_available = g_mock_hotspot_available;
    out->vram_temp_c = g_mock_vram_available ? g_mock_vram_temp : -1;
    out->hotspot_temp_c = g_mock_hotspot_available ? g_mock_hotspot_temp : -1;
    return GPU_TEMPS_OK;
}

bool gpu_temps_read(const char* pci_bus_id, gpu_temps_t* out) {
    gpu_temps_status_t s = gpu_temps_read_ex(pci_bus_id, out);
    return s == GPU_TEMPS_OK && out &&
           (out->vram_available || out->hotspot_available);
}

const char* gpu_temps_status_string(gpu_temps_status_t status) {
    return status == GPU_TEMPS_OK ? "ok" : "mock-status";
}

bool gpu_temps_close(const char* pci_bus_id) {
    (void)pci_bus_id;
    g_mock_close_calls++;
    return true;
}

bool gpu_temps_close_all(void) {
    g_mock_close_all_calls++;
    return true;
}

void gpu_temps_reset(const char* pci_bus_id) {
    (void)pci_bus_id;
    g_mock_reset_calls++;
}

bool gpu_temps_status_write(const char* path, const char* const* bus_ids,
                            const int* vram, const int* hotspot, int count) {
    (void)path; (void)bus_ids; (void)vram; (void)hotspot; (void)count;
    return false;
}

bool gpu_temps_status_read(const char* path, const char* pci_bus_id,
                           int* vram, int* hotspot) {
    (void)path; (void)pci_bus_id; (void)vram; (void)hotspot;
    return false;
}

bool gpu_fans_status_write(const char* path, const gpu_fans_entry_t* entries,
                           int count) {
    (void)path; (void)entries; (void)count;
    return false;
}

bool gpu_fans_status_read(const char* path, const char* pci_bus_id,
                          gpu_fans_t* out) {
    (void)path; (void)pci_bus_id; (void)out;
    return false;
}

} // extern "C"

// ─────────────────────────────────────────────────────────────────────────────
// Mocks de NVML
// ─────────────────────────────────────────────────────────────────────────────
static nvmlReturn_t g_mock_temp_result = NVML_SUCCESS;
static unsigned int g_mock_temp_value = 60;
static nvmlReturn_t g_mock_fan_set_result = NVML_SUCCESS;
static nvmlReturn_t g_mock_fan_get_result = NVML_SUCCESS;
static unsigned int g_mock_fan_get_value = 45;
static nvmlReturn_t g_mock_policy_result = NVML_SUCCESS;
static nvmlReturn_t g_mock_power_get_result = NVML_SUCCESS;
static nvmlReturn_t g_mock_power_set_result = NVML_SUCCESS;
static unsigned int g_mock_power_current = 350000;
static unsigned int g_mock_power_min = 100000;
static unsigned int g_mock_power_max = 400000;
static int g_mock_fan_set_calls = 0;
static int g_mock_fan_set_last = -1;
static int g_mock_power_set_calls = 0;
static unsigned int g_mock_power_set_last = 0;
static int g_mock_default_fan_calls = 0;

static nvmlDevice_t mockHandle() {
    return reinterpret_cast<nvmlDevice_t>(static_cast<uintptr_t>(1));
}

extern "C" {

const char* nvmlErrorString(nvmlReturn_t result) {
    switch (result) {
        case NVML_SUCCESS: return "Success";
        case NVML_ERROR_NOT_SUPPORTED: return "Not Supported";
        case NVML_ERROR_GPU_IS_LOST: return "GPU is lost";
        case NVML_ERROR_RESET_REQUIRED: return "Reset required";
        default: return "Mock NVML error";
    }
}

nvmlReturn_t nvmlInit(void) { return NVML_SUCCESS; }
nvmlReturn_t nvmlShutdown(void) { return NVML_SUCCESS; }
nvmlReturn_t nvmlDeviceGetCount(unsigned int* count) {
    if (count) *count = 1;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int, nvmlDevice_t* device) {
    if (device) *device = mockHandle();
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetHandleByUUID(const char*, nvmlDevice_t* device) {
    if (device) *device = mockHandle();
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t, char* name, unsigned int length) {
    if (name && length) snprintf(name, length, "Mock GPU");
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t, char* uuid, unsigned int length) {
    if (uuid && length) snprintf(uuid, length, "GPU-MOCK-0001");
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t, nvmlPciInfo_t* pci) {
    if (!pci) return NVML_ERROR_INVALID_ARGUMENT;
    memset(pci, 0, sizeof(*pci));
    pci->domain = 0;
    pci->bus = 1;
    pci->device = 0;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t, nvmlTemperatureSensors_t,
                                      unsigned int* temp) {
    if (g_mock_temp_result != NVML_SUCCESS) return g_mock_temp_result;
    if (temp) *temp = g_mock_temp_value;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t, unsigned int* count) {
    if (count) *count = 1;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetFanSpeed_v2(nvmlDevice_t, unsigned int,
                                      unsigned int* speed) {
    if (g_mock_fan_get_result != NVML_SUCCESS) return g_mock_fan_get_result;
    if (speed) *speed = g_mock_fan_get_value;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceSetFanSpeed_v2(nvmlDevice_t, unsigned int,
                                      unsigned int speed) {
    if (g_mock_fan_set_result != NVML_SUCCESS) return g_mock_fan_set_result;
    g_mock_fan_set_calls++;
    g_mock_fan_set_last = static_cast<int>(speed);
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceSetDefaultFanSpeed_v2(nvmlDevice_t, unsigned int) {
    g_mock_default_fan_calls++;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceSetFanControlPolicy(nvmlDevice_t, unsigned int,
                                           nvmlFanControlPolicy_t) {
    return g_mock_policy_result;
}
nvmlReturn_t nvmlDeviceSetPersistenceMode(nvmlDevice_t, nvmlEnableState_t) {
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t, unsigned int* limit) {
    if (g_mock_power_get_result != NVML_SUCCESS) return g_mock_power_get_result;
    if (limit) *limit = g_mock_power_current;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t,
                                                           unsigned int* min_limit,
                                                           unsigned int* max_limit) {
    if (g_mock_power_get_result != NVML_SUCCESS) return g_mock_power_get_result;
    if (min_limit) *min_limit = g_mock_power_min;
    if (max_limit) *max_limit = g_mock_power_max;
    return NVML_SUCCESS;
}
nvmlReturn_t nvmlDeviceSetPowerManagementLimit(nvmlDevice_t, unsigned int limit) {
    if (g_mock_power_set_result != NVML_SUCCESS) return g_mock_power_set_result;
    g_mock_power_set_calls++;
    g_mock_power_set_last = limit;
    return NVML_SUCCESS;
}

} // extern "C"

// ─────────────────────────────────────────────────────────────────────────────
// Framework mínimo
// ─────────────────────────────────────────────────────────────────────────────
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL línea " << __LINE__ << ": " #cond << std::endl; \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void resetHarness() {
    g_gpu_states.clear();
    g_config = Config();
    g_config.log_nvml_errors = false;
    g_config.log_pcie_errors = false;
    g_shutdown = 0;
    g_shutdown_signal = 0;
    g_control_started = false;
    g_stop_reason = "test";
    g_device_count = 0;

    g_mock_temps_status = GPU_TEMPS_OK;
    g_mock_vram_available = true;
    g_mock_vram_temp = 70;
    g_mock_hotspot_available = true;
    g_mock_hotspot_temp = 80;
    g_mock_close_calls = 0;
    g_mock_close_all_calls = 0;
    g_mock_reset_calls = 0;

    g_mock_temp_result = NVML_SUCCESS;
    g_mock_temp_value = 60;
    g_mock_fan_set_result = NVML_SUCCESS;
    g_mock_fan_get_result = NVML_SUCCESS;
    g_mock_fan_get_value = 45;
    g_mock_policy_result = NVML_SUCCESS;
    g_mock_power_get_result = NVML_SUCCESS;
    g_mock_power_set_result = NVML_SUCCESS;
    g_mock_power_current = 350000;
    g_mock_power_min = 100000;
    g_mock_power_max = 400000;
    g_mock_fan_set_calls = 0;
    g_mock_fan_set_last = -1;
    g_mock_power_set_calls = 0;
    g_mock_power_set_last = 0;
    g_mock_default_fan_calls = 0;
}

static GpuState& makeGpu(unsigned int fan_count = 1) {
    g_gpu_states.resize(1);
    GpuState& gs = g_gpu_states[0];
    gs.device = mockHandle();
    gs.pci_bus_id = "0000:01:00.0";
    gs.uuid = "GPU-MOCK-0001";
    gs.valid = true;
    gs.state = GpuRuntimeState::NORMAL;
    gs.fan_count = fan_count;
    return gs;
}

static bool recent(const std::chrono::steady_clock::time_point& t) {
    return msSince(t) < 1000;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pruebas
// ─────────────────────────────────────────────────────────────────────────────
static void testFanCurve() {
    resetHarness();

    CHECK(getFanSpeed(20, 85, 40, 100) == 40);
    CHECK(getFanSpeed(44, 85, 40, 100) == 40);
    CHECK(getFanSpeed(45, 85, 40, 100) == 40);
    CHECK(getFanSpeed(65, 85, 40, 100) == 70);
    CHECK(getFanSpeed(65, 85, 0, 100) == 50);
    CHECK(getFanSpeed(85, 85, 40, 100) == 100);
    CHECK(getFanSpeed(90, 85, 40, 100) == 100);
    CHECK(getFanSpeed(65, 85, 10, 60) == 35);

    // max_speed < min_speed se eleva al suelo, no se devuelve menos que él.
    CHECK(getFanSpeed(85, 60, 40, 20) == 40);
}

static void testFanThresholdAndHysteresis() {
    resetHarness();
    GpuState& gs = makeGpu();
    g_config.fan_update_interval_ms = 2000;
    g_config.fan_change_threshold = 2;

    auto now = std::chrono::steady_clock::now();
    auto past = now - std::chrono::milliseconds(2100);

    gs.fan_resync_needed[0] = false;
    gs.fan_applied[0] = 40;
    gs.fan_last_write_time[0] = now;

    /* Dentro del intervalo no se escribe por un cambio normal: el intervalo
     * existe para reducir escrituras. */
    CHECK(!fanNeedsUpdate(gs, 0, 42, false));
    /* Un aumento hacia la zona crítica sí es inmediato. */
    CHECK(fanNeedsUpdate(gs, 0, 85, false));
    CHECK(fanNeedsUpdate(gs, 0, 100, false));
    /* force siempre escribe. */
    CHECK(fanNeedsUpdate(gs, 0, 40, true));

    /* Pasado el intervalo manda el umbral. */
    gs.fan_last_write_time[0] = past;
    CHECK(!fanNeedsUpdate(gs, 0, 41, false));  // 1 < umbral 2
    CHECK(fanNeedsUpdate(gs, 0, 42, false));   // 2 >= umbral
    CHECK(!fanNeedsUpdate(gs, 0, 40, false));  // sin cambio
    CHECK(!fanNeedsUpdate(gs, 0, 38, false));  // 2 no es > 2 al bajar
    CHECK(fanNeedsUpdate(gs, 0, 37, false));   // 3 > 2

    /* Si nunca se aplicó, se escribe sin esperar. */
    gs.fan_applied[0] = -1;
    gs.fan_last_write_time[0] = now;
    CHECK(fanNeedsUpdate(gs, 0, 40, false));

    /* Reafirmación periódica aunque el valor no cambie. */
    gs.fan_applied[0] = 40;
    gs.fan_last_write_time[0] =
        now - std::chrono::milliseconds(FAN_WRITE_REASSERT_MS + 1);
    CHECK(fanNeedsUpdate(gs, 0, 40, false));

    /* resync_needed también fuerza la escritura. */
    gs.fan_last_write_time[0] = now;
    gs.fan_resync_needed[0] = true;
    CHECK(fanNeedsUpdate(gs, 0, 40, false));
}

static void testFanClamp() {
    resetHarness();
    GpuState& gs = makeGpu();

    applyFanSpeed(gs, 0, 0, 150, true);
    CHECK(g_mock_fan_set_calls == 1);
    CHECK(g_mock_fan_set_last == 100);
    CHECK(gs.fan_applied[0] == 100);

    applyFanSpeed(gs, 0, 0, -5, true);
    CHECK(g_mock_fan_set_last == 0);
    CHECK(gs.fan_applied[0] == 0);

    // GPU_LOST no debe escribir nada.
    gs.state = GpuRuntimeState::GPU_LOST;
    applyFanSpeed(gs, 0, 0, 50, true);
    CHECK(g_mock_fan_set_last == 0);
}

static void testSensorStaleness() {
    resetHarness();
    g_config.sensor_timeout_ms = 6000;
    g_config.sensor_interval_ms = 2000;
    g_config.use_vram_temp = true;
    g_config.use_hotspot_temp = false;

    auto now = std::chrono::steady_clock::now();
    CHECK(sensorFresh(true, now, 6000));
    CHECK(!sensorFresh(true,
        now - std::chrono::milliseconds(7000), 6000));
    CHECK(!sensorFresh(false, now, 6000));

    GpuState& gs = makeGpu();
    gs.core_available = true;
    gs.core_temp_c = 60;
    gs.core_last_ok = now;
    gs.vram_available = true;
    gs.vram_temp_c = 90;
    gs.vram_last_ok = now - std::chrono::milliseconds(7000);

    TempReading curve = computeCurveTemp(gs);
    CHECK(curve.ok);
    CHECK(curve.temp == 60);   // VRAM obsoleto no participa

    gs.vram_last_ok = now - std::chrono::milliseconds(5000);
    curve = computeCurveTemp(gs);
    CHECK(curve.ok);
    CHECK(curve.temp == 90);   // dentro del timeout, aunque marcado stale
    CHECK(curve.stale);

    gs.vram_last_ok = now;
    gs.hotspot_available = true;
    gs.hotspot_temp_c = 95;
    gs.hotspot_last_ok = now;
    g_config.use_hotspot_temp = true;
    curve = computeCurveTemp(gs);
    CHECK(curve.ok);
    CHECK(curve.temp == 95);
    CHECK(!curve.stale);
}

static void testNvmlErrorState() {
    resetHarness();
    GpuState& gs = makeGpu();

    noteNvmlError(gs, 0, NVML_ERROR_NOT_SUPPORTED, "test");
    CHECK(gs.state == GpuRuntimeState::NORMAL);
    noteNvmlError(gs, 0, NVML_ERROR_NOT_SUPPORTED, "test");
    noteNvmlError(gs, 0, NVML_ERROR_NOT_SUPPORTED, "test");
    CHECK(gs.state == GpuRuntimeState::NVML_ERROR);

    gs.fan_last_write_error[0] = NVML_SUCCESS;
    gs.sensor_fail_streak = 0;
    noteNvmlSuccess(gs);
    CHECK(gs.state == GpuRuntimeState::NORMAL);
}

static void testGpuLostSingleAndMulti() {
    resetHarness();
    g_config.stop_on_gpu_lost = true;
    g_control_started = true;

    GpuState& gpu0 = makeGpu();
    transitionGpu(gpu0, 0, GpuRuntimeState::GPU_LOST, "test");
    CHECK(gpu0.state == GpuRuntimeState::GPU_LOST);
    CHECK(!gpu0.mmio_allowed);
    CHECK(g_mock_close_calls == 1);
    CHECK(g_shutdown == 1);

    // Con otra GPU sana el servicio debe continuar.
    resetHarness();
    g_config.stop_on_gpu_lost = true;
    g_control_started = true;
    g_gpu_states.resize(2);
    g_gpu_states[0].valid = true;
    g_gpu_states[0].pci_bus_id = "0000:01:00.0";
    g_gpu_states[0].state = GpuRuntimeState::NORMAL;
    g_gpu_states[1].valid = true;
    g_gpu_states[1].pci_bus_id = "0000:02:00.0";
    g_gpu_states[1].state = GpuRuntimeState::NORMAL;

    transitionGpu(g_gpu_states[0], 0, GpuRuntimeState::GPU_LOST, "test");
    CHECK(g_shutdown == 0);
    CHECK(g_gpu_states[1].state == GpuRuntimeState::NORMAL);
}

static void testRecovery() {
    resetHarness();
    g_config.auto_recover_gpu = true;
    g_config.stop_on_gpu_lost = false;

    GpuState& gs = makeGpu();
    gs.state = GpuRuntimeState::GPU_LOST;
    gs.last_recovery_probe =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(61000);
    gs.recovery_attempts = 0;
    gs.fan_resync_needed[0] = false;

    maybeRecoverGpu(gs, 0);
    CHECK(gs.state == GpuRuntimeState::NORMAL);
    CHECK(gs.recovery_attempts == 0);
    CHECK(g_mock_reset_calls == 1);
    CHECK(gs.fan_resync_needed[0]);
    CHECK(recent(gs.core_last_ok));

    // Si el sondeo falla, se agota y queda DISABLED sin tocar nada.
    resetHarness();
    g_config.auto_recover_gpu = true;
    g_config.stop_on_gpu_lost = false;
    GpuState& lost = makeGpu();
    lost.state = GpuRuntimeState::GPU_LOST;
    lost.recovery_attempts = MAX_RECOVERY_ATTEMPTS;
    maybeRecoverGpu(lost, 0);
    CHECK(lost.state == GpuRuntimeState::DISABLED);
}

static void testManualOverride() {
    resetHarness();
    GpuState& gs = makeGpu();
    gs.effective_ok = true;
    gs.effective_temp_c = 60;
    gs.effective_last_ok = std::chrono::steady_clock::now();
    g_config.min_speed = 40;
    g_config.max_speed = 70;
    controlGpu(gs, 0, 0, true);
    CHECK(g_mock_fan_set_last == 0);
    controlGpu(gs, 0, 100, true);
    CHECK(g_mock_fan_set_last == 100);
    gs.critical_latched = true;
    controlGpu(gs, 0, 0, true);
    CHECK(g_mock_fan_set_last == 100);
}

static void testMultiGpuIndependence() {
    resetHarness();
    g_config.stop_on_gpu_lost = false;
    g_config.fan_update_interval_ms = 2000;
    g_config.fan_change_threshold = 0;

    g_gpu_states.resize(2);
    GpuState& lost = g_gpu_states[0];
    lost.valid = true;
    lost.pci_bus_id = "0000:01:00.0";
    lost.state = GpuRuntimeState::GPU_LOST;

    GpuState& healthy = g_gpu_states[1];
    healthy.device = mockHandle();
    healthy.valid = true;
    healthy.pci_bus_id = "0000:02:00.0";
    healthy.state = GpuRuntimeState::NORMAL;
    healthy.fan_count = 1;
    healthy.effective_ok = true;
    healthy.effective_temp_c = 70;
    healthy.effective_last_ok = std::chrono::steady_clock::now();
    healthy.fan_resync_needed[0] = true;

    controlGpu(healthy, 1, -1, false);
    CHECK(g_mock_fan_set_calls == 1);
    CHECK(g_mock_fan_set_last == 78);   // 40 + (70-45)/40 * 60 = 77.5 -> 78
    CHECK(lost.state == GpuRuntimeState::GPU_LOST);
}

static void testCriticalLatch() {
    resetHarness();
    g_config.critical_temp = 90;

    GpuState& gs = makeGpu();
    gs.core_available = true;
    gs.core_temp_c = 95;

    TempReading lock;
    lock.ok = true;
    lock.temp = 95;
    updateCriticalLatch(gs, 0, lock);
    CHECK(gs.critical_latched);

    lock.temp = 86;   // por encima de critical-5
    updateCriticalLatch(gs, 0, lock);
    CHECK(gs.critical_latched);

    lock.temp = 84;   // libera en <= 85
    updateCriticalLatch(gs, 0, lock);
    CHECK(!gs.critical_latched);
}

static void testPowerModes() {
    resetHarness();
    g_config.power_limit = 300;
    GpuState& gs = makeGpu();
    gs.pci_bus_id = "0000:01:00.0";

    g_config.power_limit_mode = PowerLimitMode::MONITOR;
    initPowerLimit(gs, 0);
    CHECK(g_mock_power_set_calls == 0);

    g_config.power_limit_mode = PowerLimitMode::ENFORCE;
    gs.power_limit_warned = false;
    initPowerLimit(gs, 0);
    CHECK(g_mock_power_set_calls == 1);
    CHECK(g_mock_power_set_last == 300000);

    resetHarness();
    g_config.power_limit = 300;
    g_config.power_limit_mode = PowerLimitMode::STARTUP;
    GpuState& gs2 = makeGpu();
    initPowerLimit(gs2, 0);
    CHECK(g_mock_power_set_calls == 1);

    // NOT_SUPPORTED no debe degradar el estado de la GPU ni martillear.
    resetHarness();
    g_config.power_limit = 300;
    g_config.power_limit_mode = PowerLimitMode::ENFORCE;
    g_mock_power_get_result = NVML_ERROR_NOT_SUPPORTED;
    GpuState& gs3 = makeGpu();
    initPowerLimit(gs3, 0);
    CHECK(!gs3.power_limit_supported);
    CHECK(gs3.state == GpuRuntimeState::NORMAL);
    CHECK(g_mock_power_set_calls == 0);
}

static void testCleanup() {
    resetHarness();
    CHECK(gpu_temps_close_all());
    CHECK(g_mock_close_all_calls == 1);
}

int main() {
    testFanCurve();
    testFanThresholdAndHysteresis();
    testFanClamp();
    testSensorStaleness();
    testNvmlErrorState();
    testGpuLostSingleAndMulti();
    testRecovery();
    testManualOverride();
    testMultiGpuIndependence();
    testCriticalLatch();
    testPowerModes();
    testCleanup();

    std::cout << "test_daemon: " << g_checks << " comprobaciones, "
              << g_failures << " fallo(s)" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
