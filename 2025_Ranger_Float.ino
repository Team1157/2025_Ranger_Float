#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <MS5837.h>  // Include the sensor library
#include <Preferences.h>  

// Wi-Fi credentials for Access Point
const char* ap_ssid = "Wobbegong_Float";
const char* ap_password = "Robosharks";

WebServer server(80);
Preferences preferences;  // For saving data to flash idk why they call it that

Servo buoyancyServo;
const int servoPin = 13; 

// MS5837 sensor object
MS5837 sensor;

// Depth/Temp Variables
float currentDepth = 0.0;
float uncalibrated = 0.0;
float currentTemperature = 0.0;

// Target settings
float targetDepth = 2.5; // meters
bool autoControl = true; // automatic or manual control
String manualCommand = ""; // "inflate" or "deflate"

// Data logging variables
const int MAX_DATA_POINTS = 2400; // 20 minutes at 0.5 second intervals
struct DataPoint {
  float depth;
  float temperature;
  unsigned long timestamp;
};
DataPoint dataHistory[MAX_DATA_POINTS];
int historyIndex = 0;
bool historyFull = false;

// Memory saving constants
const char* STORAGE_NAMESPACE = "wobbydata";
const int SAVE_INTERVAL = 60000;  // Save to flash every 60 seconds
unsigned long lastSaveTime = 0;
unsigned long startTime = 0;  // When the device started

// Function to read from MS5837
void readDepthSensor() {
  sensor.read();
  currentTemperature = sensor.temperature(); // °C
  uncalibrated = sensor.depth(); // Meters (fresh water by default)
  currentDepth = (uncalibrated+1.90);
  
  // Log the data
  dataHistory[historyIndex].depth = currentDepth;
  dataHistory[historyIndex].temperature = currentTemperature;
  dataHistory[historyIndex].timestamp = millis();
  
  historyIndex = (historyIndex + 1) % MAX_DATA_POINTS;
  if (historyIndex == 0) {
    historyFull = true;
  }
  
  // Save data to flash periodically
  unsigned long currentTime = millis();
  if (currentTime - lastSaveTime >= SAVE_INTERVAL) {
    saveDataToFlash();
    lastSaveTime = currentTime;
  }
}

// Save data to flash memory
void saveDataToFlash() {
  Serial.println("Saving data to flash...");
  
  // Start the preferences with our namespace
  preferences.begin(STORAGE_NAMESPACE, false);
  
  // Save current settings
  preferences.putFloat("targetDepth", targetDepth);
  preferences.putBool("autoControl", autoControl);
  preferences.putString("manualCommand", manualCommand);
  
  // Save data history state
  preferences.putInt("historyIndex", historyIndex);
  preferences.putBool("historyFull", historyFull);
  preferences.putULong("startTime", startTime);
  
  // Save the data in chunks to avoid memory issues
  const int CHUNK_SIZE = 100;
  for (int chunk = 0; chunk < MAX_DATA_POINTS / CHUNK_SIZE; chunk++) {
    int startIdx = chunk * CHUNK_SIZE;
    
    // Prepare chunk key names
    String dataKey = "data" + String(chunk);
    
    // Save chunk of data
    preferences.putBytes(dataKey.c_str(), &dataHistory[startIdx], CHUNK_SIZE * sizeof(DataPoint));
  }
  
  preferences.end();
  Serial.println("Data saved to flash.");
}

// Load data from flash memory
void loadDataFromFlash() {
  Serial.println("Loading data from flash...");
  
  // Start the preferences with our namespace
  preferences.begin(STORAGE_NAMESPACE, true); // true = read-only
  
  // Load current settings
  if (preferences.isKey("targetDepth")) {
    targetDepth = preferences.getFloat("targetDepth", 2.5);
    autoControl = preferences.getBool("autoControl", true);
    manualCommand = preferences.getString("manualCommand", "");
    
    // Data loading is skipped intentionally - we're resetting on boot
    
    Serial.println("Settings loaded from flash, but data reset as requested.");
  } else {
    Serial.println("No saved settings found.");
  }
  
  preferences.end();
  
  // Reset all data points regardless of what was in flash
  resetData();
}

