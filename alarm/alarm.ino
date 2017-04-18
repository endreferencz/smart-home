#include <SPI.h>
#include <SD.h>
#include <Dhcp.h>
#include <Dns.h>
#include <Ethernet.h>
#include <EthernetClient.h>
#include <EthernetServer.h>
#include <EthernetUdp.h>

const int alarmPin = 22;
const int servicePin = 23;
int sensorValue = 0;
int callsRemaining = 0;

unsigned long alertStartTime = 0;
unsigned long toBeArmedStartTime = 0;
unsigned long lastGsmCheck = 0;
unsigned long callStart = 0;
unsigned long tenSecondsCheck = 0;


const int ALARM = 0;
const int OK = 1;
const int CUT = 2;
const int TAMPERED = 3;

const int NU = -1;
const int EOL2 = 0;
const int NO = 1;
const int NC = 2;

const int STAND_BY = 0;
const int ARMED = 1;
const int ALERT = 2;
const int TO_BE_ARMED = 3;

const int ZONE = 0;
const int ARM = 1;
const int DISARM = 2;
const int OBSERVE = 3;
const int NOTIFY= 4;

const int LOG_BUFFER_SIZE = 1000;
int logBuffer[LOG_BUFFER_SIZE];
const int sensorPins[] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15};
const int resistance[] = {
  1100, 1100, 1100, 1100, 1100,
  1100, 1100, 1100, 1100, 1100,
  1100, 1100, 1100, 1100, 1100,
  1100, 1100, 1100, 1100, 1100,
  1100, 1100, 1100, 1100, 1100};
const int holdLength[] = {
  200, 200, 200, 200, 200,
  200, 200, 200, 200, 200,
  200, 200, 200, 200, 200,
  200, 200, 200, 200, 200,
  0, 200, 200, 200, 200
};
const int mode[] = {
  NU, NU, NC, NC, NC, 
  EOL2, EOL2, EOL2, NC, NC, 
  NC, NC, NC, NC, NU, 
  NU, NC, NC, NC, NC, 
  NO, NC, NC, NC};
const char names[][15] {
  "N/A", "N/A", "Furdo", "Gardrob", "Nappali",
  "Pince", "Garazs", "Kazan", "Etkezo", "Lila",
  "Dolgozo", "Bejarat", "Konyha", "Halo", "N/A",
  "N/A", "Garazs kapu", "Aram", "Bejarat kint", "Misi infra",
  "Taviranyito", "Halo kint", "Lajos infra", "Hatso terasz"
};
const char messages[][15] {
  "Started", "One minute", "Armed", "DisarmCall", "Alarm", "Alarm end", "DisarmWeb", "GSM lost", "GSM restored"
};
const int action[] = {
  ZONE, ZONE, ZONE, OBSERVE, OBSERVE,
  ZONE, ZONE, ZONE, OBSERVE, OBSERVE,
  ZONE, ZONE, OBSERVE, OBSERVE, ZONE,
  ZONE, ZONE, NOTIFY, ZONE, ZONE,
  ARM, ZONE, ZONE, ZONE, ZONE
};
const int NUMBER_OF_ZONES = 24;
const int MAX_MEASURE = 1024;
const int INNER_RESISTANCE = 4700;
byte statusValue[NUMBER_OF_ZONES]; 
byte newStatusValue[NUMBER_OF_ZONES];
int measurement[NUMBER_OF_ZONES];
unsigned long newStatusHoldStart[NUMBER_OF_ZONES];
bool hasConnection = false;

byte mac[] = { 0xE2, 0x82, 0x66, 0x50, 0xE0, 0xDF };
IPAddress ip(192, 168, 0, 5);
IPAddress broadcast(255, 255, 255, 255);
const int BROADCAST_PORT = 37969;
EthernetServer server(80);
EthernetUDP udpSender;

int state = STAND_BY;

char* smsRetry = 0;

int closestLimit(int zone, int value) {
  int diff[4];
  diff[0] = abs(value);
  diff[1] = abs(round(MAX_MEASURE * 1.0 * resistance[zone] / (1.0 * resistance[zone] + INNER_RESISTANCE)) - value);
  diff[2] = abs(round(MAX_MEASURE * 2.0 * resistance[zone] / (2.0 * resistance[zone] + INNER_RESISTANCE)) - value);
  diff[3] = abs(MAX_MEASURE - value);
  int min = MAX_MEASURE;
  int index = 0;
  for (int i = 0; i < 4; i++) {
    if (min > diff[i]) {
      index = i;
      min = diff[i];
    }
  }
  return index;
}


