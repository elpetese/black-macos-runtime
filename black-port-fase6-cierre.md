# Black (Xbox) → Apple Silicon macOS  
## Plan de cierre de la Fase 6

**Documento de trabajo.** 14 de septiembre de 2026.  
Complementa `black-port-progreso-ajustado.md`. No lo reemplaza: ese archivo sigue siendo el registro histórico. Este archivo es el plan operativo para **salir de la Fase 6**.

> ## ESTADO (2026-09-14): FASE 6 CERRADA con `RECOMP_AC97_READY=1` (commits `321c102`, `d192d5a`)
>
> Arranque: `RECOMP_VBLANK=1 RECOMP_AC97_READY=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"`
>
> | # | Condición | Estado | Evidencia |
> |---|-----------|--------|-----------|
> | 1 | Cargador DSP / ctor del tipo no devuelven 0 | ✅ | `sub_0008C570` → 1, `sub_0008C6C0` → `0x829B8958` |
> | 2 | Campo del objeto de juego ≠ NULL | ✅ | `sub_000BDCE0` → `0x829B8958` |
> | 3 | `sub_00084F90` deja de ser la tumba | ✅ | 16 entradas, 16 retornos |
> | 4 | Fichero #19 o posterior | ✅ | Aperturas correctas: **52+** (por defecto: 19, la última es `BkXboxFx.bin`, donde moría). Las #20–#52 no aparecen nunca sin el cambio: `FEAudio.awd`, fuentes, `GlobData.bin`, `MainUS.bin`, 17 streams, `FEMain.bin`, `Common.bin`, `FEMusic.ssh`, `videos\LO_N_US.xmv` |
>
> Verificación: condiciones 1–3 medidas en un build trazado; condición 4 en tres corridas limpias independientes con listas de ficheros idénticas (mismo md5).
>
> **Límites del cierre:** solo con `RECOMP_AC97_READY=1`; no probado en Windows/Linux; el ACK del DSP es sintético (no hay audio).
>
> Sin la variable, el arranque por defecto sigue muriendo en `sub_00084F90` (sin cambios, a propósito). Causa raíz: §6. Implementación: §7.4.
> Fase 7: ver `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md` y `black-port-progreso-ajustado.md` §7.

---

Una frase: el título ya arranca, el kernel HLE está resuelto, el front end carga 52 ficheros y llega a la pantalla de título. Cerrar la Fase 6 es hacer que la llamada a DirectSound devuelva éxito, que RenderWare construya el objeto y que aparezca el fichero **#19 o posterior**.

---

## 0. Criterio de cierre (no negociable)

La Fase 6 termina cuando se cumplen **las cuatro** condiciones a la vez:

| # | Condición | Evidencia |
|---|-----------|-----------|
| 1 | El cargador DSP / ctor del tipo deja de devolver 0 | `sub_0008C570` y `sub_0008C6C0` retornan un objeto válido |
| 2 | El campo del objeto de juego deja de ser NULL | `[esi+0xD3A0] ≠ 0` en `0x000BDD56` |
| 3 | `sub_00084F90` deja de ser el punto de muerte | La lista se recorre y la función retorna |
| 4 | El front end avanza | Aparece al menos el fichero **#19** en el log de carga |

No cuentan como cierre:

- el proceso “sigue vivo”
- bajan los stubs (70 restantes)
- hay sonido audible
- aparece un frame
- `ftell` es correcto (eso **ya** está cumplido y no era el fallo)

El criterio de la etapa siguiente (Fase 7) es otro documento. No mezclar.

---

## 1. Dónde estamos de verdad

### 1.1 Estado por fases

