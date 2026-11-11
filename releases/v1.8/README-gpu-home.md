# GPU Fan Curve Controller — Inicio rápido (v1.7)

> Ejecuta `gpuhelp` para ver todos los comandos disponibles.

## Instalación

```bash
tar -xzf gpu-fan-curve-v1.7.tar.gz
cd v1.7
chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

## Comandos

| Comando | Uso |
|---------|-----|
| `vergpu` | Monitor en vivo: Core / VRAM / Hotspot / Fans |
| `vergpu -1` | Una sola captura y salir |
| `editargpu` | Editar, ver el diff, validar y confirmar antes de reiniciar |
| `restartgpu` | Validar y aplicar cambios de configuración |
| `set_fan_curve --check-config` | Comprobar la configuración sin root ni reinicio |
| `gpustatus` | Estado rápido (servicio, GPUs y fans) |
| `gpusave` | Guardar copia de la configuración |
| `setfan 70` | Prueba manual: fija todos los ventiladores al 70 % |
| `setfan max` | Prueba manual al 100 % |
| `setfan auto` | Cancela la prueba y vuelve a la curva |
| `setfan status` | Muestra la prueba activa y las velocidades actuales |
| `gpuhelp` | Lista todos los comandos con descripción corta |
| `updategpu` | Actualiza desde GitHub conservando la configuración |

## Curva de ventiladores

Con `max_temp`, `min_speed` y `max_speed`:

```
ramp_start = max_temp - 40

curva = 0 → max_speed   entre ramp_start y max_temp
final = max(min_speed, curva)   (sin pasar de max_speed)
```

`min_speed` es solo un suelo; no forma parte de la curva. `max_speed` limita
la velocidad normal. Si se alcanza `critical_temp`, el daemon ignora ambos y
fuerza 100 %.

Ejemplo (`max_temp=100`, `min_speed=25`, `max_speed=100`, `critical_temp=95`):

```
60 °C o menos → 25 %
84 °C         → 60 %
95 °C         → 100 %
```

## Hotspot alto y `critical_temp`

El bloqueo dispara con **`>=`**. Qué sensores puede mirar:

| `use_hotspot_temp` | `safety_all_sensors` | ¿Hotspot fuerza 100 %? |
|--------------------|----------------------|-------------------------|
| 0 | 0 | No |
| 1 | 0 | Sí |
| cualquiera | 1 | Sí |

Si el hotspot llega normalmente a 105 °C y no quieres bloquear:

- `use_hotspot_temp=0` y `safety_all_sensors=0`, o
- `critical_temp=106` o más (rango 50-110).

## Validar la configuración

```bash
set_fan_curve --check-config ~/set-fan-config
```

Detecta typos, claves desconocidas, valores vacíos, no numéricos y fuera de
rango. Muestra los valores efectivos y los sensores exactos de la curva y del
bloqueo. No necesita root y no reinicia nada.

`restartgpu` valida antes de reiniciar y **no aplica** un archivo con errores.
`editargpu` muestra el diff, valida, ofrece volver a editar y solo entonces
pregunta si reiniciar el servicio.

## Prueba manual de ventiladores

```bash
setfan 70        # todos los ventiladores al 70 % hasta cancelar
setfan max       # al 100 %
setfan auto      # cancelar desde otra terminal
setfan status    # ver la prueba activa y las velocidades actuales
```

Mientras la prueba está activa se muestran core, VRAM, hotspot, potencia y
**cada ventilador** (Fan0, Fan1, …) cada 2 s. Al pulsar `Ctrl+C` se borra el
control manual y se muestra un resumen (mínimo / máximo / media). Si se cierra
la sesión SSH, el daemon detecta que el proceso murió y vuelve solo a la curva.

**El bloqueo crítico siempre tiene prioridad**: si la temperatura alcanza
`critical_temp`, el daemon fuerza 100 % aunque la prueba esté fijada por debajo.

## Configuración

`~/set-fan-config` se crea en la instalación y **se conserva** al actualizar.
Cada parámetro lleva su comentario. Los parámetros nuevos se añaden al final.

Claves principales:

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

## Temperaturas reales y sensores de ventilador

La VRAM se lee directamente de los registros PCI del GPU
(`BAR0 + 0xE2A8`), no de NVML. El daemon publica las temperaturas en
`/run/set-fan-curve/temps` y las velocidades por ventilador en
`/run/set-fan-curve/fans` para que `vergpu` funcione sin root.

## Verificar

```bash
vergpu -1
cat /run/set-fan-curve/temps
cat /run/set-fan-curve/fans
systemctl status set-fan-curve
journalctl -u set-fan-curve -n 40 --no-pager
```

## Si un ventilador está al 0 % y el otro funciona

El daemon lo detecta tras 10 s y escribe una alerta:

```bash
journalctl -u set-fan-curve -n 80 --no-pager | grep -i fan
```

Comprueba `cat /run/set-fan-curve/fans`, el conector/cable del ventilador y el
cable de alimentación. Prueba `setfan 100`; si sigue a 0 %, es hardware o
driver, no la curva.

## Si VRAM aparece N/A

1. `systemctl status set-fan-curve`
2. `journalctl -u set-fan-curve -n 50 --no-pager`
3. `ls -l /dev/mem`
4. Si el mmap falla, añade `iomem=relaxed` al kernel y reinicia.

## Si `nvidia-smi` falla con `Unknown Error`

No es la configuración (`min_speed=35` es válido). La GPU dejó de responder en
el bus PCI. Reinicia y revisa el arranque anterior:

```bash
sudo journalctl -k -b -1 --no-pager | grep -iE "xid|nvrm|fallen|pcie|AER"
sudo journalctl -u set-fan-curve -b -1 --no-pager | tail -80
```

- **Xid 79** = GPU caída del bus: alimentación, riser/PCIe o temperatura.
- **Xid 62 / 48** = fallo interno del driver/GPU; requiere reinicio.
- Sin Xid y hotspot > 100 °C: refrigeración de VRAM/pads; baja `power_limit`.

El control de ventiladores no puede hacer que la GPU desaparezca del bus.

## Desinstalar

```bash
sudo ./uninstall.sh   # conserva ~/set-fan-config
```
