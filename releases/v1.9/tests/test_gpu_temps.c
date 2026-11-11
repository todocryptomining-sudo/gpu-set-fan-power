// Tests de gpu_temps.c (lectura PCI/MMIO) sin hardware real.
//
// Se compila con -DGPU_TEMPS_TEST, que permite apuntar la biblioteca a un
// sysfs falso y a un "mem" falso mediante GPU_TEMPS_SYSFS_ROOT y
// GPU_TEMPS_MEM_PATH. El "mem" falso es un archivo regular disperso: mmap()
// funciona sobre él con el mismo offset físico que usaría /dev/mem.
//
// Compilar y ejecutar:  make test
//
// Lo que se comprueba:
//   - Tabla explícita de offsets (GA102 y GA104) y metadatos publicados.
//   - Rechazo de vendor no NVIDIA y de device NVIDIA desconocido SIN abrir mem.
//   - Validación de BDF (longitud, formato, función 0).
//   - Validación de BAR0 (existe, es memoria, start/end coherentes, tamaño y
//     offsets dentro del rango).
//   - Ausencia de /dev/mem, error de mmap y plausibilidad de temperaturas.
//   - Cierre de mapeos y reset de contexto.
//   - Formato de los archivos de estado de temps y fans.

#define _GNU_SOURCE

#include "../gpu_temps.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Dirección BAR0 falsa. Se elige baja para que el archivo disperso sea
 * pequeño; la biblioteca no exige ninguna dirección concreta. */
#define FAKE_BAR0_START 0x00100000ULL
#define FAKE_BAR0_SIZE  0x00100000ULL   /* 1 MiB */
#define FAKE_MEM_SIZE   (FAKE_BAR0_START + FAKE_BAR0_SIZE)

#define GA102_VRAM_OFFSET    0x0000E2A8u
#define GA102_HOTSPOT_OFFSET 0x0002046Cu
#define GA104_VRAM_OFFSET    0x0000EE50u

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL línea %d: %s\n", __LINE__, #cond);         \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_INT(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        g_checks++;                                                          \
        if (a_ != e_) {                                                      \
            fprintf(stderr, "FAIL línea %d: %s = %lld, esperado %lld\n",     \
                    __LINE__, #actual, a_, e_);                              \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static char g_root[PATH_MAX];
static char g_mem[PATH_MAX];
static char g_state[PATH_MAX];
static char g_fans[PATH_MAX];
static char g_missing_mem[PATH_MAX];

/* ─────────────────────────────────────────────────────────────────────────────
 * Utilidades de sistema de archivos
 * ───────────────────────────────────────────────────────────────────────────── */

static bool write_text(const char* path, const char* text) {
    FILE* fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "no se pudo crear %s: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = (fputs(text, fp) >= 0);
    if (fclose(fp) != 0) ok = false;
    return ok;
}

static bool make_dir(const char* path) {
    if (mkdir(path, 0755) == 0) return true;
    if (errno != EEXIST) return false;

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void device_dir(const char* bdf, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", g_root, bdf);
}

/* Crea el directorio del dispositivo y, opcionalmente, vendor/device/resource.
 * flags == 0 significa "sin archivo resource". */
static bool setup_device(const char* bdf, unsigned int vendor,
                         unsigned int device, bool with_resource,
                         unsigned long long bar_start,
                         unsigned long long bar_end,
                         unsigned long long flags) {
    char dir[PATH_MAX + 32];
    char path[PATH_MAX + 96];
    device_dir(bdf, dir, sizeof(dir));
    if (!make_dir(dir)) return false;

    snprintf(path, sizeof(path), "%s/vendor", dir);
    char text[64];
    snprintf(text, sizeof(text), "0x%04x\n", vendor);
    if (!write_text(path, text)) return false;

    snprintf(path, sizeof(path), "%s/device", dir);
    snprintf(text, sizeof(text), "0x%04x\n", device);
    if (!write_text(path, text)) return false;

    if (!with_resource) return true;

    snprintf(path, sizeof(path), "%s/resource", dir);
    snprintf(text, sizeof(text), "0x%016llx 0x%016llx 0x%016llx\n",
             bar_start, bar_end, flags);
    return write_text(path, text);
}

static bool write_raw32(unsigned long long absolute_offset, unsigned int value) {
    int fd = open(g_mem, O_RDWR);
    if (fd < 0) return false;

    ssize_t written = pwrite(fd, &value, sizeof(value), (off_t)absolute_offset);
    int saved = errno;
    close(fd);
    errno = saved;
    return written == (ssize_t)sizeof(value);
}

static bool set_mem_path(const char* path) {
    return setenv("GPU_TEMPS_MEM_PATH", path, 1) == 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Casos correctos
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_rtx3090_valid(void) {
    const char* bdf = "0000:01:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));
    CHECK(set_mem_path(g_mem));

    /* VRAM: (raw & 0xFFF) / 0x20 = 70  -> raw = 70 * 32 = 0x8C0. */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 70u * 0x20u));
    /* Hotspot: (raw >> 8) & 0xFF = 82  -> raw = 82 << 8 = 0x5200. */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 82u << 8));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    gpu_temps_status_t status = gpu_temps_read_ex(bdf, &temps);

    CHECK_INT(status, GPU_TEMPS_OK);
    CHECK(temps.vram_available);
    CHECK(temps.hotspot_available);
    CHECK_INT(temps.vram_temp_c, 70);
    CHECK_INT(temps.hotspot_temp_c, 82);
    CHECK_INT(temps.vendor_id, 0x10de);
    CHECK_INT(temps.device_id, 0x2204);
    CHECK_INT(temps.vram_offset, GA102_VRAM_OFFSET);
    CHECK_INT(temps.hotspot_offset, GA102_HOTSPOT_OFFSET);
    CHECK(temps.profile_known);
    CHECK_INT(temps.status, GPU_TEMPS_OK);

    CHECK(gpu_temps_read(bdf, &temps));
    CHECK_INT(temps.vram_temp_c, 70);

    CHECK(gpu_temps_close(bdf));
    CHECK(gpu_temps_close(bdf));   /* cerrar dos veces no falla */
}