// Reset all stored data
void resetData() {
  // Record the start time for relative timestamps
  startTime = millis();
  
  // Clear all data points
  for (int i = 0; i < MAX_DATA_POINTS; i++) {
    dataHistory[i].depth = 0.0;
    dataHistory[i].temperature = 0.0;
    dataHistory[i].timestamp = 0;
  }
  
  // Reset indices
  historyIndex = 0;
  historyFull = false;
  
  // Save the reset settings to flash (but not the data)
  saveSettings();
  
  Serial.println("Data reset complete.");
}

// Save just the settings without the data agh
void saveSettings() {
  preferences.begin(STORAGE_NAMESPACE, false);
  preferences.putFloat("targetDepth", targetDepth);
  preferences.putBool("autoControl", autoControl);
  preferences.putString("manualCommand", manualCommand);
  preferences.putULong("startTime", startTime);
  preferences.end();
}

// Adjust buoyancy automatically
void adjustBuoyancy() {
  if (autoControl) {
    if (currentDepth < targetDepth - 0.1) {
      buoyancyServo.write(120); // Sink (inflate)
    } else if (currentDepth > targetDepth + 0.1) {
      buoyancyServo.write(60); // Rise (deflate)
    } else {
      buoyancyServo.write(90); // Hold
    }
  } else {
    if (manualCommand == "inflate") {
      buoyancyServo.write(120);
    } else if (manualCommand == "deflate") {
      buoyancyServo.write(60);
    } else {
      buoyancyServo.write(90);
    }
  }
}

