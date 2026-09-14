# Checkpoint Fase 7 — Prioridad 0

**Fecha**: 14 de septiembre de 2026  
**Estado**: Fase 6 cerrada, Fase 7 en progreso  
**Branch**: `macos-port` (xboxrecomp) + `main` (black-recomp)  
**Plan maestro**: `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md`

## Estado verificado

### Arranque de referencia SIN `RECOMP_AC97_READY`
- Comando: `RECOMP_VBLANK=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"`
- Resultado: **Crash esperado** en `sub_00084F90` (SIGBUS en `0xFFFFFFEC`)
- Aperturas de fichero: **19** (última: `BkXboxFx.bin`)
- Consistente con documentación: `black-port-progreso-ajustado.md` §6, `black-port-fase6-cierre.md` §0

### Arranque CON `RECOMP_AC97_READY=1`
- Comando: `RECOMP_VBLANK=1 RECOMP_AC97_READY=1 ./build/black-recomp "/Users/user/Downloads/Black (USA).xiso"`
- Resultado: **Sin crash** durante >60s
- Aperturas de fichero: **52+** (supera las 48 esperadas)
- Proceso corriendo en loop de juego normal
- Ventana Metal con contenido (640x480)
- GPU: draws con coordenadas, framebuffer actualizándose
- Log: `[GPU] FBWIN set va=0x82E50000 pitch=2560` confirmado

### Backups guardados
- `functions.json`: `/tmp/functions.json.backup.20260913-085005`
- Tamaño: 4,763,253 bytes

## Reglas operativas vigentes

1. Arrancar siempre con `RECOMP_VBLANK=1`.
2. El `default.xbe` del directorio de juego y `game_files/default.xbe` son byte-idénticos (MD5: `ef09df225db8fbb97f0afccac7a7faaf`).
3. No dejar datos de juego inventados en el directorio del juego.
4. No regenerar usando un `functions.json` anterior como semilla.
5. Sembrar `0x002776B0` al desensamblar.
6. No reintroducir parches de las secciones 5 y 9 de `black-port-progreso-ajustado.md`:
   - XBE parcheado a mano
   - Alias `mmap` de la ventana contigua
   - Pliegue de bit 31 `xbox_canon_va`

## Cambios clave desde último checkpoint

### xboxrecomp (`macos-port`)
- `src/kernel/xbox_memory_layout.c`: agregado `xbox_DspAckStart/Stop()` para POSIX (pthread). Sin este hilo, el command doorbell del DSP nunca se limpia y DirectSound spinnea forever.
- `src/apu/apu_state.h`: agregado campo `bool dsp_wrote_frame` a `monitor`.
- `src/apu/apu_dsp.c`: seteado `dsp_wrote_frame = true` cuando el DSP escribe datos.
- `src/apu/apu_xaudio2.c`: backend SDL2 para POSIX (reemplaza stubs).

### black-recomp (`main`)
- `src/main_posix.c`: inicialización de APU, interceptación MMIO APU/AC97, NV2A MMIO hooks, USB/OHCI, input teclado, ventana Metal.
- `CMakeLists.txt`: enlazado con Metal/QuartzCore/Cocoa en macOS.

## Próximos pasos — Fase 7

Seguir el plan maestro `Black_Apple_Silicon_Fase7_Plan_Desbloqueo.md` en este orden:

1. **F7.0 — Instrumentación de fronteras:** crear `diagnostics/f7_boundary.log` con contadores para audio, gráficos e input.
2. **F7.1 — Audio host:** medir `frame_buf` primero; si hay samples, puentear a SDL sin implementar GP/EP.
3. **F7.2 — Graphics path discovery:** confirmar si Black usa D3D8 API o push buffer directo.
4. **F7.3 — Primer draw real:** clear + primitive + present en OpenGL.
5. **F7.4 — Present real:** `GAME_PRESENT` sin depender de `RECOMP_FB_WINDOW`.
6. **F7.5 — Texturas:** mínimo formato necesario para el primer draw.
7. **F7.6 — Input host:** SDL GameController antes que OHCI completo.
8. **F7.7 — Primer nivel:** transición frontend → nivel jugable.

Criterios detallados y NO-hacer: ver `black-port-progreso-ajustado.md` §7 y plan maestro.

## Regla de diagnóstico

**Medir primero, modificar después.**

Si un experimento rompe el arranque, restaurar `functions.json` desde el backup y revertir cambios.