static void test_rtx3070_valid(void) {
    const char* bdf = "0000:02:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2484, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));

    /* GA104 usa un offset de VRAM distinto (0xEE50). */
    CHECK(write_raw32(FAKE_BAR0_START + GA104_VRAM_OFFSET, 65u * 0x20u));
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 79u << 8));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    gpu_temps_status_t status = gpu_temps_read_ex(bdf, &temps);

    CHECK_INT(status, GPU_TEMPS_OK);
    CHECK_INT(temps.device_id, 0x2484);
    CHECK_INT(temps.vram_offset, GA104_VRAM_OFFSET);
    CHECK_INT(temps.vram_temp_c, 65);
    CHECK_INT(temps.hotspot_temp_c, 79);

    /* El offset de GA102 no debe leerse en GA104: se escribe un valor basura
     * allí y la lectura debe seguir dando 65. */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 20u * 0x20u));
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_OK);
    CHECK_INT(temps.vram_temp_c, 65);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Rechazo de dispositivos no verificados
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_unknown_vendor(void) {
    const char* bdf = "0000:03:00.0";
    CHECK(setup_device(bdf, 0x1002, 0x73bf, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_UNSUPPORTED);
    CHECK(!temps.profile_known);
}

static void test_unknown_nvidia_device_does_not_open_mem(void) {
    const char* bdf = "0000:04:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2684, true,   /* RTX 4090: no verificada */
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));

    /* Si la biblioteca intentase abrir /dev/mem devolvería NO_DEVICE. Que
     * devuelva UNSUPPORTED demuestra que no se intentó el acceso MMIO. */
    CHECK(set_mem_path(g_missing_mem));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_UNSUPPORTED);
    CHECK(!temps.profile_known);
    CHECK_INT(temps.device_id, 0x2684);

    CHECK(set_mem_path(g_mem));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * BDF inválidos
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_invalid_bdf(void) {
    gpu_temps_t temps;

    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex("0000:01:00.1", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("0000:01:00", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("000:01:00.0", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("0000:01:00.0/../x", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("eth0", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("", &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex(NULL, &temps), GPU_TEMPS_INVALID);
    CHECK_INT(gpu_temps_read_ex("0000:01:00.0", NULL), GPU_TEMPS_INVALID);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Validación de BAR0
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_bar0_validation(void) {
    gpu_temps_t temps;

    /* Sin archivo resource. */
    {
        const char* bdf = "0000:05:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, false, 0, 0, 0));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
        CHECK(!temps.profile_known || temps.status == GPU_TEMPS_INVALID);
    }

    /* BAR0 que no es memoria (flags sin IORESOURCE_MEM). */
    {
        const char* bdf = "0000:06:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                           FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                           0x0000000000000000ULL));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    }

    /* start == 0. */
    {
        const char* bdf = "0000:07:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                           0, 0xFFF, 0x0000000000040200ULL));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    }

    /* end < start. */
    {
        const char* bdf = "0000:08:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                           0x00100000, 0x000FFFFF, 0x0000000000040200ULL));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    }

    /* BAR0 de 4 KiB: los offsets verificados no caben. */
    {
        const char* bdf = "0000:09:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                           FAKE_BAR0_START, FAKE_BAR0_START + 0xFFF,
                           0x0000000000040200ULL));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    }

    /* BAR0 mayor de 16 GiB: se rechaza por rango. */
    {
        const char* bdf = "0000:0a:00.0";
        CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                           0x00100000,
                           0x00100000 + 0x500000000ULL - 1,  /* 20 GiB */
                           0x0000000000040200ULL));
        CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * /dev/mem ausente y error de mmap
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_missing_mem(void) {
    const char* bdf = "0000:0b:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));

    CHECK(set_mem_path(g_missing_mem));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_NO_DEVICE);
    CHECK(!temps.vram_available);
    CHECK(!temps.hotspot_available);

    CHECK(set_mem_path(g_mem));
}

static void test_mmap_failure(void) {
    const char* bdf = "0000:0c:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));

    /* Abrir un directorio con O_RDONLY funciona, pero mmap() sobre él falla:
     * ejercita la ruta de error MMIO sin provocar SIGBUS. */
    char dir[PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s/mmap-dir", g_root);
    CHECK(make_dir(dir));
    CHECK(set_mem_path(dir));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_MMIO_ERROR);
    CHECK(!temps.vram_available);
    CHECK(!temps.hotspot_available);

    /* Tras el fallo el contexto se cierra: con mem válido vuelve a funcionar. */
    CHECK(set_mem_path(g_mem));
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_OK);
    CHECK(temps.vram_available);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Plausibilidad y estado
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_implausible_temperatures(void) {
    const char* bdf = "0000:0d:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));
    CHECK(set_mem_path(g_mem));

    /* 0 °C y 127 °C quedan fuera del rango plausible 1..125. */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 0u));
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 0u));

    gpu_temps_t temps;
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);
    CHECK(!temps.vram_available);
    CHECK(!temps.hotspot_available);

    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 0xFFFu));  /* 127 */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 126u << 8));
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_INVALID);

    /* Un valor válido vuelve a publicarse sin reconstruir el contexto. */
    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 60u * 0x20u));
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 70u << 8));
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_OK);
    CHECK_INT(temps.vram_temp_c, 60);
    CHECK_INT(temps.hotspot_temp_c, 70);
}

