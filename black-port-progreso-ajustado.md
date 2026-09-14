# Port de Black (Xbox) a Apple Silicon macOS

**Documento único de referencia.** Última actualización: 14 de septiembre de 2026.
Reemplaza a `black-port-progreso-fase6.md` y a todas las versiones anteriores.

Plan operativo Fase 7: `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md`.

---

## Resumen en una frase

**Fase 6 cerrada** (con `RECOMP_AC97_READY=1`; commits `a6d1625`, `321c102`, `cd31faa` en xboxrecomp y `d192d5a` en black-recomp). El título
arranca, pasa DirectSound, descarga la imagen DSP, carga el front end completo
(52+ aperturas), ejecuta su bucle de frame enviando trabajo a la GPU, reproduce
los vídeos de introducción y llega a la **pantalla de título**, donde alterna con el modo demo esperando al mando.
Hay una **ventana Metal en pantalla** con el rasterizador software mostrando la evolución del frontend y el título.
Audio: pipeline SDL2 funcionando; falta el DSP GP/EP para audio audible.

Fase 7 activa según `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md`: F7.0 instrumentación → F7.1 audio host → F7.2 ruta gráfica → F7.3 primer draw → F7.4 present → F7.5 texturas → F7.6 input → F7.7 primer nivel.

---

## 1. Estado por fases

| # | Fase | Estado | Notas |
|---|------|--------|-------|
| 1 | Repo base y parche macOS | ✅ | |
| 2 | Recompilación de Black | ✅ | 13.758 funciones, 0 fallos, 70 stubs sin resolver |
| 3 | Build del runtime | ✅ | `black-recomp` enlaza y corre |
| 4 | Validación de memoria | ✅ | `test_guest_layout` en verde |
| 5 | Smoke test | ✅ | |
| 6 | Primer arranque | ✅ | Cerrada con `RECOMP_AC97_READY=1`: 52+ aperturas, pantalla de título alcanzada, ventana Metal con contenido |
| 7 | Iteración hasta boot completo | 🔄 | En marcha según plan Fase 7: fronteras (F7.0) → audio host (F7.1) → ruta gráfica (F7.2) → primer draw (F7.3) → present (F7.4) → texturas (F7.5) → input (F7.6) → primer nivel (F7.7) |

---

## 2. Entorno

| Pieza | Valor |
|---|---|
| Host | macOS Apple Silicon (arm64) |
| Toolchain | AppleClang + Ninja + CMake |
| `xboxrecomp` | rama `macos-port` |
| `black-recomp` | `/Users/user/Downloads/black-recomp`, rama `main` |
| Juego | `/Users/user/Downloads/Black (USA).xiso/` — es un **directorio** extraído, no una imagen |

**Reglas operativas:**

- **Arrancar siempre con `RECOMP_VBLANK=1`.** Sin esa variable el título espera para siempre la interrupción de la NV2A y los tres hilos quedan parados.
- El `default.xbe` del directorio de juego y `game_files/default.xbe` deben ser **byte-idénticos** (`md5`). La copia parcheada a mano se conserva solo como `default.xbe.handpatched.bak`; **no usarla**.
- No dejar datos de juego inventados en el directorio del juego.

---

## 3. Cómo reproducir y verificar

```bash
# Ejecutar el título (arranque que cierra la Fase 6)
cd /Users/user/Downloads/black-recomp
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Con mando (USB emulado): teclado del terminal -- flechas, Enter = Start,
# Espacio/z = A, x = B, c = X, v = Y, Tab = Back, wasd = stick
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_USB=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Pulsaciones programadas (ms desde el arranque), para pruebas sin teclado
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_USB=1 RECOMP_INPUT_SCRIPT="110000:start:300" ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Arranque de referencia anterior (sigue muriendo en sub_00084F90)
RECOMP_VBLANK=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Ventana Metal con rasterizador software (Fase 7)
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_FB_WINDOW=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Volcado de framebuffer a BMP (para análisis sin ventana)
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 RECOMP_FB_DUMP=/tmp/black-fb- ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Con trazas verbosas del kernel
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_KERNEL_LOG_BUDGET=1000000 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"

# Tests
cd /Users/user/Downloads/xboxrecomp
./build-macos/tests/guest_layout/test_guest_layout
python3 -m unittest tools.recomp.test_lifter_carry tools.recomp.test_carry_flag
```

