#include <Arduino.h>
#include <DHT.h>
#include "State.h"
#include "Peripherals.h"
#include "config.h"


DHT   dht(DHT_PIN , DHT_TYPE);

void printval(void)
{
    current = analogRead(CURRENT_PIN); // 0 to 4095
    voltage = analogRead(VOLTAGE_PIN); // 0 to 4095
    //to read temperature 
    temperature = dht.readTemperature(DHT_PIN );
    
    Serial.print("current POT value");
    Serial.println(current);
    
    Serial.print("Volatge POT value");
    Serial.println(voltage);
     
     if(isnan(temperature)) // to validate temerature reading 
     {
         Serial.println("ERROR in reading the Temperature");
     }
     else
     {
        Serial.print("Tmerature reading is :-   ");
        Serial.print(temperature, 1);
        Serial.println(" C");

     }


}

void plug_status(void)
{
   // detect the sw is pressed
   // plug in switch is pressed
   // change bay_status FREE to charging
   //update leds

   // plug out switch is pressed
   // change bay_status  charging to FREE
   //update leds





}