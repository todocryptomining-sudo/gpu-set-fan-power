#define _GNU_SOURCE

#include "gpu_temps.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Configuración de acceso
 * ───────────────────────────────────────────────────────────────────────────── */

#ifdef GPU_TEMPS_TEST
/* Solo para las pruebas: permite usar un sysfs y un "mem" falsos. El binario
 * de producción se compila SIN esta macro y siempre usa las rutas reales. */
static const char* temps_sysfs_root(void) {
    const char* value = getenv("GPU_TEMPS_SYSFS_ROOT");
    return (value && value[0]) ? value : "/sys/bus/pci/devices";
}
static const char* temps_mem_path(void) {
    const char* value = getenv("GPU_TEMPS_MEM_PATH");
    return (value && value[0]) ? value : "/dev/mem";
}
#else
#define temps_sysfs_root() "/sys/bus/pci/devices"
#define temps_mem_path()   "/dev/mem"
#endif

#define MAX_CONTEXTS             16
#define MAX_PAGES                 4
#define MMIO_RETRY_MS         30000ULL
#define PERMISSION_RETRY_MS   60000ULL
#define DEVICE_RETRY_MS      300000ULL
#define MIN_BAR_SIZE           4096ULL
#define MAX_BAR_SIZE    (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define IORESOURCE_MEM          0x00000200ULL
#define TEMP_MIN_VALID          1
#define TEMP_MAX_VALID        125

/* ─────────────────────────────────────────────────────────────────────────────
 * Tabla explícita de GPUs y offsets verificados.
 *
 * NO añadir un device ID "porque probablemente use el mismo offset". Los
 * registros MMIO cambian entre arquitecturas y una lectura incorrecta puede
 * devolver un valor plausible pero falso. Añadir solo tras verificarlo en
 * hardware real con la lectura y la temperatura conocida.
 * ───────────────────────────────────────────────────────────────────────────── */

typedef struct {
    unsigned int device_id;
    const char*  model;
    uint32_t     vram_offset;
    uint32_t     hotspot_offset;
} GpuTempProfile;

static const GpuTempProfile g_profiles[] = {
    { 0x2204, "GeForce RTX 3090 (GA102)",      0x0000E2A8u, 0x0002046Cu },
    { 0x2206, "GeForce RTX 3080 (GA102)",      0x0000E2A8u, 0x0002046Cu },
    { 0x2203, "GeForce RTX 3080 Ti (GA102)",   0x0000E2A8u, 0x0002046Cu },
    { 0x2484, "GeForce RTX 3070 (GA104)",      0x0000EE50u, 0x0002046Cu },
    { 0x2488, "GeForce RTX 3070 LHR (GA104)",  0x0000EE50u, 0x0002046Cu },
};

