# Auditoría v1.81 — GPU Fan Curve Controller

**Alcance:** revisión de los fuentes de `releases/v1.7` (`set_fan_curve.cpp`,
`gpu_temps.c/.h`, `nvml_min.h`, `vergpu.cpp`, `install.sh`, comandos y unidad
systemd) y cambios aplicados en `releases/v1.81`.

**Método:** lectura línea a línea, comparación con la ABI pública de NVML y con
el documento PCI/MMIO usado por el proyecto. No se dispone de GPU física ni de
`/dev/mem` en el entorno de desarrollo: los defectos de esta sección son
**confirmados por código**, no por reproducción en hardware. Las pruebas de
hardware pendientes se enumeran al final y no se declara producción.

---

## 1. Defectos confirmados en v1.7

### D1 — Cualquier device NVIDIA desconocido usaba el offset de GA102

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/gpu_temps.c`, `vram_offset_for_device()`,
  líneas 54-63; llamada en `gpu_temps_read()` línea 107.
- **Defecto:** el `default` devolvía `0xE2A8` para cualquier device ID. No se
  comprobaba `vendor`, no se distinguía GA102 de GA104 ni de una arquitectura
  futura. Un registro no verificado puede devolver un valor plausible pero
  falso.
- **Corrección v1.81:** tabla explícita `g_profiles[]` con solo cinco IDs
  verificados; `vendor != 0x10DE` y device desconocido devuelven
  `GPU_TEMPS_UNSUPPORTED` sin abrir `/dev/mem` ni tocar MMIO.

### D2 — BAR0 se leía solo como dirección, sin tamaño ni flags

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/gpu_temps.c`, `get_bar0_from_sysfs()`, líneas
  28-39; `read_reg32()`, líneas 68-85.
- **Defecto:** se leía únicamente el primer campo de
  `/sys/bus/pci/devices/<bdf>/resource` (`start`). No se comprobaba `end`,
  `flags`, que el BAR fuese memoria, que el offset cayera dentro del BAR ni que
  la suma `bar0 + offset` no desbordase. `mmap()` se hacía sin esa validación.
- **Corrección v1.81:** se leen `start`, `end` y `flags`; se exige
  `IORESOURCE_MEM`, tamaño entre 4 KiB y 16 GiB, offsets dentro del BAR,
  aritmética de 64 bits y comprobación de desbordamiento antes de mapear.

### D3 — Sin validación estricta de vendor/device/BDF

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/gpu_temps.c`, `gpu_temps_read()`, líneas 90-129.
- **Defecto:** el BDF se aceptaba con cualquier cadena de hasta 32 caracteres
  que no contuviera `/`; no se exigía el formato `dddd:bb:dd.f` ni función 0.
  No se leía `vendor`. El resultado `false` no distinguía dispositivo ausente,
  permiso, offset no soportado o error MMIO.
- **Corrección v1.81:** `valid_bdf()` con `sscanf("%4x:%2x:%2x.%1x")`, longitud
  exacta 12, sin `/` y función 0; lectura de `vendor` y `device`; estado
  detallado `gpu_temps_status_t` y `gpu_temps_status_string()`.

### D4 — `mmap()`/`munmap()` por cada lectura y retorno ignorado

- **Severidad:** media-alta.
- **Ubicación:** `releases/v1.7/gpu_temps.c`, `read_reg32()`, líneas 68-85.
- **Defecto:** cada lectura de VRAM y cada lectura de hotspot abría `/dev/mem`,
  mapeaba una página y la desmapeaba. `munmap()` no se comprobaba. No había
  caché por GPU ni cierre explícito al perder la GPU.
- **Corrección v1.81:** contexto persistente por BDF (hasta 16 GPUs), hasta 4
  páginas mapeadas por GPU, `PROT_READ`, comprobación de `open`, `mmap` y
  `munmap`, y API `gpu_temps_close()`, `gpu_temps_close_all()` y
  `gpu_temps_reset()`.

### D5 — Una lectura PCI fallida dejaba la última temperatura para siempre

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `updatePciTemps()`, líneas
  319-333.
- **Defecto:** si `gpu_temps_read()` fallaba, se conservaban
  `vram_available`/`hotspot_available` y los valores anteriores sin límite de
  tiempo. Una lectura obsoleta podía mantener el bloqueo crítico activo o, al
  contrario, ocultar una subida real. No había marca temporal ni timeout.
- **Corrección v1.81:** cada sensor tiene `*_last_ok`, `*_fail_streak` y
  `sensor_timeout_ms`; al caducar se marca no disponible y se registra. El
  bloqueo crítico solo usa lecturas frescas y plausibles.

### D6 — Un fallo del core descartaba VRAM/hotspot y congelaba los ventiladores

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`,
  `getEffectiveTemperature()`, líneas 404-431; bucle principal, líneas
  1002-1007 (`if (temp < 0) continue;`).
