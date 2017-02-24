/*******************************************************************************
 * Copyright (c) 2015 Thomas Telkamp and Matthijs Kooijman
 *
 * Permission is hereby granted, free of charge, to anyone
 * obtaining a copy of this document and accompanying files,
 * to do whatever they want with them without any restriction,
 * including, but not limited to, copying, modification and redistribution.
 * NO WARRANTY OF ANY KIND IS PROVIDED.
 *
 * Do not forget to define the radio type correctly in config.h. in lmic library
 *
 * Modified By DenniZr - First test version
 *******************************************************************************/
 
 // done: investigate OTAA, good description here: Moteino, LMIC and OTAA Walkthrough https://github.com/lukastheiler/ttn_moteino
 // TODO: OTAA test code can be tested on device
 
 // TODO: add GPS flightmode. Good test script seems available here:  https://ukhas.org.uk/guides:ublox6
 // done: GPS power save. The u-center software can be used to make the configurations you want. See: https://www.youtube.com/watch?v=iWd0gCOYsdo
 // TODO: if no good read from GPS in last 2 minutes, then reset arduino as a manual reset fixes the problem?? Or auto-reset every 120 minutes. investigate

 // TODO: Add arduino sleep mode: need to change the radio scheduler to inline mode? see https://github.com/tijnonlijn/RFM-node/blob/master/template%20ttnmapper%20node%20-%20scheduling%20removed.ino
 
 // TODO: We will also create a new account 'Kaasfabriek' at some later day where our teams wil be building their stuff.
 // TODO: Create and apply new device ID

#define DE //BUG     // if DEBUG is defined, some code is added to display some basic debug info
#define DEB // UG_XL  // if DEBUG_XL is defined, some code is added to display more detailed debug info

//#include "LowPower.h"   // help to do power save on the arduino  https://github.com/rocketscream/Low-Power

 //#include <JeeLib.h>  // Include library containing low power functions
 //ISR(WDT_vect) { Sleepy::watchdogEvent(); } // Setup for low power waiting

//////////////////////////////////////////////
// GPS libraries, mappings and things
//////////////////////////////////////////////
#include <SoftwareSerial.h> 
#include <TinyGPS.h>

SoftwareSerial ss(3, 2);  // ss RX, TX --> GPS TXD, RXD
TinyGPS gps;

bool GPS_values_are_valid = true;
boolean  gpsEnergySavingWantsToActivate = false;      // this is set to true once gps fix is found
long  gpsEnergySavingStartDelayMillis = 15000;        // Device starts in normal mode. Energy saving is started xx time after a gps fix was found.
long  gpsEnergySavingWantsToActivateStartTime = 0;
int  gpsEnergySavingActivated = false;                // this is set to true once energy saving is activated

//////////////////////////////////////////////
// LMIC and RFM95 mapping and things
//////////////////////////////////////////////

// // do not keep radio active to listen to return message in RX2. see https://github.com/matthijskooijman/arduino-lmic/blob/master/src/lmic/config.h
// #define DISABLE_JOIN     // Uncomment this to disable all code related to joining
#define DISABLE_PING     // Uncomment this to disable all code related to ping
#define DISABLE_BEACONS  // Uncomment this to disable all code related to beacon tracking.// Requires ping to be disabled too 

#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>  //MISO MOSI SCK stuff
#include "keys.h"  // the personal keys to identify our own nodes

const unsigned  TX_INTERVAL = 50; //  250;  // transmit interval
dr_t LMIC_DR_sequence[] = {DR_SF10, DR_SF7, DR_SF7, DR_SF7, DR_SF7, DR_SF7, DR_SF9, DR_SF7, DR_SF7, DR_SF7, DR_SF7, DR_SF7 };      //void LMIC_setDrTxpow (dr_t dr, s1_t txpow)
int  LMIC_DR_sequence_count = 12;
int  LMIC_DR_sequence_index = 0;
const  lmic_pinmap lmic_pins = { .nss = 14,    .rxtx = LMIC_UNUSED_PIN,    .rst = 10,    .dio = {17, 16, 15}, };

uint8_t  mydata[14];  // mydata[9] allows for GPS location. a few bytes added to the memory buffer to play with
const unsigned message_size = 11;  // 9 bytes are needed into the ttn tracker service
// byte 0, 1, 2      Latitude     -90 to +90 rescaled to 0 - 16777215
// byte 3, 4, 5      Longitude    -180 to + 180 rescaled to 0 - 16777215
// byte 6, 7         Altitude     2 bytes in meters
// byte 8            GPS DoP      1 byte
// byte 9            Arduino VCC  1 byte in 50ths Volt
// byte 10           cpu temp     1 byte -100 to 155 scaled to 0 - 255

static  osjob_t sendjob;

// os_ interfaces for callbacks only used in over-the-air activation, so functions can be left empty here
void  os_getArtEui (u1_t* buf) { }
void  os_getDevEui (u1_t* buf) { }
void  os_getDevKey (u1_t* buf) { } 

int TX_COMPLETE_was_triggered = 0;  // 20170220 added to allow full controll in main Loop

//////////////////////////////////////////////////////////
//// Kaasfabriek routines for gps
////////////////////////////////////////////

