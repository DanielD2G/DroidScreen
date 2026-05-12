# Android Latency Validation Runbook

Este documento describe el flujo que usamos para validar DroidScreen en la
tablet Android contra la app de macOS instalada y firmada. La idea es poder
repetir la prueba sin depender de memoria: compilar, instalar, abrir, capturar
pantalla, validar que no haya pantalla negra, y leer las metricas de latencia y
calidad.

## Objetivo

Validar una build end-to-end con:

- Desktop macOS instalado como `/Applications/DroidScreen.app`.
- `.app` firmada con identidad estable para conservar permisos de macOS/TCC.
- APK instalado en la tablet via `adb`.
- Conexion por USB usando `adb forward`.
- Capturas de pantalla adb en varios momentos del flujo.
- Analisis de capturas para detectar pantalla negra o imagen en blanco.
- Logs y metricas de latencia, FPS, errores de feed y calidad bajo movimiento.

## Prerrequisitos

- macOS con Xcode Command Line Tools, CMake y `codesign`.
- Android SDK disponible, normalmente en `~/Library/Android/sdk`.
- JDK disponible. En esta Mac se uso `JAVA_HOME=/Applications/PyCharm.app/Contents/jbr/Contents/Home` cuando no habia JDK global.
- Tablet conectada por USB con USB debugging activo.
- `adb devices` debe mostrar la tablet en estado `device`.
- Permisos de Screen Recording para la app firmada de desktop.

Para firma estable en macOS, configurar:

```bash
mkdir -p ~/.config/droidscreen
$EDITOR ~/.config/droidscreen/build.env
```

Ejemplo:

```bash
export DROIDSCREEN_CODESIGN_IDENTITY="Apple Development: your@email.com (TEAMID)"
```

Si no se configura `DROIDSCREEN_CODESIGN_IDENTITY`, `scripts/build_macos.sh`
firma ad-hoc (`-`). Eso sirve para pruebas rapidas, pero puede romper permisos
de Screen Recording entre rebuilds porque la identidad de codigo no es estable.

## Flujo Automatizado

El flujo completo esta en:

```bash
scripts/validate_android_latency.sh
```

Ejecucion recomendada:

```bash
JAVA_HOME=/Applications/PyCharm.app/Contents/jbr/Contents/Home \
ANDROID_HOME=$HOME/Library/Android/sdk \
ANDROID_SDK_ROOT=$HOME/Library/Android/sdk \
RUNS=1 \
RUN_SECONDS=30 \
./scripts/validate_android_latency.sh
```

Para una validacion completa de estabilidad, usar:

```bash
JAVA_HOME=/Applications/PyCharm.app/Contents/jbr/Contents/Home \
ANDROID_HOME=$HOME/Library/Android/sdk \
ANDROID_SDK_ROOT=$HOME/Library/Android/sdk \
RUNS=3 \
RUN_SECONDS=30 \
./scripts/validate_android_latency.sh
```

El script:

1. Compila Android con `./gradlew :app:assembleDebug`.
2. Compila macOS con `./scripts/build_macos.sh`.
3. Firma el bundle macOS usando `DROIDSCREEN_CODESIGN_IDENTITY`.
4. Instala la app firmada en `/Applications/DroidScreen.app`.
5. Verifica firma con `codesign --verify --deep --strict --verbose=2`.
6. Instala el APK en la tablet con `adb install --no-streaming -r`.
7. Configura `adb forward tcp:38271 tcp:38271`.
8. Limpia logcat.
9. Lanza la app Android.
10. Toma captura de la pantalla de espera.
11. Lanza `/Applications/DroidScreen.app/Contents/MacOS/droidscreen_desktop`.
12. Activa la escena de validacion con ventana real en movimiento.
13. Espera handshake y primeras metricas `lat feed=`.
14. Toma capturas a 5s, 20s y al final.
15. Analiza cada PNG para detectar pantalla negra/blanca.
16. Copia logs de desktop y logcat.
17. Parse metrics y guarda artefactos.

Los artefactos quedan en:

```bash
/tmp/droidscreen-validation/moonlight-port-YYYYMMDD-HHMMSS/run-N/
```

Archivos importantes:

- `01-waiting.png`: app Android esperando conexion.
- `02-active-5s.png`: stream activo despues de unos segundos.
- `03-active-20s.png`: stream activo con ventana en movimiento.
- `04-active-final.png`: captura final.
- `*.analysis`: analisis de luminancia, varianza, `non_black` y `blockiness`.
- `logcat.txt`: logs Android.
- `desktop.log`: stdout/stderr del desktop lanzado por el script.
- `desktop-debug.log`: log persistente de DroidScreen desktop.
- `metrics.txt`: resumen parseado.