| # | Fase | Estado | Qué significa |
|---|------|--------|----------------|
| 1 | Repo base y parche macOS | ✅ | Toolchain AppleClang + Ninja + CMake sobre `xboxrecomp` rama `macos-port` |
| 2 | Recompilación de Black | ✅ | 13.758 funciones, 0 fallos de lift, 70 stubs sin resolver |
| 3 | Build del runtime | ✅ | `black-recomp` enlaza y corre |
| 4 | Validación de memoria | ✅ | `test_guest_layout` en verde; ventana `0x80000000` es almacenamiento **privado** |
| 5 | Smoke test | ✅ | |
| 6 | Primer arranque | ✅ | 52 ficheros; pantalla de título alcanzada; ventana Metal funcionando |
| 7 | Boot completo → primer frame → jugable | 🔄 | Audio audible pendiente; D3D8/NV2A mínimo pendiente |

### 1.2 Entorno que hay que preservar

| Pieza | Valor |
|---|---|
| Host | macOS Apple Silicon (arm64) |
| Toolchain | AppleClang + Ninja + CMake |
| `xboxrecomp` | rama `macos-port` |
| `black-recomp` | `/Users/user/Downloads/black-recomp`, rama `main` |
| Juego | `/Users/user/Downloads/Black (USA).xiso/` — **directorio extraído**, no imagen |

Reglas operativas vigentes (siguen siendo ciertas):

- Arrancar **siempre** con `RECOMP_VBLANK=1`. Sin eso el título espera la NV2A para siempre.
- `default.xbe` del directorio de juego y `game_files/default.xbe` deben ser byte-idénticos (`md5`). La copia parcheada a mano solo existe como `default.xbe.handpatched.bak`. **No usarla.**
- No dejar datos de juego inventados en el directorio del juego.
- No regenerar alimentando el `functions.json` de una pasada anterior como semillas.
- Sembrar siempre `0x002776B0` al desensamblar: el desensamblador aún no lo encuentra solo.

---

## 2. Lo que ya está resuelto — no reabrir

### 2.1 Kernel, disco y runtime (sesiones anteriores)

1. Disco crudo POSIX: `Partition0/3/4/5` como imagen, `1` y `2` como directorio (`src/kernel/kernel_path.c`).
2. `TDATA`/`UDATA` traducidos a directorio de guardado escribible.
3. `xbox_translate_path_ex(..., want_directory)` distingue volumen crudo de directorio.
4. Thunks acotados a los imports reales del título: **115/115 resueltos, 0 sin resolver**.
5. Desbordamiento de pila del invitado: `0x002776B0` era función real no detectada, se alcanzaba por tail-jump y no se liberaba el marco. Hay página de guarda de 16 KB bajo la pila. **Hay que seguir sembrando `0x002776B0`**.
6. Bug del lifter: `cmov` no contaba como lector del acarreo. Arreglado en `tools/recomp/translator.py` con tests. No parchear el C generado a mano.
7. Pseudo-handles de 32 bits (`0xFFFFFFFE` / `0xFFFFFFFF`) comparados en ambos anchos.
8. `NtReadFile` / `NtWriteFile` reintentan hasta completar.
9. Objetos de despacho en memoria del invitado (`KEVENT` que el título inicializa solo).
10. Vblank existente; hay que encenderlo con `RECOMP_VBLANK=1`.
11. Ciclo de icall feedback: 706 destinos fusionados, stubs 386 → 81 (luego 70).

### 2.2 Esta etapa de arranque (ya cerrado)

12. Spin infinito en `KeWaitForSingleObject` (commit `a491a10`). Black limpia `SignalState` de un Notification en su propia memoria. El evento Win32 de respaldo quedaba señalado para siempre.  
    **Fix:** `xbox_KeWaitForSingleObject` trata el `SignalState` del invitado como fuente de verdad: si lee 0, resetea el handle del host antes de esperar.  
    **Efecto:** de 9 a 18 ficheros cargados.

13. Stubs 81 → 70 con otra pasada de icall feedback.

14. Nombre de fichero en `fopen` fallido (commit `0d52baa`). `STATUS_OBJECT_NAME_NOT_FOUND` ya no es opaco.

