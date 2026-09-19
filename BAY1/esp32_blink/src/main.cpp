#include <Arduino.h>
#include "State.h"
#include "config.h"
#include "Peripherals.h"



void setup()
{

    Serial.begin(115200);
    dht.begin();  // initialise sesnor
   
}

unsigned long now;
unsigned long last_print;

void loop()
{
    //print vals every 2 sec
    now = millis();
    if((now - last_print) > 2000)
    {
        last_print = now;
         printval();

    }

    
}

