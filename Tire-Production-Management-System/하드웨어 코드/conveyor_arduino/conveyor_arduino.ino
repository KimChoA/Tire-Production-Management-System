// Tire line Arduino UNO: PUL=D2, DIR=D3, NC stop switch D7 (GND), relay IN=D8.
// D7 LOW = closed normal contact; HIGH = stop pressed or wire disconnected.
// Relay COM/NO/NC are NOT connected to the motor supply in the supplied wiring.
#include <string.h>
const byte PUL_PIN=2, DIR_PIN=3, STOP_PIN=7, RELAY_PIN=8;
const unsigned long PULSE_DELAY_US=2000UL, WATCHDOG_MS=1500UL;
// Configure for your relay: some modules are active LOW. Relay is not a safety cutoff.
const byte RELAY_ON_LEVEL=LOW;
const byte RELAY_OFF_LEVEL=HIGH;
bool running=false, pulseHigh=false, estop=true;
unsigned long lastPulse=0, lastPing=0, lastButtonCheck=0;
char line[32]; byte n=0;
void stopMotor(){running=false; pulseHigh=false; digitalWrite(PUL_PIN,LOW);digitalWrite(RELAY_PIN,RELAY_OFF_LEVEL);}
void readStop(){
  // Simple debounce; any detected HIGH immediately removes step pulses.
  bool raw=digitalRead(STOP_PIN)==HIGH;
  if(raw) { digitalWrite(PUL_PIN,LOW); running=false; digitalWrite(RELAY_PIN,RELAY_OFF_LEVEL); }
  if(millis()-lastButtonCheck < 20) return;
  lastButtonCheck=millis();
  if(raw != estop){estop=raw; stopMotor(); Serial.println(estop?"ESTOP_ACTIVE":"ESTOP_RELEASED");}
}
void handle(const char* cmd){
  if(strcmp(cmd,"RUN")==0){
    if(estop || digitalRead(STOP_PIN)==HIGH){ stopMotor(); Serial.println("BLOCKED:ESTOP");return; }
    running=true; pulseHigh=false; lastPulse=micros(); lastPing=millis();
    digitalWrite(RELAY_PIN,RELAY_ON_LEVEL); Serial.println("RUN_OK");
  } else if(strcmp(cmd,"STOP")==0){stopMotor(); Serial.println("STOP_OK");}
  else if(strcmp(cmd,"PING")==0){ if(running)lastPing=millis(); }
  else if(strcmp(cmd,"STATUS")==0){Serial.println(estop?"ESTOP_ACTIVE":(running?"RUNNING":"STOPPED"));}
  else if(*cmd)Serial.println("UNKNOWN_COMMAND");
}
void setup(){
  pinMode(PUL_PIN,OUTPUT);pinMode(DIR_PIN,OUTPUT);pinMode(STOP_PIN,INPUT_PULLUP);
  // Off level set before OUTPUT prevents transient relay activation.
  digitalWrite(RELAY_PIN,RELAY_OFF_LEVEL);pinMode(RELAY_PIN,OUTPUT);
  digitalWrite(PUL_PIN,LOW);digitalWrite(DIR_PIN,HIGH);
  Serial.begin(9600); estop=digitalRead(STOP_PIN)==HIGH;
  Serial.println("ARDUINO_READY");Serial.println(estop?"ESTOP_ACTIVE":"ESTOP_RELEASED");
}
void loop(){
  readStop();
  while(Serial.available()){
    char ch=(char)Serial.read(); if(ch=='\r')continue;
    if(ch=='\n'){line[n]='\0';handle(line);n=0;}
    else if(n<sizeof(line)-1)line[n++]=ch;else n=0;
  }
  if(estop)return;
  if(running && millis()-lastPing>WATCHDOG_MS){stopMotor();Serial.println("WATCHDOG_STOP");}
  if(!running)return;
  unsigned long now=micros();
  if(now-lastPulse>=PULSE_DELAY_US){pulseHigh=!pulseHigh;digitalWrite(PUL_PIN,pulseHigh?HIGH:LOW);lastPulse=now;}
}
