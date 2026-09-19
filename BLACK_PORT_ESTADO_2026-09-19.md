# Black (Xbox) → Apple Silicon: estado del port nativo

**Fecha:** 19 de septiembre de 2026
**Repos:** `porting/xboxrecomp` (toolkit y runtime, rama `macos-port`) y `black-recomp` (proyecto del juego)
**Resumen:** el juego arranca, reproduce los vídeos, muestra el título y los menús, carga la primera misión (Veblensk City Street) y **se juega**: se mueve, se apunta, se dispara, se recarga y se muestran los objetivos. El nivel se renderiza correctamente con el renderizador NV2A de xemu. Quedan pendientes: la estabilidad del arranque durante los vídeos, la calidad de los vídeos, la deriva de la cámara tras disparar, el recuadro negro de los menús (hoy oculto con un filtro) y el mando de PS4.

---

## 1. Cómo funciona el port

Black es un ejecutable de Xbox (x86 de 32 bits + kernel de Xbox + GPU NV2A + audio MCPX). El port no emula la CPU: el código x86 se **recompila estáticamente a C** y se compila de forma nativa para ARM64.

| Pieza | Qué hace | Dónde |
|---|---|---|
| Desensamblado y análisis | Encuentra las funciones del XBE y su ABI | `tools/disasm`, `tools/abi_analysis` |
| Recompilador (lifter/translator) | Traduce cada instrucción x86 a C (registros como variables, memoria como `MEM32()`) | `tools/recomp/lifter.py`, `translator.py` |
| Semillas | Funciones alcanzadas solo por punteros, que el análisis no encuentra | `black-recomp/seeds/hand_seeds.json` (25) + feedback de ICALL |
| Kernel de Xbox | ~150 funciones del kernel reimplementadas sobre macOS (hilos, eventos, ficheros, memoria, DPC, IRQL…) | `src/kernel/` |
| GPU | Los métodos del pushbuffer se entregan al renderizador PGRAPH/OpenGL de **xemu** (extraído de QEMU) | `third_party/xemu_nv2a/`, `src/kernel/nv2a_pb_scan.c` |
| Audio | APU MCPX (procesador de voces de xemu) + salida SDL a 48 kHz | `src/apu/` |
| USB/mando | Controlador OHCI emulado; el mando Xbox se alimenta del teclado o de un mando SDL | `src/usb/`, `src/input/` |
| Ventana | Presenta el framebuffer del juego en una ventana Cocoa/Metal | `src/video/fb_present_posix.m` |

### Compilar y ejecutar

```sh
# Regenerar el código C (tras cambiar el recompilador o las semillas)
cd porting/xboxrecomp
python3 -m tools.recomp game_files/default.xbe --game-name Black --all --split 1000 \
    --gen-dir ../../black-recomp/src/recomp/gen

# Compilar (borrar antes los ._* del disco exFAT)
cd ../../black-recomp
find src ../porting/xboxrecomp -name '._*' -not -path '*/.git/*' -delete
make -C build-mac -j8 black-recomp

# Ejecutar
cd build-mac
RECOMP_XEMU_GPU=1 RECOMP_VBLANK=1 RECOMP_AC97_READY=1 RECOMP_PB_EXEC=1 \
RECOMP_FB_WINDOW=1 RECOMP_USB=1 ./black-recomp "/path/to/volume/backup mac/Black (USA).xiso"
```

### Controles de teclado (la ventana del juego debe tener el foco)

| Tecla | Mando Xbox | En Black |
|---|---|---|
| Enter | Start | Empezar / pausa |
| Tab / Retroceso | Back | Atrás |
| Espacio | A | Aceptar |
| X | B | Cancelar |
| Q | X | Recargar |
| E | Y | Cambiar arma |
| Flechas | Cruceta | Menús |
| W A S D | Stick izquierdo | Moverse |
| I J K L | Stick derecho | Apuntar |
| F / H | Clic de los sticks | Agacharse / cuerpo a cuerpo |
| V / C | Gatillo izq. / der. | Apuntar con mira / disparar |
| 1 / 2 | White / Black | Granadas |

Para jugar: Enter en el título → Espacio en START MISSION → NORMAL → informe de misión → esperar a que aparezca "RENDEZVOUS WITH BLACK CELL" (~40 s después de cargar; antes, el juego ignora el mando).

---

## 2. Lo que se ha hecho, con su causa y su arreglo

Los cambios se agrupan por síntoma. Cada uno está en un commit propio (hashes de `xboxrecomp` salvo que se indique `black-recomp`).

### 2.1 Arranque, repositorio y compilación