int statusInner(int zone, int value) {
  int closestLimitResult = closestLimit(zone, value);
  if (mode[zone] == NU) {
    return OK;
  } else if (mode[zone] == EOL2) {
    if (closestLimitResult == 0) {
      return TAMPERED; 
    } else if (closestLimitResult == 1) {
      return OK;
    } else if (closestLimitResult == 2) {
      return ALARM;
    } else {
      return CUT; 
    }
  } else if (mode[zone] == NO) {
    if (closestLimitResult == 0 || closestLimitResult == 1) {
      return ALARM;
    } else {
      return OK;
    }
  } else if (mode[zone] == NC) {
    if (closestLimitResult == 0 || closestLimitResult == 1) {
      return OK;
    } else {
      return ALARM;
    }
  }
}

void notifyUdpChange(int zone, int oldValue, int newValue) {
  char buf[4];
  udpSender.beginPacket(broadcast, BROADCAST_PORT);
  udpSender.write("{ \"zone\" : ");
  sprintf(buf, "%i", zone);
  udpSender.write(buf);
  udpSender.write(", \"name\" : \"");
  udpSender.write(names[zone]);
  udpSender.write("\"");
  udpSender.write(", \"old\" : ");
  sprintf(buf, "%i", oldValue);
  udpSender.write(buf);
  udpSender.write(", \"new\" : ");
  sprintf(buf, "%i", newValue);
  udpSender.write(buf);
  udpSender.write("}");
  udpSender.endPacket();
}

int status(int index, int value) {
  unsigned long currentTimeStamp = millis();
  int tempStatus = statusInner(index, value);
  if (tempStatus == statusValue[index]) {
    newStatusValue[index] = statusValue[index];
  } else {
    if (newStatusValue[index] != tempStatus) {
      newStatusValue[index] = tempStatus;
      newStatusHoldStart[index] = currentTimeStamp;
    }
    if (currentTimeStamp < newStatusHoldStart[index]) {
      newStatusHoldStart[index] = 0;
    }
    if (currentTimeStamp - newStatusHoldStart[index] >= holdLength[index]) {
      notifyUdpChange(index, statusValue[index], newStatusValue[index]);
      statusValue[index] = newStatusValue[index];
      if ((statusValue[index] == TAMPERED || statusValue[index] == ALARM || statusValue[index] == CUT) && state == ARMED && action[index] == ZONE) {
        alertStartTime = micros();
        state = ALERT;
        // char buffer[100];
        // sprintf(buffer, "Alarm: %s", names[index]);
        addEventToLogBuffer(NUMBER_OF_ZONES + 4);
        // sendSms(buffer);
        callsRemaining = 2;
        call();
      }
      if ((statusValue[index] == TAMPERED || statusValue[index] == ALARM || statusValue[index] == CUT)) {
        addEventToLogBuffer(index);     
      }
      if (action[index] == NOTIFY) {
        char buffer[100];
        sprintf(buffer, "Notify: %s, State: %d", names[index], statusValue[index] != ALARM);
        sendSms(buffer);
      }
      if (action[index] == ARM && statusValue[index] == ALARM) {
        if (state == ARMED || state == ALERT) {
          state = STAND_BY;
          addEventToLogBuffer(NUMBER_OF_ZONES + 3);
          callsRemaining = 0;
          if (callStart != 0) {
            stopCall();
            callStart = 0;
          }
        } else {
          state = TO_BE_ARMED;      
          addEventToLogBuffer(NUMBER_OF_ZONES + 1);
        }
        toBeArmedStartTime = micros();
      }
    }
  }
  return statusValue[index];
}


void addEventToLogBuffer(int event){
  int time = millis() / 60 / 1000;
  addToLogBuffer(time);
  addToLogBuffer(event);
}

void addToLogBuffer(int number) {
  for (int i = 0; i < LOG_BUFFER_SIZE - 1; i++) {
    logBuffer[i] = logBuffer[i + 1];
  }
  logBuffer[LOG_BUFFER_SIZE - 1] = number;
}

void setup() {
  for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
    logBuffer[i] = -1;
  }
  for (int i = 0; i < NUMBER_OF_ZONES; i++) {
    newStatusValue[i] = 0;
    newStatusHoldStart[i] = 0;
    statusValue[i] = 1;
  }
  pinMode(22, OUTPUT);
  pinMode(23, OUTPUT);
  digitalWrite(22, LOW); 
  digitalWrite(23, LOW); 
  
  Serial1.begin(9600);
  Serial2.begin(9600);
    
  Ethernet.begin(mac, ip);
  server.begin();
  udpSender.begin(81);
  addEventToLogBuffer(NUMBER_OF_ZONES);
}

