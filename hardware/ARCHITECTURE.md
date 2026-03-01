# EchoSense — Full Project Architecture
## AI-Based Assistive Navigation with Tier 3 Intelligence

---

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        ECHOSENSE SYSTEM                          │
│                                                                  │
│  [HC-SR04]──►[NodeMCU ESP8266]──WiFi──►[Android App]            │
│                    │                        │                    │
│              Firmware Layer          Intelligence Layer          │
│              - Sensor polling        - Occupancy Grid Map        │
│              - Median filter         - Environment Fingerprint   │
│              - Sliding window        - Gait-Aware Prediction     │
│              - Gait detection        - RL Alert Tuning           │
│              - HTTP + WebSocket      - TFLite inference          │
│                                      - Voice/Vibration output    │
└─────────────────────────────────────────────────────────────────┘
```

---

## Data Flow

```
HC-SR04 Sensor
    │
    ▼ (40kHz pulse, echo timing)
NodeMCU Firmware
    │  - Median filter (5 samples)
    │  - Sliding window buffer (10 readings)
    │  - Gait oscillation detection
    │  - Session timestamp
    ▼
JSON Data Packet (20x/sec via WebSocket)
    │
    ▼
Android App - Data Ingestion Layer
    │  - WebSocket client
    │  - Angle tracking (phone orientation)
    │  - Accelerometer fusion
    ▼
Android App - AI Layer (4 parallel pipelines)
    ├──► [1] Occupancy Grid Mapper
    ├──► [2] Environment Fingerprinter
    ├──► [3] Gait-Aware Hazard Predictor
    └──► [4] RL Alert Tuner
    │
    ▼
Output Layer
    ├── Text-to-Speech (TTS)
    ├── Vibration patterns
    └── Visual display (optional)
```

---

## Module 1: Occupancy Grid Mapping

### What it does
Builds a live 2D probability map of the environment as the user moves.
Each grid cell stores the probability that it contains an obstacle.

### How it works
```
Phone orientation (compass) → angle θ
Sensor distance             → radius r
Map coordinates             → x = r·cos(θ), y = r·sin(θ)

Grid cell update rule (Bayesian):
  P(occupied | reading) = sigmoid(logodds + sensor_model(d))
```

### Data it uses from NodeMCU
- `distance_cm` — where the obstacle/wall is
- `timestamp_ms` — to correlate with phone movement
- `session_id` — to group readings into one map

### Android Implementation Plan
```kotlin
class OccupancyGridMapper(val gridResolution: Float = 0.1f) {
    // 100x100 grid, each cell = 10cm
    val grid = Array(100) { FloatArray(100) { 0.5f } }  // 0=free, 1=occupied

    fun update(distanceCm: Float, angleDeg: Float) {
        val angleRad = Math.toRadians(angleDeg.toDouble())
        val x = (distanceCm * cos(angleRad) / 10).toInt() + 50
        val y = (distanceCm * sin(angleRad) / 10).toInt() + 50
        if (x in 0..99 && y in 0..99) {
            grid[x][y] = minOf(grid[x][y] + 0.15f, 1.0f)  // mark occupied
            // Mark cells between sensor and obstacle as free
            raycastFree(50, 50, x, y)
        }
    }
}
```

### AI Enhancement
- Train a CNN to denoise and complete partial maps
- Input: raw sparse grid from sensor sweeps
- Output: completed, smoothed occupancy grid
- Model size: ~50KB (runs on-device easily)

### Output to user
"Wall detected 1.2 meters ahead on your left"
"Open path forward for approximately 3 meters"

---

## Module 2: Environment Fingerprinting

### What it does
Recognizes recurring spatial patterns — doorways, corridors, open rooms,
corners — based purely on the signature of distance readings over time.
Over multiple sessions, it learns the user's specific environment.

### Feature Extraction
```
Raw window: [91, 89, 45, 44, 45, 88, 90, 91, 89, 90]
                      ↑ doorway signature: sudden dip and return

Features extracted per window:
  - mean, std, min, max
  - range (max - min)
  - zero-crossing rate (how often direction changes)
  - slope pattern (derivative sequence)
  - peak count
```

### Model Architecture
```
Input:  10-value sliding window  → flatten to 10 features
        + 8 statistical features → total 18 input features
