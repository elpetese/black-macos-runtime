# Black (Xbox) → Apple Silicon: revisión y plan de corrección

**Fecha:** 19 de septiembre de 2026
**Base:** `BLACK_PORT_ESTADO_2026-09-19.md`
**Qué es este documento:** (1) la verificación, contra el código y los logs, de un análisis externo del estado del port; (2) una revisión de cómo se trabajó el 19/09 y qué se hizo mal; (3) el plan de corrección resultante, ordenado y con criterios de "hecho".

---

## 1. Verificación del análisis externo

Cada afirmación se comprobó contra el código fuente (`xboxrecomp`, rama `macos-port`), el ejecutable del juego y los logs de las ejecuciones del 19/09. Leyenda: ✅ correcta · ⚠️ parcialmente correcta o sin verificar · ❌ incorrecta.

### 1.1 Niveles

| Afirmación | Veredicto | Evidencia |
|---|---|---|
| La misión 1 carga y se juega; "no carga" suele ser otra cosa | ✅ | Pruebas del 19/09: nivel cargado, disparo, recarga, objetivos. |
| El juego ignora el mando ~40 s tras cargar | ✅ observado · ⚠️ causa | Medido en varias pruebas. Que sea una intro "a propósito" **no está verificado**; puede ser la carga del streaming de audio/voz. |
| Arranque inestable ~1 de cada 3 (crash XMV, pila machacada, cuelgue a ~37 s) | ✅ | Documentado; medido antes de los arreglos de hilos del 19/09. **No se ha vuelto a medir después**: varios de esos arreglos (dispatch por hilo, IRQL, precisión FPU) atacan precisamente carreras. |
| Las builds antiguas "cargan" pero se ven vacías | ✅ | Fallo de flags en `sub_00172EB0` (commit `6932e7d`). |
| Misión 2+ no probada; puede faltar código traducido | ⚠️ | Cierto que no está probada. Pero la ICALL feedback **ya existe y está activa** (`BLACK_ICALL_FEEDBACK=ON`): `build-mac/icall_targets.dump` tiene 876 destinos y **0 sin resolver** en todo lo ejecutado. |
| Menú de pausa invisible (panel + texto colapsado en 151,210) | ✅ | Documentado. |

### 1.2 Teclado y cámara

| Afirmación | Veredicto | Evidencia |
|---|---|---|
| El mando emulado no manda ejes sucios en reposo | ✅ | `RECOMP_USB_TRACE`: ejes a 0 en reposo, solo cambian mientras se pulsa. |
| La vista cae tras disparar (recoil) | ⚠️ | Observado en pruebas con script **sin tocar el stick derecho**, así que es un comportamiento del juego y no del mapeo. La causa (recoil) es una **hipótesis** que nadie ha medido todavía. |
| La mira clavada al suelo se debe a que el teclado envía ±1.0 | ❌ | La deriva aparece en ejecuciones donde **nunca se pulsa** el stick derecho. La saturación empeora la sensación, pero no es la causa. |
| Perder el foco puede dejar un eje "pegado" | ✅ **confirmado en código** | `fb_present_posix.m`: la vista Cocoa solo reenvía `keyDown`/`keyUp`; no hay `resignKey`/`windowDidResignKey`. Una tecla pulsada mantiene `s_until = UINT64_MAX` (`keyboard_input.c`) hasta un `keyUp` que no llega si el foco cambió (Cmd-Tab, captura de pantalla…). **Encaja con "se queda dando vueltas":** un eje de giro pegado. |
| El tiempo no está anclado: present desbloqueado, dt explota | ⚠️ **corregido tras medir** | La primera estimación (~50 fps, a partir del contador de la ventana) era errónea. El contador del juego (`[STAT]`, commit `5a78406`) mide **~260 fotogramas/s en menús** (sin límite) y **8–19 en el nivel** (demasiado lento). El tiempo **no** se parece al de la Xbox. Ver P2b y P2c. |
| Redondear según la precisión también en `fst/fstp`, comparaciones, cargas, `fprem`, `fyl2x` | ❌ | En el x87 el control de precisión **solo** afecta a FADD, FSUB(R), FMUL, FDIV(R) y FSQRT (manual de Intel). Un `fstp dword` ya redondea a float por sí mismo, y las comparaciones y cargas no redondean. `fp_pc()` cubre exactamente esas operaciones, incluidas las variantes `fi*`. |
| PS4: permisos de "Monitorización de entrada" y hints HIDAPI | ⚠️ | Posible, pero hay una causa **confirmada en código** más básica: `xinput_device.c` enumera los mandos **una sola vez** al arrancar (`open_controllers()`) y **nadie bombea los eventos de SDL** (no hay `SDL_PumpEvents`/`SDL_PollEvent` en el runtime). En macOS los mandos aparecen de forma asíncrona, así que al enumerar hay 0 y nunca se vuelve a mirar. |

