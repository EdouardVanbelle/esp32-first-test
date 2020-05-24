ESP-IDF first program
====================

First play with an esp32

Features for this test:

 - PIR (movement detection) plugged on GPIO32, will trigger an HTTP call
   (linked to project **bose-music-control**)

 - OTA (periodic check for new firmware)
 - Wifi connection
 - Power saving (will warm up on raising front on GPIO32
 - read configuration from an http server on boot & watchdog
 - write config to NVS (autonomous mode if config from server is not available)
 - trigger an HTTP ping as a watchdog
 - periodic synchronisation of config file
 - BOOT button request a reload of config file 