- **Git corrupto y CMake roto por el disco exFAT.** macOS crea ficheros `._*` (AppleDouble) que rompían el índice de git y que CMake compilaba como `.c`. Se borraron, se puso `core.filemode false`, se excluyeron en `.git/info/exclude` y el glob de CMake los filtra (`black-recomp 63e4892`).
- **Funciones que no se encontraban** (fallos de ICALL: llamadas indirectas a código sin traducir). Se añadieron semillas: StartNewMission, LevelSelect, SelectDifficulty, callbacks de LevelDat, funclets de excepciones… (`black-recomp 32771c6`, `63e4892`).
- **`NtQueryDirectoryFile` con 10 argumentos, no 9.** Se corrompía la pila en cada listado de directorio.
- **Handles inválidos.** `bridge_resolve_handle` devuelve NULL para valores nunca emitidos (evita un SIGSEGV en `w32_handle_fd`).
- **Listados de directorio** que devolvían `.`, `..`, `._*` y `.DS_Store`.

### 2.2 Renderizado (GPU NV2A)

- **Integración del renderizador de xemu** (`f38cd04`): PGRAPH/OpenGL de xemu compilado fuera de QEMU, con una capa de compatibilidad, un contexto OpenGL 4.1 core en macOS y un pegamento (`nv2a_glue.c`) que resuelve los objetos DMA/RAMHT sobre la apertura de registros simulada.
- **Recorrido del pushbuffer**: seguir los saltos y el wrap-around del anillo; antes se perdían segmentos enteros de comandos (texturas sin formato, "dim 0"). Ahora también sigue **CALL/RETURN** (`3426ed9`).
- **Pantalla negra**: xemu volvía a subir las superficies desde la RAM antes de cada dibujo. Se resolvió con `tcg_enabled()` a `true`.
- **Vídeos que no arrancaban**: el decodificador XMV espera un cambio de paridad del campo de vídeo de D3D. Se replica (`xbox_Nv2aFieldParity`).
- **Fotogramas escritos en la memoria equivocada**: la apertura "tiled" (0xF0000000) se hace alias de la ventana contigua (`black-recomp 5b6b0b8`).
- **Recuadro negro sobre el título y los menús**: es un panel opaco que el propio juego pide dibujar (combiner: color = difuso negro, alpha = 1). La causa raíz está en la lógica de menús del juego (la misma que aplasta algunas líneas de texto en un punto). **Solución provisional**: un filtro que vuelve transparente exactamente ese panel y su borde (`d53dc61`, `black-recomp 38a81c0`). `RECOMP_KEEP_BLACK_PANEL=1` lo desactiva.
- **Nivel translúcido (edificios transparentes, suelo negro, sin arma)** (`6932e7d`). Era un **fallo del recompilador**:
  ```asm
  sub eax, 4
  mov eax, [0x56f02c]   ; sobrescribe eax ANTES del salto
  je  sustractivo       ; usa los flags del sub
  ```
  se traducía como `if (eax == 0)` después de la carga, así que evaluaba el valor cargado. En `sub_00172EB0` eso enviaba toda la geometría opaca al modo de mezcla **sustractivo**. Arreglo: cuando una instrucción que no toca los flags sobrescribe un operando del que dependen los flags pendientes, el lifter guarda una instantánea (`_fr`) y el salto la lee. También se corrigió que `js/jns` tras operaciones de 8/16 bits evaluaba el signo en 32 bits. Hay pruebas nuevas en `tools/recomp/test_flag_clobber.py`.
- **La imagen "se congelaba" en el nivel aunque el juego seguía** (`3478994`): la ventana seguía cualquier superficie en la que se dibujara, incluidas las de 160×120 y 80×60 del desenfoque de profundidad de campo, y no podía mostrarlas. Ahora solo sigue superficies del tamaño de la pantalla. Además, ese código escribía ~15 MB de log cada 17 minutos.

### 2.3 Controles

- **El mando aparecía como jugador 3 y los botones se quedaban pulsados**: ahora está en el puerto raíz 3 (jugador 1) y usa la API GameController de SDL.
- **El juego crasheaba en `XInputGetState` o dejaba de leer el mando** (`2b3c8dd`): la tabla de handles del mando estaba en la dirección física más baja de la ventana contigua. El juego escribe fotogramas por la apertura tiled en la dirección 0 (superficies aún sin dirección), lo que pisaba la tabla. El asignador ahora empieza 4 MB más arriba.
- **El mando dejaba de responder a mitad de partida** (`4e0789e`): la cola de DPC del kernel no tenía cerrojo y varios hilos (USB, vblank, audio) insertaban a la vez. Si se perdía el DPC del USB, el driver nunca volvía a activar las interrupciones.

