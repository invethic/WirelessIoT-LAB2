# WirelessIoT-LAB2
How to manage production and Maintenance of a Production line with TAB5 and a MQTT broker
# CPLab Tracking — Festo CP Lab 400 (smartphone cases)

Production and maintenance tracking for a 4-station Festo CP Lab 400 line, built on M5Stack Tab5, a local Mosquitto broker, Node-RED and a web supervision site. Production and maintenance are two independent sub-systems sharing the same broker, so they can be given to different student teams.

```
                         ┌──────────────── Laptop ────────────────┐
  PRODUCTION (group prod)│  Mosquitto  :1883 MQTT  :9001 WebSocket│   MAINTENANCE (group maint)
  prod-st1  Hall ───┐    │      ▲                                 │    ┌─── maint-m1  vibration
  prod-st2  Hall ───┤    │      │        Node-RED  :1880          │    ├─── maint-m2  vibration
  prod-st3  Hall ───┼───►│      ├──► /dashboard (Dashboard 2.0)   │◄───┼─── maint-m3  vibration
  prod-color Hall+cam┤   │      │    breakdown history, stats     │    └─── maint-m4  vibration
  prod-hmi  (IHM) ───┘   │      └──► web/index.html (browser)     │
                         └────────────────────────────────────────┘
```

## Contents

| Folder | What | Used for |
|---|---|---|
| `firmware/CPLab_ProdStation` | Hall counting, cadence vs takt, Kanban WIP to next station | 3 Tab5 (`STATION_NUM` 1, 2, 3) |
| `firmware/CPLab_ColorStation` | Station 4: Hall trigger + camera colour classification, calibration | 1 Tab5 |
| `firmware/CPLab_HMI` | Local operator panel: synoptic, OF progress, KPIs, Andon | 1 Tab5 |
| `firmware/CPLab_Maint` | Vibration → RUN / OFF / ABNORMAL, breakdowns 24 h / 7 days, planned actions | 4 Tab5 (`MACHINE_NUM` 1..4) |
| `nodered/cplab_flows.json` | 4 tabs: Production, Maintenance, Supervision, Simulator + dashboard | Node-RED |
| `web/index.html` + `mqtt.min.js` | Web supervision and remote control (no server needed) | Browser |
| `mosquitto/mosquitto.conf` | Broker with MQTT 1883 + WebSockets 9001 | Laptop |

`cplab_common.h` is identical in every sketch folder (Wi-Fi via the ESP32-C6, MQTT, LWT, heartbeat, supervisor messages, identify/reboot, UI helpers). Edit the copy in `firmware/` and copy it into the four folders.

## MQTT topic map (`TOPIC_ROOT = cplab`)

| Topic | Publisher | Retained | Payload |
|---|---|---|---|
| `cplab/prod/st1..st4/count` | stations | yes | `{st,id,name,count,cycle_ms,cadence,takt_s,wip_out,wip_max,kanban,state}` |
| `cplab/prod/color/count` | prod-color | yes | `{RED,GREEN,BLUE,BLACK,UNKNOWN,total}` |
| `cplab/prod/color/last` | prod-color | no | `{color,r,g,b,dist}` |
| `cplab/prod/order` | Node-RED / web | yes | `{of,qty:{RED,GREEN,BLUE,BLACK},takt_s,wip_max}` |
| `cplab/prod/andon` | prod-hmi | no | `{from,type:"supervisor"\|"quality"}` |
| `cplab/maint/m1..m4/state` | maint Tab5 | yes | `{state:"RUN"\|"OFF"\|"ABNORMAL",reason,activity,cv,th_off,th_high,since_s,sim}` |
| `cplab/maint/m1..m4/stats` | Node-RED | yes | `{day,week,mtbf_h,mttr_min,next_prev:{date,task},next_cur:{date,task}}` |
| `cplab/maint/m1..m4/plan/set` | Node-RED / web | no | `{prev:{date,task},cur:{date,task}}` |
| `cplab/maint/m1..m4/ack` | Tab5 / web | no | `{id,by}` |
| `cplab/status/<devId>` | all Tab5 (LWT) | yes | `online` / `offline` |
| `cplab/hb/<devId>` | all Tab5 | no | `{id,role,group,title,ip,rssi,up,fw,heap}` every 10 s |
| `cplab/cmd/<target>/msg` | Node-RED / web | no | `{text,from,level:"info"\|"warn"\|"alarm",duration}` |
| `cplab/cmd/<target>/action` | Node-RED / web | no | `{cmd:"identify"\|"reboot"\|"reset"\|"learn"\|"sim_fault"\|...}` |
| `cplab/ack/<devId>` | Tab5 | no | `{id,text}` (message displayed) |

`<target>` is a device id (`prod-st1`, `prod-st2`, `prod-st3`, `prod-color`, `prod-hmi`, `maint-m1`…`maint-m4`), a group (`prod`, `maint`) or `all`.

