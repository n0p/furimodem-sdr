# 274 — el estado ya no puede mentir sobre la imagen

**Fecha:** 2026-11-06 · **Repos:** `furimodem-sdr-public` (`tools/sdr-load.cpp`,
`gui/`) · **Móvil:** FuriPhone FLX1, devuelto a stock E6 verificado.

## Resumen

1. `--status` hashea la partición y la compara con el estado y la tabla: incoherencia =
   `INCOHERENT: state says sdr-active but the partition holds 22c1909d… (stock image)`.
2. `--activate`/`--resume` verifican la partición antes del módem; imagen ausente = «the image
   is not in place… write it again» (5), no «repita la activación».
3. El 12 reintenta 45 s y separa «imagen correcta, marca ausente» de «imagen equivocada»; las
   imágenes de barrido ya no dan 12.
4. La ventana enseña la partición y ofrece «Write the image again…»; verificado en el móvil,
   devuelto a stock E6 con `md1=ready` y ofono activo.

## 1. Qué comprueba ahora el estado

El fichero `/var/lib/sdr-load/state.json` es un registro de lo que hizo la herramienta, no la
verdad del teléfono: cualquier otra cosa (un flash de stock, una recuperación, una reversión
desde otra terminal) puede escribir la partición por detrás. Por eso `--status` calcula
`partition_facts()` —sha256 de los bytes útiles, tamaño, perfil de la tabla y comparación con
la imagen de recuperación— y lo imprime antes de la sección `SDR mode`:

```
partition : /dev/disk/by-partlabel/md1img_a
image now : 22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896
image size: 63018064 B usable
image kind: stock image
image check: INCOHERENT: state says sdr-active but the partition holds 22c1909dbf9f0455... (stock image)
next      : the image was written back: write the mode image again with sdr-load --image <path> (the phone restarts)
```

`image_truth()` cruza tres cosas: la partición, el sha256 que grabó el estado y la tabla de
perfiles. El estado nunca gana. Casos:

| partición | estado | veredicto |
|---|---|---|
| imagen de perfil, sha = el del estado | `sdr-active` | `OK the partition holds mode image <perfil>` |
| stock / sin perfil | `sdr-active` | `INCOHERENT: state says sdr-active but the partition holds <sha> (stock image)` |
| otra imagen de perfil | `sdr-active` | `INCOHERENT: … and the state recorded <sha>` |
| stock | `ok` | `OK the partition holds the stock image (no SDR mode loaded)` |
| no se puede leer | cualquiera | `unverified: cannot read …: Permission denied` (nunca «buena») |

## 2. `--activate` y `--resume`

`--activate` resuelve el perfil **con el sha de la partición ya calculado** (`resolve_mode_map`
acepta los hechos precalculados, así se hashea una vez). Si el sha no está en la tabla y la
partición no es de un perfil, imprime:

```
the image is not in place: the partition holds 22c1909dbf9f0455... (stock image)
that is the stock image: it has no dump engine to activate
the state says sdr-active, but the state is not the partition: it is not trusted
write the mode image again: sudo sdr-load --image <path> (the phone restarts)
```

y sale con 5 sin ejecutar el ioctl, sin bloquear radios y sin pedir un solo modo. Además se
niega a entrar en modo SDR sobre la imagen de stock aunque un fichero de perfil sin sha256 lo
pretenda.

`--resume` hace la misma comprobación antes de decidir qué falta, y separa los dos casos:

```
image: 5a625327a06fcacf... (mode image sdr-8bit): the image was not reverted
the image is in place: what is missing is the activation (loops/sweep): resuming it
```

frente a

```
ERROR: INCOHERENT: state says sdr-active but the partition holds 22c1909dbf9f0455... (stock image)
the image was written back: write the mode image again with sdr-load --image <path> (the phone restarts)
nothing is resumed on an image that is not the one of the mode
```

## 3. El código 12 ya no es un callejón sin salida

El módem publica a ráfagas (medido: 60-200 capturas en 10-45 s y luego silencio), así que una
sola lectura de la marca puede caer en un silencio y decir «no hay volcado» sobre un montaje
que funciona. Ahora:

* `--no-flash` y la activación por modos buscan la marca durante `--mark-timeout-ms` (45 s por
  defecto, `--mark-timeout-ms <n>`), no una vez. Si aparece, sigue como éxito.
* Si no aparece, el mensaje depende de la partición: con la imagen correcta, «the image is
  correct (profile X) but the dump-engine mark did not appear in 45 s: is the sweep running?»;
  con otra imagen, código 5 y «the image is not in place… write the mode image again». Nunca
  comparten mensaje.
