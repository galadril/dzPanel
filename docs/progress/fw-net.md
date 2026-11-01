# fw-net progress notes (agent fw-net)

DONE (host, ctest green): ru_cfg (test_cfg), ru_net (test_net: RU+CFG, fuzz, Improv, console demux, wifi SM),
dz_service (test_dz_service fake transport + threaded; test_dz_service_mock vs node mock).
CMake block: "fw-net agent" at the end of top-level CMakeLists.txt.
IDF glue compiled OK in /tmp/fwnet_chk (idf.py build, esp32s3, 0 warnings in our files; link used a stub OSAL). Remaining: nothing required; see report Requests.
Improv reference cloned at /tmp/improv/sdk-cpp (git clone https://github.com/improv-wifi/sdk-cpp) - may be gone.
Build/test: cmake -S . -B /tmp/vbn -DRU_BUILD_SIM=OFF && cmake --build /tmp/vbn -j8 && (cd /tmp/vbn && ctest)
IDF scratch: export IDF_TOOLS_PATH=/home/dev/.idf-shadow; . /opt/esp/idf/export.sh  (see AGENT_CONTRACT.md)

## Decisions (for the next agent)
- Console sharing: see header comment of components/ru_net/include/ru_console.h. One RX byte stream; demux by prefix ("IMPROV"+0x01
  binary packet consumed by length vs "RU+CFG " text line to '\n'); other bytes ignored; mismatch re-examined; 2 KiB line cap;
  3 s idle reset; line buffer zeroed. TX: Improv frames are "\n"+packet+"\n"; RU+CFG replies "\n"+line+"\n"; one tx mutex.
- RU+CFG extra code "store" (kv write failed) and details (update_firmware_or_installer, not_domoticz, 2048) are additions to docs/provisioning.md table.
- dz_service: bus topic default 8 (DZ_SERVICE_TOPIC_DEFAULT), coalescing on; keys: device = 16+((idx<<1)|scene), 1 model, 2 conn, 3 auth, results key 0.
- Reconfiguring a different server does not clear the model (no clear API in dz_model); next full read removes missing entries.
- dz_client_request does not wipe its stack header buffer containing the token (dz_client.c is not mine).
- Glue files compiled only (scratch IDF project /tmp/fwnet_chk): dz_transport_esp.c, ru_wifi_esp.c, ru_console_esp.c, ru_kv_nvs.c.
