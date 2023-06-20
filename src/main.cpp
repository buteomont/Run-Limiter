/*
 * Program to limit the time something can run, such as a pump or
 * air compressor.
 * By David E. Powell 
 *
 * This device will close an SSR when first powered up, then
 * will open the SSR if it stays powered up longer than a 
 * configurable amount of time.
 * An MQTT message will be sent when the device powers up, and another
 * message will be sent if the device exceeds the timeout.  The MQTT
 * Last Will and Testament message can be used as notification that the 
 * device has shut down normally.
 * 
 * Configuration is done via serial connection or by retained MQTT message.
 *
 * ********** NOTE ***************** 
 * The runtime limiter used on the Buteomont water system uses
 * a slightly different circuit than the compressor uses. The trouble LED is 
 * connected to the same pin as the output transistor, so it does not blink 
 * when the circuit times out. The definition FLASH_LED in runLimiter.h MUST 
 * be set to false when loading code for that controller.
 * ********** NOTE ***************** 
 *  
 * **** to erase the entire flash chip in PlatformIO, open
 * **** a terminal and type "pio run -t erase"
 */ 
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h> 
#include <EEPROM.h>
#include <pgmspace.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "runLimiter.h"

char *stack_start;// initial stack size

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// These are the settings that get stored in EEPROM.  They are all in one struct which
// makes it easier to store and retrieve.
typedef struct 
  {
  unsigned int validConfig=0; 
  char ssid[SSID_SIZE] = "";
  char wifiPassword[PASSWORD_SIZE] = "";
  char brokerAddress[ADDRESS_SIZE]="";
  int brokerPort=DEFAULT_MQTT_BROKER_PORT;
  char mqttUsername[USERNAME_SIZE]="";
  char mqttUserPassword[PASSWORD_SIZE]="";
  char mqttTopicRoot[MQTT_MAX_TOPIC_SIZE]="";
  char mqttRunMessage[MQTT_MAX_MESSAGE_SIZE]="";
  char mqttTimeoutMessage[MQTT_MAX_MESSAGE_SIZE]="";
  char mqttLWTMessage[MQTT_MAX_MESSAGE_SIZE]="";
  unsigned int maxRuntime=DEFAULT_MAX_RUNTIME_SECONDS; //seconds to run before timeout
  boolean debug=false;
  char mqttClientId[MQTT_CLIENTID_SIZE]=""; //will be the same across reboots
  } conf;

conf settings; //all settings in one struct makes it easier to store in EEPROM
boolean settingsAreValid=false;

String commandString = "";     // a String to hold incoming commands from serial
bool commandComplete = false;  // goes true when enter is pressed

unsigned long timeoutCount=0; //milliseconds
boolean timeoutMessageSent=false;

void printStackSize(char id)
  {
  char stack;
  Serial.print(id);
  Serial.print (F(": stack size "));
  Serial.println (stack_start - &stack);
  }

char* fixup(char* rawString, const char* field, const char* value)
  {
  String rs=String(rawString);
  rs.replace(field,String(value));
  strcpy(rawString,rs.c_str());
  printStackSize('F');
  return rawString;
  }

/************************
 * Do the MQTT thing
 ************************/

boolean publish(char* topic, const char* reading, boolean retain)
  {
  Serial.print(topic);
  Serial.print(" ");
  Serial.println(reading);
  return mqttClient.publish(topic,reading,retain); 
  }

/**
 * Handler for incoming MQTT messages.  The payload is the command to perform. 
 * The MQTT message topic sent is the topic root plus the command.
 * Implemented commands are: 
 * MQTT_PAYLOAD_SETTINGS_COMMAND: sends a JSON payload of all user-specified settings
 * MQTT_PAYLOAD_REBOOT_COMMAND: Reboot the controller
 * MQTT_PAYLOAD_VERSION_COMMAND Show the version number
 * MQTT_PAYLOAD_STATUS_COMMAND Show the most recent flow values
 */
