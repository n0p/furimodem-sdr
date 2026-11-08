# La imagen del modo SDR, construida desde tu propio teléfono

Este repositorio **no** distribuye una imagen de firmware del módem: el receptor necesita una copia
parcheada de la imagen que ya tiene tu teléfono, así que el parche se aplica en tu máquina y los dos
extremos se verifican por sha256. `tools/make-sdr-image.py` hace exactamente eso.

[English version](SDR-IMAGE.md)

## Qué necesitas

La imagen de stock del módem del FLX1, build E6:

| | |
|---|---|
| tamaño | 63.018.064 bytes |
| sha256 | `22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896` |

Es la imagen de la partición de firmware del teléfono (`/dev/disk/by-partlabel/md1img_a`), también
presente en el paquete de módem del fabricante y en cualquier copia de seguridad completa del
teléfono. `make-sdr-image.py` **rechaza cualquier otra imagen**, así que no se puede parchear por
error una versión distinta del firmware.

Búscala de tu propio teléfono y comprueba el hash antes de seguir (la partición es más grande que la
imagen, por eso el tamaño es explícito):

```sh
ssh furios@<movil> 'sudo head -c 63018064 /dev/disk/by-partlabel/md1img_a' > stock-modem.img
sha256sum stock-modem.img
# 22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896  stock-modem.img
```

## Construirla

```sh
python3 tools/make-sdr-image.py /ruta/a/stock-modem.img
# stock image : /ruta/a/stock-modem.img (63018064 bytes)
# sha256      : 22c1909d…  OK
# SDR image   : modem-sdr-8bit.img (63018064 bytes)
# sha256      : 5a625327…  OK (profile sdr-8bit)
```

`--check` verifica la imagen de stock e informa del resultado sin escribir nada. La salida es
byte a byte idéntica a la imagen `sdr-8bit` que conoce `sdr-load`, y el script comprueba que su
sha256 es `5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3` antes de entregarla.

El parche es pequeño y explícito: **1.694 bytes en siete tramos** (la herramienta guarda solo los
rangos que cambian). Llevan:

- la copia de la captura del buscador al anillo del AP, para que el waterfall tenga datos mientras el
  teléfono barre;
- el formato de 8 bits por componente (I/Q, origen 3), tres bits más que la toma de 5 bits;
- la sintonía fija en 2.115,0 MHz (banda 1) para el barrido.

El módem **sigue en servicio** con esta imagen a propósito: la captura viene de su propio barrido de
frecuencias. Por eso el receptor entrega ráfagas, no un flujo continuo.

## Cargarla, usarla y volver

El cargador ejecuta `flash_modem.sh` (publicado en `tools/`); cópialo una vez junto a la imagen:

```sh
scp modem-sdr-8bit.img tools/flash_modem.sh furios@<movil>:/home/furios/
ssh furios@<movil> chmod +x /home/furios/flash_modem.sh

sudo sdr-load --image /home/furios/modem-sdr-8bit.img   # escribe y verifica; NO reinicia
sudo systemctl reboot                                   # el bootloader lee la imagen nueva
sudo sdr-load --resume                                  # tras el arranque: entra y activa
sudo sdr-load --status                                  # perfil, anillo y siguiente paso
```

`--resume` ya ejecuta la activación (para `sdr-8bit`, lanza el barrido del teléfono). Ejecutar
`--activate` después es opcional y solo hace falta para lanzar otro barrido.

Desde el escritorio del teléfono los mismos pasos son los botones numerados de `furimodem-sdr`
(*1. Load SDR image…*, *2. Start reception*, *Open the SDR app*); la ventana no reinicia por sí
misma: muestra la orden `sudo systemctl reboot` cuando se ha escrito la imagen.

Para volver a un teléfono normal:

```sh
sudo sdr-load --deactivate                              # para el barrido (el firmware sigue cargado)
sudo sdr-load --image /home/furios/modem-E6.img         # escribe stock otra vez
sudo systemctl reboot
# o, sin escribir nada: sdr-load --restore sale del modo y devuelve las radios
# (solo reflashea la imagen de rescate si el módem no vuelve)
```

## Por qué no se distribuye la imagen

La imagen deriva de firmware del fabricante que no es nuestro para redistribuirlo. Una receta de
parche de 1,7 kB aplicada sobre la copia del usuario basta para reproducirla exactamente, y el par
de sha256 hace verificable el resultado: entra la de stock, sale la `sdr-8bit`.