### 1.3 Proceso

| Afirmación | Veredicto | Comentario |
|---|---|---|
| No hay batería de regresión (N arranques automáticos) | ✅ | Es la mayor carencia. |
| exFAT + `._*` es un problema de entorno | ✅ | Ya rompió git y CMake. |
| Demasiados logs por fotograma, que alteran el timing | ✅ | `[NV2A] DMA_PUT`, `[GPU] draw #`, `[FBWIN]` (este último ya silenciado). |
| Sospechar primero del recompilador y del kernel | ✅ | Es lo que mostraron todos los fallos graves del 19/09. |
| La ICALL coverage es manual | ⚠️ | La grabación es automática; lo manual es el paso `merge` → semillas → regenerar. |
| "Golden frames" contra xemu | ✅ idea · ⚠️ coste | Requiere tener xemu con BIOS/MCPX en este Mac; no está instalado. |
| El filtro del panel negro no es un arreglo | ✅ | Correcto, y así está documentado. |

**Conclusión:** el análisis es útil como marco de trabajo y acierta en el proceso (regresión, entorno, logs, orden de ataque) y, tras medirlo, también en que el tiempo no se parece al de la Xbox. Pero tiene errores técnicos concretos: la extensión del redondeo x87 y el teclado ±1 como causa de la deriva. Además no ve los dos fallos reales y comprobables del control: **teclas pegadas al perder el foco** y **SDL sin bombeo de eventos**. Sus comandos de ICALL (`--db … merge`) no coinciden con la herramienta real (sección 5).

---

## 2. Revisión de lo sucedido el 19/09

### 2.1 Lo que funcionó

- **Buscar la causa en el recompilador y el kernel** resolvió los tres fallos graves: los flags sobrescritos (nivel translúcido), la precisión x87 (bloqueo al disparar) y la cola DPC sin cerrojo (mando muerto).
- **El volcado automático de bloqueos** (`RECOMP_HANG_DUMP`) convirtió un "se congela" en una línea de código concreta en **dos ejecuciones**. Antes de tenerlo, el mismo problema consumió muchas más.
- **Las pruebas del recompilador**: se añadieron casos para cada fallo del lifter (93 pruebas en verde).

### 2.2 Lo que se hizo mal (y cuánto costó)

1. **Demasiadas ejecuciones de una sola sonda**, antes de montar el volcado automático. Cada intento hasta el nivel cuesta ~6 min, y muchos se perdieron por:
   - usar `$t:a` en zsh (modificador de ruta), que corrompió los scripts;
   - pulsar Start/A en el script cuando el nivel ya había cargado, lo que pausó el juego y se confundió con "no responde";
   - arrancar tests mientras seguían abiertas partidas anteriores.
2. **Se atacó un síntoma antes que la causa.** El panel negro se ocultó con un filtro; funciona, pero ocultó también que el menú de pausa sigue roto.
3. **Primera hipótesis del bloqueo equivocada** (carrera entre hilos → cerrojo IRQL). El arreglo del IRQL es correcto y se mantiene, pero no era la causa. **Lección:** capturar el estado del hilo atascado **antes** de corregir, no después.
4. **Se diagnosticó "juego congelado" cuando era la ventana** (seguía superficies de 160×120). Un contador de fotogramas del juego en el log lo habría distinguido en un minuto.
5. **No se ha vuelto a medir la estabilidad del arranque** después de los arreglos de hilos. Sigue figurando como "1 de cada 3" sin un dato posterior.
6. **Logs en el scratchpad temporal:** se perdieron los de las pruebas d1–d10 y e1–e2. Los volcados que respaldan una conclusión deben guardarse en el repo (`notes/`).
7. **Se trabajó sobre exFAT** todo el día, con limpiezas manuales de `._*` en cada compilación.