void incomingMqttHandler(char* reqTopic, byte* payload, unsigned int length) 
  {
  if (settings.debug)
    {
    Serial.println("====================================> Callback works.");
    }
  payload[length]='\0'; //this should have been done in the caller code, shouldn't have to do it here
  char charbuf[100];
  sprintf(charbuf,"%s",payload);
  const char* response;
  char settingsResp[400];

  if (strcmp(charbuf,"settings")==0) //special case, send all settings
    {
    strcpy(settingsResp,"\nssid=");
    strcat(settingsResp,settings.ssid);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"wifipass=");
    strcat(settingsResp,settings.wifiPassword);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"broker=");
    strcat(settingsResp,settings.brokerAddress);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"brokerPort=");
    strcat(settingsResp,String(settings.brokerPort).c_str());
    strcat(settingsResp,"\n");
    strcat(settingsResp,"userName=");
    strcat(settingsResp,settings.mqttUsername);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"userPass=");
    strcat(settingsResp,settings.mqttUserPassword);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"topicRoot=");
    strcat(settingsResp,settings.mqttTopicRoot);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"runMessage=");
    strcat(settingsResp,settings.mqttRunMessage);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"lwtMessage=");
    strcat(settingsResp,settings.mqttLWTMessage);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"timeoutMessage=");
    strcat(settingsResp,settings.mqttTimeoutMessage);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"maxRuntime=");
    strcat(settingsResp,String(settings.maxRuntime).c_str());
    strcat(settingsResp,"\n");
    strcat(settingsResp,"debug=");
    strcat(settingsResp,settings.debug?"true":"false");
    strcat(settingsResp,"\n");
    strcat(settingsResp,"MQTT client ID=");
    strcat(settingsResp,settings.mqttClientId);
    strcat(settingsResp,"\n");
    strcat(settingsResp,"IP Address=");
    strcat(settingsResp,WiFi.localIP().toString().c_str());
    response=settingsResp;
    }
  else if (processCommand(charbuf))
    {
    response="OK";
    }
  else
    {
    char badCmd[18];
    strcpy(badCmd,"(empty)");
    response=badCmd;
    }

  //prepare the response topic
  char topic[MQTT_MAX_TOPIC_SIZE];
  strcpy(topic,settings.mqttTopicRoot);
  strcat(topic,charbuf); //the incoming command becomes the topic suffix

  if (!publish(topic,response,false)) //do not retain
    Serial.println("************ Failure when publishing status response!");
  }

boolean sendMessage(char* topic, char* value)
  { 
  boolean success=false;
  if (!mqttClient.connected())
    {
    Serial.println("Not connected to MQTT broker!");
    }
  else
    {
    char topicBuf[MQTT_MAX_TOPIC_SIZE+MQTT_MAX_MESSAGE_SIZE];
    char reading[18];
//    boolean success=false; //only for the incoming topic and value

    //publish the radio strength reading while we're at it
    strcpy(topicBuf,settings.mqttTopicRoot);
    strcat(topicBuf,MQTT_TOPIC_RSSI);
    sprintf(reading,"%d",WiFi.RSSI()); 
    success=publish(topicBuf,reading,true); //retain
    if (!success)
      Serial.println("************ Failed publishing rssi!");
    
    //publish the message
    strcpy(topicBuf,settings.mqttTopicRoot);
    strcat(topicBuf,topic);
    success=publish(topicBuf,value,true); //retain
    if (!success)
      Serial.println("************ Failed publishing "+String(topic)+"! ("+String(success)+")");
    }
  return success;
  }

void otaSetup()
  {
  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  // ArduinoOTA.setHostname("myesp32");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() 
    {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
    });

    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  }

void setup() 
  {
  //init record of stack
  char stack;
  stack_start = &stack;  
  
  pinMode(LED_BUILTIN,OUTPUT);// The blue light on the board shows WiFi activity
  digitalWrite(LED_BUILTIN,LED_OFF);
  pinMode(RELAY_PORT,OUTPUT); // The port for the SSRs
  digitalWrite(RELAY_PORT,RELAY_ON); //turn on the device
  pinMode(LED_PORT,OUTPUT); // The port for the warning LED
  digitalWrite(LED_PORT,LED_OFF); //turn off the LED until we time out

  Serial.begin(115200);
  Serial.setTimeout(10000);
  Serial.println();
  
  while (!Serial); // wait here for serial port to connect.
  Serial.println(F("Running."));

  //reset the wifi
//  WiFi.disconnect(true); 
//  WiFi.softAPdisconnect(true);
//  ESP.eraseConfig();

  EEPROM.begin(sizeof(settings)); //fire up the eeprom section of flash
  commandString.reserve(200); // reserve 200 bytes of serial buffer space for incoming command string

  if (settings.debug)
    Serial.println(F("Loading settings"));
  loadSettings(); //set the values from eeprom

  Serial.print("Performing settings sanity check...");
  if ((settings.validConfig!=0 && 
      settings.validConfig!=VALID_SETTINGS_FLAG) || //should always be one or the other
      settings.brokerPort<0 ||
      settings.brokerPort>65535)
    {
    Serial.println("\nSettings in eeprom failed sanity check, initializing.");
    initializeSettings(); //must be a new board or flash was erased
    }
  else
    Serial.println("passed.");

  timeoutCount=settings.maxRuntime*1000; //milliseconds until timeout occurs

  if (settings.validConfig==VALID_SETTINGS_FLAG)
    connectToWiFi(); //connect to the wifi
  
  if (WiFi.status() == WL_CONNECTED)
    {
    sendMessage(MQTT_TOPIC_STATUS, settings.mqttRunMessage); //running!
    otaSetup(); //initialize the OTA stuff
    }
  }

