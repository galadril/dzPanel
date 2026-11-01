# wifi-cfg progress (option B: Wi-Fi in RU+CFG)
Plan: ru_cfg gets ru_cfg_line_t + ru_cfg_parse_line + ru_wifi_ssid_check/ru_wifi_pass_check (BAD_WIFI);
ru_cfgline ops gets wifi_apply (appended field); ru_wifi_esp gets ru_wifi_esp_apply_blocking; svc_glue wires it;
site/lib.js validateWifi/buildConfig; app.js+index.html fields; tests in test_cfg.c, test_net.c, tests/site/test_lib.mjs; docs.
Status: started (nothing edited yet).
ru_cfg.[ch] done (parse_line, validators, masked). next: ru_cfgline, wifi_esp, glue, site, tests, docs
ru_cfgline, ru_wifi_esp_apply_wait, svc_glue wired. next: host tests, site, docs, idf build, dist
host C tests (test_cfg, test_net) done and green in /tmp/vbw. next: site (lib.js app.js index.html + test_lib.mjs), docs, idf build (background, -B /tmp/fw/build), dist
site done (lib/app/index/test_lib 14 pass), differential JS-vs-C 20000 vectors 0 mismatch (/tmp/diff). next: idf build bg, docs, dist
idf build started bg: log /tmp/fw/build_wificfg.log (look for EXIT)
build OK roundos.bin 0x191a40 (61% free of 4MB slot), 0 warnings. next: merge-bin, dist, SHA256SUMS, final report
all done; report next