static const GpuTempProfile* profile_for_device(unsigned int device_id) {
    for (size_t i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]); i++) {
        if (g_profiles[i].device_id == device_id) return &g_profiles[i];
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Contexto persistente por GPU
 * ───────────────────────────────────────────────────────────────────────────── */

typedef struct {
    void*    ptr;
    size_t   len;
    uint64_t base;
} MmioPage;

typedef struct {
    bool     used;
    bool     negative;          /* fallo de metadatos/permiso, sin fd */
    bool     profile_known;
    bool     quarantined;
    uint64_t retry_after_ms;
    char     bdf[32];
    int      fd;
    uint64_t bar0;
    uint64_t bar_end;
    uint64_t bar_size;
    unsigned int vendor_id;
    unsigned int device_id;
    uint32_t vram_offset;
    uint32_t hotspot_offset;
    gpu_temps_status_t last_status;
    uint64_t last_used_ms;
    MmioPage pages[MAX_PAGES];
    int      page_count;
} GpuContext;

static GpuContext g_contexts[MAX_CONTEXTS];

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool is_power_of_two_u64(uint64_t value) {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static long page_size_value(void) {
    long value = sysconf(_SC_PAGE_SIZE);
    if (value <= 0 || !is_power_of_two_u64((uint64_t)value)) return 0;
    return value;
}

static bool valid_bdf(const char* bdf) {
    if (!bdf || strlen(bdf) != 12) return false;
    if (strchr(bdf, '/') != NULL) return false;

    unsigned int domain = 0, bus = 0, device = 0, function = 0;
    int consumed = 0;
    if (sscanf(bdf, "%4x:%2x:%2x.%1x%n",
               &domain, &bus, &device, &function, &consumed) != 4) {
        return false;
    }
    return consumed == 12 && bdf[12] == '\0' && function == 0;
}

static bool read_hex_file(const char* path, unsigned int* out) {
    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    unsigned int value = 0;
    bool ok = (fscanf(fp, "%x", &value) == 1);
    fclose(fp);

    if (ok && out) *out = value;
    return ok;
}

static bool read_bar0(const char* bdf, uint64_t* start, uint64_t* end,
                      uint64_t* flags) {
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/resource",
                     temps_sysfs_root(), bdf);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;

    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    unsigned long long s = 0, e = 0, f = 0;
    bool ok = (fscanf(fp, "%llx %llx %llx", &s, &e, &f) == 3);
    fclose(fp);

    if (!ok) return false;
    if (start) *start = (uint64_t)s;
    if (end)   *end   = (uint64_t)e;
    if (flags) *flags = (uint64_t)f;
    return true;
}

static void close_context(GpuContext* ctx, bool* munmap_ok) {
    bool ok = true;

    for (int i = 0; i < ctx->page_count; i++) {
        if (ctx->pages[i].ptr && ctx->pages[i].len > 0) {
            if (munmap(ctx->pages[i].ptr, ctx->pages[i].len) != 0) ok = false;
        }
        ctx->pages[i].ptr = NULL;
        ctx->pages[i].len = 0;
        ctx->pages[i].base = 0;
    }
    ctx->page_count = 0;

    if (ctx->fd >= 0) {
        if (close(ctx->fd) != 0) ok = false;
        ctx->fd = -1;
    }

    if (munmap_ok) *munmap_ok = ok;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->used = false;
}

static GpuContext* find_context(const char* bdf) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].used && strcmp(g_contexts[i].bdf, bdf) == 0) {
            return &g_contexts[i];
        }
    }
    return NULL;
}

static GpuContext* alloc_context(void) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!g_contexts[i].used) {
            memset(&g_contexts[i], 0, sizeof(g_contexts[i]));
            g_contexts[i].fd = -1;
            g_contexts[i].used = true;
            return &g_contexts[i];
        }
    }

    /* Reutilizar el contexto menos usado. Es una salvaguarda: el daemon
     * normalmente tiene 1-8 GPUs. */
    GpuContext* oldest = &g_contexts[0];
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].last_used_ms < oldest->last_used_ms) oldest = &g_contexts[i];
    }
    close_context(oldest, NULL);
    oldest->used = true;
    return oldest;
}

static gpu_temps_status_t fill_metadata(GpuContext* ctx, gpu_temps_t* out) {
    if (out) {
        out->vendor_id = ctx->vendor_id;
        out->device_id = ctx->device_id;
        out->vram_offset = ctx->vram_offset;
        out->hotspot_offset = ctx->hotspot_offset;
        out->profile_known = ctx->profile_known;
        out->status = ctx->last_status;
    }
    return ctx->last_status;
}

