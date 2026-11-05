# furimodem IQ — modo SDR sobre el módem del teléfono

Convierte el módem del FuriPhone en un **receptor IQ** sin necesidad de célula: el firmware se
parchea **en binario** la imagen del modem. se carga con una herramienta propia que sabe
revertirlo, y los datos se leen por SoapySDR desde una app con waterfall.

## Piezas

| ruta | qué es |
|---|---|
| `driver/` | módulo SoapySDR (`driver=furimodem_iq`): controles de sintonía, ancho y ganancia, salvaguarda de TX y lectura del anillo de IQ |
| `app/` | demo Wayland + EGL/GLES: waterfall, espectro y controles; fuentes sintética, fichero y dispositivo |
| `gui/` | ventana GTK3 para el teléfono: estado, carga, activación, descarga y vuelta a fábrica; invoca `sdr-load` como subproceso |
| `tools/sdr-load.cpp` | cargador: comprueba precondiciones, aplica la imagen del modo SDR, **activa el modo** y revierte |
| `tools/make-iqfile.py` | genera ficheros de bloques IQ con un tono conocido, para probar sin teléfono |
| `tools/test-iq5file.sh` | ciclo completo de prueba en local: 5 bits, 8 bits, volcado ambiguo y CS16 |
| `tools/crossbuild2.sh` | compilación cruzada reproducible del paquete entero (base **Debian sid**, la del teléfono) |
| `tools/iqdump/` | utillaje de análisis y generación de parches |
| `docs/266-arquitectura-modo-sdr.md` | mapa técnico: piezas, formatos, qué está medido y qué límites hay |
| `docs/261-formato-de-muestras.md` | contrato del anillo: cabecera, desempaquetado y prueba local |

## Puesta en marcha

```sh
# 1. módulo Soapy (en el teléfono, con las cabeceras de SoapySDR instaladas)
cd driver && cmake -S . -B build && cmake --build build -j
sudo cmake --install build

# 2. app
cd app && make -j && sudo make install     # deja binario, icono y accesos

# 3. modo SDR
sudo sdr-load --image <imagen-del-modo.img>     # aplica la imagen
                                                # (el móvil se reinicia)
sudo sdr-load --resume                          # completa la activación, sin reescribir
sudo sdr-load --status                          # imagen, PLL, raíles, lazos y si el anillo late
#    ... y para volver:
sudo sdr-load --deactivate                      # para los lazos
sudo sdr-load --restore                         # devuelve el firmware de stock
```

`sdr-load --activate` ejecuta la secuencia de arranque del receptor (PLL, raíles y lazos, nueve
pasos verificados) **solo con las imágenes de su tabla de perfiles**: hoy `standard-a`, `standard-b` y
`sdr-8bit`. Esta última es la del modo SDR: 8 bits por componente, origen 3 y **sintonía fija en
2.115,0 MHz** (banda 1), así que `--activate` la escribe, arranca el barrido y comprueba que el anillo
publique. **El móvil se reinicia al escribir la imagen**: se corta cualquier llamada en curso y no hay
servicio móvil hasta volver a stock.
Con una imagen que no esté en la tabla pasa lo mismo: se niega y lista las que conoce, en vez
de pedir modos a ciegas. Un perfil nuevo se pasa con `--profile` (nombre o fichero).

**`--resume` es lo que termina el trabajo tras el reinicio.** Comprueba que la partición tiene la
imagen que el estado registró (sha256 y tamaño), cierra el pendiente y ejecuta la activación; la
partición **no** se vuelve a escribir. `--activate` es idempotente y hace lo mismo, así que una
activación a medias no se queda ahí. `--status` lo dice en su línea `next`.

Con la imagen cargada, la captura la dispara el **barrido de frecuencias del propio teléfono**:

```sh
sudo busctl --timeout=30 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+CFUN=1"
sudo busctl --timeout=300 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+COPS=?"
```

Sin barrido no hay datos: el receptor entrega ráfagas, no un flujo continuo.

La misma secuencia, desde phosh: `furimodem-sdr` (ver `gui/README.md`) muestra el estado y lanza
`sdr-load` como subproceso para cargar, activar, detener y volver a fábrica. Las operaciones que
necesitan root pasan por `pkexec`, de modo que el escritorio pide autorización en su propio
diálogo; sin pkexec, o si se cancela el diálogo, la ventana enseña la orden exacta con un botón
para copiarla. La ventana es oscura por sí misma, sea cual sea el tema del escritorio.

En el teléfono aparecen dos accesos: **Furimodem IQ** (dispositivo) y **Furimodem IQ (captura)**
(reproduce un volcado guardado en `/var/lib/furimodem-iq/captura.iq`).

