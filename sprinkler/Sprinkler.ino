#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 0, 3);
EthernetServer server(80);
EthernetUDP udpSender;

char line[2000];
unsigned long lastCurrentMillis = millis();
unsigned long lastUdp = millis();
bool led = true;

int relayStatuses[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
long relayTimeout[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
long relayStartAfter[14] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
int relayPins[14] = {22, 23, 24, 25, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43};
int on[14] = {HIGH, HIGH, HIGH, HIGH, LOW, LOW, LOW, LOW, LOW, LOW, LOW, LOW, HIGH, HIGH};
int off[14] = {LOW, LOW, LOW, LOW, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, LOW, LOW};
IPAddress broadcast(255, 255, 255, 255);
const int BROADCAST_PORT = 37970;

void setup() {
  // disable SD card
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);

  Serial.begin(9600);
  Ethernet.begin(mac, ip);
  server.begin();
  udpSender.begin(81); 
  Serial.print("server is at ");
  Serial.println(Ethernet.localIP()); 
  for (int i = 0; i < 14; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], off[i]); 
  }
     pinMode(13, OUTPUT);
 
}

void readLine(EthernetClient client) {
  int delays = 0;
  int charCount = 0;
  while (client.connected() && (charCount == 0 || line[charCount - 1] != '\n') && charCount < 2000 - 1 && delays < 10000) {
    if (client.available()) {
      line[charCount++] = client.read();
    } else {
      delay(1);
      delays++;
    }
  }
  line[charCount++] = 0;
  Serial.write(line);
}

bool startsWith(const char* prefix, char* in) {
  int i = 0;
  while (in[i] != 0 && prefix[i] != 0) {
    if (in[i] != prefix[i]) {
      return false;
    }
    i++;
  }
  return prefix[i] == 0;
}

int length(char* str) {
  int i = 0;
  while (str[i] != 0) {
    i++;
  }
  return i;
}

long parseParamValue(char* paramValue) {
  int i = 0;
  long param = 0;
  while (paramValue[i] != 0 && paramValue[i] != '&' && paramValue[i] != ' ') {
    param = param * 10 + paramValue[i] - '0';
    i++;
  }
  return param;
}

long getParam(char* getLine, char* param) {
  int i = 0;
  int lineLength = length(getLine);
  int paramLength = length(param);
  while (getLine[i] != 0) {
    if (startsWith(param, getLine + i) && i + paramLength + 1 < lineLength){
      return parseParamValue(getLine + i + length(param) + 1);
    }
    i++;
  }
  return -1;
}


void parseLine() {
  if (startsWith("GET /set?", line)) {
    char valueParamName[] = "val0";
    char timeParamName[] = "tim0";
    char startAfterParamName[] = "aft0";
    for (int i = 0; i < 14; i++) {
      int offset = i;
      if (i >= 10) {
        offset += ('A' - '9' - 1);
      }
      valueParamName[3] = '0' + offset;
      timeParamName[3] = '0' + offset;
      startAfterParamName[3] = '0' + offset;
      
      long value = getParam(line, valueParamName);
      long time = getParam(line, timeParamName);
      long startAfter = getParam(line, startAfterParamName);
  
      relayStartAfter[i] = -1;
      relayStatuses[i] = 0; 
      relayTimeout[i] = time;
      if (value != 1) {
        digitalWrite(relayPins[i], off[i]);
      } else {
        if (startAfter > 0) {
          relayStartAfter[i] = startAfter;
          digitalWrite(relayPins[i], off[i]);
        } else {
          relayStatuses[i] = 1;
          digitalWrite(relayPins[i], on[i]);
        }
      }
    }
  }
}

void loop() {
  Ethernet.maintain();
  unsigned long currentMillis = millis();  
  unsigned long toSubtract = currentMillis - lastCurrentMillis;
  if (currentMillis < lastCurrentMillis) {
    lastCurrentMillis = currentMillis;
    return;
  }
  lastCurrentMillis = currentMillis;

  if (abs(currentMillis - lastUdp) > 10000) {
     udpSender.beginPacket(broadcast, BROADCAST_PORT);
     udpSender.write("sprinkler");
     udpSender.endPacket();
     lastUdp = currentMillis;
     led = !led;
     if (led) {
      digitalWrite(13, HIGH);
     } else {
      digitalWrite(13, LOW);
     }
  }

  for (int i = 0; i < 14; i++) {
    if (relayStatuses[i] > 0) {
       if (relayTimeout[i] <= toSubtract) {
         relayStartAfter[i] = -1;
         relayStatuses[i] = 0;
         relayTimeout[i] = 0;
         digitalWrite(relayPins[i], off[i]);
       } else {
         relayTimeout[i] = relayTimeout[i] - toSubtract;
       }
    } else if (relayStartAfter[i] >= 0) {
      if (relayStartAfter[i] <= toSubtract) {
        relayStartAfter[i] = -1;
        relayStatuses[i] = 1;
        digitalWrite(relayPins[i], on[i]);
      } else {
        relayStartAfter[i] = relayStartAfter[i] - toSubtract;
      }
    }
  }
  
  EthernetClient client = server.available();
  if (client) {
    Serial.println("new client");
    int loopCount = 0;
    do {
      readLine(client);
      parseLine();
      loopCount++;
    } while (line[0] != '\n' && line[0] != '\r' && line[0] != 0 && loopCount < 100);
    
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();
    client.println("{");
    for (int i = 0; i < 14; i++) {
      client.print("\"relay");
      client.print(i);
      client.print("\": \"");
      client.print(relayStatuses[i]);
      client.print("\", \"startAfter");
      client.print(i);
      client.print("\": \"");
      client.print(relayStartAfter[i]);
      client.print("\", \"timeout");
      client.print(i);
      client.print("\": \"");
      client.print(relayTimeout[i]);
      client.print("\"");
      if (i != 13) {
        client.println(", ");
      }
    }
    client.println();
    client.println("}");
    client.flush();
    client.stop();
    Serial.println("client disconnected");
  }
}