void loop()
  {
  static boolean wrapped=false; //for countdown timer
  static unsigned long lastTime=0;
  mqttClient.loop(); //This has to happen every so often or we get disconnected for some reason
  checkForCommand(); // Check for input in case something needs to be changed to work
  ArduinoOTA.handle(); //Check for new version

  if (settings.debug 
      && millis()%5000==0 
      && !wrapped 
      && settingsAreValid
      && !timeoutMessageSent) 
    {
    unsigned long remainingTime=timeoutCount-millis();
    if (remainingTime>4294900000)
      wrapped=true; //that's about as close as we can get
    if (remainingTime != lastTime && !wrapped)
      {
      Serial.print(remainingTime);
      Serial.println(" ms remaining");
      lastTime=remainingTime;
      }
    }

  if (millis()>=timeoutCount && !timeoutMessageSent)
    {
    digitalWrite(RELAY_PORT,RELAY_OFF); //turn off the device
    digitalWrite(LED_PORT,LED_ON); //turn on the failure LED
    if (settingsAreValid)
      {
      connectToWiFi(); //make sure we're connected to the broker
      timeoutMessageSent=sendMessage(MQTT_TOPIC_STATUS, settings.mqttTimeoutMessage);
      }
    }

  if (FLASH_LED)
    {
    static unsigned long nextFlashTime=millis()+250;
    if (millis()>=timeoutCount && millis()>nextFlashTime && timeoutMessageSent) //flash the led
      {
      static boolean warning_led_state=LED_ON;
      digitalWrite(LED_PORT,warning_led_state);
      warning_led_state=!warning_led_state;
      nextFlashTime=millis()+250; //half second flash rate
      }
    }
  }


/*
 * If not connected to wifi, connect.
 */
boolean connectToWiFi()
  {
  yield();
  static boolean retval=true; //assume connection to wifi is ok
  if (settingsAreValid && WiFi.status() != WL_CONNECTED)
    {
    if (settings.debug)
      {
      Serial.print(F("Attempting to connect to WPA SSID \""));
      Serial.print(settings.ssid);
      Serial.print("\" with passphrase \"");
      Serial.print(settings.wifiPassword);
      Serial.println("\"");
      }

    WiFi.mode(WIFI_STA); //station mode, we are only a client in the wifi world
    WiFi.begin(settings.ssid, settings.wifiPassword);

    //try for a few seconds to connect to wifi
    for (int i=0;i<WIFI_CONNECTION_ATTEMPTS;i++)  
      {
      if (WiFi.status() == WL_CONNECTED)
        {
        digitalWrite(LED_BUILTIN,LED_ON); //show we're connected
        break;  // got it
        }
      if (settings.debug)
        Serial.print(".");
      checkForCommand(); // Check for input in case something needs to be changed to work
      ESP.wdtFeed(); //feed the watchdog timers.
      delay(500);
      }

    if (WiFi.status() == WL_CONNECTED)
      {
      digitalWrite(LED_BUILTIN,LED_ON); //show we're connected
      if (settings.debug)
        {
        Serial.println(F("Connected to network."));
        Serial.println();
        }
      //show the IP address
      Serial.println(WiFi.localIP());
      retval=true;
      }     
    else //can't connect to wifi, try again next time
      {
      retval=false;
      Serial.print("Wifi status is ");
      Serial.println(WiFi.status());
      Serial.println(F("WiFi connection unsuccessful."));
      digitalWrite(LED_BUILTIN,LED_OFF); //stay off until we connect
      }
    }
  if (WiFi.status() == WL_CONNECTED)
    {
    reconnect(); // go ahead and connect to the MQTT broker
    }
  return retval;
  }