void manageSensors() {
  for (int i = 0; i < 16; i++) {
    sensorValue = analogRead(sensorPins[i]);  
    measurement[i] = sensorValue;
    statusValue[i] = status(i, sensorValue);
  }
  
  if (Serial2.available() > 0) {
    delay(1);
    boolean startsOk = true;
    int zeroCount = 0;
    while (Serial2.available() > 0 && zeroCount < 4 && startsOk) {
      if (Serial2.read() == 0) {
          zeroCount++;
        } else {
          startsOk = false;
        }
      }
    if (startsOk && zeroCount == 4) {
      for (int i = 0; i < 8; i++) {
        while (Serial2.available() <= 0) {}
        int byte1 = Serial2.read();
        while (Serial2.available() <= 0) {}
        int byte2 = Serial2.read();
        sensorValue = (byte1 - 1) * 100 + (byte2 - 1);
        measurement[16 + i] = sensorValue;
        statusValue[16 + i] = status(16 + i, sensorValue);
      }
    }
    while (Serial2.available() > 0) {
      Serial2.read();
    }
  }
}

void printLog(EthernetClient client) {
  int currentTimeStamp = millis() / 60 / 1000;
  client.print("[");
  for (int i = 0; i < LOG_BUFFER_SIZE; i += 2) {
    if (logBuffer[i] != -1) {
      client.print("{");
      client.print("\"timestamp\":");
      client.print(currentTimeStamp - logBuffer[i]);
      client.print(",\"event\":\"");
      if (logBuffer[i+1] < NUMBER_OF_ZONES) {
        client.print(names[logBuffer[i+1]]);
      } else {
        client.print(messages[logBuffer[i+1] - NUMBER_OF_ZONES]);
      }
      client.print("\"}");
      if (i + 2 < LOG_BUFFER_SIZE) {
         client.print(",");
      }
    }
  }
  client.print("]");
}

void printJsonWithCurrentState(EthernetClient client) {
  client.print("{");
  client.print("\"state\":");
  client.print(state);
  client.print(", \"log\":");
  printLog(client);
  client.print(", \"sensors\" : [");
  for (int i = 0; i < NUMBER_OF_ZONES; i++) {
    client.print("{");
    client.print("\"name\" : \"");
    client.print(names[i]);
    client.print("\",");
    client.print("\"status\" : ");
    client.print(statusValue[i]);
    client.print(", \"measure\" : ");
    client.print(measurement[i]);
    client.print("}");
    if (i != 23) {
      client.print(", ");      
    }
  }
  client.print("]}");
}

void manageHttpRequests() {
  char lastCharacters[10];
  for (int i = 0; i < 10; i++) {
     lastCharacters[i] = 0;
  }
  EthernetClient client = server.available();
  if (client) {
    boolean currentLineIsBlank = true;
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        for (int i = 0; i < 10 - 1; i++) {
          lastCharacters[i] = lastCharacters[i + 1];
        }
        lastCharacters[9] = c;
        if (lastCharacters[1] == 'G' && lastCharacters[2] == 'E' && lastCharacters[3] == 'T' 
          && lastCharacters[4] == ' ' && lastCharacters[5] == '/' && lastCharacters[6] == 'a' 
          && lastCharacters[7] == 'r' && lastCharacters[8] == 'm' && lastCharacters[9] == ' ') {
          state = ARMED;
          addEventToLogBuffer(NUMBER_OF_ZONES + 2);    
        }
        if (lastCharacters[0] == 'G' && lastCharacters[1] == 'E' && lastCharacters[2] == 'T' 
          && lastCharacters[3] == ' ' && lastCharacters[4] == '/' && lastCharacters[5] == 's' 
          && lastCharacters[6] == 't' && lastCharacters[7] == 'o' && lastCharacters[8] == 'p'
          && lastCharacters[9] == ' ') {
          state = STAND_BY;
          addEventToLogBuffer(NUMBER_OF_ZONES + 6);
        }
        if (c == '\n' && currentLineIsBlank) {
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: application/javascript");
          client.println("Connection: close");
          client.println();
          client.println("jsonp(");
          printJsonWithCurrentState(client);
          client.println(")");
          break;
        }
        if (c == '\n') {
          currentLineIsBlank = true;
        } else if (c != '\r') {
          currentLineIsBlank = false;
        }
      }
    }
    delay(1);
    client.stop();
    Ethernet.maintain();
  }
}

boolean checkGsmAnswer(const char* expected) {
  for (int i = 0; expected[i] != 0; i++) {
    if (!waitFor(expected[i])) {
      return false;
    }
  }
  return true;
}

boolean waitFor(char waitFor) {
  boolean finished = false;
  int max_wait = 5000;
  unsigned long start = millis();
  while(!finished && (millis() - start) < max_wait) {
    if (Serial1.available()) {
      char next = (char) Serial1.read();
      if (next != '\r' && next != '\n') {
        if (next == waitFor) {
          finished = true;
        }        
      }
    }
  }
  return finished;
}

