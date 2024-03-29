 /*
  HoloLab Holographic lab controller
  (c) Loic74 <loic74650@gmail.com> 2018-2020

  TODO:
  add PAUSE function in countdown process (add stop time in shutters struct?

***Dependencies and respective revisions used to compile this project***
  https://github.com/256dpi/arduino-mqtt/releases (rev 2.4.3)
  https://github.com/PaulStoffregen/OneWire (rev 2.3.4)
  https://github.com/milesburton/Arduino-Temperature-Control-Library (rev 3.7.2)
  https://github.com/RobTillaart/Arduino/tree/master/libraries/RunningMedian (rev 0.1.15)
  https://github.com/prampec/arduino-softtimer (rev 3.1.3)
  https://github.com/bricofoy/yasm (rev 0.9.2)
  https://github.com/bblanchon/ArduinoJson (rev 5.13.4)
  https://github.com/thijse/Arduino-EEPROMEx (rev 1.0.0)
  https://github.com/sdesalas/Arduino-Queue.h (rev )
  https://github.com/JChristensen/JC_Button (rev 2.1.1)

  Pins usage:
  relays: 2,7,8,3
  Buttons: 28,31,32
  OneWire bus: 6
  Ethernet shield: 50,51,52,10,4,53
  Relay shield: 4,7,8,12

  //Shutters command
  //val: 0 means off, 1 means on, 2 means ignore; //t: 0 means no period, otherwise if val=1, means it will shut back off after period in seconds
  //{"cd":"Shut","val":[1,1,2],"t":[60,30,0],"d":[0,60,0]}; //in this example: shutter 3 ignored, close shutter 1 for 6 secs, close shutter 2 for 3 secs after 6 secs wait

*/
#include <SPI.h>
#include <Ethernet.h>
#include <MQTT.h>
#include <SoftTimer.h>
#include <Streaming.h>
#include <avr/wdt.h>
#include <stdlib.h>
#include <ArduinoJson.h>
#include <Queue.h>
#include <JC_Button.h>
#include "OneWire.h"
#include <DallasTemperature.h>
#include <RunningMedian.h>
#include <EEPROMex.h>
#include <yasm.h>

// Firmware revision
String Firmw = "0.0.2";

//Version of config stored in Eeprom
//Random value. Change this value (to any other value) to revert the config to default values
#define CONFIG_VERSION 116

//Starting point address where to store the config data in EEPROM
#define memoryBase 32
int configAdress = 0;
const int maxAllowedWrites = 200;//not sure what this is for

//EEPROM settings structure and its default values. Pretty much empty and useless for now...
struct StoreStruct
{
  uint8_t ConfigVersion;   // This is for testing if first time using eeprom or not
} storage =
{ //default values. Change the value of CONFIG_VERSION in order to restore the default values
  CONFIG_VERSION
};

#define R1 2 //Relay 1 = Arduino pin 2
#define R2 7 //Relay 2 = Arduino pin 7
#define R3 8 //Relay 3 = Arduino pin 8
#define R4 3 //Relay 4 = Arduino pin 3 

#define PUSH0 32 //push button to manually toggle shutter0
#define PUSH1 31 //push button to manually toggle shutter1
#define PUSH2 28 //push button to manually toggle shutter2

const byte PushButton0Pin(PUSH0), PushButton1Pin(PUSH1), PushButton2Pin(PUSH2);

Button PushButton0(PushButton0Pin, 200, true, true);// define the button
Button PushButton1(PushButton1Pin, 200, true, true);// define the button
Button PushButton2(PushButton2Pin, 200, true, true);// define the button

//Queue object to store incoming JSON commands (up to 10)
Queue<String> queue = Queue<String>(10);

struct LaserShutters
{
  bool IsOpen[3];
  bool Done[3];
  unsigned long CountDown[3];
  unsigned long Start[3];
  unsigned long Delay[3];
} Shutters = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