* Las imágenes de barrido (`sdr-8bit`/`rf192t`) **no programan el motor de volcado**: su marca
  no se espera. Antes, `--no-flash` sobre ellas devolvía 12 («Repeat the activation», un
  callejón sin salida); ahora dice «the image is a sweep image … the mark is not expected» y
  sale con 0.
* En la ventana, el texto del 12 ya no dice «Repeat the activation»: explica la ventana de
  comprobación, las ráfagas y que, si la imagen se hubiera revertido, el código sería 5.

## 4. La ventana

La pestaña **State** tiene ahora `Partition`, `Image now` (sha256 + tipo de imagen) e `Image
check` (el veredicto), leídos del bloque nuevo de `--status`, no del fichero de estado. Si el
veredicto empieza por `INCOHERENT`:

* se muestra un banner «The state and the partition disagree» con el texto de la herramienta;
* **Start reception** y **Enter SDR mode** se deshabilitan (no pueden funcionar);
* **Load image...** pasa a **Write the image again...**, con su aviso de reinicio.

Los ficheros `gui/src/status.{hpp,cpp}`, `gui/src/main.cpp` y `gui/src/actions.cpp` recogen el
cambio; `make -C gui test` cubre el parseo de los tres veredictos (coherente, incoherente, sin
privilegios) y que el 12 no vuelva a decir «repeat the activation».

## 5. Verificación en el móvil

Con el estado roto de partida (`state=sdr-active`, partición stock `22c1909d…`):

```
$ sudo ./sdr-load --status
      state     : sdr-active
      image now : 22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896
      image kind: stock image
      image check: INCOHERENT: state says sdr-active but the partition holds 22c1909dbf9f0455... (stock image)
      next      : the image was written back: write the mode image again with sdr-load --image <path> (the phone restarts)

$ sudo ./sdr-load --activate --yes
      the image is not in place: the partition holds 22c1909dbf9f0455... (stock image)
      that is the stock image: it has no dump engine to activate
      write the mode image again: sudo sdr-load --image <path> (the phone restarts)
rc=5                      (md1 sigue ready, el módem sigue en servicio: no se tocó nada)

$ sudo ./sdr-load --resume --yes
ERROR: INCOHERENT: state says sdr-active but the partition holds 22c1909dbf9f0455... (stock image)
rc=5
```

Con la imagen de barrido puesta (`modem-sdr-rf192t.img`, sha `5a625327…`) y tras el reinicio:

```
$ sudo ./sdr-load --resume --yes
      image: 5a625327a06fcacf... (mode image sdr-8bit): the image was not reverted
      the image is in place: what is missing is the activation (loops/sweep): resuming it
[4/7] ring: require PUSH or CAPS to advance (max 45 s)
      ring after: push=162 caps=155 (advanced)
[5/7] frequency: read page+12 of the last capture
      page+12 = 0x529e0001  (band 1, 21150 kHz)
result: SDR mode activated (scan launched and page at 21150 kHz)
rc=0
```

`--activate`/`--resume` completan sin 12. `--no-flash` con la misma imagen tampoco devuelve 12:
detecta que es de barrido, dice que la marca no se espera y sale con 0 (5 s).

`--selftest`: 86/86 en el portátil y 86/86 en el móvil (aarch64). `make -C gui test`: 140 + 74.

Estado final del móvil: partición `22c1909d…` (E6 de fábrica, leída con `head -c`), estado
`ok`/`pending=-` con `image check: OK the partition holds the stock image`, `md1=ready`, ofono
activo y AT respondiendo `OK`, rfkill desbloqueado.

## 6. Ficheros

* `tools/sdr-load.cpp`: `PartitionFacts`/`partition_facts()`/`image_truth()`,
  `wait_dump_mark()`/`probe_dump_mark()`/`diagnose_missing_mark()`, `--mark-timeout-ms`,
  comprobación de la partición en `--status`/`--activate`/`--resume`/`--no-flash`, perfil del
  estado limpiado al flashear una imagen que no es de modo, 18 comprobaciones nuevas en
  `--selftest`.
* `gui/src/status.{hpp,cpp}`, `gui/src/main.cpp`, `gui/src/actions.cpp`, `gui/tests/parse_test.cpp`.
* `tools/README.md`, `gui/README.md`.

## 7. Límites

* El bloque `image check` compara contra la imagen de recuperación (`--recover-image`) para
  poder decir «stock image»; si ese fichero no está, la imagen se describe por su sha y como
  «no profile matches this image».
* Si no hay privilegios para leer la partición, el veredicto es `unverified`: la herramienta no
  adivina ni copia lo que diga el estado.
* La marca del motor solo existe en imágenes que lo programan; las de barrido se validan por
  anillo y frecuencia, no por marca.
