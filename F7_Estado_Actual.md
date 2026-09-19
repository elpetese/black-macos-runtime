# Black (Xbox) -> Apple Silicon macOS
## Fase 7 - Estado actual y plan de desbloqueo

**Fecha:** 14 de septiembre de 2026  
**Última actualización:** 14 de septiembre de 2026 - F7.6 input confirmado, listo para F7.7 primer nivel

---

## 1. Estado actual del proyecto

| Milestone | Estado | Nota |
|---|---|---|
| F7.0 Instrumentación | COMPLETADO | Contadores y trazas activas |
| F7.1 Audio pipeline | CONFIRMADO | DSP escribe non-zero, SDL recibe PCM |
| F7.2 Graphics path | COMPLETADO | Push buffer confirmado |
| F7.3 First draw visible | COMPLETADO | Doble buffer confirmado, scanner activo |
| F7.3a Surface scanner | COMPLETADO | 0x82E50000 tiene 32% no-cero |
| F7.3b Flip state | COMPLETADO | modulo=2, write alterna |
| F7.4 Present real | COMPLETADO | Scanner presenta 0x82E50000 automáticamente |
| F7.5 Texturas | PENDIENTE | 72 batches sin stage usable |
| F7.6 Input | CONFIRMADO | polls=2626, nonzero_buttons=41 con RECOMP_USB=1 |
| F7.7 Primer nivel | SIGUIENTE | Carga de nivel confirmada, falta verificar control |

---

## 2. Lo que se ha hecho

### F7.0 - Instrumentación de fronteras
- Archivos nuevos: `f7_instrumentation.h`, `f7_instrumentation.c`
- Instrumentado audio: VP, mixbins, DSP, SDL
- Instrumentado graphics: push buffer executor, clears, draws, batches
- Instrumentado input: polls, nonzero buttons
- F7 print counts en SIGTERM/SIGINT

### F7.1 - Audio pipeline
- Corregido bug en `apu_vp.c`/`apu_core.c`: DSP-written `frame_buf` era sobrescrito
- Medido: VP procesa ~388k voices, SDL recibe PCM no-silencioso (peak=335)
- Estado: pipeline funcional

### F7.2 - Graphics path discovery
- Confirmado: Black usa EXCLUSIVAMENTE push buffer NV2A
- D3D8 API stubs: 0 llamadas
- Push buffer executor: 221+ clears, 1466+ draws, 8482 triángulos
- 73 métodos NV2A no manejados identificados

### F7.3 - Surface candidates scanner (Paso 1)
- Implementado `build_candidates()` en `fb_present_posix.m`
- Construye candidatos: superficie actual, GPU summary, ±0x12C000, historial
- Puntúa con grilla 32×32
- Escribe BMP por candidato con addr en filename
- Presenta automáticamente el candidato con mayor score

**Hallazgo:**
```
0x82E50000 → score=357, ratio=32.46% → CON CONTENIDO
0x82F7C000 → score=0, ratio=0.00% → NEGRO
```

**Veredicto:** Doble buffer CONFIRMADO. Front buffer = 0x82E50000.

### F7.3b - Flip state instrumentation (Paso 2)
- Agregado logging en `nv2a_pb_exec.c`:
  - SET_SURFACE_COLOR_OFFSET
  - SET_FLIP_READ/WRITE/MODULO
  - FLIP_INCREMENT_WRITE
  - FLIP_STALL
- Resumen GPU ahora incluye flip state

**Hallazgo:**
```
SET_FLIP_WRITE=1
SET_FLIP_MODULO=2
FLIP_INCREMENT_WRITE -> write=0 read=0 modulo=2
FLIP_INCREMENT_WRITE -> write=1 read=0 modulo=2
```

**Veredicto:** Flip con modulo=2 confirmado. Doble buffer estándar.

### F7.4 - Present real
- Scanner de candidatas presenta automáticamente 0x82E50000
- Ventana Metal funcional
- Juego visible confirmado

### F7.6 - Input host - CONFIRMADO

**Ejecución con RECOMP_USB=1 y RECOMP_INPUT_SCRIPT:**
```bash
RECOMP_INPUT_SCRIPT="0:start:300,2000:a:300,5000:a:300,7000:start:300"
```

**Resultados:**
```
INPUT polls=2626 nonzero_buttons=41
```

**Hallazgos:**
- Input script funciona correctamente
- Juego avanza más allá del frontend
- Archivos cargados: InGameOn.bin, Loading.bin, AK_N.xmv, AK47.bks
- GPU activo en gameplay: draws ~1430, clears=217

