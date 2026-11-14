## Compile code
```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin \
  --build-property "compiler.c.extra_flags=-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=9" \
  --build-property "compiler.cpp.extra_flags=-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=9" \
  --libraries "./UwbNode/lib/Makerfabs-ESP32-UWB-DW3000" ./UwbNode
```

## Upload code
```
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin ./UwbNode
```
