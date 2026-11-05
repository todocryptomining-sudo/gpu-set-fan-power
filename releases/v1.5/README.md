# GPU Fan Curve Controller — **v1.5**

Control de ventiladores NVIDIA con **temperaturas reales de VRAM y Hotspot leídas directamente de los registros PCI** del GPU (misma técnica que `gddr6.c` / `nvml_direct_access.c`), protección térmica bloqueante y comandos globales.

> **v1.5** añade la prueba manual de ventiladores con `setfan` (fija los ventiladores a un porcentaje concreto hasta cancelar, con resumen de temperaturas), `gpuhelp` para listar todos los comandos y `updategpu` para actualizar desde GitHub. Incluye la corrección de v1.4.9 del bloqueo crítico (`safety_all_sensors`).

---

## 🎯 Qué hace

### `set_fan_curve` — daemon maestro (root)
- **VRAM real** desde PCI: `BAR0 + 0xE2A8`, fórmula `(raw & 0xFFF) / 0x20`.
- **Hotspot real** desde PCI: `BAR0 + 0x2046C`, fórmula `(raw >> 8) & 0xFF`.
- Curva sobre `max(core, VRAM[, hotspot])` según configuración.
- **Bloqueo crítico con latch**: al llegar a `critical_temp`, fans al 100 % y no bajan hasta 5 °C por debajo.
- **Dominancia de power limit**: re-afirma el límite cada 10 s.
- Persistence mode y restauración de control automático al detenerse.
- Publica las temperaturas en `/run/set-fan-curve/temps` para `vergpu`.
- **Modo manual (`setfan`)**: mantiene los ventiladores a un % fijo mientras el
  proceso `setfan` esté vivo; el bloqueo crítico siempre tiene prioridad.

### `vergpu` — monitor en vivo
- Muestra **Core / VRAM / Hotspot** lado a lado.
- Si se ejecuta como root lee PCI directamente; si no, usa el archivo del daemon.
- Uso y memoria, potencia, relojes, ventilador y persistence.

---

## 🚀 Instalación

```bash
tar -xzf gpu-fan-curve-v1.5.tar.gz
cd v1.5

chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

> **Ojo:** la carpeta se llama `v1.5` (no `v1.4.9`). Usa exactamente la que aparece al descomprimir.

El instalador:
1. Compila en un directorio temporal (si falla, **no** toca la versión anterior).
2. Detiene y limpia la instalación previa.
3. Instala binarios y comandos.
4. Conserva `~/set-fan-config` y añade parámetros nuevos con sus comentarios.
5. Crea y arranca el servicio systemd como root.

---

## ⚙️ Configuración (`~/set-fan-config`)

| Parámetro | Descripción |
|-----------|-------------|
| `max_temp` | Temperatura objetivo (50-95 °C). A partir de ahí, 100 %. |
| `min_speed` | Velocidad mínima (0-80 %). |
| `critical_temp` | Bloqueo de seguridad (> max_temp, ≤ 100 °C). Latch hasta bajar 5 °C. |
| `use_vram_temp` | Usar VRAM real por PCI (1 = sí, 0 = no). |
| `use_hotspot_temp` | Usar Hotspot por PCI (1 = sí, 0 = no). |
| `safety_all_sensors` | Bloqueo crítico con todos los sensores (1 = core+VRAM+hotspot, 0 = mismos que la curva). |
| `power_limit` | Límite de potencia en W (0 = desactivado). |
| `persistence` | Persistence mode (0/1). |

Después de editar:

```bash
editargpu
restartgpu
```

---

## 📊 Verificación

```bash
# Temperaturas reales y estado
vergpu

# Una sola captura
vergpu -1

# Estado del servicio
systemctl status set-fan-curve

# Temperaturas que publica el daemon
cat /run/set-fan-curve/temps

# Resumen cada 30 s en el journal
journalctl -u set-fan-curve -n 50 --no-pager | grep -i temp
```

Salida esperada (ejemplo):

```
GPU 0: NVIDIA GeForce RTX 3090  [0000:01:00.0]
  Core:   44 °C   VRAM:   52 °C (PCI direct)   Hotspot:  58 °C (PCI direct)
  Fan: 47 %
  ...
