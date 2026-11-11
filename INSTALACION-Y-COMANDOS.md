# GPU Fan Curve Controller — instalación y comandos

Guía en español para instalar y administrar GPU Fan Curve Controller.

> **Estado:** v1.9 experimental. Incluye consulta de procesos GPU en `vergpu`.
> No se ha validado en hardware NVIDIA real desde este entorno.

## 1. Requisitos

- Linux con systemd.
- GPU NVIDIA y controlador NVIDIA instalado.
- Acceso a `libnvidia-ml.so.1`.
- `gcc`, `g++`, `make`, `curl` y `tar`.
- Permiso root para instalar el daemon y acceder a `/dev/mem`.

Comprobar controlador y GPU:

```bash
nvidia-smi
```

## 2. Instalación desde GitHub

Actualizar índices de paquetes y herramientas necesarias en Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y gcc g++ make curl tar
```

Descargar e instalar versión más reciente:

```bash
curl -fsSL https://raw.githubusercontent.com/todocryptomining-sudo/gpu-set-fan-power/main/releases/v1.9/install.sh -o /tmp/install-gpu-fan.sh
chmod +x /tmp/install-gpu-fan.sh
sudo /tmp/install-gpu-fan.sh
```

Qué hace cada comando:

- `curl`: descarga el instalador desde GitHub.
- `chmod +x`: permite ejecutar el instalador.
- `sudo`: otorga permisos para compilar, instalar el servicio y acceder a recursos del sistema.
- `install.sh`: compila `set_fan_curve` y `vergpu`, instala scripts, crea el servicio systemd y conserva la configuración existente.

También puede actualizarse mediante el comando incluido:

```bash
sudo updategpu
```

`updategpu` descarga la rama `main`, busca la carpeta `releases/v*` más reciente y ejecuta su instalador. Conserva `~/set-fan-config`.

Consultar versión disponible sin instalar:

```bash
updategpu --check
```

## 3. Configuración

Archivo principal:

```text
~/set-fan-config
```

Abrir configuración con validación y confirmación:

```bash
editargpu
```

`editargpu`:

1. Abre el archivo con `EDITOR`, `VISUAL`, `nano`, `vim` o `vi`.
2. Muestra cambios respecto a la copia anterior.
3. Valida claves y valores.
4. No reinicia si encuentra errores.
5. Pregunta antes de aplicar y reiniciar el servicio.

Usar otro archivo de configuración:

```bash
editargpu /ruta/a/otra-configuracion
```

Validar sin reiniciar ni modificar el servicio:

```bash
set_fan_curve --check-config ~/set-fan-config
```

Reiniciar después de validar:

```bash
sudo restartgpu
```

Guardar copia de configuración:

```bash
gpusave
```

Guardar copia con nombre:

```bash
gpusave antes-de-cambiar-temperaturas
```

### Parámetros principales

```text
max_temp=85          # temperatura objetivo para la curva normal
min_speed=40         # mínimo de la curva automática
max_speed=100        # máximo de la curva normal
critical_temp=93     # umbral de bloqueo térmico; fuerza 100 %
use_vram_temp=1      # usar temperatura real de VRAM
use_hotspot_temp=0   # usar hotspot en curva y bloqueo según política
persistence=1        # activar persistence mode
power_limit=0        # 0 desactiva cambio de límite de potencia
```

La curva automática empieza en `min_speed` y sube hasta `max_speed`. Si se alcanza `critical_temp`, el bloqueo térmico fuerza 100 %, aunque el valor configurado sea menor.

Las pruebas manuales `setfan` aceptan 0–100 % sin limitación por `min_speed` o `max_speed`. El bloqueo térmico crítico conserva prioridad.

## 4. Monitorización

Monitor en vivo:

```bash
vergpu
```

`vergpu` muestra temperaturas, uso GPU, VRAM, potencia, relojes, ventiladores y, al final de cada GPU, procesos visibles mediante NVML:

- PID.
- Memoria GPU utilizada.
- Tipo (`C` compute o `G` graphics), si el driver lo informa.
- Nombre del proceso.

Una sola captura:

```bash
vergpu -1
```

Ocultar procesos:

```bash
vergpu --no-processes
```

Durante el monitor:

- `p`: mostrar u ocultar procesos.
- `b`: mostrar u ocultar barras.
- `c`: activar o desactivar colores.
- `+` / `-`: aumentar o reducir intervalo de actualización.
- `q`: salir.

La lista puede indicar `Sin procesos visibles` o `Consulta no disponible (driver o permisos)`. Esto depende del driver, permisos, contenedor y tipo de proceso.

Estado rápido:

```bash
gpustatus
```

Ayuda de comandos:

```bash
gpuhelp
```

## 5. Pruebas manuales de ventiladores

Fijar todos los ventiladores a un porcentaje:

```bash
setfan 70
```

Probar 0 %:

```bash
setfan 0
```

Fijar 100 %:

```bash
setfan max
```

Ver prueba y velocidades actuales:

```bash
setfan status
```

Cancelar prueba y volver a curva automática:

```bash
setfan auto
```

El bloqueo térmico crítico puede elevar la velocidad a 100 % durante una prueba manual. Si se cierra la sesión o muere el proceso de prueba, el daemon vuelve a la curva automática.

## 6. Servicio systemd

Ver estado:

```bash
systemctl status set-fan-curve
```

Iniciar:

```bash
sudo systemctl start set-fan-curve
```

Detener:

```bash
sudo systemctl stop set-fan-curve
```

Reiniciar:

```bash
sudo systemctl restart set-fan-curve
```

Activar al arrancar el sistema:

```bash
sudo systemctl enable set-fan-curve
```

Ver registros recientes:

```bash
journalctl -u set-fan-curve -n 80 --no-pager
```

Seguir registros en tiempo real:

```bash
journalctl -u set-fan-curve -f
```

## 7. Archivos de estado

El daemon publica información para que `vergpu` funcione sin root:

```bash
cat /run/set-fan-curve/temps
cat /run/set-fan-curve/fans
cat /run/set-fan-curve/state
cat /run/set-fan-curve/fanstate
```

- `temps`: temperaturas VRAM y hotspot por GPU.
- `fans`: ventiladores detectados y velocidades reportadas.
- `state`: estado operativo de cada GPU y sensores.
- `fanstate`: velocidad solicitada, aplicada y reportada por ventilador.

## 8. Desinstalación

```bash
sudo ./uninstall.sh
```

La desinstalación elimina binarios y servicio. La configuración `~/set-fan-config` se conserva para permitir reinstalación o rollback.

## 9. Configuración en español para futuras versiones

La próxima versión traducirá al español los comentarios, títulos y explicaciones del archivo de configuración que abre `editargpu`.

Por compatibilidad, los nombres técnicos de las claves (`max_temp`, `min_speed`, `critical_temp`, etc.) deben mantenerse sin traducir, salvo que se implemente una migración explícita. Así las configuraciones existentes seguirán funcionando durante las actualizaciones.
