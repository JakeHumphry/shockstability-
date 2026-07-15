#include <M5StickCPlus.h>
#include <driver/i2s.h>

// Undefine the conflicting macros defined by Arduino/M5StickCPlus
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Now it is safe to include standard C++ libraries
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm> // For std::max and std::clamp

// --- CONFIGURATION ---
const int SAMPLE_RATE_HZ = 50;              
const int BUFFER_SECONDS = 2;               
const int BUFFER_SIZE = SAMPLE_RATE_HZ * BUFFER_SECONDS; 
const unsigned long DISPLAY_LOCK_MS = 2500; 

// The number of samples to throw away right AT the trigger moment 
// (7 samples @ 50Hz = ~140ms). This prevents the actual click/recoil from ruining the stability score.
const int POLLUTION_OFFSET = 7; 

// --- SENSITIVE TRIGGER THRESHOLDS ---
const float MOTION_THRESHOLD = 0.25;        // Triggers on sudden 0.25G shift
const int SOUND_THRESHOLD = 8000;           // Catch sharp mechanical snaps (e.g., dry fire)

// --- MICROPHONE PINS ---
#define MIC_PIN_CLK  0
#define MIC_PIN_DATA 34

// --- DATA STRUCTURES ---
struct AccelSample {
  float x;
  float y;
  float z;
};

// Live circular buffer
AccelSample dataBuffer[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

// Saved buffer for plotting
AccelSample savedBuffer[BUFFER_SIZE];
int savedSamplesCount = 0;
float savedMeanX = 0, savedMeanY = 0, savedMeanZ = 0;
bool hasSavedData = false;

// UI & Beep States
unsigned long lastTriggerTime = 0;
bool isResultDisplayed = false;
bool isPlotDisplayed = false;

int steadySamples = 0;
bool hasBeeped = false;
unsigned long beepStopTime = 0;

void setupMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 2,
    .dma_buf_len = 128,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pin_config;
  pin_config.bck_io_num   = I2S_PIN_NO_CHANGE;
  pin_config.ws_io_num    = MIC_PIN_CLK;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num  = MIC_PIN_DATA;
  
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

void setup() {
  M5.begin();
  M5.IMU.Init(); 
  setupMic();    
  
  M5.Lcd.setRotation(3); 
  showIdleScreen();
}

void loop() {
  M5.update(); 

  // Handle the tiny non-blocking beep shutoff
  if (beepStopTime > 0 && millis() > beepStopTime) {
    M5.Beep.mute();
    beepStopTime = 0;
  }

  // --- BUTTON HANDLING ---
  if (M5.BtnA.wasPressed() && hasSavedData) {
    if (isPlotDisplayed) {
      isPlotDisplayed = false;
      i2s_zero_dma_buffer(I2S_NUM_0); 
      showIdleScreen();
    } else {
      isPlotDisplayed = true;
      drawPlot();
    }
  }

  if (isPlotDisplayed) {
    delay(20);
    return;
  }

  // --- NORMAL OPERATION ---
  if (!isResultDisplayed) {
    float accX = 0, accY = 0, accZ = 0;
    M5.IMU.getAccelData(&accX, &accY, &accZ); 
    float totalG = sqrt(accX*accX + accY*accY + accZ*accZ);
    
    dataBuffer[bufferIndex] = {accX, accY, accZ};
    bufferIndex++;
    if (bufferIndex >= BUFFER_SIZE) {
      bufferIndex = 0;
      bufferFull = true;
    }

    // Audio pacing (20ms block)
    int16_t micData[320]; 
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, (void*)micData, sizeof(micData), &bytesRead, portMAX_DELAY);
    
    int maxAmplitude = 0;
    for (int i = 0; i < bytesRead / 2; i++) {
       int absVal = abs(micData[i]);
       if (absVal > maxAmplitude) maxAmplitude = absVal;
    }

    // --- STEADY BEEP LOGIC ---
    float currentOffset = fabs(totalG - 1.0);
    if (currentOffset <= 0.15) { // If in the "Green" zone
      steadySamples++;
      if (steadySamples >= BUFFER_SIZE && !hasBeeped) {
        M5.Beep.tone(4000); // 4kHz frequency
        beepStopTime = millis() + 50; // Beep for 50ms
        hasBeeped = true;
      }
    } else {
      steadySamples = 0; // Reset counter if you wobble
      hasBeeped = false;
    }

    // --- SENSITIVE TRIGGER LOGIC ---
    bool soundTrigger = (maxAmplitude > SOUND_THRESHOLD);
    bool motionTrigger = (currentOffset > MOTION_THRESHOLD);

    if ((soundTrigger || motionTrigger) && (millis() - lastTriggerTime > DISPLAY_LOCK_MS)) {
      lastTriggerTime = millis();
      isResultDisplayed = true;
      
      // Mute beep immediately if it triggered right on a shot
      M5.Beep.mute(); 
      beepStopTime = 0;
      steadySamples = 0;
      hasBeeped = false;
      
      analyzeAndShowResults();
    } else {
      drawLiveIndicator(currentOffset);
    }
    
  } else {
    delay(20);
    if (millis() - lastTriggerTime > DISPLAY_LOCK_MS) {
      isResultDisplayed = false;
      i2s_zero_dma_buffer(I2S_NUM_0); 
      showIdleScreen();
    }
  }
}

