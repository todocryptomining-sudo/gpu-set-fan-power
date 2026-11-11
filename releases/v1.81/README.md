# GPU Fan Curve Controller — **v1.81**

Control de ventiladores NVIDIA con **temperaturas reales de VRAM y Hotspot leídas directamente de los registros PCI** del GPU (misma técnica que `gddr6.c` / `nvml_direct_access.c`), protección térmica bloqueante, estado por ventilador y comandos globales.

> **v1.81** conserva `critical_temp` hasta 110 °C (pensado para hotspots GDDR6X que normalmente llegan a 100-105 °C) y añade validación real de la configuración: `set_fan_curve --check-config` detecta typos, valores vacíos y fuera de rango **sin reiniciar el servicio**. `restartgpu` y `editargpu` validan antes de aplicar; `editargpu` muestra el diff y solo pregunta si reiniciar cuando la configuración es válida.

---

## 🎯 Qué hace

### `set_fan_curve` — daemon maestro (root)
- **VRAM real** desde PCI: `BAR0 + 0xE2A8`, fórmula `(raw & 0xFFF) / 0x20`.
- **Hotspot real** desde PCI: `BAR0 + 0x2046C`, fórmula `(raw >> 8) & 0xFF`.
- Curva sobre `max(core, VRAM[, hotspot])` según configuración.
- **Curva independiente de `min_speed`**:
  - de `max_temp - 40` a `max_temp` la curva sube de 0 % a `max_speed`;
  - después se aplica `min_speed` como suelo;
  - `max_speed` limita la velocidad normal.
- **Bloqueo crítico con latch**: al llegar a `critical_temp` (comparación `>=`), fans al 100 % ignorando `max_speed` y `min_speed`; no bajan hasta 5 °C por debajo.
- **Sensores explícitos**: el arranque registra qué sensores alimentan la curva y cuáles pueden disparar el bloqueo. El hotspot solo forma parte del bloqueo si `use_hotspot_temp=1` o `safety_all_sensors=1`.
- **Validación de configuración**: `set_fan_curve --check-config [ruta]` revisa el archivo línea a línea (claves desconocidas/typos, valores vacíos, no numéricos y fuera de rango), muestra los valores efectivos y los sensores exactos, y devuelve código 1 si hay errores. No necesita root ni toca el servicio.
- **Estado por ventilador**: lee y escribe Fan 0, Fan 1, … por separado, comprueba cada retorno NVML y avisa si un ventilador queda al 0 %.
- **Política manual NVML**: fija `NVML_FAN_POLICY_MANUAL` y la reafirma cada 60 s; al parar restaura `AUTO`.
- **Power limit por modo**: `startup`, `monitor` o `enforce`; `monitor` no escribe y se recomienda en Vast.ai.
- **Estados por GPU**: `NORMAL`, `SENSOR_ERROR`, `NVML_ERROR`, `GPU_LOST`, `RECOVERING` y `DISABLED`.
- **MMIO endurecido**: device IDs explícitos, BAR0 validado, mapeo read-only y cierre seguro.
- Persistence mode y restauración de control automático al detenerse.
- Publica temperaturas en `/run/set-fan-curve/temps` y velocidades por ventilador en `/run/set-fan-curve/fans`.
- **Modo manual (`setfan`)**: mantiene los ventiladores a un % fijo mientras el proceso `setfan` esté vivo; el bloqueo crítico siempre tiene prioridad.

### `vergpu` — monitor en vivo
- Muestra **Core / VRAM / Hotspot** y **todos los ventiladores** (`Fan0`, `Fan1`, …).
- Al final de cada GPU muestra procesos visibles: PID, memoria GPU, tipo (`C` compute / `G` graphics) y nombre de aplicación (`python`, etc.). Si NVML no entrega nombre, intenta leer `/proc/<PID>/comm`.
- `p` alterna la consulta; `--no-processes` la desactiva.
- Si se ejecuta como root lee PCI directamente; si no, usa el archivo del daemon.
- Uso y memoria, potencia, relojes, ventiladores y persistence.

---

## 🚀 Instalación

```bash
tar -xzf gpu-fan-curve-v1.81.tar.gz
cd v1.81

chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

> **Ojo:** la carpeta se llama `v1.81`. Usa exactamente la que aparece al descomprimir.

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
| `max_temp` | Temperatura objetivo (50-100 °C). La curva llega a `max_speed` aquí. |
| `min_speed` | Suelo de velocidad (0-80 %). Se aplica **después** de la curva. |
| `max_speed` | Tope normal (20-100 %). El bloqueo crítico lo ignora. |
| `critical_temp` | Bloqueo de seguridad (50-110 °C). Puede ser menor o mayor que `max_temp`. Latch hasta bajar 5 °C. |
| `use_vram_temp` | Usar VRAM real por PCI (1 = sí, 0 = no). |
| `use_hotspot_temp` | Usar Hotspot por PCI (1 = sí, 0 = no). Si está a 1, el hotspot también puede disparar el bloqueo. |
| `safety_all_sensors` | Bloqueo crítico con todos los sensores (1 = core+VRAM+hotspot siempre, 0 = mismos que la curva). |
| `power_limit` | Límite de potencia en W (0 = desactivado). |
| `persistence` | Persistence mode (0/1). |

### Cómo se calcula la velocidad

```
ramp_start = max_temp - 40