- **Defecto:** si `nvmlDeviceGetTemperature()` fallaba, la función devolvía
  `-1` aunque VRAM y hotspot estuvieran disponibles. El bucle hacía `continue`
  y los ventiladores quedaban en la última velocidad solicitada
  indefinidamente, sin estado de error ni restauración.
- **Corrección v1.81:** los tres sensores se evalúan por separado; la curva usa
  los configurados y disponibles; si no queda ninguno, tras
  `SENSOR_FAIL_STREAK_LIMIT` se entra en `SENSOR_ERROR` y se restaura AUTO o se
  fija `max_speed` según `restore_auto_on_sensor_failure`. Al volver una
  lectura válida se reanuda `NORMAL`.

### D7 — Escritura de ventilador en cada ciclo, sin estado aplicado

- **Severidad:** media.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `applyFanSpeed()`, líneas
  219-235; bucle principal, líneas 1040-1045.
- **Defecto:** `nvmlDeviceSetFanSpeed_v2()` se llamaba en cada iteración (cada
  2 s) aunque la velocidad no cambiara. No se guardaba lo aplicado, no había
  umbral, histéresis ni intervalo mínimo. Un error de escritura solo se
  registraba; la siguiente iteración volvía a intentarlo sin estrategia.
- **Corrección v1.81:** `fan_target`, `fan_desired`, `fan_applied`,
  `fan_last_write_error`, `fan_resync_needed`, `fan_update_interval_ms`,
  `fan_change_threshold`, aumentos críticos inmediatos y reafirmación cada
  60 s. Cada retorno NVML se comprueba y se clasifica.

### D8 — Fallos de lectura de fan silenciosos

- **Severidad:** media.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `readFanSpeeds()`, líneas
  204-217.
- **Defecto:** cualquier error NVML dejaba `fan_speed = -1` y `fan_read_ok =
  false` sin log, sin racha de fallos y sin distinguir `NOT_SUPPORTED`,
  `GPU_IS_LOST` o error transitorio.
- **Corrección v1.81:** racha por fan, log con límite de frecuencia,
  clasificación de `GPU_IS_LOST`/`RESET_REQUIRED` y transición de estado.

### D9 — El texto de alerta equiparaba “0 %” con “ventilador parado”

- **Severidad:** media (diagnóstico).
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `checkFanAlerts()`, líneas
  240-286; `releases/v1.7/vergpu.cpp`, línea 347.
- **Defecto:** los mensajes decían “stays at 0 %” y “fan at 0 %”, y sugerían
  cable/controlador. NVML devuelve un **porcentaje**, no RPM; un 0 % puede ser
  política manual no aplicada, índice de fan equivocado, limitación del driver
  o una lectura no soportada. No había lectura de tacómetro que permitiese
  afirmar que el ventilador estaba parado.
- **Corrección v1.81:** los mensajes dicen explícitamente “NVML informa 0 %”,
  “no demuestra 0 RPM” y enumeran política, índice, cable y controlador como
  hipótesis a comprobar. `vergpu` muestra la misma advertencia.

### D10 — Reafirmación del límite de potencia sin comprobar el retorno

- **Severidad:** media-alta.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, bucle principal, líneas
  1048-1068; `setPowerLimit()`, líneas 436-470.
- **Defecto:** la reafirmación cada 10 s llamaba a
  `nvmlDeviceSetPowerManagementLimit()` en la línea 1065 sin comprobar el
  resultado. No existían modos `startup`/`monitor`/`enforce`; el valor
  solicitado se recortaba al mínimo/máximo sin conservar por separado
  solicitado y aplicado.
- **Corrección v1.81:** `power_limit_mode=startup|monitor|enforce`, validación
  de mínimo/máximo, cada retorno comprobado, `NOT_SUPPORTED` desactiva el
  control de potencia para esa GPU sin degradar el control de fans, y
  `monitor` no escribe el límite (recomendado para Vast.ai).

