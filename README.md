# GPU Fan Curve Controller

Control de ventiladores para GPUs NVIDIA en Linux con **lectura directa de la temperatura de VRAM y hotspot desde los registros PCI** de la tarjeta, curva de ventilador configurable, bloqueo térmico crítico y pruebas manuales de ventilador.

> Probado en **RTX 3090 (GA102)** y **RTX 3080 (GA102)**. La lectura PCI de VRAM/hotspot funciona en GA102 y GA104. El control de ventilador usa NVML y depende del driver y del modelo concreto de la tarjeta.

---

## Características

- **VRAM y hotspot reales** leídos de los registros MMIO de la GPU, no del sensor "memory" de NVML.
- **Curva de ventilador** lineal entre `min_speed` y 100 % según la temperatura.
- **Bloqueo crítico con latch**: al alcanzar `critical_temp` fuerza 100 % y no baja hasta 5 °C por debajo.
- **Dominancia de power limit**: reafirma el límite cada 10 s.
- **Persistence mode** opcional.
- **`setfan`**: fija todos los ventiladores a un porcentaje concreto durante una prueba, con resumen de temperaturas.
- **`gpuhelp`**: lista todos los comandos con una descripción corta.
- **`updategpu`**: actualiza desde GitHub conservando la configuración.
- **No modifica** drivers, CUDA, Docker ni componentes de Vast.ai. No hace unbind/bind PCI, no carga módulos y no escribe en registros MMIO: solo lee.

---

## GPUs soportadas

| Arquitectura | Modelos | Offset VRAM |
|---|---|---|
| GA102 | RTX 3080, 3080 Ti, 3090 | `0xE2A8` |
| GA104 | RTX 3070, 3070 LHR | `0xEE50` |

Hotspot: `0x2046C` en GA102/GA104.

En otras arquitecturas el programa sigue funcionando para el control de ventilador, pero las temperaturas de VRAM/hotspot pueden aparecer como `N/A` si el offset no coincide.

---

## Requisitos

- Linux con driver NVIDIA propietario y `libnvidia-ml.so.1`.
- `gcc`/`g++` **solo durante la instalación** (no hace falta CUDA ni `nvcc` ni `nvml.h`).
- Acceso root para `/dev/mem` y para el servicio systemd.
- Opcional: `iomem=relaxed` en el kernel si `mmap` de `/dev/mem` falla.

Comprobación rápida:

```bash
nvidia-smi
ls -l /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1
```

---

## Instalación desde GitHub

```bash
curl -fsSL https://github.com/todocryptomining-sudo/gpu-set-fan-power/archive/refs/heads/main.tar.gz -o /tmp/gpu-fan.tar.gz
tar -xzf /tmp/gpu-fan.tar.gz -C /tmp
cd /tmp/gpu-set-fan-power-main/releases/v1.5
chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

El instalador compila los binarios, crea el servicio `set-fan-curve`, instala los comandos y deja la configuración en `~/set-fan-config`.

También puedes clonar el repositorio:

```bash
git clone https://github.com/todocryptomining-sudo/gpu-set-fan-power.git
cd gpu-set-fan-power/releases/v1.5
sudo ./install.sh
```

---

## Comandos

| Comando | Descripción |
|---|---|
| `vergpu` | Monitor en vivo: core, VRAM, hotspot, potencia, clocks. |
| `vergpu -1` | Una sola captura y salir. |
| `gpustatus` | Estado rápido del servicio y las GPUs. |
| `setfan 70` | Fija todos los ventiladores al 70 % hasta cancelar. |
| `setfan max` | Fija los ventiladores al 100 %. |
| `setfan auto` | Cancela la prueba y vuelve a la curva. |
| `setfan status` | Muestra si hay una prueba activa. |
| `editargpu` | Edita `~/set-fan-config`. |
| `restartgpu` | Reinicia el servicio y aplica los cambios. |
| `gpusave [nombre]` | Guarda una copia de la configuración. |
| `gpuhelp` | Lista todos los comandos. |
| `sudo updategpu` | Actualiza desde GitHub conservando la configuración. |
| `updategpu --check` | Muestra qué versión hay disponible. |

Durante `setfan` se muestra cada 2 s:

```
────────────────────────────────────────────────────────────────
[22:10:05] Prueba manual fijada al 70 %
  GPU 0  Core  72 °C  VRAM  68 °C  Hotspot  79 °C  Fan  70 %    220.4 W
