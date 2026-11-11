# GPU Fan Curve Controller v1.8

Control de ventiladores para GPUs NVIDIA en Linux con **lectura directa de la temperatura de VRAM y hotspot desde los registros PCI** de la tarjeta, curva de ventilador configurable, límite de velocidad, bloqueo térmico crítico y pruebas manuales de ventilador.

> Probado en **RTX 3090 (GA102)** y **RTX 3080 (GA102)**. La lectura PCI de VRAM/hotspot funciona en GA102 y GA104. El control de ventilador usa NVML y depende del driver y del modelo concreto de la tarjeta.

---

## Características

- **VRAM y hotspot reales** leídos de los registros MMIO de la GPU, no del sensor "memory" de NVML.
- **Curva de ventilador independiente de `min_speed`**: de 0 a `max_speed` entre `max_temp-40` y `max_temp`, con `min_speed` como suelo.
- **`max_speed`**: limita la velocidad normal de los ventiladores. El bloqueo crítico lo ignora y fuerza 100 %.
- **Estado por ventilador**: Fan 0, Fan 1, … se leen y escriben por separado; se comprueba cada retorno NVML.
- **Alerta de ventilador a 0 %**: NVML informa porcentaje; 0 % no demuestra 0 RPM.
- **Escrituras reducidas**: intervalo mínimo, umbral, histéresis y re-sincronización tras errores.
- **Bloqueo crítico con latch**: al alcanzar `critical_temp` (comparación `>=`) fuerza 100 % y no baja hasta 5 °C por debajo. Es independiente de `max_temp` y admite hasta 110 °C para hotspots GDDR6X.
- **Validación de configuración**: `set_fan_curve --check-config` detecta typos, claves desconocidas, valores vacíos, no numéricos y fuera de rango. `restartgpu` y `editargpu` validan antes de aplicar.
- **Power limit configurable**: `startup`, `monitor` o `enforce`; `monitor` no escribe y se recomienda en Vast.ai.
- **Estado por GPU**: `NORMAL`, `SENSOR_ERROR`, `NVML_ERROR`, `GPU_LOST`, `RECOVERING` y `DISABLED`.
- **Política NVML manual**: se fija y reafirma cada 60 s; se restaura `AUTO` al detener el servicio.
- **Persistence mode** opcional.
- **`setfan`**: fija todos los ventiladores a un porcentaje concreto durante una prueba, con resumen de temperaturas.
- **`gpuhelp`**: lista todos los comandos con una descripción corta.
- **`updategpu`**: actualiza desde GitHub conservando la configuración.
- **No modifica** drivers, CUDA, Docker ni componentes de Vast.ai. No hace unbind/bind PCI, no carga módulos y no escribe registros MMIO: solo lee.
- En GPU perdida no ejecuta `nvidia-smi --gpu-reset`, `reboot`, `systemctl restart docker` ni `systemctl restart vastai`.

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
cd /tmp/gpu-set-fan-power-main/releases/v1.7
chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

El instalador compila los binarios, crea el servicio `set-fan-curve`, instala los comandos y deja la configuración en `~/set-fan-config`.

También puedes clonar el repositorio:

```bash
git clone https://github.com/todocryptomining-sudo/gpu-set-fan-power.git
cd gpu-set-fan-power/releases/v1.7
sudo ./install.sh
```

---

## Comandos

| Comando | Descripción |
|---|---|
| `vergpu` | Monitor en vivo: core, VRAM, hotspot y todos los ventiladores. |
| `vergpu -1` | Una sola captura y salir. |
| `gpustatus` | Estado rápido del servicio, las GPUs y los ventiladores. |
| `setfan 70` | Fija todos los ventiladores al 70 % hasta cancelar. |
| `setfan max` | Fija los ventiladores al 100 %. |
| `setfan auto` | Cancela la prueba y vuelve a la curva. |
| `setfan status` | Muestra la prueba activa y las velocidades actuales. |
| `editargpu` | Edita `~/set-fan-config`, muestra el diff, valida y confirma antes de reiniciar. |
| `restartgpu` | Valida la configuración y reinicia; no aplica archivos con errores. |
| `set_fan_curve --check-config` | Comprueba la configuración sin root y sin tocar el servicio. |
| `gpusave [nombre]` | Guarda una copia de la configuración. |
| `gpuhelp` | Lista todos los comandos. |
| `sudo updategpu` | Actualiza desde GitHub conservando la configuración. |
| `updategpu --check` | Muestra qué versión hay disponible. |

Durante `setfan` se muestra cada 2 s:

```
────────────────────────────────────────────────────────────────
[22:10:05] Prueba manual fijada al 70 %
  GPU 0  Core  72 °C  VRAM  68 °C  Hotspot  79 °C  Fan0 70%  Fan1 70%    220.4 W
```

Al pulsar `Ctrl+C` se borra el control manual y se muestra un resumen (mínimo / máximo / media). Si se cierra la sesión SSH, el daemon detecta que el proceso murió y vuelve solo a la curva.

**El bloqueo crítico siempre tiene prioridad** sobre `setfan` y sobre `max_speed`.

---

## Configuración

`~/set-fan-config` se crea en la instalación y **se conserva al actualizar**. Cada clave lleva su comentario; las claves nuevas se añaden al final.

```ini
max_temp=85
min_speed=40
max_speed=100
critical_temp=93
use_vram_temp=1
use_hotspot_temp=0
safety_all_sensors=0
power_limit=0
persistence=1
```

| Clave | Significado |
|---|---|
| `max_temp` | Temperatura objetivo de la curva (50-100 °C). La curva llega a `max_speed` aquí. |
| `min_speed` | Suelo de velocidad (0-80 %). Se aplica después de la curva. |
| `max_speed` | Tope normal (20-100 %). El bloqueo crítico lo ignora. |
| `critical_temp` | Umbral del bloqueo crítico (50-110 °C). Puede ser menor o mayor que `max_temp`. |
| `use_vram_temp` | Incluye la VRAM leída por PCI en el cálculo de la curva. |
| `use_hotspot_temp` | Incluye el hotspot leído por PCI en la curva (y por tanto en el bloqueo si `safety_all_sensors=0`). |
| `safety_all_sensors` | `0` = el bloqueo usa los mismos sensores que la curva; `1` = core + VRAM + hotspot. |
| `power_limit` | Límite de potencia en vatios; `0` = no tocarlo. |
| `persistence` | `1` activa persistence mode. |

### Cálculo de la curva

```
ramp_start = max_temp - 40

curva = 0 %                     si temp <= ramp_start
curva = 0 → max_speed           si ramp_start < temp < max_temp
curva = max_speed               si temp >= max_temp

velocidad = max(min_speed, curva)      (nunca por encima de max_speed)
```

Ejemplo con `max_temp=100`, `min_speed=25`, `max_speed=100`, `critical_temp=95`:

```
60 °C o menos → 25 %
84 °C         → 60 %
95 °C         → 100 % (bloqueo crítico)
```

### Hotspot a 105 °C y bloqueo al 100 %

El bloqueo dispara con `>= critical_temp`. El hotspot solo puede dispararlo si
forma parte de los sensores del bloqueo:

| `use_hotspot_temp` | `safety_all_sensors` | ¿El hotspot fuerza 100 %? |
|---|---|---|
| 0 | 0 | No |
| 1 | 0 | Sí |
| cualquiera | 1 | Sí |

Para permitir picos normales de 105 °C: pon `use_hotspot_temp=0` y
`safety_all_sensors=0`, o sube `critical_temp` a 106-110.

### Validar y aplicar

```bash
set_fan_curve --check-config ~/set-fan-config   # no requiere root
editargpu      # diff + validación + confirmación
restartgpu     # valida y reinicia; aborta si hay errores
```

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

El daemon publica las lecturas en `/run/set-fan-curve/temps` y las velocidades por ventilador en `/run/set-fan-curve/fans` para que `vergpu` y `gpustatus` funcionen sin root.

---

## Seguridad

- El servicio corre como root porque necesita `/dev/mem` y NVML.
- El bloqueo crítico tiene prioridad sobre la curva, sobre `max_speed` y sobre `setfan`.
- Si el proceso `setfan` muere (Ctrl+C, cierre de SSH, kill), el daemon lo detecta y vuelve a la curva.
- Al detener el servicio se restaura el control automático de ventiladores (`nvmlDeviceSetDefaultFanSpeed_v2`) y la política `AUTO`.
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

### Un ventilador aparece al 0 % y el otro funciona

El daemon lo detecta tras 10 s y escribe una alerta:

```bash
journalctl -u set-fan-curve -n 80 --no-pager | grep -i fan
cat /run/set-fan-curve/fans
```

Comprueba el conector/cable del ventilador, el cable de alimentación de la tarjeta y prueba `setfan 100`. Si con `setfan max` el ventilador sigue a 0 %, el problema es hardware o driver, no la curva.

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
    └── v1.7/
        ├── set_fan_curve.cpp    # daemon + validación
        ├── vergpu.cpp           # monitor
        ├── gpu_temps.c/.h       # lectura PCI + estado de fans
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
