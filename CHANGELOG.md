# Changelog

## v1.8 (en desarrollo)

- Endurecimiento de lectura PCI/MMIO: tabla explícita por device ID, vendor NVIDIA, BDF válido, BAR0 completo, offsets dentro de rango, aritmética de 64 bits y mapeos solo lectura.
- Estado detallado por GPU: `INITIALIZING`, `NORMAL`, `SENSOR_ERROR`, `NVML_ERROR`, `GPU_LOST`, `RECOVERING` y `DISABLED`.
- En `GPU_LOST` se suspenden NVML/MMIO, se cierran mapeos y se registra el motivo; no hay reset, reboot ni reinicio de Docker/Vast.ai.
- VRAM y hotspot independientes, con timestamps, plausibilidad, timeout, rachas de fallo y último valor válido limitado.
- Escrituras de ventilador reducidas mediante `fan_update_interval_ms`, `fan_change_threshold`, histéresis y aumentos críticos inmediatos; estado solicitado/reportado/aplicado por fan.
- `power_limit_mode=startup|monitor|enforce`; `monitor` recomendado en Vast.ai. `NOT_SUPPORTED` no degrada el control de ventiladores.
- Multi-GPU independiente, recuperación opcional mediante sondeos NVML seguros y máximo de tres intentos.
- Nuevas pruebas de daemon y MMIO con mocks, sysfs falso y memoria mapeada falsa.
- Instalador v1.8 con copia de seguridad antes de upgrade y límites systemd de reinicio más conservadores.
- **No declarada producción:** falta validar en Ubuntu 24.04, RTX reales, `/dev/mem`, NVML y comportamiento físico de ventiladores.

## v1.7

- **`critical_temp` hasta 110 °C** (antes 50-100). Permite picos normales de 100-105 °C en hotspots GDDR6X sin bloquear. El bloqueo sigue disparando con `>=`.
- **Nuevo `set_fan_curve --check-config` / `--validate`**: revisa el archivo línea a línea.
  - Claves desconocidas (typos), líneas sin `=`, claves o valores vacíos y valores no numéricos → error.
  - Valores fuera de rango → error con el rango permitido.
  - Claves duplicadas → aviso; se usa la última.
  - Muestra los valores efectivos y los sensores exactos de la curva y del bloqueo crítico.
  - No necesita root, no inicializa NVML y no toca el servicio. Devuelve 1 si hay errores.
- **`restartgpu` valida antes de reiniciar**: si hay errores, no reinicia y el servicio conserva la configuración anterior.
- **`editargpu` rediseñado**: copia temporal, editor, diff con el original, validación y bucle de re-edición. Solo pregunta si reiniciar cuando la configuración es válida; antes muestra las modificaciones.
- **Diagnóstico de sensores**: el arranque y `--check-config` indican si el hotspot forma parte del bloqueo (`use_hotspot_temp` / `safety_all_sensors`).
- **Config inválida en runtime**: el daemon sigue arrancando con valores seguros y registra los errores; los scripts impiden aplicarla.
- **`gpuhelp`, `install.sh`, `uninstall.sh`, `setfan`, README y configuración de ejemplo** actualizados a v1.7.
- Incluye todos los cambios de v1.6.

## v1.6

- **Corregido `max_temp=100`**: el parser acepta 50-100 °C. Antes rechazaba 100 y usaba 85, lo que hacía que los ventiladores subieran a ~98 % a 84 °C.
- **`critical_temp` independiente**: puede ser menor que `max_temp` (por ejemplo `max_temp=100`, `critical_temp=95`). Ya no se autoajusta.
- **Curva nueva**: `min_speed` es solo un suelo; la curva va de 0 a `max_speed` entre `max_temp-40` y `max_temp`.
- **Nueva clave `max_speed`** (20-100 %): limita la velocidad normal. El bloqueo crítico la ignora y fuerza 100 %.
- **Estado por ventilador**: Fan 0, Fan 1, … se leen y escriben por separado.
- **Errores NVML**: se comprueba el retorno de cada `nvmlDeviceSetFanSpeed_v2` y se registra una vez por cambio.
- **Política manual NVML**: `NVML_FAN_POLICY_MANUAL` al arrancar, reafirmada cada 60 s y restaurada a `AUTO` al parar.
- **Alerta de ventilador a 0 %**: si un ventilador sigue a 0 % mientras otro funciona (10 s), o si todos leen 0 % con un setpoint alto.
- **Nuevo `/run/set-fan-curve/fans`**: `vergpu`, `gpustatus` y `setfan` muestran todos los sensores de ventilador.
- **`setfan`**: muestra Fan0/Fan1 y avisa del 0 %; `status` muestra las velocidades actuales.
- **Documentación y configuración de ejemplo** actualizadas.

## v1.5

- **Nuevo `setfan`**: prueba manual de ventiladores.
  - `setfan 70` fija todos los ventiladores al 70 % hasta cancelar.
  - `setfan max` fija al 100 %.
  - `setfan auto` cancela desde otra terminal.
  - `setfan status` muestra la prueba activa.
  - Resumen de mínimo / máximo / media de core, VRAM y hotspot al terminar.
  - Si el proceso muere (Ctrl+C, cierre de SSH, kill), la curva vuelve sola.
  - El bloqueo crítico siempre tiene prioridad sobre el modo manual.
- **Nuevo `gpuhelp`**: lista todos los comandos con una descripción corta.
- **Nuevo `updategpu`**: descarga e instala la última versión desde GitHub conservando `~/set-fan-config`.
- El daemon lee `/run/set-fan-curve/manual` en cada ciclo y aplica la velocidad manual a todas las GPUs.
- Incluye la corrección de v1.4.9 del bloqueo crítico.

## v1.4.9

- **Corregido**: el bloqueo crítico incluía el hotspot aunque `use_hotspot_temp=0`, dejando los ventiladores al 100 % de forma permanente.
- Nueva clave `safety_all_sensors`:
  - `0` (por defecto): el bloqueo usa los mismos sensores que la curva.
  - `1`: fuerza core + VRAM + hotspot.
- Si `critical_temp <= max_temp`, se eleva automáticamente en lugar de impedir el arranque.

## v1.4.8

- `ACTUAL_HOME` se resuelve antes de usarlo.
- BAR0 se lee de `/sys/.../resource` (texto), no de `resource0` (binario).
- El bus PCI se toma con función `.0`.
- Correcciones de compilación.
- Comandos instalados en el `~/.local/bin` real.
- Se compila antes de detener y eliminar la instalación anterior.
- Se elimina la regla udev antigua; los permisos de `/dev/nvidia*` no se tocan.

## Versiones anteriores

Las versiones v1.0 a v1.4.7 están disponibles en el historial de releases del proyecto.