// --- CORE LOGIC ---

void analyzeAndShowResults() {
  int samplesAvailable = bufferFull ? BUFFER_SIZE : bufferIndex;
  
  // Cut off the last few samples so the physical click/recoil doesn't ruin the stability score
  savedSamplesCount = samplesAvailable - POLLUTION_OFFSET; 
  
  if (savedSamplesCount < 10) {
    M5.Lcd.fillScreen(RED);
    M5.Lcd.setCursor(10, 30);
    M5.Lcd.print("Hold longer");
    return;
  }

  // 1. Unwrap the circular buffer into a chronological saved array
  float sumX = 0, sumY = 0, sumZ = 0;
  for (int i = 0; i < savedSamplesCount; i++) {
    // Read starting from the oldest available data up until (newest - POLLUTION_OFFSET)
    int idx = bufferFull ? (bufferIndex + i) % BUFFER_SIZE : i;
    savedBuffer[i] = dataBuffer[idx];
    
    sumX += savedBuffer[i].x;
    sumY += savedBuffer[i].y;
    sumZ += savedBuffer[i].z;
  }
  
  savedMeanX = sumX / savedSamplesCount;
  savedMeanY = sumY / savedSamplesCount;
  savedMeanZ = sumZ / savedSamplesCount;
  hasSavedData = true;

  // 2. Calculate Standard Deviation using the flat savedBuffer
  float varX = 0, varY = 0, varZ = 0;
  for (int i = 0; i < savedSamplesCount; i++) {
    varX += pow(savedBuffer[i].x - savedMeanX, 2);
    varY += pow(savedBuffer[i].y - savedMeanY, 2);
    varZ += pow(savedBuffer[i].z - savedMeanZ, 2);
  }
  
  float devX = sqrt(varX / savedSamplesCount);
  float devY = sqrt(varY / savedSamplesCount);
  float devZ = sqrt(varZ / savedSamplesCount);
  
  float totalDeviation = sqrt(devX*devX + devY*devY + devZ*devZ);

  // 3. Map total deviation to a 0-100% Score (Softened the math for realistic holding)
  // Changed divisor from 0.45 to 0.75 so minor wobbles don't immediately drop you to 0%.
  float stabilityScore = 100.0 * (1.0 - (totalDeviation / 0.75));
  if (stabilityScore > 100.0) stabilityScore = 100.0;
  if (stabilityScore < 0.0) stabilityScore = 0.0;

  // 4. Identify largest drift
  String driftDir = "None";
  float maxDev = std::max(devX, std::max(devY, devZ));
  if (maxDev > 0.05) { 
    if (maxDev == devX) driftDir = (savedMeanX > 0) ? "Right Tilt" : "Left Tilt";
    else if (maxDev == devY) driftDir = (savedMeanY > 0) ? "Forward Tilt" : "Backward Tilt";
    else if (maxDev == devZ) driftDir = "Vertical Shake";
  } else {
    driftDir = "Rock Solid!";
  }

  // 5. Render Score Screen
  uint16_t screenColor = TFT_GREEN;
  if (stabilityScore < 80.0) screenColor = TFT_ORANGE;
  if (stabilityScore < 50.0) screenColor = TFT_RED;

  M5.Lcd.fillScreen(screenColor);
  M5.Lcd.setTextColor(TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10); M5.Lcd.print("SHOCK DETECTED!");
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 40); M5.Lcd.printf("SCORE: %d%%", (int)stabilityScore);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 85); M5.Lcd.print(driftDir);
  M5.Lcd.setCursor(10, 110); M5.Lcd.print("Btn A for Plot ->");
}

