/*
HoloLab Holography setup controller
(c) Loic74 <loic74650@gmail.com> 2018-2019

***Dependencies and respective revisions used to compile this project***
https://github.com/256dpi/arduino-mqtt/releases (rev 2.4.3)
https://github.com/prampec/arduino-softtimer (rev 3.1.3)
https://github.com/bblanchon/ArduinoJson (rev 5.13.4)
https://github.com/sdesalas/Arduino-Queue.h (rev )
https://github.com/JChristensen/JC_Button (rev 2.1.1)

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

// Firmware revision
String Firmw = "0.0.1";

#define R1 2 //Relay 1 = Arduino pin 2
#define R2 7 //Relay 2 = Arduino pin 7
#define R3 8 //Relay 3 = Arduino pin 8
#define R4 3 //Relay 4 = Arduino pin 3 

#define PUSH0 32 //push button to manually toggle shutter0
#define PUSH1 31 //push button to manually toggle shutter1
#define PUSH2 28 //push button to manually toggle shutter2

const byte PushButton0Pin(PUSH0), PushButton1Pin(PUSH1), PushButton2Pin(PUSH2);

Button PushButton0(PushButton0Pin);   // define the button
Button PushButton1(PushButton1Pin);   // define the button
Button PushButton2(PushButton2Pin);   // define the button

//Queue object to store incoming JSON commands (up to 10)
Queue<String> queue = Queue<String>(10);

struct LaserShutters
{
   bool  IsOpen[3];
   int  CountDown[3];
   unsigned long Start[3];
}Shutters = {{0,0,0},{0,0,0},{0,0,0}};

//buffers for MQTT string payload
#define PayloadBufferLength 128
char Payload[PayloadBufferLength];

//MQTT publishing periodicity of system info, in msecs
unsigned long PublishPeriod = 30000;
                                                 
// MAC address of Ethernet shield (in case of Controllino board, set an arbitrary MAC address)
byte mac[] = { 0x90, 0xA2, 0xDA, 0x11, 0x2E, 0x15 };
String sArduinoMac;
IPAddress ip(192, 168, 0, 17);  //IP address, needs to be adapted depending on local network topology
EthernetClient net;             //Ethernet client to connect to MQTT server

//MQTT stuff including local broker/server IP address, login and pwd
MQTTClient MQTTClient;
const char* MqttServerIP = "192.168.0.14";
const char* MqttServerClientID = "ArduinoHoloLab"; // /!\ choose a client ID which is unique to this Arduino board
const char* MqttServerLogin = "XXXXX";
const char* MqttServerPwd = "XXXXX";
const char* HoloLabTopic = "HoloLab";
const char* HoloLabAPI = "HoloLab/API";
const char* HoloLabAPIEcho = "HoloLab/API/Echo";
const char* HoloLabStatus = "HoloLab/status";

//serial printing stuff
String _endl = "\n";

//Callbacks
//Here we use the SoftTimer library which handles multiple timers (Tasks)
//It is more elegant and readable than a single loop() functtion, especially
//when tasks with various frequencies are to be used
void GenericCallback(Task* me);
void PublishDataCallback(Task* me);

Task t1(10, GenericCallback);                //Various things handled/updated in this loop every 0.6 secs

void setup()
{
   //Serial port for debug info
    Serial.begin(9600);
    delay(200);

    // initialize digital pin LED_BUILTIN as an output.
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(R1, OUTPUT);
    pinMode(R2, OUTPUT);
    pinMode(R3, OUTPUT);
    pinMode(R4, OUTPUT);

    pinMode(PUSH0, INPUT_PULLUP);
    pinMode(PUSH1, INPUT_PULLUP);
    pinMode(PUSH2, INPUT_PULLUP);
  
    //String for MAC address of Ethernet shield for the log & XML file
    sArduinoMac = F("0x90, 0xA2, 0xDA, 0x11, 0x2E, 0x15");

    //8 seconds watchdog timer to reset system in case it freezes for more than 8 seconds
    wdt_enable(WDTO_8S);
    
    // initialize Ethernet device  
    Ethernet.begin(mac, ip); 
    
    // start to listen for clients
    //server.begin();  

    //Init MQTT
    MQTTClient.setOptions(60,false,10000);
    MQTTClient.setWill(HoloLabStatus,"offline",true,LWMQTT_QOS1);
    MQTTClient.begin(MqttServerIP, net);
    MQTTClient.onMessage(messageReceived);
    MQTTConnect();

    //Initialize the front panel push-buttons objects
    PushButton0.begin();
    PushButton1.begin();
    PushButton2.begin();
        
    //Generic loop
    SoftTimer.add(&t1);

    //display remaining RAM space. For debug
    Serial<<F("[memCheck]: ")<<freeRam()<<F("b")<<_endl;
}

//Connect to MQTT broker and subscribe to the PoolTopicAPI topic in order to receive future commands
//then publish the "online" message on the "status" topic. If Ethernet connection is ever lost
//"status" will switch to "offline". Very useful to check that the Arduino is alive and functional
void MQTTConnect() 
{
  MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd);
  int8_t Count=0;
  while (!MQTTClient.connect(MqttServerClientID, MqttServerLogin, MqttServerPwd) && (Count<4))
  {
    Serial<<F(".")<<_endl;
    delay(500);
    Count++;
  }

  if(MQTTClient.connected())
  {
    //String PoolTopicAPI = "Charmoisy/Pool/Api";
    //Topic to which send/publish API commands for the Pool controls
    MQTTClient.subscribe(HoloLabAPI);
  
    //tell status topic we are online
    if(MQTTClient.publish(HoloLabStatus,"online",true,LWMQTT_QOS1))
      Serial<<F("published: HoloLab/status - online")<<_endl;
    else
    {
      Serial<<F("Unable to publish on status topic; MQTTClient.lastError() returned: ")<<MQTTClient.lastError()<<F(" - MQTTClient.returnCode() returned: ")<<MQTTClient.returnCode()<<_endl;
    }
  }
  else
  Serial<<F("Failed to connect to the MQTT broker")<<_endl;
  
}

//MQTT callback
//This function is called when messages are published on the MQTT broker on the PoolTopicAPI topic to which we subscribed
//Add the received command to a message queue for later processing and exit the callback
void messageReceived(String &topic, String &payload) 
{
  String TmpStrPool(HoloLabAPI);

  //HoloLab commands. This check might be redundant since we only subscribed to this topic
  if(topic == TmpStrPool)
  {
    queue.push(payload); 
    Serial<<"FreeRam: "<<freeRam()<<" - Qeued messages: "<<queue.count()<<_endl;
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

    //Process queued incoming JSON commands if any
    if(queue.count()>0)
      ProcessCommand(queue.pop());

    //Shutter count donwns  
    for(int i=0;i<3;i++)
    {
      if((Shutters.IsOpen[i] == 1) && (Shutters.CountDown[i] > 0) && ((millis() - Shutters.Start[i]) > (Shutters.CountDown[i]*100)))
      {
        digitalWrite(LED_BUILTIN, LOW);
        Shutter(i, false);
        String Command("");
        
        if(i==0) Command = String("{\"cd\":\"Shut0\",\"val\":0}");
        if(i==1) Command = String("{\"cd\":\"Shut1\",\"val\":0}");
        if(i==2) Command = String("{\"cd\":\"Shut2\",\"val\":0}");
        MQTTClient.publish(HoloLabAPIEcho,Command,false,LWMQTT_QOS1);
      }
    } 

    //Read the front panel push-buttons
    PushButton0.read();
    PushButton1.read();
    PushButton2.read();

    //Button released 
    if(PushButton0.wasReleased())
    {
      Shutters.CountDown[0]=0;
      Shutter(0, !Shutters.IsOpen[0]);
      String _Command = ("");
      if(Shutters.IsOpen[0] == 0)
        _Command = String("{\"cd\":\"Shut0\",\"val\":0}");
      else
        _Command = String("{\"cd\":\"Shut0\",\"val\":1}");
        
      MQTTClient.publish(HoloLabAPIEcho,_Command,false,LWMQTT_QOS1);
    }
    
    //Button released
    if(PushButton1.wasReleased())
    {
      Shutters.CountDown[1]=0;
      Shutter(1, !Shutters.IsOpen[1]);  
      String _Command = ("");
      if(Shutters.IsOpen[1] == 0)
        _Command = String("{\"cd\":\"Shut1\",\"val\":0}");
      else
        _Command = String("{\"cd\":\"Shut1\",\"val\":1}");
        
      MQTTClient.publish(HoloLabAPIEcho,_Command,false,LWMQTT_QOS1);
    }

    //Button released
    if(PushButton2.wasReleased())
    {
      Shutters.CountDown[2]=0;
      Shutter(2, !Shutters.IsOpen[2]);  
       String _Command = ("");
      if(Shutters.IsOpen[2] == 0)
        _Command = String("{\"cd\":\"Shut2\",\"val\":0}");
      else
        _Command = String("{\"cd\":\"Shut2\",\"val\":1}");
        
      MQTTClient.publish(HoloLabAPIEcho,_Command,false,LWMQTT_QOS1);
    }

}


//API
//Shutters command
//val: 0 means off, 1 means on, 2 means ignore; //t: 0 means no period, otherwise if val=1, means it will shut back off after period in seconds
//{"cd":"Shut","val":[2,0,1],"t":[0,0,11]}; //in this example: shutter 1 ignored, close shutter 2 closed, open shutter 3 for 11 seconds

void ProcessCommand(String JSONCommand)
{
        //Json buffer
      StaticJsonBuffer<200> jsonBuffer;
      
      //Parse Json object and find which command it is
      JsonObject& command = jsonBuffer.parseObject(JSONCommand);
      
      // Test if parsing succeeds.
      if (!command.success()) 
      {
        Serial<<F("Json parseObject() failed");
        return;
      }
      else
      {
        Serial<<F("Json parseObject() success - ")<<endl;

        if (command[F("cd")] == F("Shut"))
        {                 
          for(int i=0; i<3;i++)
          {
            //Shutters.IsOpen[i] = command[F("val")][i];
            Shutters.CountDown[i] = command[F("t")][i];
                      
            if((int)command[F("val")][i]==0)//Shutter 1 off
            {             
              Shutter(i, bool(command[F("val")][i]));
              Serial<<"val="<<(int)command[F("val")][i]<<_endl;
            }
            else
            if((int)command[F("val")][i]==1)//Shutter 1 on
            {
              Shutter(i, bool(command[F("val")][i]));
              Shutters.Start[i] = millis();
              Serial<<"val="<<(int)command[F("val")][i]<<_endl;
            }
          }
           
          MQTTClient.publish(HoloLabAPIEcho,JSONCommand,false,LWMQTT_QOS1);
        }
      }
}

//Actuate a shutter (shutterID: 0-2 for shutter 1-3)
void Shutter(int shutterID, bool Open)
{    
  
  int PulseWidth = 50;
       
  switch (shutterID) 
  {
    case 0://open-close Shutter 0
      digitalWrite(R4, LOW);
      digitalWrite(R3, HIGH);
      break;
    case 1://open-close Shutter 1
      digitalWrite(R3, LOW);
      digitalWrite(R4, HIGH);
      break;
    case 2://open-close Shutter 2
      digitalWrite(R3, LOW);
      digitalWrite(R4, LOW);
      break;
  }
  
  if(Open)
  {
    digitalWrite(R2, LOW);
    digitalWrite(R1, HIGH);
    delay(PulseWidth);
    digitalWrite(R1, LOW);
    Shutters.IsOpen[shutterID] = 1;
  }
  else
  {
    digitalWrite(R1, LOW);
    digitalWrite(R2, HIGH);
    delay(PulseWidth);
    digitalWrite(R2, LOW);
    Shutters.IsOpen[shutterID] = 0;
  }   
  digitalWrite(R3, LOW);
  digitalWrite(R4, LOW);
}

//Compute free RAM
//useful to check if it does not shrink over time
int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}
