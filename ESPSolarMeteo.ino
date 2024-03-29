#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <PubSubClient.h>

#include "local_config.h"

///////////////////////////////////////////////////////////////////
// Definitions

#define CONFIG_VERSION_MAJOR 1
#define CONFIG_VERSION_MINOR 5

// I2C
#define CONFIG_I2C_SDA 4
#define CONFIG_I2C_SCL 5

// BME280 address
#define BME280_ADDR BME280_ADDRESS_ALTERNATE

// Status LED pin
#define CONFIG_STATUS_LED 13

// Test mode pin (activates test mode when pulled to ground)
#define CONFIG_TEST_MODE_PIN 12

// Total work timeout (s)
#define CONFIG_WORK_TIMEOUT 15

// Go to 1 minute sleep after WDT reset before resuming
#define CONFIG_SLEEP_AFTER_WDT_RESET 60

// Test mode: use test MQTT server
//#define TEST_MODE
// Test mode: override deep sleep timeout
//#define TEST_DEEPSLEEP 20

///////////////////////////////////////////////////////////////////
// Globals

Adafruit_BME280 g_bme280; // 1-VDD 2-GND 3-SCL 4-SDA

WiFiClient g_wifiClient;

#if CONFIG_SERVER_USE_MQTT
PubSubClient g_mqttClient(g_wifiClient);
#endif

#ifdef TEST_MODE
bool g_testMode = true;
#else
bool g_testMode = false;
#endif

volatile uint32_t g_measureOk = false; // measurement successful
volatile uint32_t g_wifiOk = false; // WiFi connection successful
volatile uint32_t g_sendOk = false; // Server send successful

volatile uint32_t g_shutdown = 0; // flag: shutting down

uint32_t g_adc = 0;
float g_vcc = 0;
float g_temperature = 0;
float g_humidity = 0;
float g_pressure = 0;

uint32_t g_resetReason = 0;
uint32_t g_startTime = 0;

///////////////////////////////////////////////////////////////////
// Functions

void LedOn(bool f)
{
#ifdef CONFIG_STATUS_LED
    digitalWrite(CONFIG_STATUS_LED, f ? HIGH : LOW);
#endif
}

float adcToVoltage(unsigned vadc)
{
    return (float)vadc * ((float)CONFIG_ADC_VREF_VCC / CONFIG_ADC_VREF_RAW);
}

uint32_t calcDeepSleepTime()
{
    // Calculate sleep time
    uint32_t sleepTime = CONFIG_SLEEP_TIME_NORM;
    if (g_vcc < CONFIG_VOLTAGE_NORM)
    {
        if (g_vcc <= CONFIG_VOLTAGE_MIN)
        {
            sleepTime = CONFIG_SLEEP_TIME_MAX;
        }
        else
        {
            sleepTime = CONFIG_SLEEP_TIME_MAX - (uint32_t)(
                (g_vcc - CONFIG_VOLTAGE_MIN) *
                (CONFIG_SLEEP_TIME_MAX - CONFIG_SLEEP_TIME_NORM) /
                (CONFIG_VOLTAGE_NORM - CONFIG_VOLTAGE_MIN));
        }
    }
    return sleepTime;
}

bool isValid(float v)
{
    return !isnan(v) && v != 0;
}

bool doMeasurement(float& temp, float& hum, float& prs)
{
#ifdef CONFIG_I2C_SDA
    Wire.begin(CONFIG_I2C_SDA,CONFIG_I2C_SCL);
    //Wire.setClock(50000); // for software-driven ESP8266 implementation

    if (! g_bme280.begin(BME280_ADDR))
        return false;

    g_bme280.setSampling(Adafruit_BME280::MODE_SLEEP);
    g_bme280.takeForcedMeasurement();
    g_bme280.takeForcedMeasurement();

    temp = g_bme280.readTemperature();
    hum = g_bme280.readHumidity();
    prs = g_bme280.readPressure();

    g_bme280.setSampling(Adafruit_BME280::MODE_SLEEP);

    if (isValid(temp) || isValid(hum) || isValid(prs))
        return true;
#endif
    return false;
}

bool connectWiFi()
{
    printf("Connecting to WiFi (%s) ...", CONFIG_WIFI_SSID);
    fflush(stdout);
    WiFi.mode(WIFI_STA);
    WiFi.begin(CONFIG_WIFI_SSID, CONFIG_WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
      if (millis() - g_startTime > CONFIG_WORK_TIMEOUT * 1000)
      {
          printf("TIMEOUT\n");
          return false;
      }
      ESP.wdtFeed();
      delay(100);
      printf(".");
      fflush(stdout);
    }
    ESP.wdtFeed();
    printf("OK\n");
    return true;
}

