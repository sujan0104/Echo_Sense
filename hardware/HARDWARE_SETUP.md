# EchoSense Hardware Setup Guide

## Components Required
| Component         | Quantity | Notes                          |
|-------------------|----------|--------------------------------|
| NodeMCU ESP8266   | 1        | ESP-12E or ESP-12F variant     |
| HC-SR04           | 1        | Ultrasonic sensor              |
| Breadboard        | 1        | Half-size is fine              |
| Jumper wires      | 6        | Male-to-male                   |
| USB cable         | 1        | Micro-USB for NodeMCU          |
| 1kΩ resistor      | 1        | Voltage divider (see note)     |
| 2kΩ resistor      | 1        | Voltage divider (see note)     |

---

## Wiring Diagram

```
HC-SR04                    NodeMCU ESP8266
─────────                  ───────────────
  VCC  ──────────────────── 3.3V
  GND  ──────────────────── GND
  TRIG ──────────────────── D1 (GPIO5)
  ECHO ──[1kΩ]──────────── D2 (GPIO4)
              │
            [2kΩ]
              │
             GND
```

### ⚠️ IMPORTANT - Voltage Divider on ECHO pin
The HC-SR04 ECHO pin outputs 5V but NodeMCU GPIO is 3.3V max.
Without the voltage divider you WILL damage the NodeMCU over time.

Voltage divider formula: Vout = Vin × R2/(R1+R2)
= 5V × 2000/(1000+2000) = 3.33V ✓

Alternatively, use an HC-SR04P (3.3V version) and wire ECHO directly.

---

## NodeMCU Pin Reference

```
         ┌──────────────────────┐
    RST ─┤                      ├─ TX
     A0 ─┤                      ├─ RX
    D0  ─┤                      ├─ D1  ← TRIG (GPIO5)
    D5  ─┤   NodeMCU ESP8266    ├─ D2  ← ECHO (GPIO4, via divider)
    D6  ─┤                      ├─ D3
    D7  ─┤                      ├─ D4  (built-in LED, status)
    D8  ─┤                      ├─ 3V3 ← HC-SR04 VCC
    3V3 ─┤                      ├─ GND ← HC-SR04 GND
    GND ─┤                      ├─ 5V
    VIN ─┤                      ├─ VIN
         └──────────────────────┘
              [USB Micro]
```

---

## Physical Mounting Suggestions

For wearable/assistive use:
- Mount NodeMCU + sensor in a small project box (e.g. 80×50×30mm)
- Sensor should face forward, ~chest height
- Use a lanyard or clip to attach to belt/shirt
- Phone connects via WiFi and stays in pocket

Sensor angle:
- Flat forward = detects walls, large obstacles
- Slightly downward (10-15°) = detects floor obstacles like steps

---

## Power Options

| Option              | Duration    | Notes                        |
|---------------------|-------------|------------------------------|
| USB power bank      | 4-8 hrs     | Easiest, use 1A+ output      |
| 18650 LiPo + TP4056 | 3-6 hrs     | More compact, needs charger  |
| USB from phone OTG  | Drains phone| Last resort only             |

---

## Quick Test (Before Running Full Firmware)

Upload this minimal test sketch first to verify wiring:

```cpp
#define TRIG_PIN 5
#define ECHO_PIN 4

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
}

void loop() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  float distance = (duration * 0.0343) / 2.0;
  
  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");
  delay(200);
}
```

Expected output in Serial Monitor (115200 baud):
```
Distance: 45.3 cm
Distance: 44.9 cm
Distance: 45.1 cm
```

If you see 0.00 cm → check TRIG/ECHO wiring
If you see 400+ cm → check voltage divider on ECHO

---

## API Reference (once firmware is running)

Connect phone to WiFi: **EchoSense** / password: **echosense123**

| Endpoint                    | Description                          |
|-----------------------------|--------------------------------------|
| GET http://192.168.4.1/data   | Full JSON data packet                |
| GET http://192.168.4.1/window | Sliding window array only            |
| GET http://192.168.4.1/status | Device info & connection status      |
| GET http://192.168.4.1/reset  | Start new mapping session            |
| WS  ws://192.168.4.1:81       | Real-time WebSocket stream           |

### Sample JSON Response from /data
```json
{
  "distance_cm": 87.4,
  "sensor_ok": true,
  "timestamp_ms": 12340,
  "reading_id": 247,
  "window": [91.2, 89.8, 88.5, 87.9, 87.4, 87.1, 87.4, 88.0, 87.6, 87.4],
  "step_cadence": 1.85,
  "step_count": 23,
  "step_detected": false,
  "session_id": 1000,
  "uptime_ms": 13340
}
```

---

## Troubleshooting

| Problem                      | Fix                                          |
|------------------------------|----------------------------------------------|
| WiFi AP not appearing        | Check power, re-flash firmware               |
| Distance always 0            | TRIG/ECHO swapped, check wiring              |
| Distance always 400+         | Missing voltage divider on ECHO              |
| Erratic readings             | Add capacitor (100µF) between VCC and GND    |
| WebSocket not connecting     | Make sure phone is on EchoSense WiFi network |
| HTTP 404 errors              | Check URL, IP should be 192.168.4.1          |