static gpu_temps_status_t get_context(const char* bdf, GpuContext** out) {
    *out = NULL;

    GpuContext* ctx = find_context(bdf);
    if (ctx) {
        uint64_t now = monotonic_ms();
        if (ctx->negative && ctx->retry_after_ms > now) {
            *out = ctx;
            return ctx->last_status;
        }
        if (ctx->quarantined && ctx->retry_after_ms > now) {
            *out = ctx;
            return GPU_TEMPS_QUARANTINED;
        }
        /* Cuarentena/negativo caducado: cerrar y reconstruir. */
        if (ctx->negative || ctx->quarantined) {
            close_context(ctx, NULL);
            ctx = NULL;
        } else {
            ctx->last_used_ms = now;
            *out = ctx;
            return GPU_TEMPS_OK;
        }
    }

    ctx = alloc_context();
    if (!ctx) return GPU_TEMPS_MMIO_ERROR;

    snprintf(ctx->bdf, sizeof(ctx->bdf), "%s", bdf);
    ctx->fd = -1;
    ctx->last_used_ms = monotonic_ms();
    ctx->last_status = GPU_TEMPS_INVALID;

    /* Vendor: NVIDIA (0x10DE). Cualquier otro valor se rechaza. */
    char path[512];
    unsigned int vendor = 0, device = 0;

    int n = snprintf(path, sizeof(path), "%s/%s/vendor",
                     temps_sysfs_root(), bdf);
    if (n < 0 || (size_t)n >= sizeof(path) ||
        !read_hex_file(path, &vendor) || vendor != 0x10DE) {
        ctx->negative = true;
        ctx->retry_after_ms = UINT64_MAX;
        ctx->last_status = GPU_TEMPS_UNSUPPORTED;
        *out = ctx;
        return ctx->last_status;
    }

    n = snprintf(path, sizeof(path), "%s/%s/device",
                 temps_sysfs_root(), bdf);
    if (n < 0 || (size_t)n >= sizeof(path) ||
        !read_hex_file(path, &device) || device == 0) {
        ctx->negative = true;
        ctx->retry_after_ms = UINT64_MAX;
        ctx->last_status = GPU_TEMPS_NO_DEVICE;
        *out = ctx;
        return ctx->last_status;
    }

    ctx->vendor_id = vendor;
    ctx->device_id = device;

    const GpuTempProfile* profile = profile_for_device(device);
    if (!profile) {
        /* Device NVIDIA desconocido: no tocar MMIO. */
        ctx->negative = true;
        ctx->retry_after_ms = UINT64_MAX;
        ctx->last_status = GPU_TEMPS_UNSUPPORTED;
        *out = ctx;
        return ctx->last_status;
    }

    ctx->profile_known = true;
    ctx->vram_offset = profile->vram_offset;
    ctx->hotspot_offset = profile->hotspot_offset;

    uint64_t bar0 = 0, bar_end = 0, flags = 0;
    if (!read_bar0(bdf, &bar0, &bar_end, &flags)) {
        ctx->negative = true;
        ctx->retry_after_ms = monotonic_ms() + PERMISSION_RETRY_MS;
        ctx->last_status = GPU_TEMPS_INVALID;
        *out = ctx;
        return ctx->last_status;
    }

    if (bar0 == 0 || bar_end < bar0 ||
        (flags & IORESOURCE_MEM) == 0) {
        if (getenv("GPU_TEMPS_TEST_VERBOSE")) {
            fprintf(stderr, "gpu_temps: invalid BAR0 for %s (start=0x%llx end=0x%llx flags=0x%llx)\n",
                    bdf, (unsigned long long)bar0,
                    (unsigned long long)bar_end,
                    (unsigned long long)flags);
        }
        ctx->negative = true;
        ctx->retry_after_ms = monotonic_ms() + MMIO_RETRY_MS;
        ctx->last_status = GPU_TEMPS_INVALID;
        *out = ctx;
        return ctx->last_status;
    }

    uint64_t size = bar_end - bar0 + 1ULL;
    if (size == 0 || size < MIN_BAR_SIZE || size > MAX_BAR_SIZE) {
        if (getenv("GPU_TEMPS_TEST_VERBOSE")) {
            fprintf(stderr, "gpu_temps: invalid BAR0 size for %s (size=0x%llx)\n",
                    bdf, (unsigned long long)size);
        }
        ctx->negative = true;
        ctx->retry_after_ms = monotonic_ms() + MMIO_RETRY_MS;
        ctx->last_status = GPU_TEMPS_INVALID;
        *out = ctx;
        return ctx->last_status;
    }

    /* Los dos offsets deben caber dentro del BAR0, comprobando overflow. */
    if ((uint64_t)ctx->vram_offset + 4ULL > size ||
        (uint64_t)ctx->hotspot_offset + 4ULL > size) {
        ctx->negative = true;
        ctx->retry_after_ms = UINT64_MAX;
        ctx->last_status = GPU_TEMPS_INVALID;
        *out = ctx;
        return ctx->last_status;
    }

    ctx->bar0 = bar0;
    ctx->bar_end = bar_end;
    ctx->bar_size = size;

    /* O_RDONLY es deliberado: esta biblioteca solo lee registros MMIO.
     * Permisos los aplica el kernel sobre /dev/mem; no se permite una ruta
     * alternativa en builds de producción. */
    ctx->fd = open(temps_mem_path(), O_RDONLY | O_SYNC);
    if (ctx->fd < 0) {
        gpu_temps_status_t status = (errno == EACCES || errno == EPERM)
                                        ? GPU_TEMPS_PERMISSION
                                        : GPU_TEMPS_NO_DEVICE;
        ctx->negative = true;
        ctx->retry_after_ms = monotonic_ms() + PERMISSION_RETRY_MS;
        ctx->last_status = status;
        *out = ctx;
        return status;
    }

    ctx->last_status = GPU_TEMPS_OK;
    *out = ctx;
    return GPU_TEMPS_OK;
}