//buffers for MQTT string payload
#define PayloadBufferLength 128
char Payload[PayloadBufferLength];

//MQTT publishing periodicity of system info, in msecs
unsigned long PublishPeriod = 30000;

//One wire bus for the water-cooled breadboard and air temperature measurements
//Data wire is connected to input digital pin 6 on the Arduino
#define ONE_WIRE_BUS_A 6
                                                 
// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire_A(ONE_WIRE_BUS_A);

// Pass our oneWire reference to Dallas Temperature library instance
DallasTemperature sensors_A(&oneWire_A);

//12bits (0,06°C) temperature sensor resolution
#define TEMPERATURE_RESOLUTION 12

//The measured temperature variables
double TempW, TempA;

//Signal filtering library. Only used in this case to compute the averages of the temperatures
//over multiple measurements but offers other filtering functions such as median, etc.
RunningMedian samples_Temp = RunningMedian(10);
RunningMedian samples2_Temp = RunningMedian(10);

//MAC Addresses of DS18b20 water and air temperature sensors
DeviceAddress DS18b20_0 = { 0x28, 0xFF, 0x52, 0x24, 0x71, 0x17, 0x03, 0x04 };// -> change to your unique sensor address!;
DeviceAddress DS18b20_1 = { 0x28, 0xFF, 0x01, 0xC1, 0xA2, 0x17, 0x04, 0x69 };// -> change to your unique sensor address!;
String sDS18b20_0, sDS18b20_1;

// MAC address of Ethernet shield (in case of Controllino board, set an arbitrary MAC address)
byte mac[] = { 0x90, 0xA2, 0xDA, 0x11, 0x2E, 0x15 };
//byte mac[] = { 0xA8, 0x61, 0x0A, 0xAE, 0x2F, 0x92 };
String sArduinoMac;
IPAddress ip(192, 168, 0, 105);  //IP address, needs to be adapted depending on local network topology
EthernetClient net;             //Ethernet client to connect to MQTT server

//MQTT stuff including local broker/server IP address, login and pwd
MQTTClient MQTTClient;
const char* MqttServerIP = "192.168.0.38";
const char* MqttServerClientID = "ArduinoHoloLab"; // /!\ choose a client ID which is unique to this Arduino board
const char* MqttServerLogin = nullptr;
const char* MqttServerPwd = nullptr;
const char* HoloLabTempsTopic = "HoloLab/Shutters/Temperatures";
const char* HoloLabAPI = "HoloLab/Shutters/API";
const char* HoloLabAPIEcho = "HoloLab/Shutters/API/Echo";
const char* HoloLabStatus = "HoloLab/Shutters/status";

//serial printing stuff
String _endl = "\n";

//State Machine
//Getting a 12 bits temperature reading on a DS18b20 sensor takes >750ms
//Here we use the sensor in asynchronous mode, request a temp reading and use
//the nice "YASM" state-machine library to do other things while it is being obtained
YASM gettemp;

//Callbacks
//Here we use the SoftTimer library which handles multiple timers (Tasks)
//It is more elegant and readable than a single loop() functtion, especially
//when tasks with various frequencies are to be used
void GenericCallback(Task* me);
void PublishDataCallback(Task* me);
void PublishStateCallback(Task* me);

Task t1(10, GenericCallback);                //Various things handled/updated in this loop every 0.6 secs
Task t2(30000, PublishDataCallback);          //Publish data to MQTT broker every 30 secs
Task t3(1000, PublishStateCallback);          //Publish state of shutters to MQTT broker every sec