```

> La VRAM debe ser **distinta** de Core. Si aparece `N/A`, revisa la sección de problemas.

---

## 🔧 Comandos

| Comando | Descripción |
|---------|-------------|
| `vergpu` | Monitor en vivo (Core / VRAM / Hotspot). |
| `editargpu` | Abre `~/set-fan-config`. |
| `restartgpu` | Reinicia el servicio para aplicar cambios. |
| `gpustatus` | Estado rápido del servicio y las GPUs. |
| `gpusave` | Guarda una copia de la configuración. |
| `setfan <0-100>` | Prueba manual: fija los ventiladores a ese % hasta cancelar. |
| `setfan max` | Prueba manual al 100 %. |
| `setfan auto` | Cancela la prueba manual y vuelve a la curva. |
| `setfan status` | Muestra si hay una prueba manual activa. |
| `gpuhelp` | Lista todos los comandos con una descripción corta. |
| `updategpu` | Descarga e instala la última versión desde GitHub. |

---

## 🧪 Prueba manual de ventiladores (`setfan`)

Sirve para encontrar qué porcentaje de ventilador mantiene las temperaturas
bajo control con tu carga real y tu ruido aceptable.

```bash
setfan 70        # todos los ventiladores al 70 % hasta cancelar
setfan 60        # probar otro porcentaje
setfan max       # 100 %
setfan status    # ver la prueba activa
setfan auto      # cancelar desde otra terminal
```

Durante la prueba se muestra cada 2 s:

```
────────────────────────────────────────────────────────────────
[22:10:05] Prueba manual fijada al 70 %
  GPU 0  Core  72 °C  VRAM  68 °C  Hotspot  79 °C  Fan  70 %    220.4 W
```

Al pulsar `Ctrl+C` se elimina el control manual y se muestra un resumen
(mínimo / máximo / media) de core, VRAM y hotspot.

**Seguridad:**

- El bloqueo crítico (`critical_temp`) **siempre** tiene prioridad: si la
  temperatura llega al límite, el daemon fuerza 100 % aunque `setfan 40`.
- Si cierras la sesión SSH o matas el proceso, el daemon comprueba el PID y
  vuelve solo a la curva. No queda el ventilador fijado.
- `setfan auto` borra el archivo de control desde cualquier terminal.

Cómo funciona: `setfan` escribe `/run/set-fan-curve/manual` con
`<velocidad> <pid>`; el daemon lo lee en cada ciclo y aplica esa velocidad a
todos los ventiladores de todas las GPUs.

---

## 🛠️ Problemas y soluciones

### `vergpu` muestra `VRAM: N/A`
1. Comprueba que el servicio está activo: `systemctl status set-fan-curve`.
2. Mira el journal: `journalctl -u set-fan-curve -n 50 --no-pager`.
3. Verifica `/dev/mem`:
   ```bash
   ls -l /dev/mem
   sudo /usr/local/bin/set_fan_curve   # prueba manual como root
   ```
4. Si el mmap falla, añade el parámetro de kernel `iomem=relaxed`:
   ```bash
   sudo nano /etc/default/grub
   # GRUB_CMDLINE_LINUX_DEFAULT="... iomem=relaxed"
   sudo update-grub && sudo reboot
   ```

### El servicio no arranca
```bash
journalctl -u set-fan-curve -n 60 --no-pager
```
El daemon reintenta inicializar NVML durante 60 s si el driver aún no está listo.

### `nvidia-smi`: `Unable to determine the device handle ... Unknown Error`

Esto **no** lo causa la configuración (`min_speed=35` es un valor válido, 0-80).
Es un fallo a nivel de driver/GPU: la GPU ha dejado de responder en el bus PCI.
Síntomas típicos:

```
nvidia-smi
Unable to determine the device handle for GPU0: 0000:01:00.0: Unknown Error
No devices were found
```

y en el daemon:

```
[info] Waiting for NVIDIA driver... (1/60)
```

Diagnóstico después de reiniciar (el log del arranque anterior se conserva):

```bash
# Xid / errores del kernel del arranque anterior
sudo journalctl -k -b -1 --no-pager | grep -iE "xid|nvrm|fallen|pcie|AER"

# Log del servicio del arranque anterior
sudo journalctl -u set-fan-curve -b -1 --no-pager | tail -80

