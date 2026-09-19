# Black (Xbox) -> Apple Silicon macOS
## Fase 7 - Plan de desbloqueo actualizado

**Fecha:** 14 de septiembre de 2026  
**Última actualización:** 14 de septiembre de 2026 - Ventana transparente detectada, F7.3 en investigación

---

## 1. Objetivo de la Fase 7

Conseguir un camino verificable desde el arranque hasta el primer frame jugable.

```text
BOOT
  -> FRONTEND
  -> AUDIO HOST FUNCIONAL
  -> PRIMER DRAW REAL
  -> PRESENT REAL
  -> INPUT HOST FUNCIONAL
  -> PRIMER NIVEL
  -> PRIMER FRAME CONTROLABLE
```

---

## 2. Estado actual de milestones

| Milestone | Estado | Nota |
|---|---|---|
| F7.0 Instrumentación | COMPLETADO | Contadores y trazas activas |
| F7.1 Audio pipeline | CONFIRMADO | DSP escribe non-zero, SDL recibe PCM |
| F7.2 Graphics path | COMPLETADO | Push buffer confirmado |
| F7.3 First draw visible | EN INVESTIGACIÓN | Doble buffer confirmado, ventana transparente |
| F7.3a Surface scanner | COMPLETADO | 0x82E50000 tiene 32% no-cero |
| F7.3b Flip state | COMPLETADO | modulo=2, write alterna |
| F7.4 Present real | PENDIENTE | Bloqueado por F7.3 |
| F7.5 Texturas | PENDIENTE | |
| F7.6 Input | PENDIENTE | |
| F7.7 Primer nivel | PENDIENTE | |

---

## 3. Hallazgos clave

### Audio - CONFIRMADO
- Pipeline completo funcional: VP -> mixbins -> DSP -> SDL
- DSP escribe samples no-cero en framebuffer
- SDL recibe buffers PCM no-silenciosos (peak=335)
- Bug corregido: `apu_vp.c`/`apu_core.c` preservan DSP-written `frame_buf`

### Graphics - PUSH BUFFER CONFIRMADO, DOBLE BUFFER IDENTIFICADO
- Black usa EXCLUSIVAMENTE push buffer NV2A, no D3D8 API
- Executor activo: 221+ clears, 1466+ draws, 8482 triángulos
- Doble buffer confirmado:
  - Front: 0x82E50000 (32% no-cero, tiene contenido del menú)
  - Back: 0x82F7C000 (0% no-cero, negro)
- Flip state: modulo=2, write alterna 0/1
- 73 métodos NV2A no manejados (frecuencia 1-2 calls)

### Ventana - TRANSPARENTE
- Al recompilar con cambios de presentación, la ventana se ve transparente
- No es negro, es transparente
- Clear color cambiado a rojo (1.0, 0.0, 0.0, 1.0)
- Shader con fallback UV cuando textura es negra
- Pipeline Metal: OK (nextDrawable, encoder, presentDrawable, commit)

---

## 4. Trabajo pendiente inmediato

### F7.3 - Investigar ventana transparente

**Hipótesis:**
1. Ventana pierde opacidad al cambiar código
2. CAMetalLayer no presenta correctamente después de rebuild
3. clearColor con alpha < 1.0 hace transparente la ventana
4. NSWindow level/behavior afecta visibilidad

**Próxima prueba:**
1. Verificar si clear rojo es visible
2. Si es visible: problema resuelto, proceder a F7.4
3. Si no: investigar layer/window configuration

### F7.4 - Present real
- Eliminar scanner de candidatas
- Presentar directamente front buffer
- Eliminar RECOMP_FB_CANDIDATES

### F7.5 - Texturas
- 72 batches con texcoords pero sin stage usable
- Implementar formato de textura del logo

### F7.6 - Input
- Probar con input script: START,WAIT,A
- Verificar polls durante gameplay

---

## 5. Cómo ejecutar la prueba

```bash
cd /Users/user/Downloads/black-recomp/build-fb

RECOMP_VBLANK=1 \
RECOMP_AC97_READY=1 \
RECOMP_PB_EXEC=1 \
RECOMP_FB_WINDOW=1 \
RECOMP_FB_CANDIDATES=/tmp/black-fb-candidates/ \
./black-recomp "/Users/user/Downloads/Black (USA).xiso"
```

**Verificar:**
- ¿La ventana es transparente o se ve rojo/UV?
- ¿El juego es visible?
- ¿El scanner presenta el contenido correcto?

---

## 6. Archivos de diagnóstico

- `/Users/user/Downloads/xboxrecomp/diagnostics/f7_state.json`
- `/Users/user/Downloads/xboxrecomp/diagnostics/f7_boundary.log`
- `/tmp/black-fb-candidates/` - BMPs de candidatos
