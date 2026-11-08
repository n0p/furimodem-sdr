#!/bin/sh
# Local test of the I/Q block formats, without a phone.
#
#   tools/test-iq5file.sh            (uses app/fmiq-app)
#   APP=/path/fmiq-app tools/test-iq5file.sh
#
# It generates the test files (5-bit "CSD1", 8-bit "CSD8" and the earlier CS16), runs
# them through the app in headless mode for several FFT sizes and checks that the tone
# falls on the expected bin (N/2 + N/8 for an fs/8 offset). It also checks that an old
# dump with a CSD1 signature and an 8-bit payload is still read.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$ROOT/app/fmiq-app}
TMP=${TMPDIR:-/tmp}/fmiq-iq5-test
mkdir -p "$TMP"

if [ ! -x "$APP" ]; then
    echo "missing $APP; build it with: make -C app" >&2
    exit 1
fi

echo "generating files in $TMP"
python3 "$ROOT/tools/make-iqfile.py" --out "$TMP/iq5-tone.bin" --format 5 --captures 10
python3 "$ROOT/tools/make-iqfile.py" --out "$TMP/iq8-tone.bin" --format 8 --captures 10
python3 "$ROOT/tools/make-iqfile.py" --out "$TMP/iq8-oldsig.bin" --format 8 \
    --legacy-signature --captures 4
python3 "$ROOT/tools/make-iqfile.py" --out "$TMP/legacy-cs16.bin" --format cs16 --captures 4

failures=0
run() {
    name=$1
    shift
    if "$@" >"$TMP/$name.log" 2>&1; then
        echo "  OK    $name"
        grep -E "format=|peak:|check-bin" "$TMP/$name.log" | sed -n 's/^/          /p'
    else
        echo "  FAIL  $name"
        sed -n 's/^/          /p' "$TMP/$name.log"
        failures=1
    fi
}

check_packed() {
    label=$1
    file=$2
    for n in 512 1024 2048 4096; do
        bin=$((n / 2 + n / 8))
        echo "== $label, FFT $n: the tone (fs/8) must fall on bin $bin =="
        run "$label-$n" "$APP" --headless 12 --fft "$n" --source file --file "$file" \
            --check-bin "$bin" --out "$TMP/wf-$label-$n.ppm"
    done

    echo "== $label: lit column in the waterfall (2048 bins) =="
    python3 - "$TMP/wf-$label-2048.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
width, h = map(int, d[1].split())
pix = d[3]
# The tone paints the column hot (high red); the noise stays in green/blue. Summing the
# three channels let a noise column win by chance.
best = (-1, 0)
for x in range(width):
    s = sum(pix[3 * (y * width + x)] for y in range(h))
    if s > best[0]:
        best = (s, x)
print(f"  brightest column {best[1]}/{width} (expected {width // 2 + width // 8})")
sys.exit(0 if abs(best[1] - (width // 2 + width // 8)) <= 1 else 1)
PY
    if [ $? -ne 0 ]; then failures=1; fi
}

check_packed iq5 "$TMP/iq5-tone.bin"
check_packed iq8 "$TMP/iq8-tone.bin"
check_packed iq8old "$TMP/iq8-oldsig.bin"

echo "== previous format (CS16, no header) =="
run "legacy" "$APP" --headless 12 --fft 2048 --source file --file "$TMP/legacy-cs16.bin" \
    --freq 2115 --rate 1.92 --check-bin 1280 --out "$TMP/legacy.ppm"

if [ "$failures" -ne 0 ]; then
    echo "FAILURES"
    exit 1
fi
echo "ALL OK"
