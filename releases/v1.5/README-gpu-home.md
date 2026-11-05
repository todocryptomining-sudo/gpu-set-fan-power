# GPU Fan Curve Controller — Inicio rápido (v1.5)

> Ejecuta `gpuhelp` para ver todos los comandos disponibles.

## Instalación

```bash
tar -xzf gpu-fan-curve-v1.5.tar.gz
cd v1.5
chmod +x install.sh uninstall.sh editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu
sudo ./install.sh
```

## Comandos

| Comando | Uso |
|---------|-----|
| `vergpu` | Monitor en vivo: Core / VRAM / Hotspot |
| `vergpu -1` | Una sola captura y salir |
| `editargpu` | Editar `~/set-fan-config` |
| `restartgpu` | Aplicar cambios de configuración |
| `gpustatus` | Estado rápido |
| `gpusave` | Guardar copia de la configuración |
| `setfan 70` | Prueba manual: fija los ventiladores al 70 % |
| `setfan max` | Prueba manual al 100 % |
| `setfan auto` | Cancela la prueba y vuelve a la curva |
| `setfan status` | Muestra si hay una prueba activa |
| `gpuhelp` | Lista todos los comandos con descripción corta |
| `updategpu` | Actualiza desde GitHub conservando la configuración |

## Prueba manual de ventiladores

```bash
setfan 70        # todos los ventiladores al 70 % hasta cancelar
setfan max       # al 100 %
setfan auto      # cancelar desde otra terminal
setfan status    # ver la prueba activa
```

Mientras la prueba está activa se muestran core, VRAM, hotspot, potencia y la
velocidad real cada 2 s. Al pulsar `Ctrl+C` se borra el control manual y se
muestra un resumen (mínimo / máximo / media). Si se cierra la sesión SSH, el
daemon detecta que el proceso murió y vuelve solo a la curva.

**El bloqueo crítico siempre tiene prioridad**: si la temperatura alcanza
`critical_temp`, el daemon fuerza 100 % aunque la prueba esté fijada por debajo.

## Configuración

`~/set-fan-config` se crea en la instalación y **se conserva** al actualizar.
Cada parámetro lleva su comentario. Los parámetros nuevos se añaden al final.

Claves principales:

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

## Temperaturas reales

La VRAM se lee directamente de los registros PCI del GPU
(`BAR0 + 0xE2A8`), no de NVML. El daemon las publica en
`/run/set-fan-curve/temps` para que `vergpu` funcione sin root.

## Verificar

```bash
vergpu -1
cat /run/set-fan-curve/temps
systemctl status set-fan-curve
journalctl -u set-fan-curve -n 40 --no-pager
```

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