// Get formatted time for relative timestamps
String getFormattedTimestamp(unsigned long timestamp) {
  // Calculate relative time since device started
  unsigned long relativeTime = timestamp - startTime;
  
  // Convert to hours, minutes, seconds
  unsigned long seconds = relativeTime / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds %= 60;
  minutes %= 60;
  
  char buffer[20];
  sprintf(buffer, "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buffer);
}

// Generate CSV data for download and stuff
String getCSVData() {
  String csv = "Session Time,Depth (m),Temperature (C),Runtime (ms)\r\n";
  int startIndex = historyFull ? historyIndex : 0;
  int count = historyFull ? MAX_DATA_POINTS : historyIndex;
  
  for (int i = 0; i < count; i++) {
    int idx = (startIndex + i) % MAX_DATA_POINTS;
    
    // Skip entries with 0 timestamps (unused slots)
    if (dataHistory[idx].timestamp == 0) continue;
    
    // Format as HH:MM:SS
    String formattedTime = getFormattedTimestamp(dataHistory[idx].timestamp);
    
    // Create comprehensive CSV row
    csv += formattedTime + "," + 
           String(dataHistory[idx].depth, 3) + "," +  // 3 decimal precision for depth
           String(dataHistory[idx].temperature, 2) + "," + // 2 decimal precision for temp
           String(dataHistory[idx].timestamp - startTime) + // raw milliseconds since boot
           "\r\n";
  }
  
  return csv;
}

// Get stats as a download
String getStatsData() {
  float minDepth = 10000;
  float maxDepth = -10000;
  float avgDepth = 0;
  float minTemp = 10000;
  float maxTemp = -10000;
  float avgTemp = 0;
  unsigned long firstTime = 0;
  unsigned long lastTime = 0;
  int validPoints = 0;
  
  int startIndex = historyFull ? historyIndex : 0;
  int count = historyFull ? MAX_DATA_POINTS : historyIndex;
  
  for (int i = 0; i < count; i++) {
    int idx = (startIndex + i) % MAX_DATA_POINTS;
    
    // Skip entries with 0 timestamps (unused slots)
    if (dataHistory[idx].timestamp == 0) continue;
    
    if (firstTime == 0) firstTime = dataHistory[idx].timestamp;
    lastTime = dataHistory[idx].timestamp;
    
    minDepth = min(minDepth, dataHistory[idx].depth);
    maxDepth = max(maxDepth, dataHistory[idx].depth);
    avgDepth += dataHistory[idx].depth;
    
    minTemp = min(minTemp, dataHistory[idx].temperature);
    maxTemp = max(maxTemp, dataHistory[idx].temperature);
    avgTemp += dataHistory[idx].temperature;
    
    validPoints++;
  }
  
  if (validPoints == 0) {
    return "No data points available";
  }
  
  avgDepth /= validPoints;
  avgTemp /= validPoints;
  
  String stats = "Wobby the Float Session Statistics\r\n";
  stats += "-------------------------\r\n";
  stats += "Session Duration: " + getFormattedTimestamp(lastTime - firstTime + startTime) + "\r\n";
  stats += "Total Data Points: " + String(validPoints) + "\r\n\r\n";
  
  stats += "Depth Statistics:\r\n";
  stats += "Min Depth: " + String(minDepth, 3) + " meters\r\n";
  stats += "Max Depth: " + String(maxDepth, 3) + " meters\r\n";
  stats += "Average Depth: " + String(avgDepth, 3) + " meters\r\n";
  stats += "Depth Range: " + String(maxDepth - minDepth, 3) + " meters\r\n\r\n";
  
  stats += "Temperature Statistics:\r\n";
  stats += "Min Temperature: " + String(minTemp, 2) + " C\r\n";
  stats += "Max Temperature: " + String(maxTemp, 2) + " C\r\n";
  stats += "Average Temperature: " + String(avgTemp, 2) + " C\r\n";
  stats += "Temperature Range: " + String(maxTemp - minTemp, 2) + " C\r\n";
  
  return stats;
}

// Serve the main dashboard
void handleRoot() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>Wobby the Float</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: Arial; text-align: center; margin: 10px; }
        button { padding: 10px 20px; margin: 10px; font-size: 18px; }
        input { font-size: 18px; width: 80px; }
        .section { 
          background-color: #f8f8f8; 
          padding: 15px;
          border-radius: 8px;
          margin-bottom: 20px;
          box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }
        h2 {
          margin-top: 5px;
          margin-bottom: 15px;
          color: #333;
        }
        .readings {
          font-size: 20px;
          font-weight: bold;
        }
        .export-button {
          text-decoration: none;
          background-color: #4CAF50;
          color: white;
          padding: 12px 24px;
          border-radius: 4px;
          display: inline-block;
          margin: 10px;
          font-weight: bold;
        }
        .export-button:hover {
          background-color: #45a049;
        }
        .reset-button {
          background-color: #f44336;
          color: white;
        }
        .reset-confirm {
          display: none;
          background-color: #fff0f0;
          border: 2px solid #ff6666;
          padding: 10px;
          border-radius: 5px;
          margin: 10px 0;
        }
        .session-info {
          font-style: italic;
          color: #666;
          margin-top: 15px;
        }
      </style>
    </head>
    <body>
      <h1>Wobby the Float</h1>
      
      <div class="section">
        <h2>Current Readings</h2>
        <p class="readings">Depth: <span id="depth">0.00</span> meters</p>
        <p class="readings">Temperature: <span id="temp">0.00</span> C</p>
        <p>Status: <span id="status">Initializing...</span></p>
        <p class="session-info">Session Runtime: <span id="runtime">00:00:00</span></p>
      </div>
      
      <div class="section">
        <h2>Depth Controls</h2>
        <form action="/set" method="GET">
          <p>Target Depth: <input type="number" step="0.1" name="depth" id="depthInput" value="2.5"> meters</p>
          <button type="submit" name="mode" value="auto">Set Auto</button>
        </form>
        <form action="/set" method="GET">
          <button type="submit" name="mode" value="inflate">Inflate</button>
          <button type="submit" name="mode" value="deflate">Deflate</button>
          <button type="submit" name="mode" value="hold">Hold</button>
        </form>
      </div>
      
      <div class="section">
        <h2>Data Management</h2>
        <p>
          <a href="/download" download="wobby_data.csv" class="export-button">Download CSV Data</a>
          <a href="/stats" download="wobby_stats.txt" class="export-button">Download Statistics</a>
        </p>
        <p>
          <button id="reset-button" class="reset-button" onclick="showResetConfirm()">Reset All Data</button>
        </p>
        <div id="reset-confirm" class="reset-confirm">
          <p><strong>Warning:</strong> This will erase all collected data. Are you sure?</p>
          <button onclick="resetData()">Yes, Reset Data</button>
          <button onclick="hideResetConfirm()">Cancel</button>
        </div>
        <p class="session-info">
          Data points collected: <span id="dataPoints">0</span> / 2400
        </p>
      </div>

      <script>
        // Update current readings every second
        setInterval(function() {
          fetch('/data')
            .then(response => response.json())
            .then(data => {
              document.getElementById('depth').innerText = data.depth.toFixed(2);
              document.getElementById('temp').innerText = data.temperature.toFixed(2);
              document.getElementById('runtime').innerText = data.runtime;
              document.getElementById('dataPoints').innerText = data.dataPoints;
              
              // Update status message
              if (data.auto) {
                document.getElementById('status').innerText = `Auto mode - Target: ${data.target.toFixed(2)}m`;
              } else {
                document.getElementById('status').innerText = `Manual mode - ${data.command}`;
              }
              
              // Set input field default
              document.getElementById('depthInput').value = data.target.toFixed(1);
            });
        }, 1000);
        
        // Reset data confirmation
        function showResetConfirm() {
          document.getElementById('reset-confirm').style.display = 'block';
        }
        
        function hideResetConfirm() {
          document.getElementById('reset-confirm').style.display = 'none';
        }
        
        function resetData() {
          fetch('/reset')
            .then(response => {
              if (response.ok) {
                alert('Data reset successful');
                hideResetConfirm();
              } else {
                alert('Error resetting data');
              }
            })
            .catch(error => {
              console.error('Error:', error);
              alert('Error resetting data');
            });
        }
      </script>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// Serve JSON sensor data 