void put_gpsvalues_into_sendbuffer(float flat, float flon, float alt, int hdopNumber)
{    
  uint32_t LatitudeBinary = ((flat + 90) / 180) * 16777215;
  uint32_t LongitudeBinary = ((flon + 180) / 360) * 16777215;
  uint16_t altitudeGps = alt;         // altitudeGps in meters, alt from tinyGPS is float in meters
  if (alt<0) altitudeGps=0;   // unsigned int wil not allow negative values and warps them to huge number, needs to be zero'ed
  // uint8_t accuracy = hdopNumber*10;   // needs to be /10 instead of *10 as per example JP
  uint8_t accuracy = hdopNumber/10;   // from TinyGPS horizontal dilution of precision in 100ths, TinyGPSplus seems the same in 100ths as per MNEMA string
  
  mydata[0] = ( LatitudeBinary >> 16 ) & 0xFF;
  mydata[1] = ( LatitudeBinary >> 8 ) & 0xFF;
  mydata[2] = LatitudeBinary & 0xFF;

  mydata[3] = ( LongitudeBinary >> 16 ) & 0xFF;
  mydata[4] = ( LongitudeBinary >> 8 ) & 0xFF;
  mydata[5] = LongitudeBinary & 0xFF;

  // altitudeGps in meters into unsigned int
  mydata[6] = ( altitudeGps >> 8 ) & 0xFF;
  mydata[7] = altitudeGps & 0xFF;

  // hdop in tenths of meter
  mydata[8] = accuracy & 0xFF;
}

void process_gps_values()
{ 
  // retrieve some usefull values from GPS library
  float flat, flon, alt;
  unsigned long age; 
  int hdopNumber;  
  
  gps.f_get_position(&flat, &flon, &age);  // lat -90.0 .. 90.0 as a 4 byte float, lon -180 .. 180 as a 4 byte float, age in 1/1000 seconds as a 4 byte unsigned long
  alt = gps.f_altitude();    // signed float altitude in meters
  hdopNumber = gps.hdop();   // int 100ths of a meter

  // check if possibly invalid
  GPS_values_are_valid = true;
  if (flat == TinyGPS::GPS_INVALID_F_ANGLE)    GPS_values_are_valid = false;
  if (flon == TinyGPS::GPS_INVALID_F_ANGLE)    GPS_values_are_valid = false;
  if (hdopNumber == TinyGPS::GPS_INVALID_HDOP) GPS_values_are_valid = false;
  if (age == TinyGPS::GPS_INVALID_AGE)         GPS_values_are_valid = false;
  
  if (alt == TinyGPS::GPS_INVALID_F_ALTITUDE)  GPS_values_are_valid = false;   // if alt, hdop remain giving errors, possibly the GPS character read misses every start few characters of every feed. Solution: make the code lighter so it returns quicker to character read. Or process a bit of buffer while doing other actions, see TinyGPS example.

  // if valid, put into buffer
  if (GPS_values_are_valid) put_gpsvalues_into_sendbuffer( flat, flon, alt, hdopNumber);
    // after init, sendbuffer holds 0,0 lovation; after first fix it will retain the last valid location
  
  #ifdef DEBUG
  Serial.println();
  Serial.print("Data: ");
  if (GPS_values_are_valid) Serial.print("(valid) ");
  if (!GPS_values_are_valid) Serial.print("(** INVALID");
  if (flat == TinyGPS::GPS_INVALID_F_ANGLE)    {Serial.print(" lat="); Serial.print(flat);}
  if (flon == TinyGPS::GPS_INVALID_F_ANGLE)    {Serial.print(" lon="); Serial.print(flon);}
  if (hdopNumber == TinyGPS::GPS_INVALID_HDOP) {Serial.print(" hdop="); Serial.print(hdopNumber);}
  if (age == TinyGPS::GPS_INVALID_AGE)         {Serial.print(" age="); Serial.print(age);}
  if (alt == TinyGPS::GPS_INVALID_F_ALTITUDE)  {Serial.print(" alt="); Serial.print(alt);}
  if (!GPS_values_are_valid) Serial.print(" **) ");
  Serial.print("  LAT, LON=");
  Serial.print( flat, 6);   
  Serial.print(", ");
  Serial.print(flon, 6); // 52.632656, 4.738389
  Serial.print(" hdop=");
  Serial.print( hdopNumber);
  Serial.print(" alt=");
  Serial.print( alt );
  Serial.print(" AGE=");
  Serial.print(age);
  Serial.println("");
  #endif  // debug
  
  #ifdef DEBUG_XL
  unsigned long chars = 0;
  unsigned short sentences = 0, failed = 0;
  gps.stats(&chars, &sentences, &failed);  
  Serial.print(" CHARS=");
  Serial.print(chars);
  Serial.print(" SENT=");
  Serial.print(sentences);
  Serial.print(" ERR=");
  Serial.print(failed);  
  //uint32_t sat; 
  //sat = gps.satellites();  
  //Serial.print(" SAT=");
  //Serial.print( sat);  
  if (chars == 0)
    Serial.println("** No characters from GPS: check wiring **");
  else if (age > 5000)
    Serial.println("Warning: possible stale GPS data (age over 5 seconds)");
  else
    Serial.println("GPS Data is fresh (age less than 5 seconds)");
  Serial.print("For TTN message LatitudeBinary, LongitudeBinary, altitudeGps, accuracy: ");
  Serial.println("expected   CA DA F. 83 5E 9. 0 .. .. " );     
  Serial.println("    dummy   7F FF FF 7F FF FF 0 0 0 " );    
  Serial.print(  "mydata[] = ");
  Serial.print( mydata[0], HEX );
  Serial.print(" ");
  Serial.print( mydata[1], HEX );
  Serial.print(" ");
  Serial.print( mydata[2], HEX );
  Serial.print(" ");
  Serial.print( mydata[3], HEX );
  Serial.print(" ");
  Serial.print( mydata[4], HEX );
  Serial.print(" ");
  Serial.print( mydata[5], HEX );
  Serial.print(" ");
  Serial.print( mydata[6], HEX );
  if (message_size>6) Serial.print(" ");
  if (message_size>6) Serial.print( mydata[7], HEX );
  if (message_size>7) Serial.print(" ");
  if (message_size>7) Serial.print( mydata[8], HEX );
  if (message_size>8) Serial.print(" / ");
  if (message_size>8) Serial.print( mydata[9], HEX );
  if (message_size>9) Serial.print(" ");
  if (message_size>9) Serial.print( mydata[10], HEX );
  if (message_size>10) Serial.print(" ");
  if (message_size>10) Serial.print( mydata[11], HEX );
  if (message_size>11) Serial.print(" ");
  if (message_size>11) Serial.print( mydata[12], HEX );
  Serial.println("]");
  #endif    // debug_xl
}

