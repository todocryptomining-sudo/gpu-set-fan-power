#define _GNU_SOURCE

#include "gpu_temps.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#define MEM_PATH             "/dev/mem"
#define SYSFS_PCI_DEVICES    "/sys/bus/pci/devices"
#define VRAM_REG_OFFSET      0x0000E2A8u   /* GA102 / AD102 y mayoría */
#define VRAM_REG_OFFSET_GA104 0x0000EE50u  /* RTX 3070 / 3070 LHR */
#define HOTSPOT_REG_OFFSET   0x0002046Cu

/* ─────────────────────────────────────────────────────────────────────────────
 * sysfs helpers
 * ───────────────────────────────────────────────────────────────────────────── */

/* BAR0: /sys/bus/pci/devices/<bdf>/resource es un archivo de texto.
   La primera línea contiene: <start> <end> <flags> de BAR0.
   (resource0 NO sirve: es el contenido binario del BAR, no su dirección). */
static unsigned long long get_bar0_from_sysfs(const char* pci_bus_id) {
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/resource", SYSFS_PCI_DEVICES, pci_bus_id);

    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    unsigned long long start = 0;
    if (fscanf(fp, "%llx", &start) != 1) start = 0;
    fclose(fp);
    return start;
}

static unsigned int get_device_id_from_sysfs(const char* pci_bus_id) {
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/device", SYSFS_PCI_DEVICES, pci_bus_id);

    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    unsigned int id = 0;
    if (fscanf(fp, "%x", &id) != 1) id = 0;
    fclose(fp);
    return id;
}

/* Offsets por modelo. GA104 usa 0xEE50; el resto de la tabla usa 0xE2A8. */
static uint32_t vram_offset_for_device(unsigned int device_id) {
    switch (device_id) {
        case 0x2484: /* RTX 3070    */
        case 0x2488: /* RTX 3070 LHR */
            return VRAM_REG_OFFSET_GA104;
        default:
            return VRAM_REG_OFFSET;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Lectura de un registro de 32 bits vía /dev/mem
 * ───────────────────────────────────────────────────────────────────────────── */
static int read_reg32(int fd, unsigned long long phys_addr, uint32_t* out) {
    long page = sysconf(_SC_PAGE_SIZE);
    if (page <= 0 || !out) return -1;

    unsigned long long page_mask  = (unsigned long long)page - 1ULL;
    unsigned long long base       = phys_addr & ~page_mask;
    size_t             delta      = (size_t)(phys_addr - base);

    void* map = mmap(NULL, (size_t)page, PROT_READ, MAP_SHARED,
                     fd, (off_t)base);
    if (map == MAP_FAILED) return -1;

    /* volatile: el registro es MMIO, no debe cachearse ni reordenarse */
    *out = *(volatile uint32_t*)((char*)map + delta);

    munmap(map, (size_t)page);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * API principal
 * ───────────────────────────────────────────────────────────────────────────── */
bool gpu_temps_read(const char* pci_bus_id, gpu_temps_t* out) {
    if (!out) return false;

    memset(out, 0, sizeof(*out));
    if (!pci_bus_id || !pci_bus_id[0]) return false;

    /* Sanidad: evitar rutas arbitrarias en sysfs */
    if (strchr(pci_bus_id, '/') || strchr(pci_bus_id, '.') == pci_bus_id) return false;
    if (strlen(pci_bus_id) > 32) return false;

    unsigned long long bar0 = get_bar0_from_sysfs(pci_bus_id);
    if (bar0 == 0) return false;

    int fd = open(MEM_PATH, O_RDONLY | O_SYNC);
    if (fd < 0) return false;

    unsigned int device_id = get_device_id_from_sysfs(pci_bus_id);
    uint32_t vram_offset   = vram_offset_for_device(device_id);

    uint32_t raw = 0;

    if (read_reg32(fd, bar0 + vram_offset, &raw) == 0) {
        int t = (int)((raw & 0x00000FFFu) / 0x20u);
        if (t > 0 && t <= 120) {
            out->vram_available = true;
            out->vram_temp_c    = t;
        }
    }

    if (read_reg32(fd, bar0 + HOTSPOT_REG_OFFSET, &raw) == 0) {
        int t = (int)((raw >> 8) & 0xFFu);
        if (t > 0 && t <= 120) {
            out->hotspot_available = true;
            out->hotspot_temp_c    = t;
        }
    }

    close(fd);
    return out->vram_available || out->hotspot_available;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Archivo de estado para vergpu sin root
 * ───────────────────────────────────────────────────────────────────────────── */
bool gpu_temps_status_write(const char* path,
                            const char* const* bus_ids,
                            const int* vram,
                            const int* hotspot,
                            int count) {
    if (!path || !bus_ids || !vram || !hotspot || count < 0) return false;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* fp = fopen(tmp, "w");
    if (!fp) return false;

    for (int i = 0; i < count; i++) {
        if (!bus_ids[i] || !bus_ids[i][0]) continue;
        fprintf(fp, "%s %d %d\n", bus_ids[i], vram[i], hotspot[i]);
    }

    fflush(fp);
    fclose(fp);

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }

    chmod(path, 0644);
    return true;
}

bool gpu_temps_status_read(const char* path,
                           const char* pci_bus_id,
                           int* vram,
                           int* hotspot) {
    if (!path || !pci_bus_id || !vram || !hotspot) return false;

    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char bus[64];
    int v = -1, h = -1;
    bool found = false;

    while (fscanf(fp, "%63s %d %d", bus, &v, &h) == 3) {
        if (strcmp(bus, pci_bus_id) == 0) {
            *vram = v;
            *hotspot = h;
            found = true;
            break;
        }
    }

    fclose(fp);
    return found;
}