Verificar el progreso contando `[FILE] open ok` en el log: 52+ con la variable, 19 sin ella. El orden de ficheros es idéntico entre corridas.

Con `RECOMP_PB_EXEC=1`, el rasterizador software escribe BMPs cuando `RECOMP_FB_DUMP=<prefijo>` está definido. Con `RECOMP_FB_WINDOW=1` se abre una ventana Metal en el hilo principal del título, sin necesidad de D3D8.

---

## 4. Lo que está resuelto — no reabrir

### Sesiones anteriores

1. **Disco crudo en POSIX.** `Partition0/3/4/5` como imagen, `1` y `2` como directorio. `src/kernel/kernel_path.c`.
2. **`TDATA`/`UDATA`** traducidos a un directorio de guardado escribible.
3. **`xbox_translate_path_ex(..., want_directory)`** para distinguir "abrir como volumen crudo" de "abrir como directorio".
4. **Resolución de thunks acotada a los imports reales del título** → 115/115 resueltos, 0 sin resolver. `kernel_thunks.c`.
5. **Desbordamiento de la pila del invitado.** Causa: `0x002776B0` era una función real que el desensamblador no detectaba, quedaba como stub, y se alcanzaba por salto de cola dejando el marco sin liberar. Arreglado sembrando los destinos de tail-jump. Hay además una página de guarda de 16 KB bajo la pila. **Al regenerar hay que seguir sembrando `0x002776B0` explícitamente.**
6. **Bug del lifter: `cmov` no contaba como lector del acarreo.** Arreglado en `tools/recomp/translator.py` con tests. No parchear el C generado a mano.
7. **Pseudo-handles de 32 bits** (`0xFFFFFFFE`/`0xFFFFFFFF`) comparados en ambos anchos. `win32_compat.c`.
8. **`NtReadFile`/`NtWriteFile` reintentan hasta completar** la petición.
9. **Objetos de despacho en memoria del invitado** (los `KEVENT` que el título inicializa él mismo, sin importar `KeInitializeEvent`). `kernel_sync.c`.
10. **Vblank**: existía, había que encenderlo con `RECOMP_VBLANK=1`.
11. **Ciclo de icall feedback**: 706 destinos fusionados, stubs 386 → 81.

### Esta sesión