boolean readGsm(boolean logResponse) {
  unsigned long start = millis();
  int max_wait = 5000;
  while (Serial1.available() == 0 && (millis() - start) < max_wait) {
    delay(1);
  }
  boolean finished = false;
  boolean result = false;
  char response[200]; int responsePos = 0;
  char text[7];
  while(!finished && (millis() - start) < max_wait) {
    if (Serial1.available()) {
      char next = (char) Serial1.read();
      for (int i = 0; i < 7 - 1; i++) {
        text[i] = text[i + 1];
      }
      text[6] = next;
      if (text[3] == '\n' && text[4] == 'O' && text[5] == 'K' && text[6] == '\r') {
        finished = true; result = true;
      } else if (text[0] == '\n' && text[1] == 'E' && text[2] == 'R' && text[3] == 'R' && text[4] == 'O' && text[5] == 'R' && text[6] == '\r') {  
        finished = true; result = false;
      }
      if (logResponse) {
        response[responsePos++] = next;
      }
    }
  }
  if (logResponse) {
    response[responsePos] = 0;
    udpSender.beginPacket(broadcast, BROADCAST_PORT);
    udpSender.write("{ \"gsm\" : \"");
    udpSender.write(response);
    udpSender.write("\"}");
    udpSender.endPacket();
  }
  return result;
}

void sendSms(char* text) {
  if (!tryToSendSms(text)) {
    smsRetry = text;
  }
}

void retrySms() {
  if (smsRetry != 0) {
    if (tryToSendSms(smsRetry)) {
      smsRetry = 0;
    }
  }
}

boolean tryToSendSms(char* text) {
   Serial1.println("AT+CMGS=\"06704318849\"");
   if (waitFor('>')) {
     Serial1.print(text);
     Serial1.print('\x1A');
     return readGsm(true);
   }
   return false;
}

void initGsm() {
  boolean logInit = true;
  Serial1.println("AT");
  readGsm(logInit);
  Serial1.println("ATE0");
  readGsm(logInit);
  Serial1.println("AT+CPIN?");
  readGsm(logInit);
  Serial1.println("AT+CPIN=3741");
  readGsm(logInit);
  Serial1.println("AT+CMGF=1");
  readGsm(logInit);
  Serial1.println("AT+COPS?");
  readGsm(logInit);
}

void call() {
  Serial1.print("ATD");
  if (callsRemaining == 2) {
    Serial1.print("06704318849");    
  } if (callsRemaining == 1) {
    Serial1.print("06209599429");        
  }
  Serial1.println(";");
  if (readGsm(true)) {
    callsRemaining--;
    callStart = micros();
  }
}

void stopCall() {
  Serial1.println("ATH");
  readGsm(true);
}

void checkGsm() {
  Serial1.println("AT+COPS?");
  if (!checkGsmAnswer("+COPS: 1,0,\"vodafone\"")) {
    if (hasConnection) {
      hasConnection = false;
      addEventToLogBuffer(NUMBER_OF_ZONES + 7);
    }
    initGsm();
  } else {
    if (!hasConnection) {
      hasConnection = true;
      addEventToLogBuffer(NUMBER_OF_ZONES + 8);
    }
  }
}

unsigned volatile long lastMoment;

void loop() {
  if (state == ALERT) {
    digitalWrite(alarmPin, HIGH);
    if (abs(micros() - alertStartTime) > (unsigned long) 60 * 1000 * 1000) {
      state = ARMED;
      addEventToLogBuffer(NUMBER_OF_ZONES + 5);
    }
  } else if (state == TO_BE_ARMED && (abs(micros() - toBeArmedStartTime) > (unsigned long) 60 * 1000 * 1000)) {
    state = ARMED;
    addEventToLogBuffer(NUMBER_OF_ZONES + 2);
  } else {
    digitalWrite(alarmPin, LOW);
  }
  if (abs(micros() - lastGsmCheck) > (unsigned long) 10 * 1000 * 1000) {
    checkGsm();
    retrySms();
    lastGsmCheck = micros();
  }
  if ((callStart != 0) && (abs(micros() - callStart) > (unsigned long) 30 * 1000 * 1000)) {
    stopCall();
    callStart = 0;
    tenSecondsCheck = micros();
  }
  if ((callStart == 0) && abs(micros() - tenSecondsCheck) > (unsigned long) 10 * 1000 * 1000) {
    if (callsRemaining > 0) {
      call();      
    }
  }
  manageSensors();
  manageHttpRequests();
  delay(1);
}
