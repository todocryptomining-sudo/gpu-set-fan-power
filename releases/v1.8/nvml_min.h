#pragma once
/* Minimal NVML ABI declarations.
   The NVIDIA driver ships libnvidia-ml.so.1 but Ubuntu images often lack
   nvml.h / libnvidia-nvml-dev. Layouts below match the public NVML ABI:
   name/uuid sizes, PCI info v3, memory, utilization, fans. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvmlDevice_st* nvmlDevice_t;
typedef int nvmlReturn_t;

#define NVML_SUCCESS 0
#define NVML_ERROR_UNINITIALIZED 1
#define NVML_ERROR_INVALID_ARGUMENT 2
#define NVML_ERROR_NOT_SUPPORTED 3
#define NVML_ERROR_NO_PERMISSION 4
#define NVML_ERROR_ALREADY_INITIALIZED 5
#define NVML_ERROR_NOT_FOUND 6
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_ERROR_INSUFFICIENT_POWER 8
#define NVML_ERROR_DRIVER_NOT_LOADED 9
#define NVML_ERROR_TIMEOUT 10
#define NVML_ERROR_IRQ_ISSUE 11
#define NVML_ERROR_LIBRARY_NOT_FOUND 12
#define NVML_ERROR_FUNCTION_NOT_FOUND 13
#define NVML_ERROR_CORRUPTED_INFOROM 14
#define NVML_ERROR_GPU_IS_LOST 15
#define NVML_ERROR_RESET_REQUIRED 16
#define NVML_ERROR_OPERATING_SYSTEM 17
#define NVML_DEVICE_NAME_BUFFER_SIZE 64
#define NVML_DEVICE_UUID_BUFFER_SIZE 80

typedef enum {
    NVML_FEATURE_DISABLED = 0,
    NVML_FEATURE_ENABLED  = 1
} nvmlEnableState_t;

typedef enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
    NVML_CLOCK_VIDEO    = 3
} nvmlClockType_t;

typedef int nvmlTemperatureSensors_t;

typedef enum {
    NVML_FAN_POLICY_AUTO   = 0,
    NVML_FAN_POLICY_MANUAL = 1
} nvmlFanControlPolicy_t;

typedef struct {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

nvmlReturn_t nvmlInit(void);
nvmlReturn_t nvmlShutdown(void);
const char*  nvmlErrorString(nvmlReturn_t result);
nvmlReturn_t nvmlDeviceGetCount(unsigned int* deviceCount);
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t* device);
nvmlReturn_t nvmlDeviceGetHandleByUUID(const char* uuid, nvmlDevice_t* device);
nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char* name, unsigned int length);
nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* uuid, unsigned int length);
nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t device, nvmlPciInfo_t* pci);
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int* temp);
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t* utilization);
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory);
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int* power);
nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int* limit);
nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t device, unsigned int* minLimit, unsigned int* maxLimit);
nvmlReturn_t nvmlDeviceSetPowerManagementLimit(nvmlDevice_t device, unsigned int limit);
nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int* clock);
nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t device, unsigned int* numFans);
nvmlReturn_t nvmlDeviceGetFanSpeed_v2(nvmlDevice_t device, unsigned int fan, unsigned int* speed);
nvmlReturn_t nvmlDeviceSetFanSpeed_v2(nvmlDevice_t device, unsigned int fan, unsigned int speed);
nvmlReturn_t nvmlDeviceSetDefaultFanSpeed_v2(nvmlDevice_t device, unsigned int fan);
nvmlReturn_t nvmlDeviceGetFanControlPolicy_v2(nvmlDevice_t device, unsigned int fan, nvmlFanControlPolicy_t* policy);
nvmlReturn_t nvmlDeviceSetFanControlPolicy(nvmlDevice_t device, unsigned int fan, nvmlFanControlPolicy_t policy);
nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t* mode);
nvmlReturn_t nvmlDeviceSetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t mode);

#ifdef __cplusplus
}
#endif
