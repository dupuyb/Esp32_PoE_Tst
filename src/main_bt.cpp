// #include <Arduino.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;
boolean confirmRequestPending = true;

void BTConfirmRequestCallback(uint32_t numVal) {
  confirmRequestPending = true;
  Serial.println(numVal);
}

void BTAuthCompleteCallback(boolean success) {
  confirmRequestPending = false;
  if (success){
    Serial.println("Pairing success!!");
  } else {
    Serial.println("Pairing failed, rejected by user!!");
  }
}

const char VERSION[] ="0.0.1";

// Main variables
long previousMillis = 0;

// setup -------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  Serial.printf("Start setup Ver:%s\n\r",VERSION);

  SerialBT.enableSSP();
  SerialBT.onConfirmRequest(BTConfirmRequestCallback);
  SerialBT.onAuthComplete(BTAuthCompleteCallback);
  SerialBT.begin("ESP32test"); //Bluetooth device name

}

// Main loop -----------------------------------------------------------------
void loop() {

  // Is alive executed every 2 sec.
  if ( millis() - previousMillis > 2000L) {
    previousMillis = millis();

    if (confirmRequestPending)
        SerialBT.confirmReply(true);
   
    SerialBT.printf("Hello from ESP32 occ:%d \n\r", millis()/2000);
    Serial.printf("Hello from ESP32 occ:%d confirmRequestPending=%d\n\r", millis()/2000, confirmRequestPending);

  } // End 2 second
}