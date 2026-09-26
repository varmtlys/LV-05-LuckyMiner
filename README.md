# LV-05 LuckyMiner

Firmware for the **LV-05 LuckyMiner**, a single-ASIC Bitcoin miner built around the
**Bitmain BM1397** on an ESP32-S3. It is a hardened, single-board rebuild of the
open-source [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner) / BitAxe firmware:
everything specific to other ASICs and boards has been stripped out, and the mining
hot path, network resilience and on-device display have been reworked so the board
keeps hashing and stays reachable without a USB cable.

| | |
| --- | --- |
| **ASIC** | Bitmain BM1397 (672 cores) |
| **MCU** | ESP32-S3 |
| **Display** | 128×32 SSD1306 OLED |
| **Toolchain** | ESP-IDF v5.1 |
| **Pool protocol** | Stratum V1 with BIP310 version rolling |

---

## What's fixed in this build

This firmware started as a generic multi-ASIC codebase. The changes below make it
correct and reliable for the LV-05's single BM1397.

### Mining works and can be measured
- **Measurable hashrate.** The ASIC ticket difficulty is capped at 256, so the chip
  reports a result roughly every few seconds instead of a few times per hour. The
  estimator now weighs every result by the chip's actual threshold, so the hashrate
  reading settles within a minute instead of a day. (The number may look *lower* than
  before — it is now honest rather than noise from three samples.)
- **Cadence follows the real clock.** Job timing is derived from the frequency the
  power-management task actually settled on, not the NVS target, so the chip finishes
  its nonce space before each job is replaced — even while it ramps up or throttles.
- **Correct shares.** Every reported nonce is verified and, when it beats the pool
  difficulty, submitted with the right rolled version and extranonce2.

### Network no longer "falls off" at random
- **Infinite WiFi reconnect.** The old handler stopped calling `esp_wifi_connect()`
  after 5 failed retries and never came back — a router reboot or a few seconds of
  interference took the miner off the network until it was power-cycled. It now retries
  forever (fast at first, then every 5 s) and logs the disconnect reason code.
- **Pool watchdog.** A 300 s receive timeout plus TCP keepalive means a half-open pool
  connection reconnects instead of parking the mining task in `recv()` forever.

### Safer hot path
- **No more use-after-free.** Only 32 job slots exist and they recycle about every
  0.6 s. The result reader now copies the job under the lock before verifying and
  submitting it, closing a race that could crash the miner under load.
- **Less heap churn.** Job identifiers are inlined into the job struct, removing two
  `malloc`/`free` pairs per job on a ~50 jobs/s path.

### On-device display
- The OLED shows an **animated pickaxe** while the board is actually mining (chip
  producing results *and* WiFi connected), and a **crossed-out icon** when it is not —
  visible at a glance without opening the web UI. Built with `-O2` optimisation.

### Fault handling
- A genuine fault **halts and stays reachable** (the web UI and OLED keep working)
  instead of rebooting — important because the LV-05 has no exposed USB/serial.

---

## Build

Requires **ESP-IDF v5.1**. Follow the official
[install guide](https://docs.espressif.com/projects/esp-idf/en/release-v5.1/esp32s3/get-started/index.html)
or the "Espressif IDF" VS Code extension.

```bash
idf.py set-target esp32s3
idf.py menuconfig   # set Stratum + WiFi under the project options
idf.py build
```

The web UI is bundled into `www.bin` at build time. To rebuild it:

```bash
cd main/http_server/axe-os
npm install
npm run build
```

## Flash

```bash
idf.py -p PORT flash monitor
```

Once running, the firmware can be updated over the air from its web interface
(`esp-miner.bin` for the app, `www.bin` for the UI) — no cable needed.

## Host self-check

The arithmetic in the mining hot path (extranonce2 walk, ticket difficulty, hashrate
weighting) has a hardware-free test that runs on any host:

```bash
gcc -O2 -o check_hot_path test/host/check_hot_path.c && ./check_hot_path
```

---

## Credits & licence

Derived from [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner) (BitAxe) v2.0.4.
Licensed under **GPL-3.0** — see [LICENSE](LICENSE).
