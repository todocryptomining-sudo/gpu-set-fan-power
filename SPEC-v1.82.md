# Especificación v1.82 — diagnóstico de ventiladores y configuración en español

## Objetivo

Crear v1.82 a partir de v1.81 sin romper soporte de RTX 3090 ni GPUs nuevas. Corregir diagnóstico falso en GPUs con varios ventiladores/sensores y mejorar instalaciones nuevas con configuración comentada en español.

## Alcance

1. Diagnóstico pasivo de ventiladores durante instalación.
2. Consulta opcional de RPM mediante `hwmon/sysfs`, sin asumir que `0 %` equivale a `0 RPM`.
3. Corrección de alerta falsa de `vergpu` para GPUs multi-ventilador.
4. Ordenación segura de configuración existente, sin cambiar valores ni sobrescribir el archivo original.
5. Nueva plantilla de configuración en español para instalaciones desde cero.
6. Compatibilidad conservadora: perfiles MMIO conocidos se mantienen; GPU no reconocida no recibe lecturas MMIO inventadas.

## Decisiones y límites

- `~/set-fan-config` existente no se modifica automáticamente durante actualización.
- La ordenación genera archivo nuevo o copia de respaldo; nunca reemplaza silenciosamente configuración existente.
- Los valores se conservan literalmente, incluidos comentarios desconocidos y claves futuras.
- Claves técnicas (`max_temp`, `min_speed`, `critical_temp`, etc.) permanecen en inglés para compatibilidad.
- Instalación ejecuta diagnóstico pasivo, sin cambiar velocidad de ventiladores.
- Prueba activa que cambie ventiladores queda fuera de instalación automática.
- RPM solo se muestran si fuente `hwmon/sysfs` es detectable y legible.
- `0 %` de un índice individual no confirma `0 RPM`.
- Alerta global solo aparece cuando evidencia indica condición real o todos los canales reportan cero; nunca se declara RPM física sin lectura RPM.
- No se modifican curva, bloqueo crítico ni control manual 0–100 %.

## Interfaz propuesta

Comandos nuevos o ampliados:

```bash
sudo gpu-fan-diagnostic
```

Diagnóstico pasivo: GPU, índices NVML, lecturas repetidas, RPM hwmon disponibles y clasificación tentativa.

```bash
gpusave --ordenada
```

Genera una copia ordenada sin tocar `~/set-fan-config`. El nombre exacto de salida debe mostrarse al usuario.

```bash
editargpu --ordenar
```

Ordena una copia temporal/nueva, muestra diff, valida y solo pregunta antes de instalarla. Sin confirmación, no cambia nada.

La forma final de estos comandos puede conservar otro nombre si encaja mejor con scripts actuales, pero debe cumplir la misma protección.

## Orden de configuración

La plantilla nueva y la copia ordenada usarán bloques:

1. Encabezado y advertencias.
2. Temperaturas y sensores.
3. Curva automática.
4. Protección crítica.
5. Ventiladores y temporización.
6. Potencia.
7. Recuperación y comportamiento ante errores.
8. Rutas/diagnóstico.

La ordenación debe:

- conservar comentarios del usuario cuando sea posible;
- conservar claves desconocidas en sección final de compatibilidad;
- conservar duplicados y avisar, sin elegir silenciosamente otro valor;
- validar antes de ofrecer aplicación;
- escribir permisos/propietario seguros.

## Compatibilidad de sensores

- NVML sigue siendo fuente genérica para porcentaje de ventilador.
- `hwmon/sysfs` se consulta solo lectura para RPM.
- GPU con varios índices se muestra por canal.
- Sensor lógico, no soportado o sin RPM se marca como `N/D` o `no concluyente`.
- RTX 3090 conserva perfiles GA102 existentes y lectura MMIO actual.
- GPUs no reconocidas usan fallback NVML/Core disponible; no se adivinan offsets VRAM/hotspot.

## Pruebas

Antes de publicar:

- Compilación de daemon y `vergpu`.
- Pruebas existentes de daemon/MMIO sin hardware.
- Pruebas de ordenación: valores idénticos, comentarios, claves desconocidas y duplicados.
- Pruebas de alerta: un canal 0 + otro activo no genera falsa alarma global.
- Pruebas de diagnóstico sin `hwmon` y con entradas simuladas.
- Verificación de diff limpio, paquete reproducible y SHA-256.
- Declarar expresamente que no hubo validación física si no hay GPU disponible.

## Estructura prevista

```text
releases/v1.82/
  gpu-fan-diagnostic
  set-fan-config.example
  vergpu.cpp
  set_fan_curve.cpp
  install.sh
  editargpu
  gpusave
  tests/...
dist/gpu-fan-curve-v1.82.tar.gz
```

## Criterios de éxito

- Instalación nueva crea configuración comentada en español.
- Actualización existente no cambia valores ni sobrescribe configuración.
- Usuario puede generar configuración ordenada y revisar diff antes de aplicar.
- `vergpu` distingue porcentaje NVML de RPM real.
- Caso `Fan0=0 %`, `Fan1=51 %` no produce alerta roja global de `0 RPM`.
- RTX 3090 conserva funcionamiento y perfiles existentes.
- GPU no reconocida no recibe acceso MMIO inseguro.
- Release v1.82 queda en `C:\OPENCODE\Set Fan NVML\releases\` y GitHub como experimental si falta hardware real.

## Preguntas abiertas

- Confirmar si el usuario prefiere que `editargpu --ordenar` solo genere copia o también ofrezca reemplazar tras confirmación explícita. Esta especificación adopta la opción segura: generar copia y pedir confirmación separada.