12. **Spin infinito en `KeWaitForSingleObject`** — commit `a491a10`. Black limpia el `SignalState` de un objeto despachador de tipo *Notification* directamente en su memoria. Nada reseteaba el evento Win32 de respaldo, así que tras el primer `KeSetEvent` quedaba señalado para siempre. **Fix:** `xbox_KeWaitForSingleObject` trata el `SignalState` del invitado como fuente de verdad: si lee 0, resetea el handle del host antes de esperar. **Efecto medido:** de 9 a 18 ficheros cargados.
13. **Stubs sin resolver 81 → 70** con otra pasada del ciclo de icall feedback.
14. **El nombre del fichero en un `fopen` fallido** — commit `0d52baa`.
15. **Trap APU de Windows aislado** — commit `321c102`. En macOS era un SIGBUS sin manejador en `0xFE801100`. Ahora solo corre en `_WIN32`.
16. **Registros AC'97 interceptados en ARM64** — commit `321c102`. Nuevo `src/kernel/mcpx_mmio_posix.c` + `mcpx_mmio.h`: página `0xFEC00000` en `PROT_NONE`; el manejador de `black-recomp/src/main_posix.c` llama a `xbox_mcpx_mmio_fault()`, que decodifica LDR/STR ARM64, emula el acceso sobre un bloque sombra y reanuda. Modelo: *codec ready* en GLOB_STA, RR del CR se limpia al escribirse, SR W1C. Traza: `RECOMP_MCPX_TRACE=1`.
17. **ACK del comando del DSP** — commit `321c102`. `dsp_ack_tick()`: `GPSADDR` → tabla SG → página 0 → `+0x810`, limpia el comando pendiente. Medido: comandos 3, 2, 2.
18. **Fence del push buffer D3D** — commit `d192d5a` (black-recomp). `xbox_Nv2aMirrorFence(0x0027BFF8, 0x2C, 0x30)` en `main_posix.c`. `sub_0026CC10` esperaba a la GPU: enviados 13, fence en 3.
19. **`[FILE] open ok "nombre"`** — commit `a6d1625`.
20. **Herramientas de diagnóstico** — commit `a6d1625`. `RECOMP_WATCH_FIELD=1` (`recomp_trace.c`) y `RECOMP_WATCH_ALLOC=<size>,<tag>` (`kernel_bridge.c`).
21. **Bug del lifter: flags en arista de retorno de bucle** — commit `cd31faa`. Fix en `block_flag_transfer`, `merge_flag_states` en `lifter.py`. Test nuevo `tools/recomp/test_flag_dataflow.py`.
22. **AC'97: RPBM limpia DCH** — commit `321c102`.
23. **Reproducción AC'97 e interrupción vector 6** — commit `e3d0d8d`. `mcpx_mmio_posix.c` consume la lista de buffers en tiempo real (48 kHz, longitud en muestras de 16 bits, medido: `0x1000` sobre 8192 bytes), marca BCIS/LVBCI/CELV, avanza CIV/PIV/PICB y repite el último buffer válido (BUP).
24. **Lifter: E/S de puertos x86** — commits `e3d0d8d` (runtime) y `1acffdb` (lifter). `in`/`out` llaman a `recomp_port_in/out`. Modelado: `0x80C0` (GPIO MCPX), bit 5 = campo entrelazado.
25. **Translator: `jmp reg` local** — commit `1acffdb`. Ahora se despacha a las etiquetas con las que se carga ese registro.
26. **Latido del DSP EP** — commit `e3d0d8d`. `DirectSoundDoWork` lee `[[0x224C40]]` = `0xFE85A018` y, si no vale `0x00CCCCCC`, reinicia todo el audio. `dsp_ack_tick` mantiene el latido mientras el registro de reset del EP (`0xFE85FFFC`) vale 3.
27. **Hilo de ACK del DSP en POSIX** — agregado `xbox_DspAckStart/Stop()` en `xbox_memory_layout.c` para macOS. Sin este hilo, el command doorbell del DSP nunca se limpia y DirectSound spinnea forever.

**Efecto medido de 12–19:** de 19 a 52 aperturas correctas; DirectSound devuelve `S_OK`; `sub_00084F90` retorna; el título llega al vídeo de intro y a la pantalla de título.

---

## 5. Lo que se revirtió, y por qué importa

### 5.1 El XBE parcheado a mano (dos parches, ambos sobre premisas falsas)

- **NOPs en el entry point** para saltar una lectura de `MEM32(0x10118)`. Ese offset es el **Certificate Address** del header del XBE, un campo válido.
- **Valores inventados en `.data`** en `0x296BA0`/`0x296BA4`, presentados como corrección de "vtable bounds incorrectos". La región real está a ceros; los punteros inventados causaban las llamadas indirectas fallidas.

Ninguno tenía efecto real (el C se genera de la copia sin parchear). Ambos revertidos.

### 5.2 El alias `mmap` de la ventana contigua (tres intentos, todos revertidos)

Se intentó hacer la ventana `0x80000000` un alias real de la RAM. Las tres veces regresionó a un cuelgue peor. La causa de ese cuelgue resultó ser el evento manual-reset (punto 12), no el aliasing.

### 5.3 El pliegue de bit 31 `xbox_canon_va` — commit `03ac9e0`

Plegar el bit 31 solo en `0x800F0000–0x80100000` era incorrecto y dañino. El espacio del invitado está mapeado en identidad: ese rango **es el `.text` del título**. Black limpia su bloque contiguo (`sub_00013C20`, `0x800F4000–0x800FA078`) y con el pliegue ese `memset` machacaba su propio código (tabla de saltos en `0x000F7614`).