static MmioPage* find_page(GpuContext* ctx, uint64_t base) {
    for (int i = 0; i < ctx->page_count; i++) {
        if (ctx->pages[i].base == base) return &ctx->pages[i];
    }
    return NULL;
}

static bool map_page(GpuContext* ctx, uint64_t phys, MmioPage** out) {
    if (!ctx || !out) return false;
    long page = page_size_value();
    if (page <= 0) return false;
    uint64_t page_u = (uint64_t)page;
    uint64_t page_mask = page_u - 1ULL;
    uint64_t base = phys & ~page_mask;

    if (ctx->bar_size == 0 || ctx->bar0 > UINT64_MAX - ctx->bar_size ||
        ctx->bar_end != ctx->bar0 + ctx->bar_size - 1ULL ||
        base < ctx->bar0 || base > ctx->bar_end ||
        page_u - 1ULL > ctx->bar_end - base) {
        return false;
    }

    MmioPage* existing = find_page(ctx, base);
    if (existing) {
        *out = existing;
        return true;
    }

    if (ctx->page_count >= MAX_PAGES) return false;

    /* La dirección física y el tamaño del mapeo deben caber en off_t. */
    if (base > (uint64_t)INT64_MAX ||
        page_u > (uint64_t)INT64_MAX - base) {
        return false;
    }

    void* ptr = mmap(NULL, (size_t)page, PROT_READ, MAP_SHARED,
                     ctx->fd, (off_t)base);
    if (ptr == MAP_FAILED) return false;

    MmioPage* slot = &ctx->pages[ctx->page_count++];
    slot->ptr = ptr;
    slot->len = (size_t)page;
    slot->base = base;
    *out = slot;
    return true;
}

static bool read_reg32(GpuContext* ctx, uint32_t offset, uint32_t* out) {
    if (!out || !ctx || ctx->fd < 0) return false;
    if ((offset & 0x3u) != 0) return false;
    if ((uint64_t)offset > ctx->bar_size ||
        4ULL > ctx->bar_size - (uint64_t)offset) return false;

    uint64_t phys = ctx->bar0 + (uint64_t)offset;
    if (phys < ctx->bar0) return false;  /* overflow */

    MmioPage* page = NULL;
    if (!map_page(ctx, phys, &page)) return false;

    uint64_t delta = phys - page->base;
    if (delta + 4ULL > page->len) return false;

    /* volatile: el registro es MMIO, no debe cachearse ni reordenarse. */
    if (!page->ptr || page->len < sizeof(uint32_t) ||
        delta > page->len - sizeof(uint32_t)) return false;
    *out = *(volatile uint32_t*)((char*)page->ptr + delta);
    return true;
}

