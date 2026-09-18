#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// -------------------------------------------------------------
// 1. إعدادات الشبكة وبروتوكول MQTT
// -------------------------------------------------------------
const char* ssid = "omir-omir4G";
const char* password = "101912599";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "/sensors";

// -------------------------------------------------------------
// 2. إعدادات الأرجل وحدود الحساسات (Configuration Constants)
// -------------------------------------------------------------
const int LED_PIN = 23;
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;

// الحد الأعلى لاكتشاف الحركة بالسنتيمتر (يمكنك تعديلها بسهولة من هنا)
const float DISTANCE_LIMIT = 60.0; 

// -------------------------------------------------------------
// 3. كائنات الاتصال ومتغيرات تتبع الحالة (State Machine Variables)
// -------------------------------------------------------------
WiFiClient espClient;
PubSubClient client(espClient);

bool lastMotionState = false;      // حفظ آخر حالة حركة (حتى لا يتم تكرار الإرسال)
unsigned long lastSensorRead = 0;  // مؤقت قراءة الحساس بدون delay
const long SENSOR_INTERVAL = 150;  // قراءة الحساس كل 150 ملي ثانية

// -------------------------------------------------------------
// 4. دالة قراءة حساس المسافة Ultrasonic HC-SR04
// -------------------------------------------------------------
float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // استخدام Timeout بمقدار 30,000 ميكروثانية لتجنب تعليق الكود
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 

  // في حال فشل القراءة أو انتهاء الوقت إرجاع -1
  if (duration == 0) return -1.0; 

  float distance = (duration * 0.0343) / 2.0;
  return distance;
}

// -------------------------------------------------------------
// 5. دالة الاتصال بشبكة Wi-Fi (بدون تجميد)
// -------------------------------------------------------------
void setup_wifi() {
  delay(10);
  Serial.println("\nConnecting to WiFi...");
  WiFi.begin(ssid, password);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500);
    Serial.print(".");
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi Connection Failed! Will retry in background.");
  }
}

// -------------------------------------------------------------
// 6. دالة الاتصال وخادم MQTT مع معالجة الانقطاع
// -------------------------------------------------------------
void reconnectMQTT() {
  // المحاولة فقط إذا كان Wi-Fi متصلاً
  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32_SmartStreet_" + String(random(0xffff), HEX);

    if (client.connect(clientId.c_str())) {
      Serial.println("\nMQTT Connected!");
    } else {
      Serial.print(" Failed, rc=");
      Serial.print(client.state());
      Serial.println(" Try again in next cycle.");
    }
  }
}

// -------------------------------------------------------------
// 7. دالة بناء وإرسال حمولة MQTT JSON
// -------------------------------------------------------------
void sendMQTTData(int motion, float distance, int light) {
  if (!client.connected()) return;

  StaticJsonDocument<200> doc;
  doc["motion"] = motion;
  
  // حماية: إذا كانت القراءة خطأ (-1) نرسل قيمة افتراضية للبعد
  if (distance > 0) {
    doc["distance"] = serialized(String(distance, 1)); // تقريب لخانة عشرية واحدة
  } else {
    doc["distance"] = 999.0;
  }
  
  doc["light"] = light;

  char buffer[256];
  serializeJson(doc, buffer);

  if (client.publish(mqtt_topic, buffer)) {
    Serial.println("MQTT Payload Published Successfully.");
  } else {
    Serial.println("Failed to publish MQTT Payload.");
  }
}

// -------------------------------------------------------------
// 8. التهيئة الأولية (Setup)
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // حالة الإضاءة المبدئية: مطفأة
  digitalWrite(LED_PIN, LOW);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);

  Serial.println("System Started - Smart Street IoT System Ready");
}

// -------------------------------------------------------------
// 9. الحلقة الرئيسية (Main Loop)
// -------------------------------------------------------------
void loop() {
  // إعادة الاتصال بالشبكة أو السيرفر في حال الانقطاع دون تجميد النظام
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  } else if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop(); // الحفاظ على اتصالات MQTT حية

  // جدولة قراءة الحساس باستخدام millis() بدلاً من delay()
  unsigned long currentMillis = millis();
  if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentMillis;

    float currentDistance = getDistance();

    // طباعة القراءة المستمرة في الـ Serial Monitor
    if (currentDistance > 0) {
      Serial.print("Distance: ");
      Serial.print(currentDistance, 1);
      Serial.println(" cm");
    } else {
      Serial.println("Distance: Sensor Error / Out of Range");
    }

    // تحديد حالة الحركة الحالية بناءً على المسافة والحد المسموح (DISTANCE_LIMIT)
    bool currentMotionState = (currentDistance > 0 && currentDistance <= DISTANCE_LIMIT);

    // ---------------------------------------------------------
    // معالجة تغير الحالة (State Change Detection)
    // ---------------------------------------------------------
    if (currentMotionState != lastMotionState) {
      
      if (currentMotionState) {
        // [حالة 1]: اكتشاف حركة جديدة
        digitalWrite(LED_PIN, HIGH);
        Serial.println("Motion Detected");
        Serial.println("Light ON");
        Serial.println("Alert Sent (via MQTT Event)");

        // إرسال حمولة MQTT تتضمن وجود حركة
        sendMQTTData(1, currentDistance, 1);

      } else {
        // [حالة 2]: انتهاء الحركة وابتعاد الجسم
        digitalWrite(LED_PIN, LOW);
        Serial.println("No Motion");
        Serial.println("Light OFF");

        // إرسال حمولة MQTT تتضمن عدم وجود حركة
        sendMQTTData(0, currentDistance, 0);
      }

      // تحديث الحالة السابقة لمنع تكرار الإرسال
      lastMotionState = currentMotionState;
    }
  }
}