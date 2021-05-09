#!/bin/bash
for hat in *.txtdef; do
  echo "making HAT EEPROM for: ${hat%.txtdef}"
  if [ ! -f "../overlays/${hat%.txtdef}.dtbo" ]; then
    echo "ERROR: missing overlay: ${hat%.txtdef}.dtbo"
  fi

  ./eepmake $hat ${hat%.txtdef}.eep ../overlays/${hat%.txtdef}.dtbo
done