### D11 — Política manual por GPU, no por fan; reafirmación sin retorno

- **Severidad:** media.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`,
  `setManualFanPolicy()`, líneas 183-202; reafirmación, líneas 1070-1084.
- **Defecto:** `fan_policy_manual` era un único booleano por GPU: si un fan
  aceptaba la política y otro no, la reafirmación se consideraba activa para
  todos. La reafirmación ignoraba todos los retornos.
- **Corrección v1.81:** la política se aplica y se comprueba por fan; cada error
  se registra y `GPU_IS_LOST` se clasifica. La reafirmación usa la misma ruta
  controlada y no se ejecuta en `GPU_LOST`, `DISABLED` o `RECOVERING`.

### D12 — Sin máquina de estados ni tratamiento de GPU perdida

- **Severidad:** alta.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, inicialización, líneas
  881-929; bucle principal, líneas 985-1121.
- **Defecto:** no existían estados por GPU. Errores como `GPU_IS_LOST` o
  `RESET_REQUIRED` no se distinguían de un fallo transitorio; se seguía
  leyendo MMIO y llamando a NVML. No había decisión de parada, recuperación
  controlada ni registro de incidente. `signal()` se usaba en las líneas
  949-950.
- **Corrección v1.81:** estados `INITIALIZING`, `NORMAL`, `SENSOR_ERROR`,
  `NVML_ERROR`, `GPU_LOST`, `RECOVERING`, `DISABLED`; `sigaction`; en
  `GPU_LOST` se suspenden NVML/MMIO, se cierran mapeos y no se toca potencia ni
  fans; `auto_recover_gpu` hace solo sondeos NVML seguros (máximo 3), sin
  reset/reboot.

### D13 — Publicación de estado sin comprobar errores

- **Severidad:** baja-media.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `writeStatusFile()`,
  líneas 361-364.
- **Defecto:** se ignoraban los valores de retorno de
  `gpu_temps_status_write()` y `gpu_fans_status_write()`. Un fallo de
  escritura dejaba a `vergpu`/`gpustatus` mostrando datos antiguos sin aviso.
- **Corrección v1.81:** escritura atómica con `fflush`/`fclose`/`rename`
  comprobados, `chmod` comprobado y archivos adicionales `state` y `fanstate`.

### D14 — `getFanCount()` ocultaba el fallback

- **Severidad:** baja.
- **Ubicación:** `releases/v1.7/set_fan_curve.cpp`, `getFanCount()`, líneas
  171-178.
- **Defecto:** si `nvmlDeviceGetNumFans()` fallaba o devolvía 0, se usaba 1
  fan sin log. No se refrescaba tras una recuperación.
- **Corrección v1.81:** `fan_count_fallback` se registra en el arranque y el
  recuento se refresca en `probeGpu()` antes de reutilizar los arrays.

### D15 — `vergpu` mostraba 0 °C cuando el core fallaba

- **Severidad:** media (diagnóstico).
- **Ubicación:** `releases/v1.7/vergpu.cpp`, línea 184 y líneas 301-302.
- **Defecto:** `gpu.temp_core = (r == NVML_SUCCESS) ? int(ct) : 0;` y la
  impresión posterior mostraban `0 °C`, un valor plausible y falso.
- **Corrección v1.81:** `core_sensor` separado; se muestra `N/A (NVML error)`.
  Además se muestra el estado del daemon (`/run/set-fan-curve/state`) y el
  motivo del estado PCI (`gpu_temps_status_string`).

---

## 2. Límites de diseño de v1.7 (no son fallos de implementación)

- No existían `fan_update_interval_ms`, `fan_change_threshold`,
  `sensor_interval_ms`, `sensor_timeout_ms`, `power_limit_mode`,
  `stop_on_gpu_lost`, `auto_recover_gpu`, `restore_auto_on_sensor_failure`,
  `log_nvml_errors` ni `log_pcie_errors`.
- No había cuarentena de MMIO ni cierre explícito de mapeos.
- No había archivos `state`/`fanstate`; `gpustatus` no podía mostrar el estado
  por GPU.
- El bloqueo crítico usaba los mismos sensores que la curva si
  `safety_all_sensors=0`; el hotspot no formaba parte del bloqueo por defecto.
  Esto es configuración, no un defecto: se documenta y se mantiene.

---

## 3. Incidente de `rentalai6`: hipótesis, no causas demostradas

Hechos disponibles: Xid 79, errores AER de PCIe, hotspot próximo a 105 °C y
ventiladores reportados al 0 % antes de la pérdida. No hay evidencia
suficiente para atribuir la pérdida a esta aplicación, al hotspot o a una
lectura MMIO concreta.

Hipótesis abiertas, en orden de verificabilidad:

1. **Alimentación/riser/PCIe:** un Xid 79 (GPU has fallen off the bus) es
   compatible con alimentación insuficiente, contacto deficiente en el riser,
   sobrecalentamiento de VRM o un enlace PCIe inestable. Requiere revisar
   alimentación, cables, riser y `dmesg`/AER del host.
2. **Driver/firmware:** un fallo del driver o del firmware puede dejar la GPU
   fuera del bus. Requiere versión de driver, `nvidia-smi -q`, `dmesg` y
   correlación temporal.
3. **Temperatura:** 105 °C de hotspot GDDR6X es alto pero puede ser un pico
   bajo carga; la política de v1.7 no incluía el hotspot en el bloqueo por
   defecto. Es un riesgo térmico a corregir por configuración, no una causa
   demostrada del Xid.
4. **Interacción NVML/MMIO:** no se ha demostrado que una lectura MMIO o una
   llamada NVML provoque Xid 79. v1.81 reduce la exposición (mapeos de solo
   lectura, offsets verificados, parada ante `GPU_LOST`) pero no puede
   garantizar que un acceso MMIO sea seguro en una GPU que ya está fallando.
5. **Ventilador al 0 %:** NVML informa porcentaje, no RPM. Puede ser política
   manual, índice incorrecto, limitación del driver o un fallo real del
   ventilador/controlador. Sin tacómetro no se puede concluir.

**Lo que v1.81 no afirma:** no se ha reproducido el Xid 79, no se ha medido el
hotspot real en hardware, no se ha verificado que los ventiladores físicos
giren y no se ha probado en una instancia Vast.ai. Por tanto, **v1.81 no se
declara lista para producción** hasta completar la sección 4.

---

## 4. Verificación pendiente en hardware real

1. Compilar en Ubuntu 24.04 con `g++` y `gcc`; ejecutar `make test`.
2. En RTX 3090: comprobar `gpu_temps_read_ex()` devuelve device `0x2204`,
   offsets `0xE2A8`/`0x2046C` y temperaturas coherentes con `nvidia-smi`.
3. Repetir en RTX 3080 (`0x2206`), 3080 Ti (`0x2203`) y 3070
   (`0x2484`/`0x2488`) si hay acceso.
4. Verificar que un device no listado devuelve `GPU_TEMPS_UNSUPPORTED` y que
   **no** se abre `/dev/mem`.
5. Probar `setfan 0`, `setfan 40`, `setfan 100` y `setfan auto`; comparar el
   porcentaje NVML con el comportamiento físico y, si es posible, con RPM.
6. Provocar un fallo de sensor controlado (por ejemplo, arrancar sin
   `/dev/mem`) y comprobar `SENSOR_ERROR`, la restauración AUTO y el aviso.
7. Simular `GPU_LOST` con un mock o con una GPU secundaria; verificar que no se
   tocan fans/potencia, que se cierran mapeos y que las demás GPUs siguen.
8. Verificar `power_limit_mode=monitor` en Vast.ai: no debe escribir el límite;
   `enforce` debe reafirmarlo y comprobar cada retorno.
9. Medir consumo de CPU y latencia de parada con `systemctl stop`.
10. Revisar `journalctl -u set-fan-curve` durante 24 h y confirmar que no hay
    errores repetidos ni fugas de descriptores (`lsof`/`/proc/<pid>/fd`).

---

## 5. Rollback

- La release v1.7 permanece intacta en `releases/v1.7`.
- `updategpu` selecciona la carpeta `releases/v*` más reciente; para volver a
  v1.7 hay que instalar explícitamente el asset `gpu-fan-curve-v1.7.tar.gz`.
- El instalador v1.81 guarda una copia de la instalación anterior en
  `/var/backups` antes de sustituirla y respalda la configuración existente.
  La restauración automática ante fallo de instalación aún no está verificada.
- Desinstalar: `sudo ./uninstall.sh` (elimina servicio, binarios y comandos;
  conserva la configuración salvo confirmación explícita).
