int sensorPins[] = {A0, A1, A2, A3, A4, A5, A6, A7};
long sensorValues[] = {0, 0, 0, 0, 0, 0, 0, 0};
int iteration = 100;

void setup() {
  Serial.begin(9600);
}

void loop() {
  for (int i = 0; i < iteration; i++) {
    for (int j = 0; j < 8; j++) {
      int read = analogRead(sensorPins[j]);
      sensorValues[j] += read;
    }
    delay(1);
  }
  Serial.write(0);
  Serial.write(0);
  Serial.write(0);
  Serial.write(0);
  for (int i = 0; i < 8; i++) {
     int read = sensorValues[i] / iteration;
     sensorValues[i] = 0;
     Serial.write(read / 100 + 1);
     Serial.write(read % 100 + 1);
  } 
}