void setup()
{
  //Serial port for debug info
  Serial.begin(9600);
  delay(200);

  //Initialize Eeprom
  EEPROM.setMemPool(memoryBase, EEPROMSizeMega);

  //Get address of "ConfigVersion" setting
  configAdress = EEPROM.getAddress(sizeof(StoreStruct));

  //Read ConfigVersion. If does not match expected value, restore default values
  uint8_t vers = EEPROM.readByte(configAdress);

  if (vers == CONFIG_VERSION)
  {
    Serial << F("Stored config version: ") << CONFIG_VERSION << F(". Loading settings from eeprom") << _endl;
    loadConfig();//Restore stored values from eeprom
  }
  else
  {
    Serial << F("Stored config version: ") << CONFIG_VERSION << F(". Loading default settings, not from eeprom") << _endl;
    saveConfig();//First time use. Save default values to eeprom
  }

  // initialize pin directions.
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(R1, OUTPUT);
  pinMode(R2, OUTPUT);
  pinMode(R3, OUTPUT);
  pinMode(R4, OUTPUT);

  pinMode(PUSH0, INPUT_PULLUP);
  pinMode(PUSH1, INPUT_PULLUP);
  pinMode(PUSH2, INPUT_PULLUP);

  pinMode(ONE_WIRE_BUS_A, INPUT);

  //8 seconds watchdog timer to reset system in case it freezes for more than 8 seconds
  wdt_enable(WDTO_8S);

  // initialize Ethernet device
  Ethernet.begin(mac, ip);

  //Start temperature measurement state machine
  gettemp.next(gettemp_start);

  //Init MQTT
  MQTTClient.setOptions(60, false, 10000);
  MQTTClient.setWill(HoloLabStatus, "offline", true, LWMQTT_QOS1);
  MQTTClient.begin(MqttServerIP, net);
  MQTTClient.onMessage(messageReceived);
  MQTTConnect();

  //Initialize the front panel push-buttons objects
  PushButton0.begin();
  PushButton1.begin();
  PushButton2.begin();

  //Generic loop
  SoftTimer.add(&t1);
  t1.init();

  //Publish loop
  SoftTimer.add(&t2);
  t2.init();

  //PublishState loop
//  SoftTimer.add(&t3);
//  t3.init();

  //display remaining RAM space. For debug
  Serial << F("[memCheck]: ") << freeRam() << F("b") << _endl;

  //Init shutters
  for (int i = 0; i < 3; i++)
    Shutters.Done[i] = true;
}

//PublishData loop. Publishes measured temperatures to MQTT broker every XX secs (30 secs by default)
void PublishDataCallback(Task* me)
{
  if (!MQTTClient.connected())
  {
    MQTTConnect();
    //Serial.println("MQTT reconnecting...");
  }

  if (MQTTClient.connected())
  {
    //send a JSON to MQTT broker. /!\ Split JSON if longer than 128 bytes
    //Will publish something like {"TmpW":2130,"TmpA":2018} //Temp Water and Temp Air x100
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();

    root.set<int>("TmpW", (int)(TempW * 100));
    root.set<int>("TmpA", (int)(TempA * 100));

    //char Payload[PayloadBufferLength];
    if (jsonBuffer.size() < PayloadBufferLength)
    {
      root.printTo(Payload, PayloadBufferLength);
      if (MQTTClient.publish(HoloLabTempsTopic, Payload, strlen(Payload), false, LWMQTT_QOS1))
      {
        Serial << F("Payload: ") << Payload << F(" - ");
        Serial << F("Payload size: ") << jsonBuffer.size() << _endl;
      }
      else
      {
        Serial << F("Unable to publish the following payload: ") << Payload << _endl;
        Serial << F("MQTTClient.lastError() returned: ") << MQTTClient.lastError() << F(" - MQTTClient.returnCode() returned: ") << MQTTClient.returnCode() << _endl;
      }
    }
    else
    {
      Serial << F("MQTT Payload buffer overflow! - ");
      Serial << F("Payload size: ") << jsonBuffer.size() << _endl;
    }
  }
  else
    Serial << F("Failed to connect to the MQTT broker") << _endl;

}