## Estado y límites (importante)

Lo que **hoy funciona**, medido en el teléfono: el waterfall pinta **I/Q real del módem** en la
pantalla, con el eje puesto en la frecuencia que trae cada bloque; durante un barrido se siguieron
**213 frecuencias distintas entre 763,0 y 2.685,0 MHz** en 50 s; y la captura en **8 bits por
componente** funciona con la toma de RF correcta (un CW de prueba aparece en el bin que le toca en
**9/9** capturas). El waterfall en vivo se probó con el formato de 5 bits; el de 8 bits, sobre
capturas ya volcadas y abiertas con el mismo lector y la misma app.

Los dos límites que condicionan el uso:

- **El camino de 8 bits es menos sensible.** Gana tres bits de resolución (18 dB de cuantización) y
  quita la atadura entre bits bajos del formato de 5 bits, pero llega del orden de **26 dB más
  débil** porque no pasa por el control automático de ganancia. Con el mismo tono: 38,1 dB
  pico/mediana en 5 bits frente a 11,7 dB en 8 bits. Es más resolución con menos nivel, no mejor
  relación señal/ruido. Igualar la ganancia de las dos tomas queda pendiente.
- **La frecuencia barre; no se sintoniza.** El receptor recorre su plan de barrido y cada bloque
  lleva en la cabecera la frecuencia de esa ráfaga, que es lo que la app pinta en el eje. Los
  controles de frecuencia de la interfaz **no mueven el oscilador**: la app envía la petición al
  módulo y la mantiene visible dos segundos, pero la cabecera vuelve a imponerse y un aviso ámbar
  indica que la sintonía la controla el perfil activado (o la cabecera de bloque, con un fichero).
  Forzar la etiqueta en la imagen sigue **etiquetando pero no sintonizando** (medido: con la
  etiqueta fija el tono desaparece). Para ver una banda concreta hay que esperar a que el barrido
  pase por ella. Detalles: `docs/271-controles-de-la-app.md`.

Otros límites:

- **Sin barrido no hay flujo**: no hay captura continua ni grabación continua. Con el lazo de
  copia antiguo se midieron 13,5-15 volcados/s sostenidos (43/s con el lector corregido), del orden
  de 0,3-0,5 MS/s de muestras útiles; el anillo del buscador publica ~100 capturas/s durante el
  barrido.
- **La cuantización de 5 bits** sirve para detectar y situar una señal, no para demodular ni medir
  amplitud fina. La de 8 bits da tres bits más, con el límite de sensibilidad de arriba.
- **TX no está implementado** y el driver lo bloquea por diseño.
- El modo SDR **desactiva la radio del módem**: con la imagen cargada el servicio de telefonía no
  arranca porque no hay HAL de radio. Es el comportamiento buscado; se vuelve con `--restore`.

Detalle completo en `docs/266-arquitectura-modo-sdr.md`.

## Uso de cada herramienta

```sh
# app: ventana en el teléfono, o análisis sin GPU
fmiq-app                                              # waterfall en vivo (Wayland + EGL)
fmiq-app --headless 60 --source soapy --out /tmp/demo.ppm   # sin ventana, fuente dispositivo
fmiq-app --headless 12 --fft 2048 --source file --file volcado.bin --check-bin 1280
fmiq-app --gpu-test 120 --fft 512 --size 480x1025 --source soapy --out frame.ppm

# ventana GTK3 del modo SDR: estado, carga, activación y vuelta a fábrica
furimodem-sdr

# volcar el anillo a un fichero y abrirlo luego en el portátil
sudo ./driver/tools/iqring_pull 20 /tmp/iqring.bin     # en el teléfono
./app/fmiq-app --headless 4 --source file --file iqring.bin

# probes y pruebas del driver
SOAPY_SDR_PLUGIN_PATH=$PWD/driver/build SoapySDRUtil --probe=driver=furimodem_iq
sudo ./driver/build/selftest_transport --live /dev/ccci_ccb_meta 5
```

## Verificación

```sh
cd driver && ctest          # salvaguarda de TX · lector del anillo · transporte
cd app && make test         # FFT, rendimiento y waterfall
cd gui && make test         # lectura de estado, órdenes y códigos de salida
tools/test-iq5file.sh       # 5 bits, 8 bits, volcado ambiguo y CS16, sin teléfono
```

No se distribuye ninguna imagen de firmware: los parches se generan desde la imagen de stock en la
máquina del usuario y se verifican por sha256 antes y después de escribir.
## Licencia

Este proyecto se distribuye bajo la **GNU General Public License v3.0** (GPLv3).
El texto completo está en [`LICENSE`](LICENSE).

Copyright (C) 2026 los autores de furimodem-sdr.