---

## 3. Plan de corrección

Orden pensado para que cada paso haga medibles los siguientes. Cada bloque tiene su criterio de **hecho**.

### Estado de P0 (19/09, tarde) — hecho

| Paso | Commit | Verificado |
|---|---|---|
| P0.1 Soltar teclas al perder el foco; `keyUp` por tecla física | `67bc0b7` | Compila. **Falta probarlo a mano** (Cmd-Tab con una tecla pulsada). |
| P0.2 SDL inicializado y actualizado en el hilo que lee el mando, con búsqueda continua | `1dc1f8c` | SDL ya enumera en ese hilo (log `[INPUT] SDL sees …`). **Falta probarlo con el mando de PS4 conectado.** |
| P0.3 Logs por fotograma detrás de `RECOMP_VERBOSE=1`; línea `[STAT]` cada 10 s | `5a78406` | Prueba hasta el nivel: 132 KB de log en 7 min (antes ~15 MB), sin bloqueos ni crashes. |
| Script de prueba en el repo | `black-recomp 0f3df8e` | `scripts/run-scripted.sh NOMBRE SEGUNDOS level`; logs en `~/black-runs`. |

### Nuevas mediciones con `[STAT]` (cambian las prioridades)

| Fase | Fotogramas del juego/s | Esperado en Xbox |
|---|---|---|
| Vídeos/menús | **~260** | 60 (o 30) |
| Nivel | **~8–19** | 30–60 |

1. **El nivel va muy lento.** Causa probable, confirmada en el código: el stub de memoria modificada de la capa xemu (`compat/xemu_compat_types.h`) responde siempre "modificada". `check_texture_dirty` (`pgraph/gl/texture.c`) vuelve a validar, hashear y convertir cada textura en cada uno de los ~600 dibujos por fotograma. **Nuevo P2b.**
2. **Los menús van sin límite.** El juego presenta ~4 veces más fotogramas que la Xbox, así que la presentación no espera al vblank. Sospechosos: la paridad de campo simulada (`xbox_Nv2aFieldParity`, que podría cambiar en cada lectura) y la espera de vblank de D3D. Cualquier lógica ligada al fotograma corre a otra velocidad. **Nuevo P2c.**

Ambos pueden estar detrás de "el control se siente mal" y de la cámara: con 8 fps y un dt grande, un retroceso integrado por fotograma se comporta distinto.

### P2b — Rendimiento del nivel (seguimiento real de escrituras)

- Marcar como "modificadas" solo las páginas que la CPU escribe: proteger en solo lectura (`mprotect`) las páginas de las texturas ya subidas, y en el manejador de SIGBUS/SIGSEGV (ya existe uno para MMIO) marcar la página y desprotegerla. `memory_region_test_and_clear_dirty` consulta ese mapa de bits.
- Alternativa más barata para empezar: el ~90 % de las texturas del nivel son estáticas. Un contador de generación por página que solo avance con escrituras del guest a la ventana contigua ya reduce el trabajo.
- **Medir antes y después** con `[STAT]` y `sample` durante 10 s en el nivel.
- **Hecho cuando:** el nivel va a ≥30 fotogramas/s.

### P2c — Ritmo de fotogramas como en la Xbox

- Registrar (con `RECOMP_VERBOSE`) cuándo D3D espera el vblank y qué lee (paridad de campo, contador de vblank, `KeWaitForSingleObject` sobre el evento de vblank).
- Hacer que la paridad solo cambie con el vblank real (60 Hz) y no en cada lectura; comprobar que los vídeos XMV siguen funcionando, ya que dependen de ella.
- **Hecho cuando:** los menús van a 60 fotogramas/s y los vídeos a su velocidad.

### P0 — Arreglos confirmados y baratos (menos de 1 h cada uno)

