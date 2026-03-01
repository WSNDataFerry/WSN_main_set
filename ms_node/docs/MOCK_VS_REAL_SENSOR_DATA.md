# Mock vs Real Sensor Data — How It Works

This document explains **when the node uses mock (synthetic) data** and **when it uses real sensor values**, and confirms the behaviour is correct.

---

## 1. High-level behaviour

| Situation | What happens |
|-----------|----------------|
| **Sensor connected and read succeeds** | That sensor’s **real** value is used in the payload; `real_xxx = true` for that sensor. |
| **Sensor not connected / read fails** | That sensor’s struct is filled with **dummy/mock** values; we still set `ok_xxx = true` so the rest of the pipeline runs. |
| **Payload flags** | If **at least one** sensor read was real → `SENSOR_PAYLOAD_FLAG_SENSORS_REAL` is set (**REAL**). If **no** sensor read was real → flag is 0 (**MOCK**). |

So: **mock data is used for any sensor that isn’t connected or fails; real data is used for every sensor that succeeds.** The payload can mix real and mock (e.g. real BME + mock ENS160). The **flags** field tells the CH whether *any* part of the payload came from real sensors.

---

## 2. Where it happens (code flow)

All of this lives in **`ms_node.c`** in the main loop, in the sensor-reading and payload-building block.

### Step 1: Per-sensor read and fallback

For each sensor type the code:

1. **Tries a real read** (e.g. `bme280_read(&bme)`, `aht21_read_with_raw(...)`, etc.).
2. **If the read fails** (sensor not connected, I2C error, etc.):
   - Sets **dummy values** in that sensor’s struct (e.g. BME: synthetic T/H/P; ENS: synthetic AQI/TVOC/eCO2; mag: synthetic x/y/z; audio: synthetic RMS).
   - Sets `ok_xxx = true` anyway so downstream code always has “something” to use.
   - Sets `real_xxx = false` (this read was not real).
3. **If the read succeeds**:
   - Uses the real values already in the struct.
   - Sets `real_xxx = true`.

So after this block:

- **`ok_xxx`** = “we have a value for this sensor (real or dummy).”
- **`real_xxx`** = “this value came from a successful hardware read.”

Example (BME280):

```c
ok_bme = (bme280_read(&bme) == ESP_OK);
real_bme = ok_bme;
if (!ok_bme) {
  bme.temperature_c = 25.0f + 5.0f * sinf(now_ms / 10000.0f);  // dummy
  bme.humidity_pct = 50.0f + 10.0f * cosf(now_ms / 10000.0f);
  bme.pressure_hpa = 1013.0f + 5.0f * sinf(now_ms / 20000.0f);
  ok_bme = true;  // so rest of code sees “we have BME data”
}
```

Same pattern for AHT21, ENS160, GY-271 (mag), INA219, INMP441 (audio).

### Step 2: “Any sensor” and payload build

- **`any_ok = ok_bme || ok_aht || ok_ens || ok_mag || ok_ina || ok_audio`**
- The **payload is built and given to metrics only when `any_ok` is true** (i.e. at least one sensor type was attempted and produced a value, real or dummy).

So:

- **Normal case (sensors present or not):**  
  At least one sensor is enabled and we run its read. Either it succeeds (real) or we fill dummy and set `ok_xxx = true`. So `any_ok` is true, we build the payload and call `metrics_set_sensor_data(&payload)`.

- **Edge case (no sensor ever attempted):**  
  If no sensor is enabled or no read is ever run in that loop iteration, `any_ok` stays false. Then we **do not** update metrics. The state machine will keep sending the **last** payload that was in metrics (or zeros at boot). So “all sensors disabled / never run” → no new mock payload is written; the last one (or zeros) is sent.

### Step 3: REAL vs MOCK flag

- **`s_sensors_real = real_bme || real_aht || real_ens || real_mag || real_ina || real_audio`**
- **`payload.flags = (s_sensors_real ? SENSOR_PAYLOAD_FLAG_SENSORS_REAL : 0) | (s_battery_real ? ...)`**

So:

- If **at least one** sensor read was real → `payload.flags` has **SENSOR_PAYLOAD_FLAG_SENSORS_REAL** → log and CH see **REAL**.
- If **no** sensor read was real (all failed and we used dummies) → flag is 0 → log and CH see **MOCK**.

### Step 4: Filling the payload (real where ok, mock where not)

- **Temperature / humidity / pressure:**  
  If `ok_bme` → use BME values; else if `ok_aht` → use AHT T/H; else leave 0. Then the **MOCK DATA FALLBACK** block (see below) fills **only** the parts that are still missing.

- **Gas (AQI, TVOC, eCO2):**  
  If `ok_ens` → use ENS values; else fallback block fills mock.

- **Mag, audio:**  
  If `ok_mag` / `ok_audio` → use real; else fallback block fills mock.

So each field is either:

- from a **successful read** (real), or  
- from the **MOCK DATA FALLBACK** block (synthetic values when that sensor wasn’t available).

### Step 5: MOCK DATA FALLBACK block

This block only **fills fields that were not set** by a successful sensor:

- **No BME and no AHT** → `payload.temp_c`, `payload.hum_pct` set to random-looking synthetic values (and pressure stays 0 from `payload = {0}` unless BME was ok).
- **No ENS** → `payload.aqi` (and related) set to synthetic.
- **No mag** → `payload.mag_x/y/z` set to synthetic.
- **No audio** → `payload.audio_rms` set to synthetic.

So:

- **When sensors are not connected:**  
  Reads fail → we use dummy in structs and/or this fallback → payload is fully filled with mock values → `s_sensors_real` is false → **MOCK**.

- **When sensors are connected and read succeeds:**  
  Real values are copied into the payload, and the fallback does not overwrite them (conditions like `if (!ok_bme && !ok_aht)` etc.). `s_sensors_real` is true → **REAL**.

---

## 3. Is the functionality correct?

- **Sensors not connected:**  
  Yes. Each sensor read that fails gets dummy values in its struct and/or in the payload fallback. The payload is still built (as long as `any_ok` is true), and `payload.flags` has no REAL bit → **MOCK**.

- **Sensors connected:**  
  Yes. Successful reads set `real_xxx = true`, real values are copied into the payload, and `SENSOR_PAYLOAD_FLAG_SENSORS_REAL` is set → **REAL**.

- **Mix of connected and not:**  
  Yes. Real fields come from successful reads; missing fields are filled by the MOCK DATA FALLBACK. The flag is REAL if at least one sensor was real.

- **Sending to CH:**  
  The state machine calls `metrics_get_sensor_data(&payload)` and sends that payload. The same `payload.flags` (REAL/MOCK) is what we log and what the CH receives, so **mock data is sent to the CH when sensors are not connected**, and **real data when they are**, with the flag matching.

The only subtle case is when **no sensor is ever attempted** (`any_ok` always false): metrics are not updated, so the member keeps sending the previous payload (or zeros). That is consistent with “no new data.”

---

## 4. Summary table

| Sensors situation           | real_xxx / s_sensors_real | payload content        | payload.flags | Log/CH sees |
|----------------------------|----------------------------|------------------------|---------------|-------------|
| All reads fail             | all false                  | all from dummies/fallback | 0             | **MOCK**    |
| At least one read succeeds | at least one true          | mix real + fallback for missing | SENSOR_PAYLOAD_FLAG_SENSORS_REAL | **REAL** |
| No sensor attempted        | —                          | metrics not updated    | (previous)    | (previous)  |

So: **when sensors are not connected the node uses mock data and marks it MOCK; when sensors are connected and read OK it uses real values and marks REAL.** The implementation matches that behaviour.