15. Trap APU de Windows aislado (commit `321c102`). Con `RECOMP_AC97_READY=1`, `xbox_memory_layout.c` ponía la APU en `PAGE_NOACCESS` para el hook VEH de Windows; en macOS era un SIGBUS sin manejador en `0xFE801100`. Ahora solo corre en `_WIN32`.

16. Registros AC'97 interceptados en ARM64 (commit `321c102`). Nuevo `src/kernel/mcpx_mmio_posix.c` + `mcpx_mmio.h`: página `0xFEC00000` en `PROT_NONE`; el manejador de `black-recomp/src/main_posix.c` llama a `xbox_mcpx_mmio_fault()`, que decodifica la instrucción ARM64 (LDR/STR), emula el acceso sobre un bloque sombra y reanuda. Modelo: *codec ready* en GLOB_STA, RR del CR de canal se limpia al escribirse, SR W1C. Traza: `RECOMP_MCPX_TRACE=1`.

17. ACK del comando del DSP (commit `321c102`). `dsp_ack_tick()` en el hilo de ack: `GPSADDR` → tabla SG → página 0 → `+0x810`, limpia el comando pendiente. Medido en Black: `GPSADDR=0x83224000`, página 0 `0x83218000`, comandos 3, 2, 2.

18. Fence del push buffer D3D (commit `d192d5a`). `xbox_Nv2aMirrorFence(0x0027BFF8, 0x2C, 0x30)` en `main_posix.c`. `sub_0026CC10` esperaba a la GPU: enviados 13, fence en 3.

19. `[FILE] open ok "nombre"` (commit `a6d1625`).

20. Herramientas de diagnóstico (commit `a6d1625`). `RECOMP_WATCH_FIELD=1` y `RECOMP_WATCH_ALLOC=<size>,<tag>`.

21. Bug del lifter: flags en arista de retorno de un bucle (commit `cd31faa`). Fix en `block_flag_transfer`, `merge_flag_states` en `lifter.py`. Test nuevo `tools/recomp/test_flag_dataflow.py`.

22. AC'97: RPBM limpia DCH (commit `321c102`).

23. Reproducción AC'97 e interrupción vector 6 (commit `e3d0d8d`). `mcpx_mmio_posix.c` consume la lista de buffers en tiempo real (48 kHz, longitud en muestras de 16 bits, medido: `0x1000` sobre 8192 bytes), marca BCIS/LVBCI/CELV, avanza CIV/PIV/PICB y repite el último buffer válido (BUP).

24. Lifter: E/S de puertos x86 (commits `e3d0d8d` y `1acffdb`). `in`/`out` llaman a `recomp_port_in/out`. Modelado: `0x80C0` (GPIO MCPX), bit 5 = campo entrelazado.

25. Translator: `jmp reg` de vuelta a la propia función (commit `1acffdb`). Ahora se despacha a las etiquetas con las que se carga ese registro.

26. Latido del DSP EP (commit `e3d0d8d`). `DirectSoundDoWork` (`sub_00219877`, cada frame) lee `[[0x224C40]]` = `0xFE85A018` y, si no vale `0x00CCCCCC`, reinicia todo el audio. `dsp_ack_tick` mantiene el latido mientras el registro de reset del EP (`0xFE85FFFC`) vale 3.

27. Hilo de ACK del DSP en POSIX (agregado en esta sesión). Sin este hilo, el command doorbell del DSP nunca se limpia y DirectSound spinnea forever.

**Efecto medido de 12–19:** de 19 a 52 aperturas correctas; DirectSound devuelve `S_OK`; `sub_00084F90` retorna; el título llega al vídeo de intro y a la pantalla de título.

---

## 3. Lo que se revirtió, y por qué importa

### 3.1 XBE parcheado a mano (dos parches, premisas falsas)