#if CONFIG_SERVER_USE_MQTT

bool sendMqtt()
{
    const char *srvName = g_testMode ? CONFIG_MQTT_HOST_TEST : CONFIG_MQTT_HOST;
    printf("Connecting to MQTT server at %s:%u\n", srvName, CONFIG_MQTT_PORT);

    g_mqttClient.setServer(srvName, CONFIG_MQTT_PORT);
    if (! g_mqttClient.connect(CONFIG_MQTT_CLIENT_ID, CONFIG_MQTT_USERNAME, CONFIG_MQTT_PASSWORD))
    {
        printf("Error connecting to MQTT server\n");
        return false;
    }

    printf("Publishing data\n");
    unsigned nSent = 0;

    //if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/status", "online"))
    //    ++nSent;
    //ESP.wdtFeed();

    char msgBuf[32];

    snprintf(msgBuf, sizeof(msgBuf), "%.2f", g_vcc);
    if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/U1", msgBuf))
        ++nSent;
    ESP.wdtFeed();

    unsigned MSGCNT = 1; // expected sent messages count

    if (g_testMode)
    {
        snprintf(msgBuf, sizeof(msgBuf), "%u", g_resetReason);
        if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/RST", msgBuf))
            ++nSent;
        ESP.wdtFeed();

        snprintf(msgBuf, sizeof(msgBuf), "%u", g_adc);
        if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/ADC", msgBuf))
            ++nSent;
        ESP.wdtFeed();

        MSGCNT += 2;
    }

    if (isValid(g_temperature))
    {
        snprintf(msgBuf, sizeof(msgBuf), "%.2f", g_temperature);
        if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/T1", msgBuf))
            ++nSent;
        ESP.wdtFeed();
        MSGCNT += 1;
    }

    if (isValid(g_humidity))
    {
        snprintf(msgBuf, sizeof(msgBuf), "%.0f", g_humidity);
        if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/H1", msgBuf))
            ++nSent;
        ESP.wdtFeed();
        MSGCNT += 1;
    }

    if (isValid(g_pressure))
    {
        snprintf(msgBuf, sizeof(msgBuf), "%.0f", g_pressure);
        if (g_mqttClient.publish(CONFIG_MQTT_TOPIC "/P1", msgBuf))
            ++nSent;
        ESP.wdtFeed();
        MSGCNT += 1;
    }

    g_mqttClient.disconnect();
    ESP.wdtFeed();

    if (nSent != MSGCNT)
    {
        printf("Data sending failed (sent %u/%u)\n", nSent, MSGCNT);
        return false;
    }

    printf("Data sent successfully\n");
    return true;
}

#else // CONFIG_SERVER_USE_MQTT

bool sendTcp()
{
    char msgBuf[256];
    char *p = msgBuf;

    printf("Sending data to TCP server %s:%u\n", CONFIG_TCP_HOST, CONFIG_TCP_PORT);

    WiFiClient sock;
    if (!sock.connect(CONFIG_TCP_HOST, CONFIG_TCP_PORT))
        return false;

    ESP.wdtFeed();

    snprintf(p, msgBuf + sizeof(msgBuf) - p, "#%s\n#U1#%.2f\n", CONFIG_SERVER_DEVICE_ID, g_vcc);
    p += strlen(p);

    if (g_testMode)
    {
        snprintf(p, msgBuf + sizeof(msgBuf) - p, "#RST#%u\n#ADC#%u\n", g_resetReason, g_adc);
        p += strlen(p);
    }

    if (isValid(g_temperature))
    {
        snprintf(p, msgBuf + sizeof(msgBuf) - p, "#T1#%.2f\n", g_temperature);
        p += strlen(p);
    }

    if (isValid(g_humidity))
    {
        snprintf(p, msgBuf + sizeof(msgBuf) - p, "#H1#%.2f\n", g_humidity);
        p += strlen(p);
    }

    if (isValid(g_pressure))
    {
        snprintf(p, msgBuf + sizeof(msgBuf) - p, "#P1#%.0f\n", g_pressure);
        p += strlen(p);
    }

    sprintf(p,"##");
    p += strlen(p);

    const size_t sz = p - msgBuf;
    sock.write((const uint8_t *)msgBuf, sz);

    ESP.wdtFeed();
    printf("Waiting fot server reply\n");

    // wait for server reply
    unsigned long t0 = millis();
    while(sock.available() == 0)
    {
        if ((millis() - t0) > CONFIG_TCP_REPLY_TIMEOUT)
        {
            printf("No reply from server\n");
            return false;
        }
        delay(10);
        ESP.wdtFeed();
    }

    ESP.wdtFeed();
    sock.stop();

    ESP.wdtFeed();
    printf("Data sent successfully\n");
    return true;
}