static void test_reset_context(void) {
    const char* bdf = "0000:0e:00.0";
    CHECK(setup_device(bdf, 0x10de, 0x2204, true,
                       FAKE_BAR0_START, FAKE_BAR0_START + FAKE_BAR0_SIZE - 1,
                       0x0000000000040200ULL));
    CHECK(set_mem_path(g_mem));
    CHECK(write_raw32(FAKE_BAR0_START + GA102_VRAM_OFFSET, 55u * 0x20u));
    CHECK(write_raw32(FAKE_BAR0_START + GA102_HOTSPOT_OFFSET, 66u << 8));

    gpu_temps_t temps;
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_OK);
    CHECK_INT(temps.vram_temp_c, 55);

    gpu_temps_reset(bdf);

    /* El contexto se reconstruye y vuelve a leer el mismo valor. */
    memset(&temps, 0, sizeof(temps));
    CHECK_INT(gpu_temps_read_ex(bdf, &temps), GPU_TEMPS_OK);
    CHECK_INT(temps.vram_temp_c, 55);
    CHECK_INT(temps.hotspot_temp_c, 66);

    gpu_temps_reset("0000:ff:00.0");   /* inexistente: no debe fallar */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Archivos de estado
 * ───────────────────────────────────────────────────────────────────────────── */

