# Build log for ESP-IDF

Generated: 2026-09-01T11:19:44

$ /Users/willema/.espressif/v6.0.2/esp-idf/tools/idf.py build
Executing action: all (aliases: build)
Running ninja in directory /Users/willema/Documents/espidfProjects.nosync/radioProject/build
Executing "ninja all"...
[1/5] cd /Users/willema/Documents/espidfProjects.nosync/radioProject/build && /Users/willema/.espressif/tools/python/v6.0.2/venv/bin/python /Users/willema/.espressif/v6.0.2/esp-idf/components/partition_table/check_sizes.py --offset 0x8000 partition --type app /Users/willema/Documents/espidfProjects.nosync/radioProject/build/partition_table/partition-table.bin /Users/willema/Documents/espidfProjects.nosync/radioProject/build/radioProject.bin
radioProject.bin binary size 0x1aa950 bytes. Smallest app partition is 0x200000 bytes. 0x556b0 bytes (17%) free.
[2/5] Performing build step for 'bootloader'
[1/1] cd /Users/willema/Documents/espidfProjects.nosync/radioProject/build/bootloader && /Users/willema/.espressif/tools/python/v6.0.2/venv/bin/python /Users/willema/.espressif/v6.0.2/esp-idf/components/partition_table/check_sizes.py --offset 0x8000 bootloader 0x0 /Users/willema/Documents/espidfProjects.nosync/radioProject/build/bootloader/bootloader.bin
Bootloader binary size 0x5240 bytes. 0x2dc0 bytes (36%) free.
[3/5] No install step for 'bootloader'
[4/5] cd /Users/willema/Documents/espidfProjects.nosync/radioProject/build/esp-idf/radio_storage && /Users/willema/Documents/espidfProjects.nosync/radioProject/build/littlefs_py_venv/bin/littlefs-python create /Users/willema/Documents/espidfProjects.nosync/radioProject/littlefs /Users/willema/Documents/espidfProjects.nosync/radioProject/build/storage.bin -v --fs-size=0x1f0000 --name-max=64 --block-size=4096
LittleFS Configuration:
  Block Size:       4096  /  0x1000
  Image Size:    2031616  /  0x1F0000
  Block Count:       496
  Name Max:           64
  Image:       /Users/willema/Documents/espidfProjects.nosync/radioProject/build/storage.bin
Adding File:      favicon.ico
Adding File:      index.html
Adding File:      stations.json
Adding File:      style.css
Adding File:      app.js
[5/5] Completed 'bootloader'

Project build complete. To flash, run:
 idf.py flash
or
 idf.py -p PORT flash
or
 python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/radioProject.bin 0x210000 build/storage.bin
or from the "/Users/willema/Documents/espidfProjects.nosync/radioProject/build" directory
 python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"

Source manifest: /Users/willema/Documents/espidfProjects.nosync/radioProject/build/flasher_args.json