void calcChecksum(byte *checksumPayload, byte payloadSize) {
  byte CK_A = 0, CK_B = 0;
  for (int i = 0; i < payloadSize ;i++) {
    CK_A = CK_A + *checksumPayload;
    CK_B = CK_B + CK_A;
    checksumPayload++;
  }
  *checksumPayload = CK_A;
  checksumPayload++;
  *checksumPayload = CK_B;
}

// Send a byte array of UBX protocol to the GPS  https://ukhas.org.uk/guides:ublox6
void sendUBX(uint8_t *MSG, uint8_t len) {
  for(int i=0; i<len; i++) {
    ss.write(MSG[i]);
    Serial.print(MSG[i], HEX);
  }
  ss.println();
}

// Calculate expected UBX ACK packet and parse UBX response from GPS  https://ukhas.org.uk/guides:ublox6
boolean getUBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
  Serial.print(" * Reading ACK ");
 
  // Construct the expected ACK packet    
  ackPacket[0] = 0xB5;  // header
  ackPacket[1] = 0x62;  // header
  ackPacket[2] = 0x05;  // class
  ackPacket[3] = 0x01;  // id
  ackPacket[4] = 0x02;  // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2];  // ACK class
  ackPacket[7] = MSG[3];  // ACK id
  ackPacket[8] = 0;   // CK_A
  ackPacket[9] = 0;   // CK_B
 
  // Calculate the checksums
  for (uint8_t i=2; i<8; i++) {
    ackPacket[8] = ackPacket[8] + ackPacket[i];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  } 
  while (1) { 
    // Test for success
    if (ackByteID > 9) {
      // All packets in order!
      Serial.println(" (GPS cmd okay)");
      return true;
    }
 
    // Timeout if no valid response in 3 seconds
    if (millis() - startTime > 3000) { 
      Serial.println(" (GPS cmd timeout)");
      //ss.flush();  // try to fix
      return false;
    }
 
    // Make sure data is available to read
    if (ss.available()) {
      b = ss.read();
 
      // Check that bytes arrive in sequence as per expected ACK packet
      if (b == ackPacket[ackByteID]) { 
        ackByteID++;
        Serial.print(b, HEX);
      } 
      else {
        ackByteID = 0;  // Reset and look again, invalid order
      }
 
    }
  }
}

void gps_init() {
    // load the send buffer with dummy location 0,0. This location 0,0 is recognized as dummy by TTN Mapper and will be ignored
    put_gpsvalues_into_sendbuffer( 0, 0, 0, 0);
    
    // GPS serial starting
    ss.begin(9600);         // software serial with GPS module. Reviews tell us software serial is not best choice; 
                            // https://www.pjrc.com/teensy/td_libs_TinyGPS.html explains to use UART Serial or NewSoftSerial 
  
    //   https://ukhas.org.uk/guides:ublox6
    // THE FOLLOWING COMMAND SWITCHES MODULE TO 4800 BAUD
    // THEN SWITCHES THE SOFTWARE SERIAL TO 4800 BAUD
    //for what reason would we want this, stability of read maybe?
    //ss.print("$PUBX,41,1,0007,0003,4800,0*13\r\n"); 
    //ss.begin(4800);
    //ss.flush();
    
//    gps_SetMode_gpsOn();
//    gps_setNavMode(6);  // 6 = airborne 1g, max 50km
//    //gps_setDataRate(1);
//    gps_setStrings();
//    gps_setPowerMode(2);
 
//    // infinite loop for GPS testing...
//    while(1)  {
//      if(ss.available()) {
//        char c = ss.read();
//        Serial.write(c); 
//        if (gps.encode(c)) {   // Did a new valid sentence come in?
//          Serial.print(" [valid] ");
//        }
//      }
//    }
  
}

void gps_read_data_and_adjust_power() {
  Serial.println("\nRead GPS ");
    char c;
    unsigned long start = millis();
    do {   
      while (ss.available()) {
        char c = ss.read();
        //Serial.write(c); // uncomment this line if you want to see the GPS data flowing
        #ifdef DEBUG_XL
        Serial.write(c); // uncomment this line if you want to see the GPS data flowing
        #endif
        
        if (gps.encode(c)) { // Did a new valid sentence come in?
            process_gps_values();
            //show me something
            Serial.print("gps ");
            
         }          
      }
    } while (millis() - start < 5000); // explanation:
    // keep xxxx millis focussed on reading the ss. the datablurp will be less than one second
    // a 3 second focus also works great if gps is in power saving mode
    // if too low a value the gps blurp of data will be interrupted and incomplete due conflicting system interrupts
    // if too high a value then system wil delay scheduled jobs and the LMIC send sequence will take too long
}