```

Al pulsar `Ctrl+C` se borra el control manual y se muestra un resumen (mínimo / máximo / media). Si se cierra la sesión SSH, el daemon detecta que el proceso murió y vuelve solo a la curva.

**El bloqueo crítico siempre tiene prioridad** sobre `setfan`.

---

## Configuración

`~/set-fan-config` se crea en la instalación y **se conserva al actualizar**. Cada clave lleva su comentario; las claves nuevas se añaden al final.

```ini
max_temp=85
min_speed=40
critical_temp=93
use_vram_temp=1
use_hotspot_temp=0
safety_all_sensors=0
power_limit=0
persistence=1
```

| Clave | Significado |
|---|---|
| `max_temp` | Temperatura objetivo de la curva (50-95 °C). |
| `min_speed` | Velocidad mínima por debajo del inicio de rampa (0-80 %). |
| `critical_temp` | Umbral del bloqueo crítico; se ajusta solo si queda por debajo de `max_temp`. |
| `use_vram_temp` | Incluye la VRAM leída por PCI en el cálculo de la curva. |
| `use_hotspot_temp` | Incluye el hotspot leído por PCI en la curva. |
| `safety_all_sensors` | `0` = el bloqueo usa los mismos sensores que la curva; `1` = core + VRAM + hotspot. |
| `power_limit` | Límite de potencia en vatios; `0` = no tocarlo. |
| `persistence` | `1` activa persistence mode. |

Después de editar:

```bash
restartgpu
```

---

## Cómo se leen VRAM y hotspot

La técnica es la misma que usan herramientas como `gddr6.c` / `nvml_direct_access.c`. El programa:

1. Lee la dirección base de BAR0 desde `/sys/bus/pci/devices/<bdf>/resource`.
2. Abre `/dev/mem` con `O_RDONLY`.
3. Mapea una página con `mmap(..., PROT_READ, ...)`.
4. Lee un registro de 32 bits y aplica la fórmula:
   - VRAM: `(raw & 0xFFF) / 0x20`
   - Hotspot: `(raw >> 8) & 0xFF`

**Solo lectura.** No se escribe ningún registro MMIO, no se hace unbind del dispositivo y no se tocan los permisos de `/dev/nvidia*`.

El daemon publica las lecturas en `/run/set-fan-curve/temps` para que `vergpu` y `gpustatus` funcionen sin root.

---

## Seguridad

- El servicio corre como root porque necesita `/dev/mem` y NVML.
- El bloqueo crítico tiene prioridad sobre la curva y sobre `setfan`.
- Si el proceso `setfan` muere (Ctrl+C, cierre de SSH, kill), el daemon lo detecta y vuelve a la curva.
- Al detener el servicio se restaura el control automático de ventiladores (`nvmlDeviceSetDefaultFanSpeed_v2`).
- No se modifican drivers, CUDA, Docker ni Vast.ai.

---

## Actualización

```bash
updategpu --check   # ver la versión disponible
sudo updategpu      # instalar la última
```

`updategpu` descarga la rama `main`, localiza la carpeta `releases/v*` más reciente y ejecuta su instalador. No necesita `git`; solo `curl` y `tar`. La configuración se conserva.

---

## Solución de problemas

### VRAM aparece `N/A`

1. `systemctl status set-fan-curve`
2. `journalctl -u set-fan-curve -n 50 --no-pager`
3. `ls -l /dev/mem`
4. Si `mmap` falla, añade `iomem=relaxed` al kernel y reinicia.

### `nvidia-smi`: `Unable to determine the device handle ... Unknown Error`

No lo causa la configuración. La GPU dejó de responder en el bus PCI. Reinicia y revisa el arranque anterior:

```bash
sudo journalctl -k -b -1 --no-pager | grep -iE "xid|nvrm|fallen|pcie|AER"
sudo journalctl -u set-fan-curve -b -1 --no-pager | tail -80
```

- **Xid 79** (`GPU has fallen off the bus`): alimentación, riser/PCIe, conectores o temperatura.
- **Xid 62 / 48**: fallo interno del driver/GPU; requiere reinicio.
- Sin Xid y hotspot > 100 °C: refrigeración de VRAM/pads degradada.

El control de ventiladores no puede hacer que una GPU desaparezca del bus PCIe.

### El ventilador marca 0 % en una RTX 3080

Comprueba primero si los ventiladores giran físicamente. Con carga alta, si realmente estuvieran parados la temperatura subiría rápido. Después:

```bash
setfan max
journalctl -u set-fan-curve -n 120 --no-pager | grep -iE "fan|error|warn"
nvidia-settings -a "[gpu:0]/GPUFanControlState=1" -a "[fan:0]/GPUTargetFanSpeed=70"
```

Si `setfan max` acelera los ventiladores, el control funciona y el 0 % es una lectura de NVML. Si no cambia nada, abre un issue con el modelo exacto, la versión del driver y el journal.

### El servicio no arranca

```bash
journalctl -u set-fan-curve -n 60 --no-pager
```

El daemon reintenta inicializar NVML durante 60 s si el driver aún no está listo.

---

## Estructura del repositorio

```
.
├── README.md
├── LICENSE
├── CHANGELOG.md
└── releases/
    └── v1.5/
        ├── set_fan_curve.cpp    # daemon
        ├── vergpu.cpp           # monitor
        ├── gpu_temps.c/.h       # lectura PCI
        ├── nvml_min.h           # declaraciones NVML mínimas
        ├── install.sh
        ├── uninstall.sh
        ├── setfan
        ├── gpuhelp
        ├── updategpu
        └── ...
```

Cada versión vive en su propia carpeta `releases/vX.Y`. `updategpu` elige la más reciente.

---

## Licencia y aviso

MIT. Consulta [LICENSE](LICENSE).

Este software lee registros de hardware y controla ventiladores mediante APIs no oficiales. Úsalo bajo tu responsabilidad. No todos los modelos o drivers aceptan el control manual; verifica siempre las temperaturas después de instalarlo. Los autores no se responsabilizan de daños derivados del uso.