static bool plausible_temp(int value) {
    return value >= TEMP_MIN_VALID && value <= TEMP_MAX_VALID;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * API principal
 * ───────────────────────────────────────────────────────────────────────────── */

const char* gpu_temps_status_string(gpu_temps_status_t status) {
    switch (status) {
        case GPU_TEMPS_OK:            return "ok";
        case GPU_TEMPS_UNSUPPORTED:   return "GPU no incluida en la tabla de offsets verificados";
        case GPU_TEMPS_NO_DEVICE:     return "dispositivo PCI o /dev/mem no disponible";
        case GPU_TEMPS_PERMISSION:    return "sin permiso para /dev/mem (¿root?)";
        case GPU_TEMPS_INVALID:       return "BDF/BAR0/rango de registro inválido";
        case GPU_TEMPS_MMIO_ERROR:    return "error de mmap/munmap o lectura MMIO";
        case GPU_TEMPS_DEVICE_LOST:   return "la GPU dejó de responder (device lost)";
        case GPU_TEMPS_QUARANTINED:   return "lecturas MMIO en cuarentena";
        default:                      return "estado desconocido";
    }
}

gpu_temps_status_t gpu_temps_read_ex(const char* pci_bus_id, gpu_temps_t* out) {
    if (!out) return GPU_TEMPS_INVALID;

    memset(out, 0, sizeof(*out));
    out->status = GPU_TEMPS_INVALID;
    out->vram_temp_c = -1;
    out->hotspot_temp_c = -1;

    if (!valid_bdf(pci_bus_id)) return GPU_TEMPS_INVALID;

    GpuContext* ctx = NULL;
    gpu_temps_status_t status = get_context(pci_bus_id, &ctx);
    if (!ctx) return status;

    if (!ctx->profile_known) {
        fill_metadata(ctx, out);
        return ctx->last_status;
    }

    if (status == GPU_TEMPS_QUARANTINED) {
        fill_metadata(ctx, out);
        return status;
    }
    if (status != GPU_TEMPS_OK) {
        fill_metadata(ctx, out);
        return status;
    }

    /* También en la ruta de éxito se publican vendor/device/offsets: los usan
     * los logs y vergpu para diagnosticar qué perfil se aplicó. */
    fill_metadata(ctx, out);

    uint32_t raw = 0;
    bool mmio_failed = false;

    if (read_reg32(ctx, ctx->vram_offset, &raw)) {
        int value = (int)((raw & 0x00000FFFu) / 0x20u);
        if (plausible_temp(value)) {
            out->vram_available = true;
            out->vram_temp_c = value;
        }
    } else {
        mmio_failed = true;
    }

    raw = 0;
    if (read_reg32(ctx, ctx->hotspot_offset, &raw)) {
        int value = (int)((raw >> 8) & 0xFFu);
        if (plausible_temp(value)) {
            out->hotspot_available = true;
            out->hotspot_temp_c = value;
        }
    } else {
        mmio_failed = true;
    }

    if (mmio_failed) {
        /* Cerrar el mapeo y no volver a tocar MMIO durante un tiempo. Si el
         * dispositivo está realmente perdido, el daemon llamará a
         * gpu_temps_close() y no habrá más accesos. */
        fill_metadata(ctx, out);
        out->status = GPU_TEMPS_MMIO_ERROR;
        close_context(ctx, NULL);
        return GPU_TEMPS_MMIO_ERROR;
    }

    out->status = GPU_TEMPS_OK;
    if (out->vram_available || out->hotspot_available) {
        out->status = GPU_TEMPS_OK;
    } else {
        out->status = GPU_TEMPS_INVALID;
    }
    return out->status;
}

bool gpu_temps_read(const char* pci_bus_id, gpu_temps_t* out) {
    gpu_temps_status_t status = gpu_temps_read_ex(pci_bus_id, out);
    if (status == GPU_TEMPS_OK && out) {
        return out->vram_available || out->hotspot_available;
    }
    return false;
}

bool gpu_temps_close(const char* pci_bus_id) {
    if (!pci_bus_id) return false;

    GpuContext* ctx = find_context(pci_bus_id);
    if (!ctx) return true;

    bool munmap_ok = true;
    close_context(ctx, &munmap_ok);
    return munmap_ok;
}

bool gpu_temps_close_all(void) {
    bool ok = true;
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].used) {
            bool local_ok = true;
            close_context(&g_contexts[i], &local_ok);
            if (!local_ok) ok = false;
        }
    }
    return ok;
}