void gps_setNavMode(int mode) {
  Serial.print("\nGPS Set Nav mode ");    
  Serial.println(mode);

  //Generate the configuration string for Navigation Mode
  //  [0]NavMode, https://wiki.paparazziuav.org/wiki/Module/GPS_UBlox_UCenter
  //    xx 0x00 = Portable, deviation=medium. max 12km height, maz speed 310 m/s, vert 50 m/s  
  //    xx 0x02 = Stationary timing applications, deviation=small. max 9km heighth, max speed 10 m/s, vert 6 m/s 
  //    0x03 = Pedestrian Mode, deviation=small. max 9km height, max speed 30m/s, vert 20m/s 
  //    0x04 = Automotive Mode, deviation=medium. max 5km heightm max speed 62 m/s, vert 15 m/s 
  //    0x05 = Sea Mode, deviation=medium. max alt 500 meters, max speed 25 m/s, vert 5 m/s
  //    0x06 = Airborne < 1G Mode, deviation=large. no 2d fix supported, max 50km height, max speed 250 m/s, vert 100 m/s; high altitude ballooning, less accurate
  //    0x07 = Airborne < 2G Mode, deviation=large. recommended for typical airborne, no 2d fix supported, max 50km height, max speed 250 m/s, vert 100 m/s;
  //    0x08 = Airborne < 4G Mode, deviation=large. only for extremely dynamic, no 2d fix supported, max 50km height, max speed 500 m/s, vert 100 m/s;
  //    0x09 = wrist worn watch (not supported in protocol versions less than 18)
  
  byte arrCommand[] = {
        0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 
          mode, 
        0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x05, 0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 
        0x2C, 0x01, 0x00,  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  
  byte gps_okay = 0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }

  //  ----- the internet has told us:
  //  #define UBLOX_STATIONARY 0xB5,0x62,0x06,0x24,0x24,0x00,0xFF,0xFF, 0x03,0x02,0x00,0x00,0x00,0x00,0x10,0x27,0x00,0x00,0x05,0x00,0xFA,0x00,0xFA,0x00,0x64,0x00,0x2C,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x12,0x54
  //  #define UBLOX_PEDESTRIAN 0xB5,0x62,0x06,0x24,0x24,0x00,0xFF,0xFF, 0x03,0x03,0x00,0x00,0x00,0x00,0x10,0x27,0x00,0x00,0x05,0x00,0xFA,0x00,0xFA,0x00,0x64,0x00,0x2C,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x13,0x76
  //  #define UBLOX_AUTOMOTIVE 0xB5,0x62,0x06,0x24,0x24,0x00,0xFF,0xFF, 0x04,0x03,0x00,0x00,0x00,0x00,0x10,0x27,0x00,0x00,0x05,0x00,0xFA,0x00,0xFA,0x00,0x64,0x00,0x2C,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x14,0x98
  //  #define UBLOX_SEA        0xB5,0x62,0x06,0x24,0x24,0x00,0xFF,0xFF, 0x05,0x03,0x00,0x00,0x00,0x00,0x10,0x27,0x00,0x00,0x05,0x00,0xFA,0x00,0xFA,0x00,0x64,0x00,0x2C,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x15,0xBA
  //  #define UBLOX_AIRBORN    0xB5,0x62,0x06,0x24,0x24,0x00,0xFF,0xFF, 0x06,0x03,0x00,0x00,0x00,0x00,0x10,0x27,0x00,0x00,0x05,0x00,0xFA,0x00,0xFA,0x00,0x64,0x00,0x2C,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x16,0xDC

  //Dynamic Model '6' Aiborne < 1g Large Deviation
  //0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x05, 0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0xDC
  //Dynamic Model '3' Pedestrian Small Deviation
  //0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x05, 0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x76         

}

void gps_setDataRate(int rate) {
//  Serial.println("\nGPS Set data rate ");
//  Serial.println(rate);
//
////    //DataRate:  http://playground.arduino.cc/UBlox/GPS
////    //1Hz     = 0xE8 0x03
////    //2Hz     = 0xF4 0x01
////    //3Hz     = 0x4D 0x01
////    //3.33Hz  = 0x2C 0x01
////    //4Hz     = 0xFA 0x00
////    //5Hz     = 0xC8 0x00
//
//  byte datarate01;
//  byte datarate02;
//  switch (rate) {
//    case 1:
//      datarate01 = 0xE8;
//      datarate02 = 0x03;
//      break;
//    case 2:
//      datarate01 = 0xF4;
//      datarate02 = 0x01;
//      break;
//    case 3:
//      datarate01 = 0x4D;
//      datarate02 = 0x01;
//      break;
//    case 4:
//      datarate01 = 0xFA;
//      datarate02 = 0x00;
//      break;
//    default: 
//      // assume 5
//      datarate01 = 0xC8;
//      datarate02 = 0x00;
//      break;
//    break;
//  }
//  
//  byte arrCommand[] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, datarate01, datarate02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
//  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
//  byte gps_okay=0;
//  while(!gps_okay) {
//    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
//    gps_okay=getUBX_ACK(arrCommand);
//  }
//    
////  #define UBLOX_1HZ        0xB5,0x62,0x06,0x08,0x06,0x00,   0xE8,0x03,  0x01,0x00,0x01,0x00,0x01,0x39       // set rate to 1Hz
////  #define UBLOX_2HZ        0xB5,0x62,0x06,0x08,0x06,0x00,   0xF4,0x01,  0x01,0x00,0x01,0x00,0x0B,0x77       // set rate to 2Hz
////  #define UBLOX_3HZ        0xB5,0x62,0x06,0x08,0x06,0x00,   0x4D,0x01,  0x01,0x00,0x01,0x00,0x64,0x8D       // set rate to 3Hz
////  #define UBLOX_4HZ        0xB5,0x62,0x06,0x08,0x06,0x00,   0xFA,0x00,  0x01,0x00,0x01,0x00,0x10,0x96       // set rate to 4Hz
////  #define UBLOX_5HZ        0xB5,0x62,0x06,0x08,0x06,0x00,   0xC8,0x00,  0x01,0x00,0x01,0x00,0xDE,0x6A       // set rate to 5Hz
}