Hidden: Dense(32, relu) → Dense(16, relu)
Output: Dense(6, softmax)
        → [open_path, wall, doorway, corridor, corner, room_center]
```

### Training Data Plan
| Environment Type | Samples Needed | How to Collect          |
|------------------|----------------|-------------------------|
| Open path        | 200            | Walk across open room   |
| Wall             | 200            | Point sensor at wall    |
| Doorway          | 150            | Pass through door frame |
| Corridor         | 150            | Walk down hallway       |
| Corner           | 100            | Point into room corner  |
| Room center      | 100            | Stand in open room      |
| **Total**        | **900**        |                         |

### On-Device Personalization
```python
# After 20+ sessions, fine-tune the last layer only:
base_model.trainable = False
personal_layer = Dense(6, activation='softmax')(base_model.output)
personal_model = Model(base_model.input, personal_layer)
personal_model.compile(optimizer=Adam(0.001), loss='categorical_crossentropy')
# Train on user's own labeled sessions (done on-device via TFLite training)
```

### Output to user
"You are approaching a doorway"
"Corridor detected — clear path ahead"
"Corner on your right"

---

## Module 3: Gait-Aware Hazard Prediction

### What it does
Times obstacle warnings relative to the user's walking rhythm so alerts
arrive exactly when the user can act on them — not too early, not too late.

### Gait Data Pipeline
```
NodeMCU:
  - Detects distance oscillation (body bob = ~1-3cm rhythmic variation)
  - Calculates step_cadence (steps/sec)
  - Sends step_detected flag in each packet

Android (adds):
  - Accelerometer Y-axis rhythm
  - Step phase estimation (0.0 = heel strike, 1.0 = toe off)
  - Stride length estimate from cadence + typical human gait
```

### Prediction Logic
```
stride_length_cm ≈ cadence × 65  (average human stride ~65cm at 1 step/sec)
steps_to_obstacle = distance_cm / stride_length_cm
time_to_obstacle_ms = steps_to_obstacle × (1000 / cadence)

Alert thresholds:
  time_to_obstacle < 2000ms → URGENT (vibrate + voice)
  time_to_obstacle < 4000ms → WARNING (voice only)
  time_to_obstacle < 6000ms → CAUTION (soft chime)
```

### AI Enhancement (LSTM)
```
Input sequence: last 20 sensor readings + accelerometer
                shape: (20, 4)  → [distance, accel_x, accel_y, accel_z]
LSTM(32) → Dense(16, relu) → Dense(3, softmax)
Output: [safe, caution, danger] with confidence score

Why LSTM: captures the temporal rhythm of walking + approach pattern
          better than a single-shot classifier
```

### Stride-Synchronized Alert Timing
```kotlin
fun scheduleAlert(stepsToObstacle: Float, cadence: Float) {
    // Alert during the weight-transfer phase of next stride
    // (most natural time to change direction)
    val msPerStep = if (cadence > 0) 1000f / cadence else 600f
    val alertDelay = (stepsToObstacle * msPerStep * 0.8f).toLong()
    Handler(Looper.getMainLooper()).postDelayed({ triggerAlert() }, alertDelay)
}
```

### Output to user
"Obstacle in 2 steps — slow down"
"Wall approaching — turn right available"

---

## Module 4: RL Alert Tuning

### What it does
Learns from implicit user behavior — did they slow down after an alert?
Did they collide? Did they ignore the alert? — to tune when alerts fire,
how loud/strong they are, and which modality (voice vs vibration) works
best for this specific user in this context.

### State Space
```
State s = [
    distance_to_obstacle,     // cm
    user_speed,               // cm/s estimated from sensor + gait
    environment_type,         // from fingerprinting module
    time_of_day_bucket,       // morning / afternoon / night
    alert_count_last_minute,  // avoid alert fatigue
    last_alert_response       // slowed / ignored / collision
]
```

### Action Space
```
Actions:
  0 = No alert
  1 = Soft vibration
  2 = Strong vibration
  3 = Quiet voice alert
  4 = Loud voice alert
  5 = Voice + vibration