Prueba decisiva: con el pliegue desactivado y la página baja protegida contra escritura, Black **nunca escribe** `0x000F9FB8`. Solo usa la forma `0x80xxxxxx`. La ventana contigua es almacenamiento privado. `test_guest_layout` fija el contrato.

> No volver a añadir el pliegue. Si una lectura por bit 31 devuelve ceros, el bug es la escritura que nunca ocurrió, no el mapeo.

---

## 6. Bloqueo de la Fase 6 — resuelto

### 6.1 Síntoma

`SIGBUS` en VA `0xFFFFFFEC` dentro de `sub_00084F90`: recorre una lista con la cabeza a NULL porque `[esi+0xD3A0]` es 0. Eso viene de que el cto propio del tipo RenderWare (`sub_0008C6C0`) falla en el cargador DSP `sub_0008C570`, que llega a `sub_0021821C → sub_0021C1F3 → sub_0021BF86 → sub_00221770` y recibe `E_OUTOFMEMORY` porque `[0x01101228]` (`[objeto7+0x14]`) es 0.

### 6.2 Causa raíz — trazada en vivo

```
sub_0021B350 (crea el objeto #7, 0x01101214)
  └ sub_0021D04E → sub_0022257A        (init; aborta al primer HRESULT < 0)
       ├ sub_00221D73 → sub_00222D4E → sub_0022261B   ← DETECCIÓN AC'97
       │     0xFEC0012C |= 2            (GLOB_CNT, cold reset)
       │     espera 0xFEC00130 & 0x100  (GLOB_STA, codec ready)
       │     KeStallExecutionProcessor(20) × 1000   ("ordinal 151 x1000")
       │     memoria pasiva = 0  →  DSERR_NODRIVER (0x88780078)
       └ sub_002220C4 → sub_00221A90   ← NUNCA CORRÍA
             crea X (0x3C bytes), [objeto7+0x14] = X, sub_0021BF11(X)
```

El título ignora el fallo de `DirectSoundCreate` (`sub_0021B489`) y sigue hasta que la carga del DSP usa el campo nulo.

### 6.3 Verificación del cierre

| # | Condición | Evidencia |
|---|-----------|-----------|
| 1 | Cargador DSP / ctor del tipo no devuelven 0 | `sub_0008C570` → 1, `sub_0008C6C0` → `0x829B8958` |
| 2 | `[esi+0xD3A0] ≠ 0` en `0x000BDD56` | `eax=0x829B8958`, `esi=0x800ECC80`; lldb: `[0x800FA020] = 0x829B8958` |
| 3 | `sub_00084F90` retorna | 16 entradas, 16 retornos |
| 4 | Fichero #19 o posterior | 52 aperturas correctas (19 sin el cambio) |

Detalle completo: `black-port-fase6-cierre.md` §6–§7.

---

## 7. Fase 7 — Plan de desbloqueo

Plan maestro: `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md`.

### 7.0 Principio rector

**Medir antes de implementar.** Cada hipótesis debe producir: **CONFIRMADO**, **DESCARTADO** o **NO DETERMINADO**. Una hipótesis en estado "NO DETERMINADO" no debe convertirse en un parche.

### 7.1 Mapa de dependencias

```
F7.0  Instrumentación de fronteras
           |
           +--------------------+
           |                    |
           v                    v
F7.1  Audio host        F7.2  Graphics path discovery
           |                    |
           |          +---------+---------+
           |          |                   |
           |          v                   v
           |      D3D8 path        Push-buffer path
           |          |                   |
           +----------+-------------------+
                      v
               F7.3 First real draw
                      |
                      v
               F7.4 Present real
                      |
                      v
               F7.5 Input host
                      |
                      v
               F7.6 First level
                      |
                      v
               F7.7 Stability
```

Los 70 stubs dejan de ser un hito independiente. Solo se investigan cuando aparecen en el camino de ejecución relevante.

### 7.2 F7.0 — Instrumentación de fronteras

**Objetivo:** Responder, sin modificar la arquitectura, las preguntas fundamentales de cada subsistema.

**Audio:** ¿VP produce samples? ¿mixbins contienen samples no nulos? ¿`apu_dsp_frame()` recibe/procesa samples? ¿`monitor.frame_buf` contiene audio real? ¿el host recibe frames? ¿el backend macOS los reproduce?