void gps_setStrings() {
  Serial.println("\nGPS switch off some data strings ");

//    // Turning off all GPS NMEA strings apart from GPGGA (fix information) on the uBlox modules
// we need lat, lon, alt, HDOP  --> keep GGA, GSA
    ss.print("$PUBX,40,GLL,0,0,0,0*5C\r\n");  // GLL = Lat/Lon
    ss.print("$PUBX,40,ZDA,0,0,0,0*44\r\n");  // ZDA = date, time
    ss.print("$PUBX,40,VTG,0,0,0,0*5E\r\n");  // VTG = Vector Track and speed over ground
    ss.print("$PUBX,40,GSV,0,0,0,0*59\r\n");  //GSV = Detailed satellite data
//   // ss.print("$PUBX,40,GSA,0,0,0,0*4E\r\n");  // GSA = Overall Satelite data
    ss.print("$PUBX,40,RMC,0,0,0,0*47\r\n");    // RMC = recommended minimum data for GPS, no Alt
////    // can switch off GGA if you want to do manual polling...
//    ss.println("$PUBX,40,GGA,0,0,0,0*5A");   // GGA = Fix information

//    // manual polling is possible to instruct gps output one specific string:
//    ss.println("$PUBX,00*33");

//  #define GGA_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x00,0x00,0xFA,0x0F                            // switch GGA off
//  #define GLL_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x01,0x00,0xFB,0x11                            // switch GLL off
//  #define GSA_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x02,0x00,0xFC,0x13                            // switch GSA off
//  #define GSV_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x03,0x00,0xFD,0x15                            // switch GSV off
//  #define RMC_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x04,0x00,0xFE,0x17                            // switch RMC off
//  #define VTG_OFF          0xB5,0x62,0x06,0x01,0x03,0x00,0xF0,0x05,0x00,0xFF,0x19                            // switch VTG off
//  #define POSLLH_ON        0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x02,0x01,0x0E,0x47                            // set POSLLH MSG rate
//  #define STATUS_ON        0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x03,0x01,0x0F,0x49                            // set STATUS MSG rate
//  #define SOL_ON           0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x06,0x01,0x12,0x4F                            // set SOL MSG rate
//  #define VELNED_ON        0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x12,0x01,0x1E,0x67                            // set VELNED MSG rate
//  #define SVINFO_ON        0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x30,0x01,0x3C,0xA3                            // set SVINFO MSG rate
//  #define TIMEUTC_ON       0xB5,0x62,0x06,0x01,0x03,0x00,0x01,0x21,0x01,0x2D,0x85                            // set TIMEUTC MSG rate
//  #define SBAS_ON          0xB5,0x62,0x06,0x16,0x08,0x00,0x03,0x07,0x03,0x00,0x51,0x08,0x00,0x00,0x8A,0x41   // set WAAS to EGNOS

}

void gps_setBaud() {
  Serial.println("\nGPS set baud rate ");

//  #define UBLOX_115200     0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,0x00,0xC2,0x01,0x00,0x07,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xBE,0x72 //set speed to 115200
//  #define UBLOX_57600      0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,0x00,0xE1,0x00,0x00,0x07,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xDC,0xBD //set speed to 57600
//  #define UBLOX_38400      0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,0x00,0x96,0x00,0x00,0x07,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x91,0x84 //set speed to 38400
//  #define UBLOX_19200      0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,0x00,0x4B,0x00,0x00,0x07,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x46,0x4B //set speed to 19200
//  #define UBLOX_9600       0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,0x80,0x25,0x00,0x00,0x07,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xA0,0xA9 //set speed to 9600

}