The Kanban logic runs on each station itself (it subscribes to the next station's count), so the line keeps working even if Node-RED is stopped. Node-RED is the source of truth only for the breakdown history and the maintenance plan.

## Kanban rules (production stations)

WIP in the buffer between station n and n+1 = `count(n) − count(n+1)`, compared with `wip_max` from the OF.

| WIP | Colour | Instruction on the Tab5 |
|---|---|---|
| < WIP max | green | PRODUIRE |
| = WIP max | amber | LIMITE ATTEINTE |
| > WIP max | red | STOP KANBAN (overproduction) |
| < 0 | amber | ECART COMPTAGE (sensor problem) |

The cycle time (mean of the last 6 pieces) is compared with the takt time: green ≤ takt, amber ≤ 1.15 × takt, red above (bottleneck). Station 1 also shows the OF progress and turns blue when the OF quantity is reached.

## Maintenance state detection

The vibration sensor's digital output is sampled at 1 kHz. Each second an activity index is computed: `max(transitions per second, % of time active)`, or the AC RMS if an analog output is wired (`VIB_AO_PIN`). OFF below `th_off`, ABNORMAL above `th_high` (excessive) or when std/mean over 10 s exceeds `CV_ABNORMAL` (irregular), RUN otherwise, each with a confirmation delay. "Apprendre normal" measures 10 s on a correctly running machine and sets the thresholds. "Simuler panne" / "Simuler arret" force a state for 20 s for training. A breakdown = a transition to ABNORMAL; Node-RED counts them over rolling 24 h and 7 days and computes MTTR and a rough MTBF.

## Wiring — read before connecting

The ESP32-P4 GPIOs are 3.3 V only, and the Grove port supplies 5 V. A sensor module powered at 5 V with an on-board pull-up will put 5 V on the GPIO.

- Hall sensor A3144 / KY-003: the A3144 needs ≥ 4.5 V. Power it at 5 V and either remove the module's pull-up resistor (the firmware enables the internal pull-up to 3.3 V; the output is open collector) or add a divider on the signal (10 kΩ series, 20 kΩ to GND). A 3.3 V-capable Hall switch avoids the problem.
- Vibration SW-420 / 801S (LM393): works at 3.3 V — power it from 3.3 V. Inductive modules (5–15 V, e.g. DFRobot SEN0433): power at 5 V and use a divider on DO. Set `VIB_ACTIVE_LOW` according to your board (most SW-420: HIGH while vibrating).
- Default pin is G53 (Grove Port A). Any free GPIO of the Tab5 expansion header works; check the pinout on docs.m5stack.com before choosing.
- Glue a small magnet on each piece carrier (or piece) so the Hall sensor sees one pulse per piece; tune `MIN_PIECE_INTERVAL_MS` so a single passage counts once.

## Setup

1. Mosquitto: install, copy `mosquitto/mosquitto.conf`, restart the service, open ports 1883 and 9001 in the laptop firewall.
2. Arduino IDE: board "M5Tab5" (m5stack esp32 core 3.2.x), PSRAM enabled. Libraries: M5Unified, M5GFX, PubSubClient, ArduinoJson 7. In each `config.h`, set the Wi-Fi, the laptop IP, and the station / machine number, then flash.
3. Node-RED: install `@flowfuse/node-red-dashboard` from the palette, import `cplab_flows.json`, deploy. Dashboard at `http://<laptop>:1880/dashboard`. To keep the breakdown history across restarts, add to `settings.js`:
   ```js
   contextStorage: { default: { module: "localfilesystem" } },
   ```
4. Web site: open `web/index.html` in a browser (double-click works), or serve it from Node-RED by setting `httpStatic` to the `web` folder. Enter `ws://<laptop-ip>:9001` and connect.
5. Without the line: on the "CPLab Simulator" tab, click "Start simulation". All dashboards, the web site and the HMI Tab5 come alive with coherent data. Stop it and reset the counts before using the real line, because the simulator overwrites the retained counts.

## Colour station (camera)

The Tab5 camera (MIPI-CSI) is driven through Espressif `esp_video` (V4L2 API) in `tab5_camera.h`. This is the part to validate first on hardware: if the core does not ship `esp_video`, or its structures differ, the station shows "Camera indisponible" and the four colour buttons register pieces instead, so the rest of the project is not blocked. Calibration: press "Calibrer", place a piece of the colour under the camera, press the colour button. References are stored in NVS. Fixed lighting and a neutral background under the camera make the classification far more reliable. Fallback hardware option if the camera is not usable: an M5Stack Color Unit (TCS3472, I2C) on Port A.

## Splitting the work between students

Production team (SNPI / AR4): Hall wiring and debounce, station firmware, colour calibration, HMI. Maintenance team: vibration wiring, threshold learning, state machine tuning, breakdown statistics in Node-RED, maintenance plan. Integration: topic contract (table above), Node-RED dashboard, web site.

BTS ATI 2nd year (Kanban, Organisation Industrielle) can work only with the Node-RED simulator and the web site at first: change `wip_max` and takt, observe where the line blocks, identify the bottleneck (station 3 is slower by design in the simulator), compute the required takt from a customer demand, compare cadence to takt per station, and propose a buffer sizing. Then repeat on the real line with the Tab5 stations.

## Library links list

https://arduino.esp8266.com/stable/package_esp8266com_index.json
https://dl.espressif.com/dl/package_esp32_index.json
https://espressif.github.io/arduino-esp32/package_esp32_index.json
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json

## Known limits

- Firmware written for the m5stack esp32 core 3.2.x but not compiled in this package; expect small fixes on first build (pin choices, camera driver).
- Tab5 screen texts are without accents (GFX free fonts are ASCII).
- `cplab/prod/stN/count` is retained: after a test, reset the counters ("Remettre les compteurs a zero") so stations and dashboards restart from zero.