**Gráficos:** ¿Black llama `Direct3DCreate8`? ¿`CreateDevice`? ¿`DrawPrimitive`/`DrawIndexedPrimitive`? ¿`Present`/`Swap`? ¿escribe `DMA_PUT`? ¿`DMA_GET` avanza? ¿`pgraph_method` recibe métodos?

**Input:** ¿Existe abstracción de estado del mando? ¿el juego la consulta? ¿se puede inyectar estado sin USB?

**Instrumentación propuesta:** contadores y trazas para `D3D8_CreateDevice_count`, `D3D8_DrawPrimitive_count`, `D3D8_Present_count`, `NV2A_DMA_PUT_changes`, `NV2A_DMA_GET_changes`, `NV2A_PGRAPH_method_count`, `APU_mixbin_nonzero_samples`, `APU_framebuf_rms`, `APU_frames_submitted`, `APU_frames_played`, `INPUT_poll_count`, `INPUT_nonzero_buttons`.

**Criterio de DONE:** `diagnostics/f7_boundary.log` con resultado explícito para cada frontera.

### 7.3 F7.1 — Audio host macOS

**Hipótesis:** No asumir que GP/EP son el bloqueo principal. Comprobar la arquitectura de extremo a extremo antes de implementar DSP adicional.

**Experimento — Caso A:** Si `frame_buf` contiene samples válidos → implementar solo `APU monitor.frame_buf → macOS audio backend → CoreAudio/SDL`. No implementar GP/EP todavía.

**Experimento — Caso B:** Si `frame_buf` está silencioso → comprobar VP output, mixbin routing, voice state, DSP copy. Solo si la medición demuestra que el fallo está dentro del DSP, abrir el trabajo GP/EP.

**Primer test funcional:** `D:\Sound\Streams\LOGOS.bks` (ya existe evidencia de que el juego carga recursos de audio del frontend).

**Criterio de DONE:**
```
Audio frame buffer no-zero
+
backend host reproduciendo
+
sonido audible
+
sin reset/underrun continuo
```

### 7.4 F7.2 — Descubrir el camino gráfico real de Black

**No construir todavía un renderer nuevo.** Determinar si el camino principal es:
- **Ruta A:** `Black → D3D8 API`
- **Ruta B:** `Black → NV2A push buffer`

**Instrumentación:** Registrar `Direct3DCreate8`, `CreateDevice`, `CreateResource`, `SetRenderState`, `SetTexture`, `DrawPrimitive`, `DrawIndexedPrimitive`, `Present`, `DMA_PUT`, `DMA_GET`, `PFIFO`, `PGRAPH method`, `PGRAPH draw`.

**Decisión:**
- Si predominan llamadas D3D8 → `Black → xbox_d3d8 → backend POSIX/OpenGL`
- Si predominan llamadas de push buffer → `Black → NV2A push buffer → PGRAPH → renderer`

No mantener ambas como arquitecturas principales salvo que la evidencia demuestre que Black necesita ambas.

### 7.5 F7.3 — Primer draw real

**Backend objetivo para macOS:**
```
Xbox D3D8
   -> xbox_d3d8
   -> OpenGL
   -> SDL window
   -> macOS
```

No usar `D3D8 → D3D11 → DXGI swap chain` para macOS.

**Primer milestone gráfico:** Conseguir `Clear + primitive/triangle + Present`. El primer objetivo es demostrar un draw real.

**No implementar aún:** todos los formatos de textura, todos los shaders Xbox, todos los register combiners, todos los efectos, optimización GPU. Implementar únicamente aquello que el primer draw demuestre necesitar.

### 7.6 F7.4 — Present real

La ventana Metal que escanea la RAM del framebuffer (`RECOMP_FB_WINDOW=1`) debe mantenerse solo como herramienta de diagnóstico (`DEBUG_PRESENT`). No debe confundirse con `GAME_PRESENT`.

**Criterio de DONE:**
```
game render
   -> back buffer
   -> Present / Swap
   -> macOS window
```

Y debe poder apagarse (`RECOMP_FB_WINDOW=0`) sin perder la imagen generada por el renderer real.