#endif

///////////////////////////////////////////////////////////////////
// Main

void setup()
{
#ifdef CONFIG_STATUS_LED
    pinMode(CONFIG_STATUS_LED, OUTPUT);
#endif
#ifdef CONFIG_TEST_MODE_PIN
    pinMode(CONFIG_TEST_MODE_PIN, INPUT_PULLUP);
#endif
    Serial.begin(115200);

    printf("\n\nESPSolarMeteo ver.%u.%u\n", CONFIG_VERSION_MAJOR, CONFIG_VERSION_MINOR);
    g_startTime = millis();

    const rst_info *rstInfo = system_get_rst_info();
    g_resetReason = rstInfo->reason;
    printf("Reset reason: %u\n", g_resetReason);

#ifdef CONFIG_TEST_MODE_PIN
    if (digitalRead(CONFIG_TEST_MODE_PIN) == 0)
    {
        printf("Test mode activated by pin %u\n", CONFIG_TEST_MODE_PIN);
        g_testMode = true;
    }
#endif

    ESP.wdtDisable(); // disable SW watchdog to force hardware watchdog reset
    ESP.wdtFeed();

    if (g_resetReason == REASON_WDT_RST)
    {
        uint32_t deepSleepTime = CONFIG_SLEEP_AFTER_WDT_RESET;
#ifdef TEST_DEEPSLEEP
        deepSleepTime = TEST_DEEPSLEEP;
#endif
        printf("Reset caused by hardware WDT\n");
        printf("Going to deep sleep for %us before resuming operations\n", deepSleepTime);
        
        ESP.wdtFeed();
        system_deep_sleep(1000 * 1000 * deepSleepTime);

        return;
    }

    g_adc = analogRead(0);
    g_vcc = adcToVoltage(g_adc);
    printf("Vadc=%u  Vcc=%.2f\n", g_adc, g_vcc);

    LedOn(true);

    if (g_resetReason != REASON_DEEP_SLEEP_AWAKE)
    {
        // Awaken not by deep sleep - check battery level
        if (g_vcc < CONFIG_LOW_BATTERY_START_LEVEL)
        {
            // Low battery level, go to deep sleep before normal operations
            printf("Low-battery startup sleep for %us\n", CONFIG_LOW_BATTERY_START_DELAY);
        
            ESP.wdtFeed();
            system_deep_sleep(1000 * 1000 * CONFIG_LOW_BATTERY_START_DELAY);

            delay(20);
            LedOn(false);
            
            return;
        }
    }

    ESP.wdtFeed();

    g_wifiOk = connectWiFi();

    g_measureOk = doMeasurement(g_temperature, g_humidity, g_pressure);
    if (g_measureOk)
        printf("BME280: Temperature=%.2f C  Humidity=%.1f %%  Pressure=%.0f Pa\n", g_temperature, g_humidity, g_pressure);
    else
        printf("BME280: Measurement failed\n");

    if (g_wifiOk)
    {
#if CONFIG_SERVER_USE_MQTT
        g_sendOk = sendMqtt();
#else
        g_sendOk = sendTcp();
#endif
    }

    LedOn(false);

    // report status using 1,2,3,4 blinks
    // 1 blink: measure done
    // 2 blinks: wifi ok
    // 3 blinks: data sent to test server
    // 4 blinks; data sent to public server
    int nBlink = 0;
    if (g_sendOk)
        nBlink = g_testMode ? 3 : 4;
    else if (g_measureOk)
        nBlink = g_wifiOk ? 2 : 1;

    for (int i = 0; i < nBlink; ++i)
    {
        ESP.wdtFeed();
        delay(150);
        LedOn(true);
        ESP.wdtFeed();
        delay(150);
        LedOn(false);
    }

    ESP.wdtFeed();
    uint32_t deepSleepTime = calcDeepSleepTime();

#ifdef TEST_DEEPSLEEP
    deepSleepTime = TEST_DEEPSLEEP;
#endif

    printf("Going to deep sleep for %u sec\n", deepSleepTime);

    // go to deep sleep
    ESP.wdtFeed();
    system_deep_sleep(1000 * 1000 * deepSleepTime);
}

// loop() does nothing
void loop()
{
  delay(50);
}
