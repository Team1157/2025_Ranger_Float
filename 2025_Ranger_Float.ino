#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <MS5837.h>  // Include the sensor library

// Wi-Fi credentials for Access Point
const char* ap_ssid = "Wobbegong_Float";
const char* ap_password = "Robosharks";

WebServer server(80);

Servo buoyancyServo;
const int servoPin = 13; // Change to actual servo control pin, I haven't decided on layout yet

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

// Function to read from MS5837
void readDepthSensor() {
  sensor.read();
  currentTemperature = sensor.temperature(); // °C
  uncalibrated = sensor.depth(); // Meters (fresh water by default)
  currentDepth = (uncalibrated-149.65)
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

// Serve the main dashboard
void handleRoot() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ROV Float Dashboard</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: Arial; text-align: center; margin-top: 30px; }
        button { padding: 10px 20px; margin: 10px; font-size: 18px; }
        input { font-size: 18px; width: 80px; }
      </style>
    </head>
    <body>
      <h1>ROV Float Dashboard</h1>
      <p><b>Depth:</b> <span id="depth">0.00</span> meters</p>
      <p><b>Temperature:</b> <span id="temp">0.00</span> °C</p>

      <h2>Controls</h2>
      <form action="/set" method="GET">
        <p>Target Depth: <input type="number" step="0.1" name="depth" id="depthInput"> meters</p>
        <button type="submit" name="mode" value="auto">Set Auto</button>
      </form>
      <form action="/set" method="GET">
        <button type="submit" name="mode" value="inflate">Inflate</button>
        <button type="submit" name="mode" value="deflate">Deflate</button>
        <button type="submit" name="mode" value="hold">Hold</button>
      </form>

      <script>
        setInterval(function() {
          fetch('/data')
            .then(response => response.json())
            .then(data => {
              document.getElementById('depth').innerText = data.depth.toFixed(2);
              document.getElementById('temp').innerText = data.temperature.toFixed(2);
            });
        }, 1000);
      </script>
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// Serve JSON sensor data
void handleData() {
  String json = "{";
  json += "\"depth\":" + String(currentDepth, 2) + ",";
  json += "\"temperature\":" + String(currentTemperature, 2);
  json += "}";
  server.send(200, "application/json", json);
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
  sensor.setModel(MS5837::MS5837_30BA); // Use Bar30 model
  sensor.setFluidDensity(997); // Freshwater = 997 kg/m^3

  // Setup routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  readDepthSensor();
  adjustBuoyancy();
  server.handleClient();
  delay(500);
}