**P0.1 Soltar todas las teclas al perder el foco**
- Dónde: `src/video/fb_present_posix.m` (la vista de teclado) y `src/input/keyboard_input.c`.
- Qué: añadir `xbox_KeyboardReleaseAll()` (poner `s_until[i] = 0` para todos los slots) y llamarla desde `resignFirstResponder` de la vista y desde `windowDidResignKey:` del delegado de la ventana.
- Hecho cuando: pulsar L, hacer Cmd-Tab y volver no deja la vista girando, y `RECOMP_USB_TRACE` muestra los ejes a 0.

**P0.2 Detectar mandos en caliente (PS4)**
- Dónde: `src/input/xinput_device.c`.
- Qué: en `xbox_InputGetState` (o en un temporizador), llamar a `SDL_PumpEvents()` y, si `SDL_NumJoysticks()` cambió, volver a ejecutar `open_controllers()`. Activar `SDL_HINT_JOYSTICK_HIDAPI` y `SDL_HINT_JOYSTICK_HIDAPI_PS4` antes de `SDL_InitSubSystem`. Registrar en el log el nombre y el GUID de cada mando.
- Si sigue a 0: revisar el permiso de Monitorización de entrada del terminal y probar el mando con otra app SDL2.
- Hecho cuando: el log muestra ≥1 mando y sus ejes en reposo a 0.

**P0.3 Silenciar los logs por fotograma**
- `[NV2A] DMA_PUT`, `[GPU] draw #`, `[GPU] transform`: moverlos detrás de `RECOMP_NV2A_TRACE` / `RECOMP_PB_EXEC_VERBOSE`.
- Hecho cuando: una partida de 10 minutos genera menos de 1 MB de log.

### P1 — Entorno y regresión

**P1.1 Salir de exFAT.** Copiar `xboxrecomp` y `black-recomp` a APFS (disco interno o una imagen sparse APFS en el disco externo). Mantener el filtro de `._*` como red de seguridad.

**P1.2 `rebuild-black.sh`**: borrar `._*`, ejecutar las pruebas del lifter (abortar si fallan), regenerar y compilar.

```sh
cd porting/xboxrecomp
python3 -m unittest discover -s tools/recomp -p 'test_*.py' -t . || exit 1
python3 -m tools.recomp game_files/default.xbe --game-name Black --all --split 1000 \
    --gen-dir ../../black-recomp/src/recomp/gen
cd ../../black-recomp && make -C build-mac -j8 black-recomp
```

**P1.3 Batería de arranques `boot-test.sh`**: N arranques de 120 s con `RECOMP_HANG_DUMP=8`, `RECOMP_FB_SNAP` y un script que pulse Start. Clasificar cada uno (menú alcanzado / crash XMV / pila machacada / cuelgue) leyendo el log, y guardar los volcados de los fallos en `notes/boot-fails/`. Varias copias en paralelo (3 caben bien en este Mac).

**P1.4 Contador de fotogramas del juego** en el log (1 línea cada 10 s: flips de xemu y fps). Así se distingue "juego parado" de "ventana parada".

**Hecho cuando:** `boot-test.sh 20` da una cifra, y esa cifra es la línea base de P2.

### P2 — Estabilidad del arranque (vídeos)

1. **Medir primero** con P1.3: los arreglos del 19/09 pueden haber cambiado la tasa.
2. Para cada tipo de fallo restante:
   - **Crash al final de un XMV** (`sub_0013DD20` vía `0x140D90`, dirección `0x9xxxxxxx`): volcar el objeto del decodificador con los registros en vivo; buscar un uso tras liberar o una lectura de un puntero sin inicializar. Revisar la traducción de esa función con el mismo método que `sub_00172EB0` (flags, precisión, `sbb/adc`).
   - **`esp` = puntero del heap en `sub_0020F6A0`**: comprobar el balance de pila de las llamadas indirectas cercanas (ya existe `RECOMP_ABI_CALL` con comprobación de violaciones) y los tamaños de argumentos stdcall de las llamadas al kernel de ese tramo (el precedente es `NtQueryDirectoryFile`, 9 → 10).
   - **Cuelgue sin crash**: ya lo vuelca `RECOMP_HANG_DUMP`; leer qué espera cada hilo.