# Temperaturas y motivos de throttling actuales
nvidia-smi --query-gpu=temperature.gpu,clocks_throttle_reasons.active --format=csv
nvidia-smi -q -d PERFORMANCE | grep -A6 "Clocks Event Reasons"
```

Interpretación habitual:

- **Xid 79** (`GPU has fallen off the bus`): problema de alimentación, riser/PCIe,
  conectores o temperatura. Revisa la fuente, el cableado y la refrigeración.
- **Xid 62** (`Internal micro-controller halt`) o **Xid 48**: fallo interno del
  driver/GPU; suele requerir reinicio.
- Sin Xid pero con hotspot muy alto (100 °C+) y core normal: refrigeración de
  VRAM/pads degradada. Baja `power_limit` y considera repaste.

En cualquiera de estos casos el reinicio es la recuperación correcta. El
control de ventiladores no puede provocar que la GPU desaparezca del bus.

### Permisos
El servicio corre como **root** porque `/dev/mem` y el control de ventiladores lo requieren. **No** se modifican permisos de `/dev/nvidia*` ni reglas udev: así no se afecta a Docker, CUDA ni Vast.ai.

---

## 🔄 Actualización

```bash
tar -xzf gpu-fan-curve-vX.Y.Z.tar.gz
cd vX.Y   # carpeta real del paquete
sudo ./install.sh
```

Tu `~/set-fan-config` se conserva. Solo se añaden los parámetros nuevos al final, con sus comentarios.

---

## ⬆️ Actualización desde GitHub

El proyecto se publica en:

```
https://github.com/todocryptomining-sudo/gpu-set-fan-power
```

En cualquier máquina con Internet:

```bash
updategpu --check   # ver qué versión hay disponible
sudo updategpu      # descargar e instalar la última
```

`updategpu` descarga la rama `main`, elige la carpeta `releases/v*` más
reciente y ejecuta su `install.sh`. La configuración `~/set-fan-config` se
conserva. No requiere `git`; solo `curl` y `tar`.

También puedes actualizar manualmente:

```bash
git clone https://github.com/todocryptomining-sudo/gpu-set-fan-power.git
cd gpu-set-fan-power/releases/v1.5
sudo ./install.sh
```

---

## 🗑️ Desinstalación

```bash
sudo ./uninstall.sh
```

Elimina servicio, binarios, comandos y `/etc/set-fan-curve`. **Conserva** `~/set-fan-config`.

---

## 📦 Contenido del paquete

```
v1.5/
├── gpu_temps.h          # API de lectura PCI
├── gpu_temps.c          # BAR0 vía sysfs + /dev/mem + fórmulas
├── nvml_min.h           # Declaraciones NVML mínimas (sin nvml-dev)
├── set_fan_curve.cpp    # Daemon
├── vergpu.cpp           # Monitor
├── editargpu            # Editar configuración
├── restartgpu           # Reiniciar servicio
├── gpustatus            # Estado rápido
├── gpusave              # Copia de configuración
├── setfan               # Prueba manual de ventiladores
├── gpuhelp              # Ayuda rápida de comandos
├── updategpu            # Actualización desde GitHub
├── set-fan-config.example
├── install.sh
├── uninstall.sh
└── README.md
```

---

## ⚠️ Alcance

- No modifica drivers NVIDIA, CUDA, Docker ni componentes de Vast.ai.
- Solo lee registros MMIO del GPU y usa la API NVML de ventiladores/potencia.
- La lectura PCI está pensada para GPUs NVIDIA soportadas por los offsets indicados
  (RTX 3090 = GA102; GA104 usa `0xEE50`).

---

## 📋 Cambios v1.5 vs v1.4.9

| Aspecto | v1.4.9 | v1.5 |
|---------|--------|------|
| Prueba manual de ventiladores | no existía | `setfan <0-100>` / `max` / `auto` / `status` |
| Modo manual | — | archivo `/run/set-fan-curve/manual` con velocidad + PID |
| Cancelación | — | `Ctrl+C`, cierre de SSH o `setfan auto` |
| Resumen de la prueba | — | mínimo / máximo / media de core, VRAM y hotspot |
| Seguridad | latch crítico | el latch crítico sigue teniendo prioridad sobre el modo manual |
| Ayuda | — | `gpuhelp` lista todos los comandos con descripción corta |
| Actualización | manual | `sudo updategpu` desde GitHub, conservando la configuración |
| Comandos instalados | 4 | 7 (añadidos `setfan`, `gpuhelp` y `updategpu`) |