void gps_setPowerMode(int mode) {
  Serial.print("\nGPS set power mode ");
  Serial.println(mode);

  byte powerMode;
  // really nice description of power saving modes https://ukhas.org.uk/guides:ublox_psm#ublox_6_power_saving_modes
    
  switch (mode) {
    case 1:
      //Max Performance Mode (default)
      powerMode = 0x00;
      break;
    case 2:
      //Eco Mode Don't want this one but here for reference.
      powerMode = 0x04;
      break;
    default: 
      // assume 3
      //Power Save Mode
      powerMode = 0x01;
      break;
    break;
  }
  byte arrCommand[] = {0xB5, 0x62, 0x06, 0x11, 0x02, 0x00, 0x08, powerMode, 0x22, 0x92};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

void gps_setPowerMode2() {
  Serial.println("\nGPS set cyclic power mode ");

    //CFG-PM2
    //Not sure what the implication of "do not enter 'inactive for search' state when no fix" is.
    //Update Period 1 second do not enter 'inactive for search' state when no fix unchecked (default setting)
    //0xB5, 0x62, 0x06, 0x3B, 0x2C, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x94, 0x02, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x4F, 0xC1, 0x03, 0x00, 0x87, 0x02, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x64, 0x40, 0x01, 0x00, 0x9B, 0x75 
    //Update Period 1 second do not enter 'inactive for search' state when no fix checked
    //0xB5, 0x62, 0x06, 0x3B, 0x2C, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x90, 0x03, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x4F, 0xC1, 0x03, 0x00, 0x86, 0x02, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x64, 0x40, 0x01, 0x00, 0x96, 0xEB
    //Update Period 10 seconds , do not enter 'inactive for search' state when no fix checked
    //0xB5, 0x62, 0x06, 0x3B, 0x2C, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x90, 0x03, 0x00, 0x10, 0x27, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x4F, 0xC1, 0x03, 0x00, 0x87, 0x02, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x64, 0x40, 0x01, 0x00, 0xE4, 0x8B
    //Update Period 10 seconds , do not enter 'inactive for search' state when no fix unchecked
    //0xB5, 0x62, 0x06, 0x3B, 0x2C, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x90, 0x02, 0x00, 0x10, 0x27, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x4F, 0xC1, 0x03, 0x00, 0x86, 0x02, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x64, 0x40, 0x01, 0x00, 0xE1, 0x51
    
}

void gps_SetMode_gpsOff() {
  Serial.println("\nGPS off ");
  
  ////Set GPS to backup mode (sets it to never wake up on its own) minimal current draw <5mA, loses all settings
  //uint8_t GPSoff[] = {0xB5, 0x62, 0x02, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4D, 0x3B};
  ////Restart GPS
  //uint8_t GPSon[] = {0xB5, 0x62, 0x02, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x37};
  
  byte arrCommand[] = {0xB5, 0x62, 0x02, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4D, 0x3B};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

void gps_SetMode_gpsOn() {
  Serial.println("\nGPS on ");

  byte arrCommand[] = {0xB5, 0x62, 0x02, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x37};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

void gps_SetMode_gpsRfOff() {
  Serial.println("\nGPS RF off ");

  ////Switch the RF GPS section off, draws about 5mA, retains its settings, wakes on serial command.
  //uint8_t GPSoff[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00,0x08, 0x00, 0x16, 0x74}
  //uint8_t GPSon[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00,0x09, 0x00, 0x17, 0x76};

  byte arrCommand[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00,0x08, 0x00, 0x16, 0x74};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

void gps_SetMode_gpsRfOn() {
  Serial.println("\nGPS RF on ");

  byte arrCommand[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00,0x09, 0x00, 0x17, 0x76};
  calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

void gps_requestColdStart() {
  Serial.println("\nGPS cold start ");

  //GPS Cold Start (Forced Watchdog)
  //0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0xFF, 0x87, 0x00, 0x00, 0x94, 0xF5

  byte arrCommand[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0xFF, 0x87, 0x00, 0x00, 0x94, 0xF5};
  //calcChecksum(&arrCommand[2], sizeof(arrCommand) - 4);
  byte gps_okay=0;
  while(!gps_okay) {
    sendUBX(arrCommand, sizeof(arrCommand)/sizeof(uint8_t));
    gps_okay=getUBX_ACK(arrCommand);
  }
}

//////////////////////////////////////////////////
// Kaasfabriek routines for rfm95
///////////////////////////////////////////////

// void do_send(osjob_t* j){   20170220  do_send call is no longer scheduled in event handler
void do_send(){  
  // starting vesion was same as https://github.com/tijnonlijn/RFM-node/blob/master/template%20ttnmapper%20node%20-%20scheduling%20removed.ino
    
    Serial.println("\ndo_send ");
    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND, not sending"));
    } else {
        // Prepare upstream data transmission at the next possible time.

        #ifdef DEBUG
        Serial.println("  expected   CA DA F? 83 5E 9? 0 ?? ?? " );  
        Serial.println("    dummy   7F FF FF 7F FF FF 0 0 0 " );    
        #endif  
        Serial.print(" mydata[] = [");
        Serial.print( mydata[0], HEX );
        Serial.print(" ");
        Serial.print( mydata[1], HEX );
        Serial.print(" ");
        Serial.print( mydata[2], HEX );
        Serial.print(" ");
        Serial.print( mydata[3], HEX );
        Serial.print(" ");
        Serial.print( mydata[4], HEX );
        Serial.print(" ");
        Serial.print( mydata[5], HEX );
        Serial.print(" ");
        Serial.print( mydata[6], HEX );
        if (message_size>6) Serial.print(" ");
        if (message_size>6) Serial.print( mydata[7], HEX );
        if (message_size>7) Serial.print(" ");
        if (message_size>7) Serial.print( mydata[8], HEX );
        if (message_size>8) Serial.print(" / ");
        if (message_size>8) Serial.print( mydata[9], HEX );
        if (message_size>9) Serial.print(" ");
        if (message_size>9) Serial.print( mydata[10], HEX );
        if (message_size>10) Serial.print(" ");
        if (message_size>10) Serial.print( mydata[11], HEX );
        if (message_size>11) Serial.print(" ");
        if (message_size>11) Serial.print( mydata[12], HEX );
        Serial.print("]    ");
        
        Serial.print("DR [ ");
        Serial.print( LMIC_DR_sequence_index );
        Serial.print(" ] = ");
        Serial.print( LMIC_DR_sequence[LMIC_DR_sequence_index] );
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF7) Serial.print(" DR_SF7 "); 
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF8) Serial.print(" DR_SF8 "); 
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF9) Serial.print(" DR_SF9 "); 
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF10) Serial.print(" DR_SF10 "); 
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF11) Serial.print(" DR_SF11 "); 
        if ( LMIC_DR_sequence[LMIC_DR_sequence_index]==DR_SF12) Serial.print(" DR_SF12 "); 
        
        // Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library)
        // for the ttn mapper always use SF7. For Balloon, up to SF12 can be used, however that will require 60 minutes quiet time
        LMIC_setDrTxpow(LMIC_DR_sequence[LMIC_DR_sequence_index],14);   // void LMIC_setDrTxpow (dr_t dr, s1_t txpow)... Set data rate and transmit power. Should only be used if data rate adaptation is disabled.
        
        LMIC_DR_sequence_index = LMIC_DR_sequence_index + 1;
        if (LMIC_DR_sequence_index >= LMIC_DR_sequence_count) LMIC_DR_sequence_index=0;

        // NOW SEND SOME DATA OUT
        //  LMIC_setTxData2( LORAWAN_APP_PORT, LMIC.frame, LORAWAN_APP_DATA_SIZE, LORAWAN_CONFIRMED_MSG_ON );
        LMIC_setTxData2(1, mydata, message_size, 0);   
        Serial.println(" - Packet queued");
    }
    // Next TX is scheduled after TX_COMPLETE event.
}

// event gets hooked into the system
void onEvent (ev_t ev) {
    Serial.println("\n\nonEvent was called    ************************** ");
    Serial.print(os_getTime());
    Serial.print(": ");
    switch(ev) {
        case EV_SCAN_TIMEOUT:
            Serial.println(F("EV_SCAN_TIMEOUT"));
            break;
        case EV_BEACON_FOUND:
            Serial.println(F("EV_BEACON_FOUND"));
            break;
        case EV_BEACON_MISSED:
            Serial.println(F("EV_BEACON_MISSED"));
            break;
        case EV_BEACON_TRACKED:
            Serial.println(F("EV_BEACON_TRACKED"));
            break;
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            break;
        case EV_RFU1:
            Serial.println(F("EV_RFU1"));
            break;
        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            break;
        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            break;
        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
            TX_COMPLETE_was_triggered = 1;  // 20170220 our custom code see https://github.com/tijnonlijn/RFM-node/blob/master/template%20ttnmapper%20node%20-%20scheduling%20removed.ino
            
            if (LMIC.txrxFlags & TXRX_ACK)
              Serial.println(F("Received ack"));
            if (LMIC.dataLen) {
              Serial.println(F("Received "));
              Serial.println(LMIC.dataLen);
              Serial.println(F(" bytes of payload"));
            }
       //     // Schedule next transmission   20170220 disabled the interrupt chain, now all controll in main Loop
       //     os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
            break;
        case EV_LOST_TSYNC:
            Serial.println(F("EV_LOST_TSYNC"));
            break;
        case EV_RESET:
            Serial.println(F("EV_RESET"));
            break;
        case EV_RXCOMPLETE:
            // data received in ping slot
            Serial.println(F("EV_RXCOMPLETE"));
            break;
        case EV_LINK_DEAD:
            Serial.println(F("EV_LINK_DEAD"));
            break;
        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            break;
         default:
            Serial.println(F("Unknown event"));
            break;
    }
}