//Update temperature values
void getMeasures(DeviceAddress deviceAddress_0, DeviceAddress deviceAddress_1)
{
  //Water Temperature
  samples_Temp.add(sensors_A.getTempC(deviceAddress_0));
  TempW = samples_Temp.getAverage(10);
  if (TempW == -127.00) {
    Serial << F("Error getting temperature from DS18b20_0") << _endl;
  } else {
    Serial << F("DS18b20_0: ") << TempW << F("°C") << F(" - ");
  }

  //Air Temperature
  samples2_Temp.add(sensors_A.getTempC(deviceAddress_1));
  TempA = samples2_Temp.getAverage(10);
  if (TempA == -127.00) {
    Serial << F("Error getting temperature from DS18b20_1") << _endl;
  } else {
    Serial << F("DS18b20_1: ") << TempA << F("°C") << _endl;
  }
}

bool loadConfig()
{
  EEPROM.readBlock(configAdress, storage);

  Serial << storage.ConfigVersion << '\n';
  return (storage.ConfigVersion == CONFIG_VERSION);
}

void saveConfig()
{
  //update function only writes to eeprom if the value is actually different. Increases the eeprom lifetime
  EEPROM.writeBlock(configAdress, storage);
}

//Connect to MQTT broker and subscribe to the HoloLabAPI topic in order to receive future commands
//then publish the "online" message on the "status" topic. If Ethernet connection is ever lost
//"status" will switch to "offline". Very useful to check that the Arduino is alive and functional
void MQTTConnect()
{
  MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd);
  int8_t Count = 0;
  while (!MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd) && (Count < 4))
  {
    Serial << F(".") << _endl;
    delay(500);
    Count++;
  }

  if (MQTTClient.connected())
  {
    //Topic to which send/publish API commands for the Shutters control
    MQTTClient.subscribe(HoloLabAPI);

    //tell status topic we are online
    if (MQTTClient.publish(HoloLabStatus, "online", true, LWMQTT_QOS1))
      Serial << F("published: HoloLab/status - online") << _endl;
    else
    {
      Serial << F("Unable to publish on status topic; MQTTClient.lastError() returned: ") << MQTTClient.lastError() << F(" - MQTTClient.returnCode() returned: ") << MQTTClient.returnCode() << _endl;
    }
  }
  else
    Serial << F("Failed to connect to the MQTT broker") << _endl;

}

//MQTT callback
//This function is called when messages are published on the MQTT broker on the HoloLabAPI topic to which we subscribed
//Add the received command to a message queue for later processing and exit the callback
void messageReceived(String &topic, String &payload)
{
  String TmpStrPool(HoloLabAPI);

  //HoloLab commands. This check might be redundant since we only subscribed to this topic
  if (topic == TmpStrPool)
  {
    queue.push(payload);
    Serial << F("Received command on API topic: ") << payload << _endl;
    Serial << "FreeRam: " << freeRam() << " - Qeued messages: " << queue.count() << _endl;
  }
}


