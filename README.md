# M5StickCPlus Stability Tracker

This project turns an M5StickCPlus into a  stability training aid. It continuously monitors your hold stability using the built-in IMU and listens for the mechanical "snap" of a dry fire using the built-in microphone. When a shot is detected, it grades how steady you were in the **2 seconds immediately preceding the shot**, actively ignoring the recoil/movement of the trigger break itself.

## Key Features

* **Pre-Shot Circular Buffer:** Constantly records the last 2 seconds of accelerometer data at 50Hz.
* **Ready Chirp:** Emits a tiny 50ms beep once you've held the device perfectly still for a full 2 seconds, letting you know you are cleared to break the shot.
* **Recoil Exclusion (Look-back):** Automatically discards the last ~140ms of movement data *right at* the moment of the shot so the physical trigger break doesn't ruin your hold score.
* **Drift Diagnosis:** Tells you your primary error direction (e.g., "Left Tilt", "Vertical Shake").
* **Visual Plotting:** Graphs your X, Y, and Z axis micromovements on-screen after a shot.

## How to Use It

1. **Mount it:** Attach the M5StickCPlus to your device (screen facing you, landscape mode).
2. **Watch the Dot:** The idle screen features a live stability dot. 
   * **Green:** Steady.
   * **Yellow:** Minor wobble.
   * **Red:** Too much movement.
3. **Wait for the Beep:** Hold steady in the Green zone for 2 seconds. The device will emit a tiny, high-pitched chirp. This means the buffer is full of clean data.
4. **cause a shock** . The microphone or the sudden kinetic jolt will trigger the calculation.
5. **Read the Score:** The screen will flash Green (Great), Orange (Okay), or Red (Poor) with your percentage score and your primary drift direction.
6. **View the Graph:** Press **Button A** (the big "M5" button) to view a line graph of your hold. Press it again to return to the idle screen.

## Tuning and Adjustments

If the device is triggering too easily, scoring too harshly, or missing shots, you can adjust these variables at the top of the sketch:

### 1.  Sensitivity

* `SOUND_THRESHOLD = 8000;`
  * **What it does:** Sets how loud a mechanical click needs to be to trigger the device.
  * **Adjust:** Lower this (e.g., `5000`) if it's missing the triggers. Raise it (e.g., `12000`) if background noise or handling noise is causing false triggers.
* `MOTION_THRESHOLD = 0.25;`
  * **What it does:** Triggers if the device detects a sudden G-force spike.
  * **Adjust:** Raise to `0.40` or `0.50` if just bumping the device causes a false trigger.

### 2. Recoil Exclusion (The "Pollution Offset")

* `POLLUTION_OFFSET = 7;`
  * **What it does:** The number of samples to delete right at the moment the trigger is pulled. At 50Hz, 7 samples = 140 milliseconds.
  * **Adjust:** If your score is always 0% because your trigger has a very heavy, slow break that shakes the gun *before* it clicks, increase this to `10` (200ms). If you want strict grading right up to the exact millisecond of the click, lower it to `2` or `3`.

### 3. Buffer Window & Beep Timing

* `BUFFER_SECONDS = 2;`
  * **What it does:** Defines how long you must hold still to get the "Ready" beep, and how much time is evaluated for your final score.
  * **Adjust:** Change to `3` for a much harder endurance challenge.

### 4. Scoring Strictness

To make it easier or harder to get a 100% score, look for this line inside the `analyzeAndShowResults()` function:

```cpp
float stabilityScore = 100.0 * (1.0 - (totalDeviation / 0.75));