void gpu_temps_reset(const char* pci_bus_id) {
    if (!pci_bus_id) return;
    GpuContext* ctx = find_context(pci_bus_id);
    if (ctx) close_context(ctx, NULL);
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
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE* fp = fopen(tmp, "w");
    if (!fp) return false;

    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (!bus_ids[i] || !bus_ids[i][0]) continue;
        if (fprintf(fp, "%s %d %d\n", bus_ids[i], vram[i], hotspot[i]) < 0) {
            ok = false;
            break;
        }
    }

    if (fflush(fp) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }

    if (chmod(path, 0644) != 0) return false;
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

/* ─────────────────────────────────────────────────────────────────────────────
 * Archivo de estado de ventiladores
 * ───────────────────────────────────────────────────────────────────────────── */
bool gpu_fans_status_write(const char* path,
                           const gpu_fans_entry_t* entries,
                           int count) {
    if (!path || !entries || count < 0) return false;

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE* fp = fopen(tmp, "w");
    if (!fp) return false;

    bool ok = true;
    for (int i = 0; i < count; i++) {
        const gpu_fans_entry_t* e = &entries[i];
        if (!e->bus_id || !e->bus_id[0]) continue;

        int fans = e->count;
        if (fans < 0) fans = 0;
        if (fans > GPU_TEMPS_MAX_FANS) fans = GPU_TEMPS_MAX_FANS;

        if (fprintf(fp, "%s %d", e->bus_id, fans) < 0) { ok = false; break; }
        for (int f = 0; f < fans; f++) {
            if (fprintf(fp, " %d", e->speed[f]) < 0) { ok = false; break; }
        }
        if (!ok || fputc('\n', fp) == EOF) { ok = false; break; }
    }

    if (fflush(fp) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }

    if (chmod(path, 0644) != 0) return false;
    return true;
}

bool gpu_fans_status_read(const char* path,
                          const char* pci_bus_id,
                          gpu_fans_t* out) {
    if (!path || !pci_bus_id || !out) return false;

    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char line[512];
    bool found = false;

    while (fgets(line, sizeof(line), fp)) {
        char bus[64];
        int n = 0;

        if (sscanf(line, "%63s %d", bus, &n) != 2) continue;
        if (strcmp(bus, pci_bus_id) != 0) continue;

        if (n < 0) n = 0;
        if (n > GPU_TEMPS_MAX_FANS) n = GPU_TEMPS_MAX_FANS;

        out->count = n;
        for (int i = 0; i < GPU_TEMPS_MAX_FANS; i++) out->speed[i] = -1;

        char* p = line;
        p = strchr(p, ' ');           /* después del bus */
        if (!p) break;
        p++;
        p = strchr(p, ' ');           /* después del contador */
        if (!p) break;
        p++;

        for (int i = 0; i < n; i++) {
            char* end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v < -1) v = -1;
            if (v > 100) v = 100;
            out->speed[i] = (int)v;
            p = end;
        }

        found = true;
        break;
    }

    fclose(fp);
    return found;
}