void lmic_init() {
    // LMIC init
    os_init();
    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    // Set static session parameters. Instead of dynamically establishing a session
    // by joining the network, precomputed session parameters are be provided.
    #ifdef PROGMEM
    // On AVR, these values are stored in flash and only copied to RAM
    // once. Copy them to a temporary buffer here, LMIC_setSession will
    // copy them into a buffer of its own again.
    uint8_t appskey[sizeof(APPSKEY)];
    uint8_t nwkskey[sizeof(NWKSKEY)];
    memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
    memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
    LMIC_setSession (0x1, DEVADDR, nwkskey, appskey);
    #else
    // If not running an AVR with PROGMEM, just use the arrays directly
    LMIC_setSession (0x1, DEVADDR, NWKSKEY, APPSKEY);
    #endif

    #if defined(CFG_eu868)
    // Set up the channels used by the Things Network, which corresponds
    // to the defaults of most gateways. Without this, only three base
    // channels from the LoRaWAN specification are used, which certainly
    // works, so it is good for debugging, but can overload those
    // frequencies, so be sure to configure the full frequency range of
    // your network here (unless your network autoconfigures them).
    // Setting up channels should happen after LMIC_setSession, as that
    // configures the minimal channel set.
    // NA-US channels 0-71 are configured automatically
    LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);      // g-band
    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);      // g2-band
    // TTN defines an additional channel at 869.525Mhz using SF9 for class B
    // devices' ping slots. LMIC does not have an easy way to define set this
    // frequency and support for class B is spotty and untested, so this
    // frequency is not configured here.
    #elif defined(CFG_us915)
    // NA-US channels 0-71 are configured automatically
    // but only one group of 8 should (a subband) should be active
    // TTN recommends the second sub band, 1 in a zero based count.
    // https://github.com/TheThingsNetwork/gateway-conf/blob/master/US-global_conf.json
    LMIC_selectSubBand(1);
    #endif

    // Disable data rate adaptation - per http://platformio.org/lib/show/842/IBM%20LMIC%20framework%20v1.51%20for%20Arduino
    //      and http://www.developpez.net/forums/attachments/p195381d1450200851/environnements-developpement/delphi/web-reseau/reseau-objet-connecte-lorawan-delphi/lmic-v1.5.pdf/
    //LMIC_setAdrMode(0);     // Enable or disable data rate adaptation. Should be turned off if the device is mobile
    // Disable link check validation
    LMIC_setLinkCheckMode(0);  //Enable/disable link check validation. Link check mode is enabled by default and is used to periodically verify network connectivity. Must be called only if a session is established.
    // Disable beacon tracking
    //LMIC_disableTracking ();  // Disable beacon tracking. The beacon will be no longer tracked and, therefore, also pinging will be disabled.
    // Stop listening for downstream data (periodical reception)
    //LMIC_stopPingable();  //Stop listening for downstream data. Periodical reception is disabled, but beacons will still be tracked. In order to stop tracking, the beacon a call to LMIC_disableTracking() is required

    // TTN uses SF9 for its RX2 window.
    LMIC.dn2Dr = DR_SF9;

    // Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library)
    LMIC_setDrTxpow(DR_SF7,14);   // void LMIC_setDrTxpow (dr_t dr, s1_t txpow)... Set data rate and transmit power. Should only be used if data rate adaptation is disabled.
}


///////////////////////////////////////////////
//  some other measurements
///////////////////////////////////////////

double GetTemp(void) { //http://playground.arduino.cc/Main/InternalTemperatureSensor

  unsigned int wADC;
  double t;
  
  // The internal temperature has to be used with the internal reference of 1.1V.
  // Set the internal reference and mux.
  ADMUX = (_BV(REFS1) | _BV(REFS0) | _BV(MUX3));
  ADCSRA |= _BV(ADEN);  // enable the ADC
  delay(20);            // wait for voltages to become stable.
  ADCSRA |= _BV(ADSC);  // Start the ADC

  // Detect end-of-conversion
  while (bit_is_set(ADCSRA,ADSC));  
  // Reading register "ADCW" takes care of how to read ADCL and ADCH.
  wADC = ADCW;
  
  t = (wADC - 324.31 ) / 1.22;  // The offset of 324.31 could be wrong. It is just an indication.
  // The returned temperature is in degrees Celsius.
  t = t - 10; // before this correction we had readings of 30 in a room which was 20 deg C, readings of 20 outdoors when it was 6 deg C

  return (t);
}

long readVcc() {  //http://dumbpcs.blogspot.nl/2013/07/arduino-secret-built-in-thermometer.html
  long result;
  // Read 1.1V reference against AVcc
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Convert
  while (bit_is_set(ADCSRA,ADSC));
  result = ADCL;
  result |= ADCH<<8;
  result = 1126400L / result; // Back-calculate AVcc in mV
  return result;
}

