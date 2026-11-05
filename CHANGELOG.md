# Changelog

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