//Loop where various tasks are updated/handled
void GenericCallback(Task* me)
{
  //clear watchdog timer
  wdt_reset();
  //Serial<<F("Watchdog Reset")<<_endl;

  //Update MQTT thread
  MQTTClient.loop();

  //request temp reading
  gettemp.run();

  //Process queued incoming JSON commands if any
  if (queue.count() > 0)
    ProcessCommand(queue.pop());

  //Shutters count donwns
  for (int i = 0; i < 3; i++)
  {
    if ((Shutters.IsOpen[i] == 1) && (Shutters.CountDown[i] > 0) && ((millis() - Shutters.Start[i]) > (Shutters.CountDown[i] * 100)))
    {
      digitalWrite(LED_BUILTIN, LOW);
      Shutter(i, false);
      String Command("");

      if (i == 0) Command = String("{\"Shutt\":\"Shut0\",\"val\":0}");
      if (i == 1) Command = String("{\"Shutt\":\"Shut1\",\"val\":0}");
      if (i == 2) Command = String("{\"Shutt\":\"Shut2\",\"val\":0}");
      //MQTTClient.publish(HoloLabAPIEcho, Command, false, LWMQTT_QOS1);
    }
  }

  //Shutters delayed start
  for (int i = 0; i < 3; i++)
  {
    if ((Shutters.IsOpen[i] == 0) && (Shutters.CountDown[i] > 0) && (Shutters.Delay[i] > 0) && !Shutters.Done[i] && ((millis() - Shutters.Start[i]) > (Shutters.Delay[i] * 100)))
    {
      digitalWrite(LED_BUILTIN, HIGH);
      Shutter(i, true);
      Shutters.Start[i] = millis();
      String Command("");

      if (i == 0) Command = String("{\"Shutt\":\"Shut0\",\"val\":1}");
      if (i == 1) Command = String("{\"Shutt\":\"Shut1\",\"val\":1}");
      if (i == 2) Command = String("{\"Shutt\":\"Shut2\",\"val\":1}");
      //MQTTClient.publish(HoloLabAPIEcho, Command, false, LWMQTT_QOS1);
    }
  }

  //Read the front panel push-buttons
  PushButton0.read();
  PushButton1.read();
  PushButton2.read();

  //Button released
  if (PushButton0.wasReleased())
  {
    Shutters.CountDown[0] = 0;
    Shutter(0, !Shutters.IsOpen[0]);
    String _Command = ("");
    if (Shutters.IsOpen[0] == 0)
      _Command = String("{\"Shutt\":\"Shut0\",\"val\":0}");
    else
      _Command = String("{\"Shutt\":\"Shut0\",\"val\":1}");

    //MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
  }

  //Button released
  if (PushButton1.wasReleased())
  {
    Shutters.CountDown[1] = 0;
    Shutter(1, !Shutters.IsOpen[1]);
    String _Command = ("");
    if (Shutters.IsOpen[1] == 0)
      _Command = String("{\"Shutt\":\"Shut1\",\"val\":0}");
    else
      _Command = String("{\"Shutt\":\"Shut1\",\"val\":1}");

    //MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
  }

  //Button released
  if (PushButton2.wasReleased())
  {
    Shutters.CountDown[2] = 0;
    Shutter(2, !Shutters.IsOpen[2]);
    String _Command = ("");
    if (Shutters.IsOpen[2] == 0)
      _Command = String("{\"Shutt\":\"Shut2\",\"val\":0}");
    else
      _Command = String("{\"Shutt\":\"Shut2\",\"val\":1}");

    //MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
  }

}

//Publish state of shutters to MQTT broker every 1 sec
void PublishStateCallback(Task* me)
{
  String _Command = ("");
  
  if (Shutters.IsOpen[0] == 0)
    _Command = String("{\"Shutt\":\"Shut0\",\"val\":0}");
  else
    _Command = String("{\"Shutt\":\"Shut0\",\"val\":1}");

  MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
  
  if (Shutters.IsOpen[1] == 0)
    _Command = String("{\"Shutt\":\"Shut1\",\"val\":0}");
  else
    _Command = String("{\"Shutt\":\"Shut1\",\"val\":1}");

  MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
  
  if (Shutters.IsOpen[2] == 0)
    _Command = String("{\"Shutt\":\"Shut2\",\"val\":0}");
  else
    _Command = String("{\"Shutt\":\"Shut2\",\"val\":1}");

  MQTTClient.publish(HoloLabAPIEcho, _Command, false, LWMQTT_QOS1);
}

//API
//Shutters command
//val: 0 means off, 1 means on, 2 means ignore; //t: 0 means no period, otherwise if val=1, means it will shut back off after period in seconds
//{"cd":"Shut","val":[1,1,2],"t":[60,30,0],"d":[0,60,0]}; //in this example: shutter 3 ignored, close shutter 1 for 6 secs, close shutter 2 for 3 secs after 6 secs wait