- NOPs en el entry point para saltar una lectura de `MEM32(0x10118)`. Ese offset es el **Certificate Address** del header XBE, un campo válido.
- Valores inventados en `.data` en `0x296BA0`/`0x296BA4`, presentados como corrección de "vtable bounds incorrectos". La región real está a ceros; los punteros inventados causaban las llamadas indirectas fallidas que el propio documento reportaba como síntoma.

Ninguno tenía efecto real (el C se genera de la copia sin parchear). Ambos revertidos.

### 3.2 Alias `mmap` de la ventana contigua (tres intentos)

Hacer `0x80000000` un alias real de la RAM regresionó a un cuelgue peor. La causa de ese cuelgue era el evento manual-reset (punto 12), no el aliasing.

### 3.3 Pliegue de bit 31 `xbox_canon_va` (commit `03ac9e0`)

Plegar el bit 31 solo en `0x800F0000–0x80100000` era incorrecto y dañino. El espacio del invitado está mapeado en identidad: ese rango **es el `.text` del título**. Black limpia su bloque contiguo (`sub_00013C20`, `0x800F4000–0x800FA078`) y con el pliegue ese `memset` machacaba su propio código (tabla de saltos en `0x000F7614`). El fallo aparecía miles de llamadas después.

Prueba decisiva: con el pliegue desactivado y la página baja protegida contra escritura, Black **nunca escribe** `0x000F9FB8`. Solo usa la forma `0x80xxxxxx`. Las dos formas no tienen que coincidir. La ventana contigua es almacenamiento privado. `test_guest_layout` fija el contrato.

> No volver a añadir el pliegue. Si una lectura por bit 31 devuelve ceros, el bug es la escritura que nunca ocurrió, no el mapeo.

---

## 4. Bloqueo de la Fase 7 — en progreso

### 4.1 Síntoma actual

El juego arranca, carga 52+ ficheros, reproduce el vídeo de intro y llega a la pantalla de título. La ventana Metal muestra contenido. El audio pipeline llega a SDL2 pero con silencio porque el DSP GP/EP siguen en stubs.

### 4.2 Pendiente Fase 7

1. Audio audible: completar DSP GP/EP o puentear mixbins a SDL.
2. D3D8/NV2A mínimo: dispositivo + primer frame.
3. Swap/present.
4. Push buffer del front end.
5. Texturas cuando haya evidencia de que el front end las necesita.
6. Input HID/mando físico en macOS.

---

## 5. Protocolo de una corrida (checklist)

```
[ ] checkpoint git del estado de 52 ficheros
[ ] RECOMP_VBLANK=1  RECOMP_AC97_READY=1
[ ] un solo cambio (trap / modelo AC'97 / APU)
[ ] rebuild
[ ] misma línea de arranque
[ ] ¿cuelgue?  → sample <pid> 3, localizar el bucle, leer bytes originales
[ ] ¿crash?    → VA / función / evidencia antes de tocar nada
[ ] ¿[FILE] open ok count ≥ 52?
[ ] regenerar sin --trace-functions
[ ] documentar el nuevo bloqueo antes de tocarlo
```

Tests de regresión que deben seguir verdes después de cualquier cambio de memoria:

```bash
cd /Users/user/Downloads/xboxrecomp
./build-macos/tests/guest_layout/test_guest_layout
python3 -m unittest tools.recomp.test_lifter_carry tools.recomp.test_carry_flag
```

Arranque de referencia:

```bash
cd /Users/user/Downloads/black-recomp
RECOMP_VBLANK=1 RECOMP_AC97_READY=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"
```

---

## 6. Veredicto

**Fase 6 cerrada.** El bloqueo de DirectSound se resolvió con el corte correcto: el XDK corre entero y el MCPX de macOS responde como hardware (registros AC'97 interceptados en ARM64, ACK del comando del DSP, hilo de ACK en POSIX), y el fence del push buffer D3D deja avanzar el bucle de frame. El título carga el front end completo (52 aperturas) y llega a la pantalla de título.