void showSub(char* topic, bool subgood)
  {
  if (settings.debug)
    {
    Serial.print("++++++Subscribing to ");
    Serial.print(topic);
    Serial.print(":");
    Serial.println(subgood);
    }
  }


/*
 * Reconnect to the MQTT broker
 */
void reconnect() 
  {
  // Loop until we're reconnected
  if (!mqttClient.connected()) 
    {      
    Serial.print("Attempting MQTT connection...");

    mqttClient.setBufferSize(500); //default (256) isn't big enough
    mqttClient.setServer(settings.brokerAddress, settings.brokerPort);
    mqttClient.setCallback(incomingMqttHandler);
    
    // Attempt to connect
    char willTopic[MQTT_MAX_TOPIC_SIZE]="";
    strcpy(willTopic,settings.mqttTopicRoot);
    strcat(willTopic,MQTT_TOPIC_STATUS);


    if (mqttClient.connect(settings.mqttClientId,
                          settings.mqttUsername,
                          settings.mqttUserPassword,
                          willTopic,
                          0,                  //QOS
                          true,               //retain
                          settings.mqttLWTMessage))
      {
      Serial.println("connected to MQTT broker.");

      //resubscribe to the incoming message topic
      char topic[MQTT_MAX_TOPIC_SIZE];
      strcpy(topic,settings.mqttTopicRoot);
      strcat(topic,MQTT_TOPIC_COMMAND_REQUEST);
      bool subgood=mqttClient.subscribe(topic);
      showSub(topic,subgood);
      }
    else 
      {
      Serial.print("failed, rc=");
      Serial.println(mqttClient.state());
      Serial.println("Will try again in a second");
      
      // Wait a second before retrying
      // In the meantime check for input in case something needs to be changed to make it work
      checkForCommand(); 
      
      delay(1000);
      }
    }
  mqttClient.loop(); //This has to happen every so often or we get disconnected for some reason
  }

//Generate an MQTT client ID.  This should not be necessary very often
char* generateMqttClientId(char* mqttId)
  {
  strcpy(mqttId,strcat(MQTT_CLIENT_ID_ROOT,String(random(0xffff), HEX).c_str()));
  if (settings.debug)
    {
    Serial.print("New MQTT userid is ");
    Serial.println(mqttId);
    }
  return mqttId;
  }

void showSettings()
  {
  Serial.print("ssid=<wifi ssid> (");
  Serial.print(settings.ssid);
  Serial.println(")");
  Serial.print("wifipass=<wifi password> (");
  Serial.print(settings.wifiPassword);
  Serial.println(")");
  Serial.print("broker=<address of MQTT broker> (");
  Serial.print(settings.brokerAddress);
  Serial.println(")");
  Serial.print("brokerPort=<port number MQTT broker> (");
  Serial.print(settings.brokerPort);
  Serial.println(")");
  Serial.print("userName=<user ID for MQTT broker> (");
  Serial.print(settings.mqttUsername);
  Serial.println(")");
  Serial.print("userPass=<user password for MQTT broker> (");
  Serial.print(settings.mqttUserPassword);
  Serial.println(")");
  Serial.print("topicRoot=<MQTT topic base to which status or other topics will be added> (");
  Serial.print(settings.mqttTopicRoot);
  Serial.println(")");
  Serial.print("runMessage=<status message to send when power is applied> (");
  Serial.print(settings.mqttRunMessage);
  Serial.println(")");
  Serial.print("lwtMessage=<status message to send when power is removed> (");
  Serial.print(settings.mqttLWTMessage);
  Serial.println(")");
  Serial.print("timeoutMessage=<status message to send when runtime is exceeded> (");
  Serial.print(settings.mqttTimeoutMessage);
  Serial.println(")");
  Serial.print("maxRuntime=<maximum allowable seconds to run> (");
  Serial.print(settings.maxRuntime);
  Serial.println(")");
  Serial.print("debug=<print debug messages to serial port> (");
  Serial.print(settings.debug?"true":"false");
  Serial.println(")");
  Serial.print("MQTT client ID=<automatically generated client ID> (");
  Serial.print(settings.mqttClientId);
  Serial.println(") **Use \"resetmqttid=yes\" to regenerate");
  Serial.println("\n*** Use \"factorydefaults=yes\" to reset all settings ***");
  Serial.print("\nIP Address=");
  Serial.println(WiFi.localIP());
  }

