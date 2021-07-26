#include <Arduino.h>
#include <WiFi.h>
#include <MqttClient.h>

#define HW_UART_SPEED 115200L

#define LOG_PRINTFLN(fmt, ...) printfln_P(PSTR(fmt), ##__VA_ARGS__)
#define LOG_SIZE_MAX 128
void printfln_P(const char *fmt, ...)
{
	char buf[LOG_SIZE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf_P(buf, LOG_SIZE_MAX, fmt, ap);
	va_end(ap);
	Serial.println(buf);
}

static MqttClient *mqtt = NULL;
static WiFiClient network;

String CHIP_ID = String((uint32_t)ESP.getEfuseMac(), HEX);
String DEVICE = "ESP32";

// translate CHIP_ID to upper
String StringToUpper(String strToConvert)
{
	std::transform(strToConvert.begin(), strToConvert.end(), strToConvert.begin(), ::toupper);

	return strToConvert;
}

String tmp_topic_pub_status = "client/" + DEVICE + "/" + StringToUpper(CHIP_ID) + "/status";
String tmp_topic_pub_sensor = "v1/" + DEVICE + "/" + StringToUpper(CHIP_ID) + "/sensor/temperature";
String tmp_topic_sub_command = "v1/" + DEVICE + "/" + StringToUpper(CHIP_ID) + "/" + "command";

char *MQTT_TOPIC_PUB_STATUS = &tmp_topic_pub_status[0];
char *MQTT_TOPIC_PUB_SENSOR = &tmp_topic_pub_sensor[0];
char *MQTT_TOPIC_SUB_COMMAND = &tmp_topic_sub_command[0];

// mqtt server connection
const char *MQTT_HOST = "";
const char *MQTT_USERNAME = "";
const char *MQTT_PASSWORD = "";

// connection
const char *WIFI_SSID = "";
const char *WIFI_PASS = "";

// ============== Object to supply system functions ============================
class System : public MqttClient::System
{
public:
	unsigned long millis() const
	{
		return ::millis();
	}

	void yield(void)
	{
		::yield();
	}
};

// connect ot WiFi network
void connectNetwork()
{
	// Setup WiFi network
	WiFi.persistent(false);
	WiFi.mode(WIFI_STA);
	WiFi.hostname(StringToUpper(CHIP_ID));
	WiFi.begin(WIFI_SSID, WIFI_PASS);

	while (WiFi.status() != WL_CONNECTED)
	{
		delay(1000);
		LOG_PRINTFLN("Establishing connection to WiFi..");
	}

	LOG_PRINTFLN("Connected to network");
	LOG_PRINTFLN("MAC: %s", WiFi.macAddress().c_str());
	LOG_PRINTFLN("IP: %s", WiFi.localIP().toString().c_str());
	LOG_PRINTFLN("RSSI: %s", String(WiFi.RSSI()).c_str());
}

// connecting mqtt server
void connectMqtt()
{
	MqttClient::System *mqttSystem = new System;
	MqttClient::Logger *mqttLogger = new MqttClient::LoggerImpl<HardwareSerial>(Serial);
	MqttClient::Network *mqttNetwork = new MqttClient::NetworkClientImpl<WiFiClient>(network, *mqttSystem);
	// 128 bytes send buffer
	MqttClient::Buffer *mqttSendBuffer = new MqttClient::ArrayBuffer<128>();
	// 128 bytes receive buffer
	MqttClient::Buffer *mqttRecvBuffer = new MqttClient::ArrayBuffer<128>();
	// up to 2 subscriptions simultaneously
	MqttClient::MessageHandlers *mqttMessageHandlers = new MqttClient::MessageHandlersImpl<2>();
	// client options
	MqttClient::Options mqttOptions;
	// command timeout to 10 seconds
	mqttOptions.commandTimeoutMs = 10000;
	mqtt = new MqttClient(
		mqttOptions, *mqttLogger, *mqttSystem, *mqttNetwork, *mqttSendBuffer,
		*mqttRecvBuffer, *mqttMessageHandlers
	);

	// start TCP connection
	network.connect(MQTT_HOST, 1883);
	if (!network.connected())
	{
		LOG_PRINTFLN("Can't establish the TCP connection");
		delay(5000);
	}

	// start MQTT connection
	{
		MqttClient::ConnectResult connectResult;
		MQTTPacket_connectData options = MQTTPacket_connectData_initializer;
		options.MQTTVersion = 4;
		options.clientID.cstring = (char *)StringToUpper(CHIP_ID).c_str();
		options.username.cstring = (char *)MQTT_USERNAME;
		options.password.cstring = (char *)MQTT_PASSWORD;
		options.cleansession = true;
		options.keepAliveInterval = 15; // 15 seconds
		// setup LWT
		options.willFlag = true;
		options.will.topicName.cstring = MQTT_TOPIC_PUB_STATUS;
		options.will.message.cstring = (char *)"disconnected";
		options.will.retained = true;
		options.will.qos = MqttClient::QOS1;
		MqttClient::Error::type rc = mqtt->connect(options, connectResult);
		if (rc != MqttClient::Error::SUCCESS)
		{
			LOG_PRINTFLN("Connection error: %i", rc);
			delay(5000);
			ESP.restart();
		}
	}

	// init publish `connected` message
	{
		String msg = "message from: " + DEVICE + "-" + StringToUpper(CHIP_ID).c_str();
    	const char *buf = msg.c_str();
		MqttClient::Message message;
		message.qos = MqttClient::QOS1;
		message.retained = true;
		message.dup = false;
		message.payload = (void *)buf;
		message.payloadLen = strlen(buf) + 1;
		MqttClient::Error::type rc = mqtt->publish(MQTT_TOPIC_PUB_STATUS, message);
		if (rc != MqttClient::Error::SUCCESS)
		{
			LOG_PRINTFLN("Publish error: %i", rc);
			delay(2000);
			ESP.restart();
		}
	}
}

// callback received message from mqtt
void processMessage(MqttClient::MessageData &md)
{
	const MqttClient::Message &msg = md.message;
	char payload[msg.payloadLen + 1];
	memcpy(payload, msg.payload, msg.payloadLen);
	payload[msg.payloadLen] = '\0';
	LOG_PRINTFLN(
		"Message arrived: qos %d, retained %d, dup %d, packetid %d, payload:[%s]",
		msg.qos, msg.retained, msg.dup, msg.id, payload);
}

// ============== Setup all objects ============================================
void setup()
{
	// setup hardware serial for logging
	Serial.begin(HW_UART_SPEED);

	LOG_PRINTFLN("Wait for WiFi...");
	connectNetwork();

	// connecting mqtt server
	connectMqtt();
	LOG_PRINTFLN("Connected to MQTT server");
	LOG_PRINTFLN("Subscribed to: %s", MQTT_TOPIC_SUB_COMMAND);
}

// ============== main loop ====================================================
void loop()
{
	// subscribe
	{
		MqttClient::Error::type rc = mqtt->subscribe(
			MQTT_TOPIC_SUB_COMMAND, MqttClient::QOS1, processMessage);
		if (rc != MqttClient::Error::SUCCESS)
		{
			LOG_PRINTFLN("Subscribe error: %i", rc);
			LOG_PRINTFLN("Drop connection");
			mqtt->disconnect();
			ESP.restart();
		}
	}

	// init publish send message
	{
		String msg = "message from: " + DEVICE + "-" + StringToUpper(CHIP_ID).c_str();

		const char *buf = msg.c_str();
		MqttClient::Message message;
		message.qos = MqttClient::QOS1;
		message.retained = true;
		message.dup = false;
		message.payload = (void *)buf;
		message.payloadLen = strlen(buf) + 1;
		MqttClient::Error::type rc = mqtt->publish(MQTT_TOPIC_PUB_SENSOR, message);
		if (rc != MqttClient::Error::SUCCESS)
		{
			LOG_PRINTFLN("Publish error: %i", rc);
			delay(2000);
			ESP.restart();
		}
		delay(10000);
	}
}