```

### Reward Signal (implicit, no user input needed)
```
+2.0  → User slowed down within 2 seconds of alert (alert was useful)
+1.0  → User changed direction after alert
-1.0  → Alert fired but user did not change behavior (false alarm)
-2.0  → Collision detected (sudden distance jump to ~0 or rapid approach)
-0.5  → More than 3 alerts in 60 seconds (alert fatigue penalty)
```

### Algorithm: Q-Learning (lightweight, runs on-device)
```python
# Q-table: 6D state (discretized) × 6 actions
# State space: 5×4×6×3×4×3 = 4320 states × 6 actions = 25,920 values
# Memory: ~100KB float32 → fits easily on phone

def update_q(state, action, reward, next_state):
    alpha = 0.1   # learning rate
    gamma = 0.9   # discount factor
    current_q = Q[state][action]
    max_next_q = max(Q[next_state])
    Q[state][action] = current_q + alpha * (reward + gamma * max_next_q - current_q)
```

### Epsilon-Greedy Exploration
```
Early sessions (ε=0.3): tries different alert types to learn
Later sessions (ε=0.05): mostly uses best known action
ε decays by 0.01 per session automatically
```

### Output effect
Alert system becomes personalized:
- Some users prefer vibration → RL learns to favor it
- Quiet environments → RL learns louder alerts work better
- Crowded environments → RL reduces alert frequency

---

## Build Order (Recommended)

```
Phase 1 — Hardware (NOW)
  ✅ NodeMCU firmware (echosense_firmware.ino)
  ✅ Wiring + setup
  □  Test sensor + WiFi API working

Phase 2 — Android App Shell
  □  WiFi connection + WebSocket client
  □  Basic distance display
  □  Accelerometer integration
  □  Voice/vibration output layer

Phase 3 — Data Collection
  □  Collect 900 labeled samples for fingerprinting
  □  Collect 500 gait-annotated samples

Phase 4 — AI Models (Python training)
  □  Train Environment Fingerprinting classifier
  □  Train Gait LSTM model
  □  Convert both to .tflite
  □  Add to Android app assets

Phase 5 — Tier 3 AI Integration
  □  Occupancy Grid Mapper (Kotlin, no training needed)
  □  Environment Fingerprinting (TFLite)
  □  Gait-Aware Hazard Prediction (TFLite LSTM)
  □  RL Alert Tuner (Q-table, pure Kotlin)

Phase 6 — Testing & Tuning
  □  Indoor testing across 5+ environments
  □  10+ RL sessions to see alert personalization
  □  Latency benchmarking (target: <100ms end-to-end)
```

---

## File Structure (Full Project)

```
EchoSense/
├── hardware/
│   ├── echosense_firmware.ino     ← NodeMCU code (DONE)
│   ├── HARDWARE_SETUP.md          ← Wiring guide (DONE)
│   └── libraries_required.txt     ← Arduino dependencies (DONE)
│
├── android/
│   ├── app/src/main/java/com/example/echosense/
│   │   ├── MainActivity.kt            ← next to build
│   │   ├── SensorClient.kt            ← WebSocket + HTTP client
│   │   ├── OccupancyGridMapper.kt     ← Tier 3: Module 1
│   │   ├── EnvironmentFingerprinter.kt← Tier 3: Module 2
│   │   ├── GaitHazardPredictor.kt     ← Tier 3: Module 3
│   │   ├── RLAlertTuner.kt            ← Tier 3: Module 4
│   │   ├── ModelHelper.kt             ← TFLite inference wrapper
│   │   └── AlertManager.kt            ← TTS + vibration output
│   └── app/src/main/assets/
│       ├── fingerprint_model.tflite
│       └── gait_model.tflite
│
├── ai_training/
│   ├── collect_data.py            ← Data collection script
│   ├── train_fingerprint.py       ← Train Env Fingerprinting model
│   ├── train_gait_lstm.py         ← Train Gait LSTM model
│   └── convert_to_tflite.py       ← Export to .tflite
│
└── ARCHITECTURE.md                ← This file
```

---

## Target Performance

| Metric                    | Target          |
|---------------------------|-----------------|
| Sensor → App latency      | < 60ms          |
| AI inference time         | < 20ms per frame|
| End-to-end alert latency  | < 100ms         |
| Fingerprinting accuracy   | > 85%           |
| Gait prediction accuracy  | > 80%           |
| Model sizes (each)        | < 200KB         |
| Battery life (phone)      | > 4 hours       |
| NodeMCU range             | ~15m WiFi       |
| Sensor range              | 2cm – 4m        |
