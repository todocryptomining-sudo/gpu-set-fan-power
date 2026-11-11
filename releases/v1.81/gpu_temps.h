#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Lectura directa de temperatura VRAM y Hotspot desde registros PCI del GPU.
 *
 * Misma técnica que gddr6.c / nvml_direct_access.c:
 *   - Valida vendor/device contra una tabla explícita de GPUs compatibles.
 *   - Localiza BAR0 en /sys/bus/pci/devices/<bdf>/resource (primera línea).
 *   - Comprueba que el BAR0 es memoria, que su rango cubre el offset y que
 *     no hay desbordamiento de direcciones (aritmética de 64 bits).
 *   - Mapea /dev/mem en modo SOLO LECTURA, con mmap() persistente por GPU.
 *   - Lee el registro de 32 bits y aplica la fórmula del perfil.
 *
 * Perfiles verificados (v1.81):
 *   GA102 (RTX 3080 / 3080 Ti / 3090):
 *     VRAM:    BAR0 + 0x0000E2A8,  temp = (raw & 0x00000FFF) / 0x20
 *     Hotspot: BAR0 + 0x0002046C,  temp = (raw >> 8) & 0xFF
 *   GA104 (RTX 3070 / 3070 LHR):
 *     VRAM:    BAR0 + 0x0000EE50,  temp = (raw & 0x00000FFF) / 0x20
 *     Hotspot: BAR0 + 0x0002046C,  temp = (raw >> 8) & 0xFF
 *
 * Un device ID desconocido NO se lee: devuelve GPU_TEMPS_UNSUPPORTED. Los
 * offsets no son universales; añadir una entrada a la tabla solo después de
 * verificarla en hardware real.
 *
 * Requiere root (acceso a /dev/mem).
 */

#include <stdbool.h>
#include <stdint.h>

/* Maximum number of fan sensors handled per GPU. Most consumer cards have
   1-3; the array is fixed so the on-disk format stays simple. */
#define GPU_TEMPS_MAX_FANS 8

typedef enum {
    GPU_TEMPS_OK = 0,
    GPU_TEMPS_UNSUPPORTED,       /* device ID fuera de la tabla */
    GPU_TEMPS_NO_DEVICE,         /* no existe el BDF o falta /dev/mem */
    GPU_TEMPS_PERMISSION,        /* sin permiso para /dev/mem */
    GPU_TEMPS_INVALID,           /* BDF/BAR0/rango inválido */
    GPU_TEMPS_MMIO_ERROR,        /* mmap/munmap/lectura falló */
    GPU_TEMPS_DEVICE_LOST,       /* el dispositivo dejó de responder */
    GPU_TEMPS_QUARANTINED       /* lecturas suspendidas tras un fallo */
} gpu_temps_status_t;

typedef struct {
    bool vram_available;
    bool hotspot_available;
    int  vram_temp_c;
    int  hotspot_temp_c;
    /* Metadatos de la validación (v1.81). */
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int vram_offset;
    unsigned int hotspot_offset;
    bool profile_known;
    gpu_temps_status_t status;
} gpu_temps_t;

typedef struct {
    int count;                          /* number of valid entries */
    int speed[GPU_TEMPS_MAX_FANS];      /* percent, -1 = read failed */
} gpu_fans_t;

typedef struct {
    const char* bus_id;
    int count;
    int speed[GPU_TEMPS_MAX_FANS];
} gpu_fans_entry_t;

/* Lee VRAM y Hotspot directamente de los registros PCI.
   pci_bus_id en formato "0000:01:00.0" (dominio de 4 dígitos, función .0).
   Devuelve true si al menos una de las dos temperaturas es válida.
   El estado detallado queda en out->status. */
bool gpu_temps_read(const char* pci_bus_id, gpu_temps_t* out);

/* Igual que gpu_temps_read(), pero devuelve el estado detallado. */
gpu_temps_status_t gpu_temps_read_ex(const char* pci_bus_id, gpu_temps_t* out);

/* Texto estable para logs (nunca NULL). */
const char* gpu_temps_status_string(gpu_temps_status_t status);

/* Cierra el mapeo/fd persistente de una GPU. Devuelve false si munmap()
   falló (el recurso se marca como cerrado igualmente). */
bool gpu_temps_close(const char* pci_bus_id);

/* Cierra todos los mapeos/fds persistentes. Devuelve false si algún munmap()
   falló. Llamar al parar el daemon o al detectar GPU_LOST. */
bool gpu_temps_close_all(void);

/* Borra la cuarentena de una GPU para permitir un nuevo intento de lectura
   (usar solo desde RECOVERING, nunca en un bucle caliente). */
void gpu_temps_reset(const char* pci_bus_id);

/* Archivo de estado compartido con vergpu (para que un usuario sin root
   pueda ver las temperaturas que lee el daemon como root).
   Formato, una línea por GPU:  <bdf> <vram_c|-1> <hotspot_c|-1>  */
bool gpu_temps_status_write(const char* path,
                            const char* const* bus_ids,
                            const int* vram,
                            const int* hotspot,
                            int count);

bool gpu_temps_status_read(const char* path,
                           const char* pci_bus_id,
                           int* vram,
                           int* hotspot);

/* Archivo de estado de ventiladores (para vergpu y comandos sin root).
   Formato, una línea por GPU:  <bdf> <n> <s0> <s1> ... <sn-1>
   donde cada velocidad es un porcentaje 0-100 o -1 si la lectura falló. */
bool gpu_fans_status_write(const char* path,
                           const gpu_fans_entry_t* entries,
                           int count);

bool gpu_fans_status_read(const char* path,
                          const char* pci_bus_id,
                          gpu_fans_t* out);

#ifdef __cplusplus
}
#endif