3. Ante un fallo del lifter, añadir **una prueba** antes de regenerar.

**Hecho cuando:** ≥19/20 arranques llegan al menú y cada fallo restante tiene su volcado en `notes/`.

### P3 — Cámara que cae tras disparar

1. **Medir**, con una sola ejecución: script que llega al nivel, espera a "RENDEZVOUS", dispara una vez y no toca nada durante 10 s, y luego una ráfaga de 10 disparos. Con `RECOMP_FB_SNAP` y un volcado periódico del objeto del jugador (localizar su dirección con el volcado de registros del hilo principal durante el disparo).
2. Localizar la escritura del ángulo vertical (float en el objeto del jugador) y la función que la hace.
3. Revisar su traducción: flags, `fp_pc`, `sbb/adc`, `js` en 8/16 bits, conversiones `cvt*`/`fistp`, y el dt que usa (el juego lee `KeQueryPerformanceCounter` / `KeTickCount`; comprobar que las frecuencias son las que espera).
4. Prueba del lifter para el caso encontrado, regenerar y repetir el paso 1.

**No hacer:** un límite "si el ángulo pasa de X, resetear". Esconde el fallo.

**Hecho cuando:** tras un disparo sin tocar el stick, el ángulo vuelve a su valor (±0,5°), y tras 10 disparos la vista no acaba en el suelo.

### P4 — Teclado más jugable (calidad, no la causa)

- Stick derecho con rampa: al pulsar, subir a ~0,5 del máximo en ~150 ms; al soltar, volver a 0 en ~80 ms. El movimiento (WASD) puede seguir al máximo.
- Opcional: ratón → stick derecho, con la ventana capturando el cursor.
- Hecho cuando: se recorre la primera calle con teclado apuntando con precisión.

### P5 — Menús: panel negro y pausa invisible

1. `RECOMP_XEMU_DRAWLOG` en un fotograma de menú y en uno de pausa; identificar el dibujo del panel y los quads de texto colapsados.
2. Seguir hacia atrás desde el flusher de lotes `0xD4C50` / buffer `0x563120` hasta la función que decide visibilidad y posición.
3. Revisar su traducción igual que en P3. Añadir la prueba del lifter.
4. Cuando el panel desaparezca solo y el texto se vea, quitar el filtro por defecto (`RECOMP_KEEP_BLACK_PANEL` pasa a ser innecesario).

**Hecho cuando:** la pausa se ve sin filtro.

### P6 — Resto de misiones

Ciclo real con la herramienta existente (`tools/recomp/icall_feedback.py`):

```sh
# 1. Compilar con BLACK_ICALL_FEEDBACK=ON (ya lo está en build-mac) y jugar la misión nueva.
#    Al salir o en un crash se escribe build-mac/icall_targets.dump.
cd porting/xboxrecomp
python3 -m tools.recomp.icall_feedback merge ../../black-recomp/build-mac/icall_targets.dump
python3 -m tools.disasm --seed-functions tools/recomp/output/icall_targets.json   # + semillas de black-recomp/seeds
# 2. Regenerar (P1.2) y repetir hasta que el volcado no tenga destinos con flag 2/3 (sin resolver).
```

Antes de ejecutar, comprobar las opciones exactas con `python3 -m tools.recomp.icall_feedback --help`, y combinarlas con `black-recomp/seeds/hand_seeds.json` como en la regeneración actual.

**Hecho cuando:** cada misión probada carga, o falla con un destino ICALL o un volcado de bloqueo con nombre, nunca con una pantalla negra muda.

### P7 — Calidad de los vídeos

`RECOMP_FMV_DUMP` sobre un fotograma decodificado. Sin xemu disponible, comparar con los valores esperados del propio formato: los bloques DC-only indican coeficientes AC perdidos. Revisar la traducción MMX/SSE del IDCT (saturaciones `packuswb`, `pmulhw`, desplazamientos).

### P8 — Audio

- Confirmar de oído el remuestreo (`2d4e619`).
- Después: interpolación de mejor calidad y mezcla de los mixbins según el DSP (hoy es passthrough: bins 0–15 izquierda, 16–31 derecha).