/*
 * Check for configuration input via the serial port.  Return a null string 
 * if no input is available or return the complete line otherwise.
 */
String getConfigCommand()
  {
  if (commandComplete) 
    {
    String newCommand=commandString;
    if (settings.debug)
      {
      Serial.print("Command is -->");
      Serial.print(newCommand);
      Serial.println("<--");
      }
    commandString = "";
    commandComplete = false;
    return newCommand;
    }
  else return "";
  }

bool processCommand(String cmd)
  {
  const char *str=cmd.c_str();
  char *val=NULL;
  char *nme=strtok((char *)str,"=");
  if (nme!=NULL)
    val=strtok(NULL,"=");

  char zero[]=""; //zero length string

  //Get rid of the carriage return and/or linefeed. Twice because could have both.
  if (val!=NULL && strlen(val)>0 && (val[strlen(val)-1]==13 || val[strlen(val)-1]==10))
    val[strlen(val)-1]=0; 
  if (val!=NULL && strlen(val)>0 && (val[strlen(val)-1]==13 || val[strlen(val)-1]==10))
    val[strlen(val)-1]=0; 

  //do it for the command as well.  Might not even have a value.
  if (nme!=NULL && strlen(nme)>0 && (nme[strlen(nme)-1]==13 || nme[strlen(nme)-1]==10))
    nme[strlen(nme)-1]=0; 
  if (nme!=NULL && strlen(nme)>0 && (nme[strlen(nme)-1]==13 || nme[strlen(nme)-1]==10))
    nme[strlen(nme)-1]=0; 

  if (settings.debug)
    {
    Serial.print("Processing command \"");
    Serial.print(nme);
    Serial.println("\"");
    Serial.print("Length:");
    Serial.println(strlen(nme));
    Serial.print("Hex:");
    Serial.println(nme[0],HEX);
    Serial.print("Value is \"");
    Serial.print(val);
    Serial.println("\"\n");
    }

  if (val==NULL)
    val=zero;

  if (nme==NULL || val==NULL || strlen(nme)==0) //empty string is a valid val value
    {
    showSettings();
    return false;   //not a valid command, or it's missing
    }
  else if (strcmp(nme,"ssid")==0)
    {
    strcpy(settings.ssid,val);
    saveSettings();
    }
  else if (strcmp(nme,"wifipass")==0)
    {
    strcpy(settings.wifiPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"broker")==0)
    {
    strcpy(settings.brokerAddress,val);
    saveSettings();
    }
  else if (strcmp(nme,"brokerPort")==0)
    {
    settings.brokerPort=atoi(val);
    saveSettings();
    }
  else if (strcmp(nme,"userName")==0)
    {
    strcpy(settings.mqttUsername,val);
    saveSettings();
    }
  else if (strcmp(nme,"userPass")==0)
    {
    strcpy(settings.mqttUserPassword,val);
    saveSettings();
    }
  else if (strcmp(nme,"lwtMessage")==0)
    {
    strcpy(settings.mqttLWTMessage,val);
    saveSettings();
    }
  else if (strcmp(nme,"runMessage")==0)
    {
    strcpy(settings.mqttRunMessage,val);
    saveSettings();
    }
  else if (strcmp(nme,"timeoutMessage")==0)
    {
    strcpy(settings.mqttTimeoutMessage,val);
    saveSettings();
    }
  else if (strcmp(nme,"topicRoot")==0)
    {
    strcpy(settings.mqttTopicRoot,val);
    saveSettings();
    }
  else if (strcmp(nme,"maxRuntime")==0)
    {
    settings.maxRuntime=atoi(val);
    saveSettings();
    }
  else if ((strcmp(nme,"resetmqttid")==0)&& (strcmp(val,"yes")==0))
    {
    generateMqttClientId(settings.mqttClientId);
    saveSettings();
    }
  else if (strcmp(nme,"debug")==0)
    {
    settings.debug=strcmp(val,"false")==0?false:true;
    saveSettings();
    }
  else if ((strcmp(nme,"factorydefaults")==0) && (strcmp(val,"yes")==0)) //reset all eeprom settings
    {
    Serial.println("\n*********************** Resetting EEPROM Values ************************");
    initializeSettings();
    saveSettings();
    delay(2000);
    ESP.restart();
    }
  else if ((strcmp(nme,"reset")==0) && (strcmp(val,"yes")==0)) //reset the device
    {
    Serial.println("\n*********************** Resetting Device ************************");
    delay(1000);
    ESP.restart();
    }
  else
    {
    showSettings();
    return false; //command not found
    }
  return true;
  }