void put_other_values_into_sendbuffer() {
  long vcc = readVcc();
  uint8_t vccBinary = vcc /20 ;  // rescale 0-5100 milli volt into 0 - 255 values
  mydata[9] = vccBinary;
  #ifdef DEBUG
  Serial.print("Vcc = ");
  Serial.print(vcc);
  Serial.print(" milli Volt. vccBinary = ");
  Serial.print(vccBinary);
  #endif

  double temperature = GetTemp();
  uint8_t temperatureBinary = temperature + 100;   // rescale -100 to 155 into 0 - 255 values
  mydata[10] = temperatureBinary;
  #ifdef DEBUG
  Serial.print(" Temperature = ");
  Serial.print(temperature);
  Serial.print(" temperatureBinary = ");
  Serial.println(temperatureBinary);
  #endif
}


///////////////////////////////////////////////
//  arduino init and main
///////////////////////////////////////////

void setup() {
    Serial.begin(115200);   // whether 9600 or 115200; the gps feed shows repeated char and cannot be interpreted, setting high value to release system time
    
    Serial.print("\n\nStarting "); Serial.print(myDeviceName); Serial.print(" ("); Serial.print(DEVADDR); Serial.println(") ");
    Serial.println();


    gps_init();
    lmic_init();  
    
}


void loop() {

    // TODO: rewrite to make linear:
    // 1. get gps data
    //  1a. wake up gps, resume previous performance level
    //  1b. read a few cycles, adjust power settings, store any received good gps value
    //    - not ever had a fix:                           go to 100% full performance
    //    - not had one fix in previous 5x cycle time:    go to 100% full performance
    //    - not had one fix in previous 2x cycle time:    go to half performance
    //    - not had one fix in previous cycle time:       go to half performance
    //    - had one fix in previous cycle time:           go to half performance
    //    - had 3 or more fix in previous cycle time:     go to half performance
    //    - had 1 fix in current cycle time:              go to half performance
    //    - had 3 or more fix in current cycle time:      go to sleep mode, go to next step
    //  1c. put gps into snooze mode
    // 2. get other values = VCC, cpu_temp, processing time + prev send time in percent of total
    // 3. send out TTN message as per example https://github.com/tijnonlijn/RFM-node/blob/master/template%20ttnmapper%20node%20-%20scheduling%20removed.ino
    //  3a. queue a message 
    //  3b. wait and process system interrupts till TX complete
    // 4. sleep to fill up time so one complete sycle is as per definition
  
    Serial.println();
    Serial.println("\nRead GPS ");

    //gps_wakeup();
    gps_read_data_and_adjust_power();
    //gps_Snooze();

    Serial.println("\other values ");
    put_other_values_into_sendbuffer();
    
    Serial.println("\nSending TTN message ");
    do_send();
    Serial.println("Waiting..");  
    while (TX_COMPLETE_was_triggered == 0) {
      os_runloop_once();     // system picks up just the first job from all scheduled jobs, needed for the scheduled and interrupt tasks
    }
    TX_COMPLETE_was_triggered = 0;
    Serial.println("TX_COMPLETE");
    
    
//    Serial.println("\nSleep the GPS ");
//    gps_SetMode_gpsOff();
    //gps_SetMode_gpsRfOff();
    //delay(1000);// not sure if needed, might be needed to allow serial stream to shut down
    //LowPower.powerDown(SLEEP_4S, ADC_OFF, BOD_OFF);  
    //Serial.println("sleep2 ");
    //Sleepy::loseSomeTime(8000);  // max 60.000 (60 sec) 
    //Serial.println("delay ");

    // sleep does not work
    delay(20000);
    
//    gps_SetMode_gpsOn();
//    //gps_SetMode_gpsRfOn();
    delay(1000);  // not sure if needed, might be needed to allow serial stream to wake up 
    Serial.println("sleep completed");
}

void loop_old() {
//
//    Serial.println("\nRead GPS.. ");
//    char c;
//    unsigned long start = millis();
//    do {   
//      while (ss.available()) {
//        char c = ss.read();
//        //Serial.write(c); // uncomment this line if you want to see the GPS data flowing
//        #ifdef DEBUG_XL
//        Serial.write(c); // uncomment this line if you want to see the GPS data flowing
//        #endif
//        
//        if (gps.encode(c)) { // Did a new valid sentence come in?
//            process_gps_values();
//            //show me something
//            Serial.print("gps ");
//            
//            // allow energy saving mode only if a fix has been achieved
//            if(GPS_values_are_valid && !gpsEnergySavingWantsToActivate && !gpsEnergySavingActivated ) { 
//              Serial.println("\nFirst gps fix found, counting down to switch to Energy Saving. ");
//              gpsEnergySavingWantsToActivate = true;
//              gpsEnergySavingWantsToActivateStartTime = millis();
//            }  
//         }          
//      }
//    } while (millis() - start < 5000); // explanation:
//    // keep xxxx millis focussed on reading the ss. the datablurp will be less than one second
//    // a 3 second focus also works great if gps is in power saving mode
//    // if too low a value the gps blurp of data will be interrupted and incomplete due conflicting system interrupts
//    // if too high a value then system wil delay scheduled jobs and the LMIC send sequence will take too long
//
//    put_other_values_into_sendbuffer();
//    os_runloop_once();  // system picks up just the first job from all scheduled jobs
//
//    // can we go into energy saving mode yet?
//    if(gpsEnergySavingWantsToActivate && !gpsEnergySavingActivated ) {
//      if(millis() - gpsEnergySavingWantsToActivateStartTime > gpsEnergySavingStartDelayMillis) {
//        Serial.println("\nGwitching GPS to Less frequent data burst. ");
//        uint8_t data[] = {0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 0x10, 0x27, 0x01, 0x00, 0x01, 0x00, 0x4D, 0xDD}; // from u-center software - the changes the data interval to every 10 seconds instead of every 1 second
//        ss.write(data, sizeof(data));
//        
//        gpsEnergySavingWantsToActivate = false;
//        gpsEnergySavingActivated = true;
//      }
//    }
//    
}




