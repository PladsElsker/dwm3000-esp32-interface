## Compile code
```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin --libraries "./UwbNode/lib/Makerfabs-ESP32-UWB-DW3000" ./UwbNode
```

## Upload code
```
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin ./UwbNode
```