void initializeSettings()
  {
  settings.validConfig=0; 
  strcpy(settings.ssid,"");
  strcpy(settings.wifiPassword,"");
  settings.maxRuntime=DEFAULT_MAX_RUNTIME_SECONDS;
  strcpy(settings.brokerAddress,"");
  settings.brokerPort=DEFAULT_MQTT_BROKER_PORT;
  strcpy(settings.mqttLWTMessage,DEFAULT_MQTT_LWT_MESSAGE);
  strcpy(settings.mqttRunMessage,DEFAULT_MQTT_RUN_MESSAGE);
  strcpy(settings.mqttTimeoutMessage,DEFAULT_MQTT_TIMEOUT_MESSAGE);
  strcpy(settings.mqttTopicRoot,DEFAULT_MQTT_TOPIC_ROOT);
  strcpy(settings.mqttUsername,"");
  strcpy(settings.mqttUserPassword,"");
  generateMqttClientId(settings.mqttClientId);
  settings.debug=false;
  saveSettings();
  }

void checkForCommand()
  {
  if (Serial.available())
    {
    incomingData();
    String cmd=getConfigCommand();
    if (cmd.length()>0)
      {
      processCommand(cmd);
      }
    }
  }
  
/*
*  Initialize the settings from eeprom and determine if they are valid
*/
void loadSettings()
  {
  EEPROM.get(0,settings);
  if (settings.validConfig==VALID_SETTINGS_FLAG)    //skip loading stuff if it's never been written
    {
    settingsAreValid=true;
    if (settings.debug)
      Serial.println("Loaded configuration values from EEPROM");
    }
  else
    {
    Serial.println("Skipping load from EEPROM, device not configured.");    
    settingsAreValid=false;
    }
  }

/*
 * Save the settings to EEPROM. Set the valid flag if everything is filled in.
 */
boolean saveSettings()
  {
  if (strlen(settings.ssid)>0 &&
    strlen(settings.ssid)<=SSID_SIZE &&
    strlen(settings.wifiPassword)>0 &&
    strlen(settings.wifiPassword)<=PASSWORD_SIZE &&
    strlen(settings.brokerAddress)>0 &&
    strlen(settings.brokerAddress)<ADDRESS_SIZE &&
    strlen(settings.mqttLWTMessage)>0 &&
    strlen(settings.mqttLWTMessage)<MQTT_MAX_MESSAGE_SIZE &&
    strlen(settings.mqttRunMessage)>0 &&
    strlen(settings.mqttRunMessage)<MQTT_MAX_MESSAGE_SIZE &&
    strlen(settings.mqttTimeoutMessage)>0 &&
    strlen(settings.mqttTimeoutMessage)<MQTT_MAX_MESSAGE_SIZE &&
    strlen(settings.mqttTopicRoot)>0 &&
    strlen(settings.mqttTopicRoot)<MQTT_MAX_TOPIC_SIZE &&
    settings.brokerPort>0 &&
    settings.brokerPort<65535 &&
    settings.maxRuntime>0)
    {
    Serial.println("Settings deemed complete.");
    settings.validConfig=VALID_SETTINGS_FLAG;
    settingsAreValid=true;
    }
  else
    {
    Serial.println("Settings still incomplete");
    settings.validConfig=0;
    settingsAreValid=false;
    }

  //The mqttClientId is not set by the user, but we need to make sure it's set  
  if (strlen(settings.mqttClientId)==0)
    {
    generateMqttClientId(settings.mqttClientId);
    }

  EEPROM.put(0,settings);
  return EEPROM.commit();
  }

/*
  SerialEvent occurs whenever a new data comes in the hardware serial RX. This
  routine is run between each time loop() runs, so using delay inside loop can
  delay response. Multiple bytes of data may be available.
*/
void incomingData() 
  {
  while (Serial.available()) 
    {
    // get the new byte
    char inChar = (char)Serial.read();
    Serial.print(inChar);

    // if the incoming character is a newline, set a flag so the main loop can
    // do something about it 
    if (inChar == '\n') 
      {
      commandComplete = true;
      if (settings.debug)
        Serial.println(F("Command is complete."));
      }
    else
      {
      // add it to the inputString 
      commandString += inChar;
      }
    }
  }