## Flujo Manual Equivalente

Usar este flujo cuando se quiera depurar paso a paso.

### 1. Compilar Android

```bash
cd android
JAVA_HOME=/Applications/PyCharm.app/Contents/jbr/Contents/Home \
ANDROID_HOME=$HOME/Library/Android/sdk \
ANDROID_SDK_ROOT=$HOME/Library/Android/sdk \
./gradlew :app:assembleDebug
cd ..
```

APK esperado:

```bash
android/app/build/outputs/apk/debug/app-debug.apk
```

### 2. Compilar y firmar macOS

```bash
./scripts/build_macos.sh
```

Bundle esperado:

```bash
build-macos/desktop/macos/droidscreen_desktop.app
```

### 3. Instalar la `.app` firmada en `/Applications`

Cerrar cualquier instancia anterior:

```bash
pkill -f /Applications/DroidScreen.app/Contents/MacOS/droidscreen_desktop || true
```

Instalar:

```bash
rm -rf /Applications/DroidScreen.app
/usr/bin/ditto build-macos/desktop/macos/droidscreen_desktop.app /Applications/DroidScreen.app
xattr -dr com.apple.quarantine /Applications/DroidScreen.app 2>/dev/null || true
codesign --verify --deep --strict --verbose=2 /Applications/DroidScreen.app
```

Verificar identidad si hace falta:

```bash
codesign -dv --verbose=4 /Applications/DroidScreen.app 2>&1 | sed -n '1,20p'
```

La app que se debe abrir para pruebas es:

```bash
/Applications/DroidScreen.app/Contents/MacOS/droidscreen_desktop
```

No usar directamente el bundle dentro de `build-macos` para pruebas de permisos,
porque el objetivo es conservar una identidad estable en `/Applications`.

### 4. Instalar APK en la tablet

```bash
adb devices
adb install --no-streaming -r android/app/build/outputs/apk/debug/app-debug.apk
```

### 5. Preparar conexion USB

DroidScreen Android escucha en la tablet. El desktop se conecta a localhost en
la Mac mediante forward:

```bash
adb forward --remove tcp:38271 2>/dev/null || true
adb forward tcp:38271 tcp:38271
```

### 6. Abrir la app Android

```bash
adb logcat -c
adb shell am force-stop com.droidscreen.app || true
adb shell am start -n com.droidscreen.app/.MainActivity
sleep 2
adb shell pidof com.droidscreen.app
```

Captura de espera:

```bash
mkdir -p /tmp/droidscreen-manual
adb exec-out screencap -p > /tmp/droidscreen-manual/01-waiting.png
```

### 7. Abrir desktop firmado

Para validacion con escena automatica y ventana real moviendose:

```bash
DROIDSCREEN_VALIDATION_SCENE=1 \
DROIDSCREEN_VALIDATION_HELPER_PATH="$PWD/build-macos/desktop/macos/droidscreen_validation_scene_helper" \
/Applications/DroidScreen.app/Contents/MacOS/droidscreen_desktop \
  --port 38271 \
  > /tmp/droidscreen-manual/desktop.log 2>&1
```

Para uso manual sin escena de validacion, abrir:

```bash
/Applications/DroidScreen.app/Contents/MacOS/droidscreen_desktop --port 38271
```

La app crea el display virtual, captura con ScreenCaptureKit, codifica, y envia
el stream a la tablet por el tunnel USB.

### 8. Tomar capturas del flujo

En otra terminal:

```bash
sleep 5
adb exec-out screencap -p > /tmp/droidscreen-manual/02-active-5s.png

sleep 15
adb exec-out screencap -p > /tmp/droidscreen-manual/03-active-20s.png

sleep 10
adb exec-out screencap -p > /tmp/droidscreen-manual/04-active-final.png
```

Abrir las capturas y validar visualmente:

- No debe verse negra.
- Debe verse el contenido del display virtual.
- En validacion, debe verse la ventana `DroidScreen Motion` moviendose.
- No debe haber pixelacion fuerte al mover ventanas.

## Metricas Que Hay Que Mirar

Android emite lineas similares a:

```text
lat feed=10.5ms release=21.0ms desk=5.8ms android=12.1ms rtt=1.8ms capenc=5.8ms encsend=0.0ms
decode[sync]: fed=... rendered=... err=0 | 66.2 fed/s 65.9 render/s
```

Campos:

- `lat feed`: captura desktop hasta feed al decoder Android.
- `lat release`: captura desktop hasta release/render en Android.
- `desk`: captura hasta envio desde desktop.
- `android`: recepcion Android hasta release.
- `rtt`: round-trip estimado por ping.
- `capenc`: captura hasta encode listo.
- `encsend`: encode listo hasta envio.
- `feed_errors_max`: debe ser 0 o casi 0.
- `fed_fps_p50`: FPS alimentado al decoder.
- `render_fps_p50`: FPS renderizado.

Desktop emite lineas de calidad:

```text
[quality] fps=66.3 kbps=8310 frame=129.2kb delta=124.7kb key=493.6kb motion=3.51 bits_per_motion=37116 budget=250.0kb
```

Campos:

- `fps`: FPS codificado/enviado por desktop.
- `kbps`: bitrate real observado.
- `frame`: kbits promedio por frame.
- `delta`: kbits promedio en P-frames.
- `key`: kbits promedio en keyframes.
- `motion`: diferencia promedio de luminancia entre frames capturados.
- `bits_per_motion`: bits disponibles normalizados por movimiento.
- `budget`: kbits/frame teoricos segun bitrate configurado y FPS objetivo.

Si `motion` sube y `bits_per_motion` cae fuerte al mismo tiempo que se ve
pixelacion, el problema es presion de compresion/bitrate. Si hay pixelacion con
`feed_errors` o backlog, investigar decoder/transporte. Si aparece pantalla
negra, mirar las capturas `*.analysis` y logs de handshake/decoder.

Las capturas generan analisis como:

```text
mean=49.83 variance=1656.66 non_black=0.9998 blockiness=0.208
```

Campos:

- `mean`: luminancia media.
- `variance`: varianza de imagen.
- `non_black`: proporcion de pixeles no negros.
- `blockiness`: indicador aproximado de bordes en grilla 16x16.

Para pantalla negra/blanca, el script falla si `mean`, `variance` o
`non_black` estan fuera de umbral.

## Criterio De Aceptacion Practico

Para una corrida buena:

- Las 3 capturas activas deben ser visibles.
- `non_black` debe estar alto, normalmente cerca de `1.0000`.
- `feed_errors_max` debe ser `0`.
- `render_fps_p50` debe estar estable para el modo probado.
- `release_p50` debe ser lo mas bajo posible sin romper imagen.
- Bajo movimiento, deben existir P-frames reales: `delta` debe ser no-cero y
  `key` solo debe aparecer periodicamente.
- No debe haber pantalla negra al reconectar.

Nota: el script mantiene un umbral estricto de `release_p50 <= 15ms`. En la
POCO Pad, durante las pruebas con ventana movil, una corrida util para depurar
dio aproximadamente:

```text
release_p50=21.0ms feed_errors_max=0 render_fps_p50=65.9 quality_kbps_p50=8310 motion_p50=3.51
```

Ese resultado no pasa el umbral agresivo de 15ms, pero si confirma imagen
visible, P-frames reales y ausencia de errores de feed.

## Problemas Comunes

### Pantalla negra en tablet

Revisar:

```bash
cat /tmp/droidscreen-validation/.../run-1/*.analysis
rg -n "handshake|decoder|configure|lat feed|feed_errors" /tmp/droidscreen-validation/.../run-1/logcat.txt
```

Tambien confirmar que la app abierta sea `/Applications/DroidScreen.app`, no un
bundle temporal sin permisos.

### macOS pide permisos otra vez

Confirmar firma estable:

```bash
codesign -dv --verbose=4 /Applications/DroidScreen.app 2>&1 | rg "Authority|TeamIdentifier|Identifier"
```

Si esta ad-hoc, configurar `DROIDSCREEN_CODESIGN_IDENTITY` y reinstalar la app
en `/Applications`.

### No conecta

Revisar:

```bash
adb devices
adb forward --list
adb shell pidof com.droidscreen.app
```

Recrear el forward:

```bash
adb forward --remove tcp:38271 2>/dev/null || true
adb forward tcp:38271 tcp:38271
```

### Pixelacion al mover ventanas

Mirar `desktop-debug.log`:

```bash
rg -n "\[quality\]" /tmp/droidscreen-validation/.../run-1/desktop-debug.log
```

La senal esperada despues del fix es:

- NALs H.264 tipo IDR al inicio y luego P-frames.
- `delta` no-cero.
- `render_fps_p50` estable.
- `feed_errors_max=0`.

Si `delta=0.0kb` durante toda la corrida y `key` aparece en todos los frames,
el encoder esta comportandose como intra-only; eso produce mayor bitrate por
frame, menor FPS y peor calidad bajo movimiento.