**Veredicto:** F7.6 CONFIRMADO. Input host funcional.

---

## 3. Hallazgos clave

1. **Audio pipeline funcional:** DSP escribe non-zero en framebuffer, SDL recibe PCM audible
2. **Graphics usa push buffer NV2A exclusivamente:** D3D8 API no se usa
3. **Doble buffer confirmado:** Dos superficies separadas por 0x12C000
   - Front: 0x82E50000 (32% no-cero, tiene el menú)
   - Back: 0x82F7C000 (0% no-cero, negro)
4. **Flip state activo:** modulo=2, write alterna 0/1
5. **Scanner de candidatas funciona:** Presenta 0x82E50000 automáticamente
6. **Input confirmado:** polls=2626, nonzero_buttons=41 con RECOMP_USB=1
7. **Juego avanza a gameplay:** Carga archivos de nivel (InGameOn.bin, Loading.bin, AK_N.xmv)

---

## 4. Lo que falta

### F7.7 - Primer nivel
- [ ] Verificar que el juego carga un nivel completamente
- [ ] Confirmar que el input controla el personaje
- [ ] Verificar que el audio reproduce en gameplay
- [ ] Capturar file opens específicos del nivel
- [ ] Medir estabilidad en sesión extendida

### F7.5 - Texturas
- [ ] 72 batches con texcoords pero sin stage usable (disabled=760)
- [ ] Implementar formato de textura del logo del title
- [ ] Esto mejorará la calidad visual del juego

### F7.8 - Primer frame controlable
- [ ] Sesión estable >= 10 min
- [ ] Sin crashes ni hangs
- [ ] Input responde correctamente

### F7.9 - Stubs críticos
- [ ] Identificar stubs alcanzados en primer nivel
- [ ] Implementar solo los críticos (clase A)

---

## 5. Próximo paso inmediato

**Objetivo:** Confirmar primer nivel completo.

**Comando:**
```bash
cd /Users/user/Downloads/black-recomp/build-fb

RECOMP_VBLANK=1 \
RECOMP_AC97_READY=1 \
RECOMP_PB_EXEC=1 \
RECOMP_FB_WINDOW=1 \
RECOMP_FB_CANDIDATES=/tmp/black-fb-candidates/ \
RECOMP_USB=1 \
RECOMP_INPUT_SCRIPT="0:start:300,2000:a:300,5000:a:300,7000:start:300,9000:a:300,11000:a:300" \
./black-recomp "/Users/user/Downloads/Black (USA).xiso"
```

**Verificar:**
1. ¿El juego carga un nivel completamente?
2. ¿El personaje es controlable?
3. ¿Hay audio en gameplay?
4. ¿Qué archivos nuevos se abren?
5. ¿Cuántos draws/clears hay en el nivel?

**Según resultado:**
- Si carga el nivel → F7.7 CONFIRMADO
- Si se cuelga → identificar crash VA y subsistema
- Si no responde a input → investigar polling

---

## 6. Archivos de diagnóstico

- `/Users/user/Downloads/xboxrecomp/diagnostics/f7_state.json`
- `/Users/user/Downloads/xboxrecomp/diagnostics/f7_boundary.log`
- `/tmp/black-fb-candidates/` - BMPs de candidatos
- `/tmp/black_input_usb.log` - Log de última ejecución con input

---

## 7. Lecciones aprendidas

1. **No asumir que la ventana = juego visible:** El test pattern funcionaba pero el juego era negro
2. **El log puede ser engañoso:** `update_pixels OK` no significa que el contenido sea correcto
3. **BMP dumps son esenciales:** Confirmaron que el problema era contenido negro, no presentación
4. **Las direcciones de superficie pueden cambiar:** El juego puede usar múltiples buffers
5. **F7.0 instrumentation fue crítica:** Sin los contadores no sabríamos que el GPU estaba activo
6. **Input requiere RECOMP_USB=1:** Sin esto, xbox_InputInit no se llama y el script no funciona
7. **El formato de script es estricto:** `ms:botón:hold_ms`, no `START,WAIT,A`
8. **Las sesiones largas son necesarias:** En 40s no aparecen polls, en 90s sí aparecen 2626 polls

---

## 8. Próximos milestones

| Milestone | Estado | Próximo paso |
|---|---|---|
| F7.7 Primer nivel | EN PROGRESO | Confirmar carga completa y control |
| F7.8 Estabilidad | PENDIENTE | Sesión >= 10 min sin crashes |
| F7.9 Stubs críticos | PENDIENTE | Implementar solo clase A |