### 2.4 Bloqueo en el nivel al disparar/romper objetos

Síntoma: tras varios disparos, el juego dejaba de dibujar (la ventana mostraba la última imagen) mientras el proceso seguía vivo. Siempre justo después de cargar `DESTRUCT.bks`.

- **Diagnóstico** (herramienta nueva, `ebcb00c`): con `RECOMP_HANG_DUMP=8`, si no se presenta ningún fotograma durante 8 s se vuelca, para cada hilo del juego: la llamada al kernel en curso, el objeto que espera, los **registros en vivo**, la pila, la cola de DPC y una muestra nativa (`sample`). `RECOMP_HANG_LIST=cabeza,off_sig,off_valor` recorre además una lista enlazada.
- **Causa** (`c444e06`): el hilo principal giraba sin fin en `sub_00082550`, la inserción ordenada en la lista de prioridades de sonido 3D (cabeza `0x2C0270`). Black llama al arrancar a `_controlfp(_PC_24, _MCW_PC)`, es decir, **FPU x87 en precisión simple**. El port guardaba la palabra de control pero calculaba siempre en doble. Así, `101/255` en la pila de la FPU ≠ `101/255` guardado como float, la comparación decía "101 < 101" y la búsqueda rebotaba entre dos nodos para siempre. **Arreglo**: los resultados de `fadd/fsub/fmul/fdiv/fsqrt` se redondean según el campo de precisión (`fp_pc()` en `templates/runtime/recomp_types.h`), como hace el chip. Esto afecta a todo el cálculo en coma flotante del juego.
- **Tres fallos más del kernel encontrados por el camino** (`ebcb00c`):
  1. `g_kernel_dispatch_slot` era una global compartida entre la búsqueda y la llamada. Otro hilo podía cambiarla entre medias y **un hilo ejecutaba la llamada al kernel de otro**. Ahora es por hilo.
  2. **El IRQL no excluía nada.** En la Xbox (una CPU), subir a DISPATCH_LEVEL impide que corran los DPC; aquí los DPC corren en otro núcleo a la vez. Ahora DISPATCH_LEVEL es un cerrojo recursivo que también toman los DPC.
  3. `KfRaiseIrql/KfLowerIrql` son fastcall (argumento en `ecx`), pero se leía de la pila.
- **Resultado:** en las pruebas se dispara y se rompen objetos (`DESTRUCT.bks` se cargó 9 y 6 veces) sin bloqueos.

### 2.5 Sonido acelerado

- **Causa** (`2d4e619`): el procesador de voces calculaba el pitch de cada voz pero **lo ignoraba** (`(void)rate; /* Ignored until we add proper resampling */`), porque libsamplerate no está en el port. Cada voz se reproducía a una muestra de origen por cada muestra de salida a 48 kHz. Medido en Black (`RECOMP_APU_RATES=1`): el 40 % de las voces son de 12–20 kHz o de 32 kHz, así que sonaban **~3× y 1,5× más rápidas** y agudas.
- **Arreglo:** remuestreo con interpolación lineal y posición fraccionaria por voz, que se reinicia en VOICE_ON y VOICE_OFF.
- **Pendiente de confirmar de oído** (las pruebas automáticas no escuchan).

### 2.6 Herramientas de diagnóstico añadidas

| Variable | Qué hace |
|---|---|
| `RECOMP_HANG_DUMP=<s>`, `RECOMP_HANG_DIR`, `RECOMP_HANG_LIST` | Volcado automático de bloqueos (ver 2.4) |
| `RECOMP_XEMU_DRAWLOG=L<n>` | Registra todos los dibujos del n-ésimo fotograma con más de 300 dibujos (superficies, texturas, combiners, arrays de vértices, métodos de mezcla) |
| `RECOMP_XEMU_ZLOG=1` | Resumen de profundidad por fotograma |
| `RECOMP_XEMU_SKIP_COMPOSITE=1` | Omite el compuesto del desenfoque (depuración) |
| `RECOMP_USB_TRACE=1` | Estado del OHCI cada 5 s, informes del mando y ejes |
| `RECOMP_APU_RATES=1` | Histograma de frecuencias de las voces |
| `RECOMP_INPUT_SCRIPT="ms:boton[:ms],…"` | Pulsaciones programadas (máx. 64) |
| `RECOMP_FB_SNAP=<dir>/` | Una captura por segundo |
| `RECOMP_FBWIN_VERBOSE=1` | Log de la selección de superficie de la ventana |

