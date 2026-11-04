#!/bin/zsh
# Двойной клик в Finder: ждёт USB-порт платы и прошивает env nm-epd-420.
cd "$(dirname "$0")"
setopt NULL_GLOB

# локальное ядро PlatformIO рядом с репо (если есть) — иначе ~/.platformio
[[ -d ../.pio-core ]] && export PLATFORMIO_CORE_DIR="$PWD/../.pio-core"

PIO=$(command -v pio || command -v platformio)
if [[ -z $PIO ]]; then
  for c in ~/.platformio/penv/bin/pio ~/Library/Python/*/bin/pio; do
    [[ -x $c ]] && PIO=$c && break
  done
fi
if [[ -z $PIO ]]; then
  echo "pio не найден: pip3 install platformio (или brew install platformio)"
  read; exit 1
fi

echo "=== NM-EPD-420 flash ($PIO) ==="
echo "Если порта нет — воткни USB и нажми RESET"
for i in {1..120}; do
  for p in /dev/cu.usbmodem* /dev/cu.usbserial*; do
    if [[ -e $p ]]; then
      echo "FOUND $p (@${i}s)"
      "$PIO" run -e nm-epd-420 -t upload --upload-port "$p"
      ec=$?
      echo "exit=$ec"
      echo; echo "Enter чтобы закрыть"; read
      exit $ec
    fi
  done
  printf "\rжду USB... %ds " $i
  sleep 1
done
echo
echo NO_PORT
echo; read
exit 1