static void test_status_files(void) {
    const char* bdf_a = "0000:01:00.0";
    const char* bdf_b = "0000:02:00.0";
    const char* buses[2] = { bdf_a, bdf_b };
    const int vram[2] = { 61, -1 };
    const int hotspot[2] = { 72, 83 };

    CHECK(gpu_temps_status_write(g_state, buses, vram, hotspot, 2));

    int v = 0, h = 0;
    CHECK(gpu_temps_status_read(g_state, bdf_a, &v, &h));
    CHECK_INT(v, 61);
    CHECK_INT(h, 72);

    v = 0;
    h = 0;
    CHECK(gpu_temps_status_read(g_state, bdf_b, &v, &h));
    CHECK_INT(v, -1);
    CHECK_INT(h, 83);

    CHECK(!gpu_temps_status_read(g_state, "0000:99:00.0", &v, &h));
    CHECK(!gpu_temps_status_write("/ruta/que/no/existe/temps",
                                  buses, vram, hotspot, 2));

    gpu_fans_entry_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].bus_id = bdf_a;
    entries[0].count = 2;
    entries[0].speed[0] = 30;
    entries[0].speed[1] = -1;
    entries[1].bus_id = bdf_b;
    entries[1].count = 1;
    entries[1].speed[0] = 100;

    CHECK(gpu_fans_status_write(g_fans, entries, 2));

    gpu_fans_t fans;
    memset(&fans, 0, sizeof(fans));
    CHECK(gpu_fans_status_read(g_fans, bdf_a, &fans));
    CHECK_INT(fans.count, 2);
    CHECK_INT(fans.speed[0], 30);
    CHECK_INT(fans.speed[1], -1);

    memset(&fans, 0, sizeof(fans));
    CHECK(gpu_fans_status_read(g_fans, bdf_b, &fans));
    CHECK_INT(fans.count, 1);
    CHECK_INT(fans.speed[0], 100);

    memset(&fans, 0, sizeof(fans));
    CHECK(!gpu_fans_status_read(g_fans, "0000:99:00.0", &fans));
}

static void test_close_all(void) {
    CHECK(gpu_temps_close_all());
    CHECK(gpu_temps_close_all());
}

/* ─────────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────────── */

static void cleanup_temp_dir(void) {
    static const char* bdfs[] = {
        "0000:01:00.0", "0000:02:00.0", "0000:03:00.0",
        "0000:04:00.0", "0000:05:00.0", "0000:06:00.0",
        "0000:07:00.0", "0000:08:00.0", "0000:09:00.0",
        "0000:0a:00.0", "0000:0b:00.0", "0000:0c:00.0",
        "0000:0d:00.0", "0000:0e:00.0",
    };
    char dir[PATH_MAX + 32];
    char file[PATH_MAX + 64];

    for (size_t i = 0; i < sizeof(bdfs) / sizeof(bdfs[0]); i++) {
        snprintf(dir, sizeof(dir), "%s/%s", g_root, bdfs[i]);
        snprintf(file, sizeof(file), "%s/vendor", dir);
        remove(file);
        snprintf(file, sizeof(file), "%s/device", dir);
        remove(file);
        snprintf(file, sizeof(file), "%s/resource", dir);
        remove(file);
        rmdir(dir);
    }

    snprintf(dir, sizeof(dir), "%s/mmap-dir", g_root);
    rmdir(dir);

    remove(g_state);
    remove(g_fans);
    remove(g_mem);
    rmdir(g_root);
}

int main(void) {
    char tmp_template[] = "/tmp/gpu-temps-test-XXXXXX";
    char* dir = mkdtemp(tmp_template);
    if (!dir) {
        fprintf(stderr, "mkdtemp falló: %s\n", strerror(errno));
        return 1;
    }

    snprintf(g_root, sizeof(g_root), "%s", dir);
    snprintf(g_mem, sizeof(g_mem), "%s/fake-mem", dir);
    snprintf(g_state, sizeof(g_state), "%s/temps", dir);
    snprintf(g_fans, sizeof(g_fans), "%s/fans", dir);
    snprintf(g_missing_mem, sizeof(g_missing_mem), "%s/no-existe-mem", dir);

    if (setenv("GPU_TEMPS_SYSFS_ROOT", g_root, 1) != 0 ||
        setenv("GPU_TEMPS_MEM_PATH", g_mem, 1) != 0) {
        fprintf(stderr, "setenv falló: %s\n", strerror(errno));
        return 1;
    }

    /* Archivo disperso del tamaño justo para cubrir BAR0. */
    int fd = open(g_mem, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "no se pudo crear %s: %s\n", g_mem, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)FAKE_MEM_SIZE) != 0) {
        fprintf(stderr, "ftruncate falló: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    test_rtx3090_valid();
    test_rtx3070_valid();
    test_unknown_vendor();
    test_unknown_nvidia_device_does_not_open_mem();
    test_invalid_bdf();
    test_bar0_validation();
    test_missing_mem();
    test_mmap_failure();
    test_implausible_temperatures();
    test_reset_context();
    test_status_files();
    test_close_all();

    printf("test_gpu_temps: %d comprobaciones, %d fallo(s)\n",
           g_checks, g_failures);

    cleanup_temp_dir();

    return g_failures == 0 ? 0 : 1;
}
