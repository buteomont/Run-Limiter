
#define LED_ON LOW
#define LED_OFF HIGH
#define RELAY_ON HIGH
#define RELAY_OFF LOW
#define RELAY_PORT D1
#define LED_PORT D2
#define WIFI_CONNECTION_ATTEMPTS 15
#define VALID_SETTINGS_FLAG 0xDAB0
#define SSID_SIZE 100
#define PASSWORD_SIZE 50
#define ADDRESS_SIZE 30
#define USERNAME_SIZE 50

#define DEFAULT_MAX_RUNTIME_SECONDS 60*5

#define MQTT_CLIENTID_SIZE 25
#define DEFAULT_MQTT_BROKER_PORT 1883
#define MQTT_MAX_TOPIC_SIZE 50
#define MQTT_MAX_MESSAGE_SIZE 15
#define DEFAULT_MQTT_TOPIC_ROOT "esp8266/runlimiter/"
#define MQTT_CLIENT_ID_ROOT "RunTimeLimiter"
#define MQTT_TOPIC_RSSI "rssi"
#define MQTT_TOPIC_STATUS "status"
#define DEFAULT_MQTT_RUN_MESSAGE "started"
#define DEFAULT_MQTT_TIMEOUT_MESSAGE "timeout"
#define DEFAULT_MQTT_LWT_MESSAGE "stopped"
#define MQTT_TOPIC_COMMAND_REQUEST "command"

//prototypes
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length);
unsigned long myMillis();
bool processCommand(String cmd);
void checkForCommand();
bool connectToWiFi();
void showSettings();
void reconnect(); 
void showSub(char* topic, bool subgood);
void initializeSettings();
void loadSettings();
bool saveSettings();
void serialEvent(); 
void setup(); 
void loop();