void ProcessCommand(String JSONCommand)
{
  //Json buffer
  StaticJsonBuffer<200> jsonBuffer;

  //Parse Json object and find which command it is
  JsonObject& command = jsonBuffer.parseObject(JSONCommand);

  // Test if parsing succeeds.
  if (!command.success())
  {
    Serial << F("Json parseObject() failed");
    return;
  }
  else
  {
    Serial << F("Json parseObject() success - ") << endl;

    if (command[F("cd")] == F("Shut"))
    {
      for (int i = 0; i < 3; i++)
      {
        //Shutters.IsOpen[i] = command[F("val")][i];
        Shutters.CountDown[i] = command[F("t")][i];
        Shutters.Delay[i] = command[F("d")][i];

        if ((int)command[F("val")][i] == 0) //Shutter 1 off
        {
          Shutter(i, bool(command[F("val")][i]));
          Shutters.Done[i] = true;
          Serial << "val=" << (int)command[F("val")][i] << _endl;
        }
        else if ((int)command[F("val")][i] == 1) //Shutter 1 on
        {
          if (Shutters.Delay[i] == 0) //open shutter right away
            Shutter(i, bool(command[F("val")][i]));
          Shutters.Start[i] = millis();
          Shutters.Done[i] = false;
          Serial << "val=" << (int)command[F("val")][i] << _endl;
        }
      }

      //MQTTClient.publish(HoloLabAPIEcho, JSONCommand, false, LWMQTT_QOS1);
    }
  }
}

//Actuate a shutter (shutterID: 0-2 for shutter 1-3)
void Shutter(int shutterID, bool Open)
{

  int PulseWidth = 100;

  switch (shutterID)
  {
    //select the appropriate Shutter
    case 0://Shutter 0
      digitalWrite(R4, LOW);
      digitalWrite(R3, HIGH);
      break;
    case 1://Shutter 1
      digitalWrite(R3, LOW);
      digitalWrite(R4, HIGH);
      break;
    case 2://Shutter 2
      digitalWrite(R3, LOW);
      digitalWrite(R4, LOW);
      break;
  }

  if (!Open)
  {
    digitalWrite(R2, LOW);
    digitalWrite(R1, HIGH);
    delay(PulseWidth);
    digitalWrite(R1, LOW);
    Shutters.IsOpen[shutterID] = 0;
    Shutters.Done[shutterID] = true;
  }
  else
  {
    digitalWrite(R1, LOW);
    digitalWrite(R2, HIGH);
    delay(PulseWidth);
    digitalWrite(R2, LOW);
    Shutters.IsOpen[shutterID] = 1;
    Shutters.Done[shutterID] = false;
  }
  digitalWrite(R3, LOW);
  digitalWrite(R4, LOW);
}

////////////////////////gettemp state machine///////////////////////////////////
//Init DS18B20 one-wire library
void gettemp_start()
{
  // Start up the library
  sensors_A.begin();

  // set the resolution
  sensors_A.setResolution(DS18b20_0, TEMPERATURE_RESOLUTION);
  sensors_A.setResolution(DS18b20_1, TEMPERATURE_RESOLUTION);

  //don't wait ! Asynchronous mode
  sensors_A.setWaitForConversion(false);

  gettemp.next(gettemp_request);
}

//Request temperature asynchronously
void gettemp_request()
{
  sensors_A.requestTemperatures();
  gettemp.next(gettemp_wait);
}

//Wait asynchronously for requested temperature measurement
void gettemp_wait()
{ //we need to wait that time for conversion to finish
  if (gettemp.elapsed(1000 / (1 << (12 - TEMPERATURE_RESOLUTION))))
    gettemp.next(gettemp_read);
}

//read and print temperature measurement
void gettemp_read()
{
  getMeasures(DS18b20_0, DS18b20_1);
  gettemp.next(gettemp_request);
}

//Compute free RAM
//useful to check if it does not shrink over time
int freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}