curva = 0 %                     si temp <= ramp_start
curva = 0 → max_speed           si ramp_start < temp < max_temp
curva = max_speed               si temp >= max_temp

velocidad = max(min_speed, curva)      (nunca por encima de max_speed)
```

Excepciones:

- `critical_temp`: fuerza **100 %** y se mantiene hasta bajar 5 °C.
- `setfan`: velocidad explícita; solo el bloqueo crítico la anula.

Ejemplo con `max_temp=100`, `min_speed=25`, `max_speed=100`, `critical_temp=95`:

```
60 °C o menos → 25 % (suelo)
84 °C         → 60 %
95 °C         → 100 % (bloqueo crítico)
```

Con `max_temp=100` la curva normal nunca llega a `max_speed`; el límite real
de seguridad es `critical_temp`. Si quieres que los ventiladores suban antes,
baja `max_temp` (por ejemplo 85).

### ¿Por qué los ventiladores se van al 100 % si el hotspot llega a 105 °C?

El bloqueo crítico usa `critical_temp` como umbral y dispara con `>=`. Los
sensores que puede mirar dependen de la configuración:

| `use_hotspot_temp` | `safety_all_sensors` | ¿El hotspot puede forzar 100 %? |
|--------------------|----------------------|----------------------------------|
| 0 | 0 | **No** (se muestra, pero no bloquea) |
| 1 | 0 | Sí, porque la curva incluye el hotspot |
| cualquiera | 1 | Sí, siempre |

Si el hotspot de tu GDDR6X llega de forma normal a 105 °C y no quieres que eso
dispare el bloqueo:

- deja `use_hotspot_temp=0` y `safety_all_sensors=0`, o
- sube `critical_temp` a **106-110** (el bloqueo es `>=`, así que 105 no dispara
  con 106).

`set_fan_curve --check-config ~/set-fan-config` imprime exactamente qué
sensores usa la curva y cuáles usa el bloqueo.

### Validar antes de aplicar

```bash
# No requiere root y no toca el servicio
set_fan_curve --check-config ~/set-fan-config
```

Salida típica con un typo:

```
[error] línea 12: clave desconocida 'critcal_temp' (¿typo?)
[FAIL] 1 error(es). NO reinicies el servicio con este archivo.
```

Después de editar:

```bash
editargpu      # muestra el diff, valida y pregunta antes de reiniciar
restartgpu     # valida y reinicia; aborta si hay errores
```

`editargpu` guarda una copia temporal, abre el editor, muestra el diff con el
original y ejecuta la validación. Si hay errores ofrece volver a editar y no
reinicia nada. Solo cuando el archivo es válido pregunta si aplicar los cambios.

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

# Velocidades por ventilador
cat /run/set-fan-curve/fans

# Resumen cada 30 s en el journal
journalctl -u set-fan-curve -n 50 --no-pager | grep -i temp
```

Salida esperada (ejemplo con dos ventiladores):

```
GPU 0: NVIDIA GeForce RTX 3090  [0000:01:00.0]
  Core:   44 °C   VRAM:   52 °C (PCI direct)   Hotspot:  58 °C (PCI direct)
  Fans: Fan0  47 %   Fan1  46 %
  ...
```

> La VRAM debe ser **distinta** de Core. Si aparece `N/A`, revisa la sección de problemas.

---

## 🔧 Comandos

| Comando | Descripción |
|---------|-------------|
| `vergpu` | Monitor en vivo (Core / VRAM / Hotspot / Fans). |
| `editargpu` | Edita `~/set-fan-config`: muestra el diff, valida y pregunta antes de reiniciar. |
| `restartgpu` | Valida la configuración y reinicia; no aplica archivos con errores. |
| `set_fan_curve --check-config` | Comprueba la configuración sin root y sin tocar el servicio. |
| `gpustatus` | Estado rápido del servicio, GPUs y ventiladores. |
| `gpusave` | Guarda una copia de la configuración. |
| `setfan <0-100>` | Prueba manual: fija todos los ventiladores a ese % hasta cancelar. |
| `setfan max` | Prueba manual al 100 %. |
| `setfan auto` | Cancela la prueba manual y vuelve a la curva. |
| `setfan status` | Muestra la prueba activa y las velocidades actuales. |
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
setfan status    # ver la prueba activa y las velocidades actuales
setfan auto      # cancelar desde otra terminal
```

Durante la prueba se muestra cada 2 s:

```
────────────────────────────────────────────────────────────────
[22:10:05] Prueba manual fijada al 70 %
  GPU 0  Core  72 °C  VRAM  68 °C  Hotspot  79 °C  Fan0 70%  Fan1 70%    220.4 W