---

## 4. Matriz por sesión

```
[ ] pruebas del lifter en verde
[ ] regenerado y compilado desde rebuild-black.sh (sin ._*)
[ ] boot-test: ___/20 al menú (volcados en notes/)
[ ] nivel: "RENDEZVOUS" visible, el mando responde después
[ ] 1 disparo sin tocar el stick → el ángulo vuelve
[ ] Cmd-Tab con una tecla pulsada → nada queda pegado
[ ] log de 10 min < 1 MB
[ ] volcados que respaldan conclusiones guardados en notes/
```

## 5. Correcciones a los comandos del análisis externo

- **`RECOMP_ICALL_FEEDBACK=1` como variable de entorno no existe:** es la opción de compilación `BLACK_ICALL_FEEDBACK=ON`, ya activa en `build-mac`.
- **`icall_feedback --db … merge` / `seeds --out … --align 16`:** la herramienta documenta `merge <fichero>` y escribe `tools/recomp/output/icall_targets.json`, que se pasa a `tools.disasm --seed-functions`. Confirmar con `--help` antes de usarlo.
- **`timeout 120`** no existe en macOS por defecto (`gtimeout` de coreutils, o un bucle con `kill`, como en los scripts de prueba usados el 19/09).
- **`SDL_Init(SDL_INIT_VIDEO | …)`:** no conviene, porque la ventana es Cocoa propia. Basta con `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)` más los hints y el bombeo de eventos (P0.2).

---

## 6. Hallazgos de las mediciones m1–m2 (19/09, noche)

Dos ejecuciones largas (`black-recomp/scripts/diag-level.sh`), cada una hasta el nivel y con un perfil de CPU de 15 s en el nivel **sin reiniciar el juego**. Logs y perfiles en `~/black-runs/m1.*` y `~/black-runs/m2.*`.

| Medida | Antes | Después | Cambio |
|---|---|---|---|
| Menús / vídeos | ~260 fps | **60,0 fps** | `FLIP_STALL` espera al siguiente tick de 60 Hz (`fb87d22`) |
| Nivel | ~8 fps | **~14 fps** | `getenv` una vez por sitio y sin recorrer la caché de texturas (`b6f991b`) |

### Dónde se va el tiempo en el nivel (perfil m2)

1. **Hilo principal del juego, ~40 %: esperando el vaciado del búfer de escritura en cada KickOff.** *(Corregido el 20/09 tras leer el código; la explicación anterior, "espera a la GPU ociosa por el flag `[0x279B48]`", era **incorrecta**.)*
   - `[0x279B48]` es la entrada 128 de la tabla de estados de textura de D3D (`0x279948 + (etapa×32+estado)×4`, 4 etapas × 32 estados). Solo se escribiría con la etapa 4 o un estado ≥ 32, y ningún llamador estático lo hace. Ese bucle no se ejecuta.
   - El tiempo real está en `sub_0026D0C0`, que el compilador integró en `sub_0026CF10`: activa el bit `0x10000` de `NV_PFB_WBC` (`0xFD100410`) y **espera a que la GPU lo borre**. En el port lo borraba `nv2a_ack_thread` **entre tramos del pushbuffer**, y ese mismo hilo dibuja con xemu, así que cada KickOff esperaba a que la GPU terminara el tramo anterior: **CPU y GPU en serie.**
   - **Corrección** (pendiente de medir): los bits "ocupado" (`NV2A_ACK`) se borran desde un hilo propio cada ~20 µs; los de interrupción siguen como antes.
2. **Hilo temporizador, 68–74 %: el DPC de vblank de D3D girando** (`sub_00273C40`). Escribe 1 en `PCRTC_INTR_0` y espera a que baje el bit 24 de `PMC_INTR_0`, que en el port solo se borraba con el tick periódico del hilo de la GPU. Corregido en `f66f63d` (se borran tras el ISR); **pendiente de medir**.
3. **Hilo de la GPU: ya es trabajo real de OpenGL.** `glDrawElements` 28 %, `glBufferSubData` 10 % (subida de vértices por dibujo), `beginRenderPass` 8 % (la capa OpenGL→Metal de Apple abre pases de render a menudo), `bind_vertex_attributes` 10 %. Mejorarlo requiere agrupar dibujos y evitar subidas repetidas, un trabajo más largo que conviene dejar para después de los puntos 1 y 2.
4. **Audio:** el remuestreo nuevo cuesta ~4,6 % de su hilo. No es un problema.