void handleData() {
  int dataPoints = historyFull ? MAX_DATA_POINTS : historyIndex;
  
  String json = "{";
  json += "\"depth\":" + String(currentDepth, 2) + ",";
  json += "\"temperature\":" + String(currentTemperature, 2) + ",";
  json += "\"auto\":";
  json += autoControl ? "true" : "false";
  json += ",\"target\":" + String(targetDepth, 2) + ",";
  json += "\"command\":\"" + manualCommand + "\",";
  json += "\"runtime\":\"" + getFormattedTimestamp(millis()) + "\",";
  json += "\"dataPoints\":" + String(dataPoints);
  json += "}";
  
  server.send(200, "application/json", json);
}

// Handle data reset
void handleReset() {
  resetData();
  server.send(200, "text/plain", "Data reset successful");
}

// Serve CSV data for download
void handleDownloadCSV() {
  String csvData = getCSVData();
  server.sendHeader("Content-Disposition", "attachment; filename=wobby_data.csv");
  server.sendHeader("Content-Type", "text/csv");
  server.send(200, "text/csv", csvData);
}

// Serve statistics data for download
void handleStats() {
  String statsData = getStatsData();
  server.sendHeader("Content-Disposition", "attachment; filename=wobby_stats.txt");
  server.sendHeader("Content-Type", "text/plain");
  server.send(200, "text/plain", statsData);
}

// Handle settings from web page
void handleSet() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");

    if (mode == "auto") {
      autoControl = true;
      if (server.hasArg("depth")) {
        targetDepth = server.arg("depth").toFloat();
      }
    } else if (mode == "inflate" || mode == "deflate" || mode == "hold") {
      autoControl = false;
      manualCommand = mode;
    }
    
    // Save settings immediately
    saveSettings();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  buoyancyServo.attach(servoPin);

  // Start Access Point
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("Access Point Started");
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  // Setup MS5837 sensor
  if (!sensor.init()) {
    Serial.println("MS5837 not found. Check wiring.");
    while (1); // halt
  }
  sensor.setModel(MS5837::MS5837_02BA); // Use Bar02 model
  sensor.setFluidDensity(997); // Freshwater = 997 kg/m^3

  // Load settings from flash but reset data as requested
  loadDataFromFlash();
  
  // Setup routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);
  server.on("/reset", handleReset);
  server.on("/download", handleDownloadCSV);
  server.on("/stats", handleStats);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  readDepthSensor();
  adjustBuoyancy();
  server.handleClient();
  delay(500);
  
  // Handle potential millis() overflow
  if (millis() < lastSaveTime) {
    lastSaveTime = millis();
  }
}