### 7.7 F7.5 — Texturas

Las texturas se atacan después del primer draw, no antes.

**Instrumentación mínima:** Cada `CreateTexture` debe registrar `width`, `height`, `levels`, `format`, `usage`, `pool`, `address`. Cada upload debe registrar `source address`, `byte size`, `swizzled?`, `converted?`.

**Estrategia incremental:** Implementar primero el formato utilizado por Black en el primer frame real. Después, ampliar formato por formato según aparezcan fallos reales. No implementar de antemano todos los formatos D3D8 del toolkit.

### 7.8 F7.6 — Input host

**Estrategia recomendada:** No comenzar emulando el camino completo OHCI/XID salvo que una prueba demuestre que es necesario.

**Primera opción:**
```
macOS HID / SDL GameController
         ->
xbox_input
         ->
Xbox controller state
         ->
Black
```

**Primer objetivo:** Debe funcionar al menos `A`, `B`, `X`, `Y`, `Start`, `Back`, `D-pad`, `Left stick`, `Right stick`, `Triggers`.

**Estrategia de prueba:** Primero usar inyección artificial (`INPUT_SCRIPT`), después probar hardware real. Esto separa los fallos de lógica del juego de los fallos del backend HID.

### 7.9 F7.7 — Primer nivel

Este es el milestone que importa para determinar si el proyecto es realmente jugable.

```
Frontend
   -> START
   -> loading
   -> level resources
   -> renderer
   -> audio
   -> input
   -> game tick
   -> player controllable
```

Durante la transición al nivel registrar: `state changes`, `file opens`, `CreateTexture`, `CreateVertexBuffer`, `CreateIndexBuffer`, `Draw calls`, `Present`, `Audio starts`, `Input polls`.

La transición al primer nivel es el punto esperado para revelar nuevas dependencias y stubs que el frontend no ejecuta.

---

## 8. Tratamiento de los 70 stubs

**No usar:**
```
70 -> 69 -> 68 -> 67 ...
```
como métrica principal.

**Usar:**
```
70 total
X alcanzables en frontend
Y alcanzables en primer nivel
Z en camino crítico
```

**Clasificación:**
```
A = crítico y ejecutado
B = ejecutado pero no crítico
C = reachable futuro
D = muerto/no alcanzable
```

Solo los `A` bloquean la Fase 7.

---

## 9. Bucle de depuración

Cada bloqueo debe seguir:

```
1. reproducir
2. capturar síntoma
3. localizar función
4. clasificar subsistema
5. formular hipótesis
6. cambiar UNA cosa
7. rebuild
8. repetir
```

Categorías mínimas: `kernel`, `memory`, `D3D8`, `NV2A`, `audio`, `input`, `game logic`.

---

## 10. Artefactos de diagnóstico

Crear:

```
diagnostics/
    f7_boundary.log
    f7_audio.log
    f7_graphics.log
    f7_input.log
    f7_level.log
```

Y un estado resumido:

```json
{
  "audio_host": "pending",
  "graphics_path": "unknown",
  "first_draw": "pending",
  "present": "pending",
  "input_host": "partial",
  "first_level": "pending",
  "playable": false
}
```

Cada commit relevante debe actualizar uno o más estados de forma justificable.

---

## 11. Milestones y criterios de salida

| Milestone | Definición de DONE |
|---|---|
| F7.0 | Todas las fronteras críticas están instrumentadas |
| F7.1 | Audio audible desde el juego |
| F7.2 | Ruta gráfica D3D8 vs PB identificada experimentalmente |
| F7.3 | Primer draw real en OpenGL |
| F7.4 | Present real sin framebuffer scanner |
| F7.5 | Primera textura funcional |
| F7.6 | Mando host controla el juego |
| F7.7 | Primer nivel carga |
| F7.8 | Primer frame controlable |
| F7.9 | Sesión estable >= 10 min |

---

## 12. Orden exacto de trabajo