```

Si un ventilador se queda al 0 % mientras el otro funciona, la línea muestra
`[!] fan at 0 %` y el daemon registra una alerta en el journal.

Al pulsar `Ctrl+C` se elimina el control manual y se muestra un resumen
(mínimo / máximo / media) de core, VRAM y hotspot.

**Seguridad:**

- El bloqueo crítico (`critical_temp`) **siempre** tiene prioridad: si la
  temperatura llega al límite, el daemon fuerza 100 % aunque `setfan 40`.
- `max_speed` **no** limita `setfan`: es una prueba explícita. Solo el bloqueo
  crítico puede subirla por encima de lo pedido.
- Si cierras la sesión SSH o matas el proceso, el daemon comprueba el PID y
  vuelve solo a la curva. No queda el ventilador fijado.
- `setfan auto` borra el archivo de control desde cualquier terminal.

Cómo funciona: `setfan` escribe `/run/set-fan-curve/manual` con
`<velocidad> <pid>`; el daemon lo lee en cada ciclo y aplica esa velocidad a
todos los ventiladores de todas las GPUs.

---

## 🛠️ Problemas y soluciones

### Un ventilador aparece al 0 % y el otro funciona

El daemon lo detecta tras 10 s (5 ciclos) y escribe una alerta en el journal:

```bash
journalctl -u set-fan-curve -n 80 --no-pager | grep -i "fan"
```

Comprueba:

1. `cat /run/set-fan-curve/fans` — ¿el sensor 0 y el 1 existen?
2. `nvidia-smi --query-gpu=fan.speed --format=csv` — valor que reporta NVML.
3. El conector/cable del ventilador y el cable de alimentación de la tarjeta.
4. Si la GPU es de segunda mano o tiene varios ventiladores, puede ser el
   controlador PWM de la tarjeta o un ventilador desconectado.

Prueba manual para descartar la curva:

```bash
setfan 100     # si sigue a 0 %, el problema es hardware/driver
setfan auto
```

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
cd gpu-set-fan-power/releases/v1.7
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
v1.7/
├── gpu_temps.h          # API de lectura PCI y estado de ventiladores
├── gpu_temps.c          # BAR0 vía sysfs + /dev/mem + fórmulas + fans
├── nvml_min.h           # Declaraciones NVML mínimas (sin nvml-dev)
├── set_fan_curve.cpp    # Daemon + validación --check-config
├── vergpu.cpp           # Monitor
├── editargpu            # Editar, diff, validar y confirmar reinicio
├── restartgpu           # Validar y reiniciar servicio
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

## 📋 Cambios v1.81 vs v1.7

| Aspecto | v1.6 | v1.7 |
|---------|------|------|
| `critical_temp` | 50-100 °C | 50-110 °C (hotspots GDDR6X de 100-105 °C) |
| Validación | silenciosa; valores malos usaban el default | `--check-config`: typos, claves desconocidas, vacíos, no numéricos y rangos |
| `restartgpu` | reiniciaba siempre | valida primero; aborta si hay errores |
| `editargpu` | preguntaba "Apply changes?" sin mostrar nada | muestra el diff, valida, permite re-editar y confirma antes de reiniciar |
| Diagnóstico | `[warn]` disperso | informe con valores efectivos y sensores de curva/bloqueo |
| Log de arranque | no detallaba sensores | imprime `Curve sensors` y `Critical lock sensors` |
| Config inválida en runtime | usaba defaults en silencio | sigue arrancando con valores seguros y registra los errores |

## 📋 Cambios v1.6 vs v1.5

| Aspecto | v1.5 | v1.6 |
|---------|------|------|
| `max_temp` | 50-95 (100 se rechazaba y usaba 85) | 50-100 |
| `critical_temp` | debía ser > `max_temp` | independiente; puede ser menor |
| Curva | `min_speed → 100` | `0 → max_speed`, luego suelo `min_speed` |
| `max_speed` | no existía | 20-100 %, configurable |
| Bloqueo crítico | 100 % | 100 % ignorando `min_speed` y `max_speed` |
| Sensores de ventilador | solo Fan 0 | todos (Fan 0, Fan 1, …) |
| Errores NVML | no se comprobaban | se comprueba cada retorno |
| Ventilador al 0 % | sin aviso | alerta tras 10 s si otro funciona |
| Política NVML | no se fijaba | MANUAL + reafirmación cada 60 s, AUTO al parar |
| Estado publicado | `/run/set-fan-curve/temps` | también `/run/set-fan-curve/fans` |
| `vergpu` / `gpustatus` / `setfan` | un ventilador | todos los ventiladores |