### Próxima ejecución (una sola, con todo)

```sh
cd black-recomp
RECOMP_KERNEL_WATCH=0x279B48 RECOMP_KERNEL_WATCH_ALL=1 scripts/diag-level.sh m3 600
python3 scripts/profile-summary.py ~/black-runs/m3.profile_level.txt 30
```

Confirma a la vez la corrección del vblank (punto 2) y quién activa la espera de GPU ociosa (punto 1).

---

## 7. Auditoría estática del 20/09 (sin ejecutar el juego)

Revisión sistemática del ejecutable y del C generado, por **clases** de error (las que ya habían causado fallos), no caso a caso.

| Clase | Qué se buscó | Resultado |
|---|---|---|
| 1. Argumentos de las llamadas al kernel | Las 89 funciones del kernel que usa Black, contra sus prototipos reales | **Todas correctas.** Las entradas "sin tabla" son exportaciones de datos (`XboxHardwareInfo`, `LaunchDataPage`…). |
| 2. Esperas sobre registros de hardware (apertura = RAM) | Todos los bucles cortos que leen registros NV2A | Las esperas de "GPU ociosa" terminan al instante (la RAM da 0 = ocioso). **Problemas reales:** el KickOff (`0x100410`) y el vblank (`PMC_INTR_0`/`PCRTC_INTR_0`), más el canal de interrupciones por software (abajo). |
| 3. Flags que cruzan funciones | Saltos que usan flags del llamador o de la función llamada | 2 casos reales, ambos en la librería matemática de C (`0xA3F09`/`0xA3FD5` acos/asin, `0xA35A4`): usan flags de `0xA6F6C`/`0xA6F29` (exponente Inf/NaN). **Solo afecta a Inf/NaN.** Pendiente (baja prioridad): soporte en el lifter. |
| 4. Instrucciones sin traducir en código real | `TODO` del lifter en funciones referenciadas | La mayoría son datos desensamblados como código. Reales: `cli`/`sti` en DirectSound (`0x280D9F`, una lectura cortísima), `ldmxcsr`/`stmxcsr` (`0x2B480`, modo SSE, efecto menor), y un parche de XAPI al kernel (`0x26F0D`) que no aplica al port. **Nada grave.** |

### Hallazgo principal: los registros de interrupción de la GPU no tenían la semántica del hardware

- En la Xbox, `PGRAPH_INTR`, `PCRTC_INTR_0`, `PFIFO_INTR_0` y `PBUS_INTR_0` se borran **escribiendo 1**, y `PMC_INTR_0` es el **resumen** de los pendientes de cada unidad. En el port eran RAM y un hilo los ponía a 0 periódicamente. Consecuencias:
  - el DPC de vblank de D3D gira hasta ese borrado periódico (68–74 % de un hilo);
  - borrarlos antes de tiempo rompió el arranque (la ventana en blanco del 20/09).
- **Las interrupciones por software de la GPU se descartaban.** D3D usa `NO_OPERATION` con parámetro para que la GPU le avise. El manejador del juego (`0x273EA0` → `0x2740F0`, tabla de saltos en `0x274304`) hace:
  - **1:** contabilidad del Swap y callback de swap;
  - **5:** `KeSetEvent` del evento de espera de fences. `sub_0026CC10` espera ese evento **sin límite de tiempo** cuando la GPU va lejos: **candidato a los cuelgues**;
  - **12/13:** `InsertCallback`;
  - **8:** escritura de registros RDI (el `0x28` del arranque).
