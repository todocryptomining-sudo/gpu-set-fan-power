#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Lectura directa de temperatura VRAM y Hotspot desde registros PCI del GPU.
 *
 * Misma técnica que gddr6.c / nvml_direct_access.c:
 *   - Localiza BAR0 en /sys/bus/pci/devices/<bdf>/resource (primera línea, primer campo)
 *   - Mapea /dev/mem en la página que contiene el registro
 *   - Lee el registro de 32 bits y aplica la fórmula
 *
 * RTX 3090 (GA102, device id 0x2204):
 *   VRAM:    BAR0 + 0x0000E2A8,  temp = (raw & 0x00000FFF) / 0x20
 *   Hotspot: BAR0 + 0x0002046C,  temp = (raw >> 8) & 0xFF
 *
 * Requiere root (acceso a /dev/mem).
 */

#include <stdbool.h>

typedef struct {
    bool vram_available;
    bool hotspot_available;
    int  vram_temp_c;
    int  hotspot_temp_c;
} gpu_temps_t;

/* Lee VRAM y Hotspot directamente de los registros PCI.
   pci_bus_id en formato "0000:01:00.0" (dominio de 4 dígitos, función .0).
   Devuelve true si al menos una de las dos temperaturas es válida. */
bool gpu_temps_read(const char* pci_bus_id, gpu_temps_t* out);

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

#ifdef __cplusplus
}
#endif