void drawPlot() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(1);
  
  // Top Legend
  M5.Lcd.setTextColor(TFT_RED);   M5.Lcd.setCursor(5, 5);   M5.Lcd.print("X(L/R)");
  M5.Lcd.setTextColor(TFT_GREEN); M5.Lcd.setCursor(55, 5);  M5.Lcd.print("Y(F/B)");
  M5.Lcd.setTextColor(TFT_CYAN);  M5.Lcd.setCursor(105, 5); M5.Lcd.print("Z(U/D)");
  M5.Lcd.setTextColor(TFT_WHITE); M5.Lcd.setCursor(170, 5); M5.Lcd.print("Scale: 0.5G");

  // Draw Center Line (Zero Deviation)
  int centerY = 75;
  M5.Lcd.drawLine(0, centerY, 240, centerY, TFT_DARKGREY);

  int prevX = 0, prevY_X = centerY, prevY_Y = centerY, prevY_Z = centerY;
  
  // Loop through chronological data and plot lines
  for(int i = 0; i < savedSamplesCount; i++) {
     int px = map(i, 0, savedSamplesCount - 1, 0, 240);
     
     int pyX = centerY - (savedBuffer[i].x - savedMeanX) * 100;
     int pyY = centerY - (savedBuffer[i].y - savedMeanY) * 100;
     int pyZ = centerY - (savedBuffer[i].z - savedMeanZ) * 100;
     
     pyX = std::clamp(pyX, 15, 134);
     pyY = std::clamp(pyY, 15, 134);
     pyZ = std::clamp(pyZ, 15, 134);

     if (i > 0) {
        M5.Lcd.drawLine(prevX, prevY_X, px, pyX, TFT_RED);
        M5.Lcd.drawLine(prevX, prevY_Y, px, pyY, TFT_GREEN);
        M5.Lcd.drawLine(prevX, prevY_Z, px, pyZ, TFT_CYAN);
     }
     
     prevX = px; prevY_X = pyX; prevY_Y = pyY; prevY_Z = pyZ;
  }
}

void showIdleScreen() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(15, 20); M5.Lcd.print("READY...");
  M5.Lcd.setCursor(15, 50); M5.Lcd.print("Waiting for shot");
  
  if (hasSavedData) {
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(15, 115); 
    M5.Lcd.print("Btn A: View Last Plot");
  }
}

void drawLiveIndicator(float currentOffset) {
  uint16_t indicatorColor = GREEN;
  if (currentOffset > 0.15) indicatorColor = YELLOW;
  if (currentOffset > 0.35) indicatorColor = RED;
  
  M5.Lcd.fillCircle(120, 105, 8, indicatorColor);
}