- **Corrección** (commit de este apartado, **pendiente de ejecutar**):
  - las tres páginas de 16 KB (`0xFD000000`, `0xFD400000`, `0xFD600000`) se interceptan y se sirven desde una copia con semántica real: borrado al escribir 1 y `PMC_INTR_0` calculado;
  - el vblank activa su bit con `xbox_Nv2aIntrRaise` y queda pendiente hasta que el juego lo reconoce;
  - los `NO_OPERATION` con parámetro se pasan al ISR del juego desde el hilo temporizador, que se despierta al momento; la GPU espera el reconocimiento (máximo 200 ms, después se descarta con aviso);
  - el hilo rápido de bits "ocupado" ya **no** toca los registros de interrupción. Lo hacía con la tabla anterior y habría repetido el fallo de la ventana en blanco.
  - Desactivable con `RECOMP_NV2A_IRQ=0` (vuelve al comportamiento anterior).

### Otras observaciones

- Crash al final de los vídeos (`sub_0013DD20`): recorre un array de objetos de sonido (`[ebp+i*8]`, contador `[ebp+0x1C]`) y lee un puntero basura. Con el código solo no se distingue entre una entrada no inicializada y una modificada por otro hilo. Varios arreglos de hilos del 19–20/09 atacan ese tipo de carrera; **medir**.

### Verificación pendiente (una sola ejecución)

```sh
cd black-recomp && scripts/diag-level.sh v1 600
# esperar: log con "NV2A interrupt registers: emulated"; ningún "[HANG]";
# [STAT] del nivel > 14 fps; perfil sin sub_00273C40 ni sub_0026CF10 dominando
```
Si algo falla: `RECOMP_NV2A_IRQ=0` aísla el cambio de los registros; el hilo de bits "ocupado" es independiente.

---

## 8. Verificación v2 (20/09): una ejecución, todo registrado

Ejecución única con `scripts/diag-level.sh v2 600` (perfil de menús y de nivel, volcado de bloqueos, `[STAT]` cada 10 s, trazas USB/APU, capturas). Logs y perfiles en `~/black-runs/v2.*`.

**Antes de v2 hubo una v1 con pantalla negra.** El volcado de v1 la explicó sin repetir: `sub_0026CBA0` (la espera de fence de D3D) sondeaba `PATT_COLOR0` (`0xFD400B10`), que la GPU rellena con el método de fence de D3D (`NV044_SET_MONOCHROME_COLOR0`, subcanal 5) y que la apertura nunca reflejaba. Lo mismo pasaba con `COLORCLEARVALUE`/`ZSTENCILCLEARVALUE`, que usa el aviso 9. **Corrección `07c56ed`:** esos tres registros se sirven desde xemu.

| Comprobación | v2 |
|---|---|
| Arranque, menús y vídeos | 60 fps estables |
| Nivel | alcanzado; **~15,5 fps** |
| Bloqueos / crashes | 0 / 0 |
| Destrucción (`DESTRUCT.bks`) | 4 cargas sin congelarse |
| Avisos por software entregados | ~35.000 (Swap 1: ~22.000; fence 5: ~12.900; 8 y 9: 13), **0 sin reconocer** |

**Perfil del nivel:**
- **Hilo temporizador:** 98 % en reposo. El giro del DPC de vblank ha desaparecido.
- **Hilo principal:** 92 % **esperando** el evento de fence (`sub_0026CC10` → `KeWaitForSingleObject`), en reposo. La CPU va por delante y espera a la GPU, como en el hardware.
- **Hilo de la GPU:** 100 %, **único cuello de botella**. Casi todo es la capa OpenGL→Metal de Apple: `glDrawElements` 23 %, `glBufferSubData` 7 %, pases de render y *command buffers* ~15 %, `glReadPixels` de superficies 2 %.

**Siguiente trabajo de rendimiento** (renderizador, proyecto aparte):
1. evitar la subida de vértices por dibujo (buffers persistentes);
2. no descargar superficies a la CPU salvo que el juego las lea;
3. agrupar dibujos con el mismo estado;
4. alternativa de fondo: el renderizador Vulkan de xemu sobre MoltenVK.

**Pendiente, sin cambios:** PTIMER (`0xFD009400`) no se actualiza, así que las estadísticas de tiempo de D3D salen a 0 (no bloquea). Flags entre funciones en la librería matemática (solo Inf/NaN). Crash al final de vídeos (`sub_0013DD20`): no apareció en v2; medir en varias ejecuciones.
