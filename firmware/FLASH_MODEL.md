# Flash WakeNet model to model partition:
```
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x3A0000 firmware/srmodels.bin
```