1. **Prioridad 1:** F7.0 — instrumentación
2. **Prioridad 2:** F7.1 — audio host
3. **Prioridad 3:** F7.2 — identificar ruta gráfica
4. **Prioridad 4:** F7.3 — primer draw
5. **Prioridad 5:** F7.4 — present real
6. **Prioridad 6:** F7.5 — textura mínima
7. **Prioridad 7:** F7.6 — input host
8. **Prioridad 8:** F7.7 — primer nivel
9. **Prioridad 9:** stubs críticos

---

## 13. Lo que NO hacer

- NO implementar GP/EP completo sin demostrar que son el bloqueo.
- NO construir un D3D11 swap chain para macOS.
- NO reescribir todo el renderer NV2A antes de descubrir la ruta real.
- NO implementar todos los formatos de textura de antemano.
- NO implementar OHCI completo antes de probar un host gamepad.
- NO perseguir la cifra "70 stubs" como KPI principal.
- NO parchear código C generado manualmente.
- NO volver a introducir el alias `0x80000000`.
- NO volver a introducir el fold de bit 31.
- NO volver a usar `functions.json` viejo como seeds.

Las últimas restricciones están respaldadas por regresiones ya documentadas en el proyecto de Black.

---

## 14. Estructura de commits propuesta

```
f7-00-instrumentation
f7-01-audio-host
f7-02-graphics-path
f7-03-first-draw
f7-04-present
f7-05-texture-minimum
f7-06-input-host
f7-07-first-level
f7-08-critical-stubs
f7-09-stability
```

Cada commit debe incluir:
1. 1 cambio conceptual
2. 1 prueba
3. 1 resultado
4. 1 estado de regresión

---

## 15. Objetivo real de la Fase 7

La Fase 7 no termina cuando Black abre una ventana. Tampoco termina cuando carga 52 archivos ni cuando hay triángulos visibles en un framebuffer auxiliar.

La Fase 7 termina cuando se puede demostrar:

```
Black
  - renderiza mediante su camino gráfico real
  - presenta mediante el backend macOS
  - produce audio audible
  - acepta input de mando
  - carga un nivel
  - ejecuta un frame controlable continuamente
```

Ese es el verdadero **first playable**.

---

## 16. Diagnóstico final del estado actual

La Fase 6 está suficientemente madura como para dejar de tratar boot, kernel y memoria como trabajo principal, salvo regresiones.

La documentación de Black ya identifica como resueltos problemas de paths, stack, sincronización, AC'97, DSP ACK y fence del push buffer.

Por ello, el cuello de botella de Fase 7 debe trasladarse a las fronteras:

```
APU -> host
D3D8/NV2A -> host GPU
Input host -> Xbox controller state
```

La prioridad recomendada es:
```
1. Instrumentación
2. Audio host
3. Descubrir ruta gráfica
4. Primer draw
5. Present
6. Input
7. Primer nivel
```

y no:
```
1. GP/EP completo
2. D3D11 completo
3. OHCI completo
4. Reducir 70 stubs
```

El cambio de estrategia busca sacar al proyecto del estancamiento mediante milestones pequeños, medibles y reversibles.

---

## 17. Criterios de avance

| Hito | Evidencia requerida |
|---|---|
| DSP cerrado ✅ | DirectSound `S_OK`, ACK del DSP, el constructor devuelve objeto |
| Frontend ✅ | 52 aperturas: `FEMain.bin`, `Common.bin`, `FEMusic.ssh`, vídeo de intro |
| F7.0 | `diagnostics/f7_boundary.log` completo |
| F7.1 | Audio audible desde el juego |
| F7.2 | Ruta D3D8 vs PB confirmada |
| F7.3 | Primer draw real en OpenGL |
| F7.4 | Present real sin framebuffer scanner |
| F7.5 | Primera textura funcional |
| F7.6 | Mando host controla el juego |
| F7.7 | Primer nivel carga |
| F7.8 | Primer frame controlable |
| F7.9 | Sesión estable >= 10 min |
| Port jugable | frontend + nivel + render + audio + input funcionan juntos |

---

## 18. Regla de diagnóstico

**Medir primero, modificar después.**

No convertir una inferencia del desensamblado en un parche hasta tener una traza que demuestre el valor incorrecto. Los problemas anteriores muestran que las mediciones han sido más fiables que las inferencias aisladas.