Ruta automática al nivel (≈ 5–6 min): `start` a 32–47 s; `a`/`start` alternos cada 3 s de 150 a 222 s; el nivel carga hacia 245–380 s y acepta el mando ~40 s después.

---

## 3. Lo que falta, por prioridad

### 3.1 Arranque inestable en los vídeos (alta)

En aproximadamente 1 de cada 3 ejecuciones el juego se cae o se queda colgado antes del menú, durante los vídeos:
- Crash en `sub_0013DD20` (vía `0x140D90`) al terminar un vídeo XMV, accediendo a `0x9xxxxxxx` (fuera de la memoria).
- Crash con la pila del hilo principal sobrescrita (`esp` = puntero del heap) en el bucle principal `sub_0020F6A0`.
- Cuelgue a ~37 s (sin crash).

**Siguiente paso:** ejecutar con `RECOMP_HANG_DUMP` (los cuelgues ya se vuelcan solos) y ver si alguno de los arreglos de hoy (precisión FPU, dispatch por hilo, IRQL) ya los reduce: varios encajan con carreras entre hilos.

### 3.2 Deriva de la cámara hacia el suelo (alta)

Con teclado la vista acaba mirando al suelo. **Descartado**: el mando envía valores correctos (ejes a 0 en reposo, solo cambian mientras se pulsa: verificado con `RECOMP_USB_TRACE`). Aparece **tras disparar**. Hipótesis: la recuperación del retroceso del arma se pasa de largo (cálculo de coma flotante o de tiempo por fotograma).

**Siguiente paso:** comparar el ángulo de la cámara del jugador antes y después de un disparo sin tocar los sticks, y localizar la función de retroceso con el volcado de registros.

### 3.3 Calidad de los vídeos (media)

Los vídeos (fondo del título, intro) se ven con bloques muy marcados, como si se decodificaran a baja calidad o faltaran coeficientes. El decodificador XMV es código del juego recompilado; puede ser otro caso del recompilador (SSE/MMX) o de la precisión.

**Siguiente paso:** volcar un fotograma decodificado (`RECOMP_FMV_DUMP`) y compararlo con el de xemu.

### 3.4 Recuadro negro y texto aplastado en los menús (media)

El filtro oculta el panel, pero la causa está en la lógica de menús: el juego cree que el panel debe verse, y algunas tiras de texto se colapsan en un punto (151, 210). Consecuencia visible: **el menú de pausa no se ve**.

**Siguiente paso:** rastrear el objeto que decide la visibilidad del panel (flusher de lotes `0xD4C50`, buffer `0x563120`) buscando otro fallo de traducción como el de los flags.

### 3.5 Mando de PS4 (media)

SDL no detecta joysticks (`SDL sees 0 joystick(s)`), por cable o Bluetooth. Hay que revisar la inicialización del subsistema de mandos de SDL en macOS (permisos de Input Monitoring y el hint de HIDAPI).

### 3.6 Otros

- **Audio:** la mezcla final suma los bins 0–15 a la izquierda y 16–31 a la derecha (el DSP del MCPX está en modo passthrough). Los efectos (reverb) y el posicionamiento 3D real no se emulan.
- **Un error PGRAPH al arrancar** (`NO_OPERATION` con parámetro 0x28): es un aviso de software de D3D, inocuo, pero se podría atender como en xemu.
- **`MmGetPhysicalAddress`** devuelve la dirección virtual (0x8xxxxxxx) en lugar de la física (0x0xxxxxxx). Funciona porque los consumidores actuales lo toleran.
- **Rendimiento:** ~2 núcleos al 100 %; el nivel va fluido en las capturas, pero no se ha medido la tasa de fotogramas real.
- **Rendimiento y ruido del log:** quedan logs por fotograma (`[NV2A] DMA_PUT`, `[GPU] draw #`) que conviene poner detrás de variables.

---

## 4. Lecciones útiles para seguir

1. **Casi todos los fallos "gráficos" o "de lógica" graves de hoy eran del recompilador o del kernel**, no del renderizador: los flags sobrescritos, la precisión de la FPU, el dispatch compartido y el IRQL. Ante un comportamiento raro, conviene sospechar primero de la traducción.
2. **Una sola ejecución bien instrumentada** (volcado de bloqueo + registro de dibujo + trazas USB/APU + capturas) vale más que muchas ejecuciones con una sonda cada una.
3. **Las pruebas del recompilador** (`python3 -m unittest discover -s tools/recomp -p 'test_*.py' -t .`) deben pasar antes de regenerar: hoy hay 93.
4. **Después de cambiar el lifter o la plantilla `recomp_types.h`, hay que regenerar todo** el código del juego; la cabecera se copia en cada regeneración.
