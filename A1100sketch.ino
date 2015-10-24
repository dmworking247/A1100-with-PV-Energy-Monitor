/* 
Purpose of sketch
This version will read meter totals and energy direction from the Elster A1100 meter IR port, 
and calculate instantaneous power from the pulsing LED, separating the pulses into distinct 'import' 
and 'export' LEDs which you can then attach a simple monitor like the Watts Clever EW4500. 

The code incorporates a RTC and pushes a 'log file' to the Serial port. 

Later code revisions will publish this over RF to other devices, and/or upload via Ethernet. 
*/


/* 
Source notes:
  
  Many thanks to Pedro (https://pedrosnotes.wordpress.com/2015/09/26/decoding-electricity-meter-elster-a1100-first-steps/) 
  for his adaptation of a sketch for decoding the IR sensor on an Elter A1100 meter, which was in turn originally based on
  Dave's code for an Elter A100C meter:    http://www.rotwang.co.uk/projects/meter.html 
*/

/* 
Hardware information for my project:
1x Arduino UNO
1x RPM7138-R from Jaycar (Australia), catalog number ZD1952. Signal to pin 2, VCC to 5v.  (Reportedly, this is the only sensor to be known to work with tthe Elter meters)
1x Switch light detection sensor from ebay (sends digital high/low signal when light detected above the threshold set by the variable resistor on the module). A0 to Arduino pin 3. VCC to 5v.
1x 5mm LED. Digital pin to annode, 100R resistor on Kathode to ground. Note in my sketch I flash 'two' LEDS (one for import, one for export) but for my hardware I only use one LED on
   the box, and use a 2 way hardware switch to decide which pin to listen to (think of it like a pulse on import/pulse on export switch). 
1x RTC module ZS-042 DS3231 from ebay, running at 5V, SDA and SCL to dedicated pins on Uno (also works on A4/A5)
*/

/*
Change log:
2015-10-22: Added RTC module. "Publish" function now captures datetime.
2015-10-23: Added daily totals, created timeout monitor to prevent near-zero readings (due to low pulse counts) causing
            the code to assume power values from last reading were still true.
*/

// define details for RTC module (remove if you will not have one)
#include <Wire.h>
#include <Time.h>
#include <DS1307RTC.h>

int currenthour = 99; // create variable for hours. Dummy value assigned by default.
int currentminute = 99; // create variable for minutes. Dummy value assigned by default.
// int currentsecond = 99;  // create variable for seconds. Dummy value assigned by default.
int currenttime = 9999; // create variable for time. Dummy value assigned by default.
int currentday = 99; // create variable for month. Dummy value assigned by default.
int currentmonth = 99; // create variable for month. Dummy value assigned by default.
int currentyear = 9999; // create variable for year. Dummy value assigned by default.



// setup parameters for publishing data
const int uploadinterval = 1; // interval in MINUTES that data should be uploaded
const int publishinterval = 5; // interval in SECONDS that data should be displayed (or pushed to a display device)
// unsigned int statusFlag; // superceded by ImportExport string variable
float imports;
float exports;

float baseimport;
float baseexport;
float todayimport;
float todayexport;
boolean startofday;


// setup parameters for local LEDs
const int exportled = 9; // LED used when pulsing exports
const int importled = 8; // LED used when pulsing imports
const int errorled = 13; // LED used for status information. I will illuminate this if an error occurs, suggesting to the operator to check the hardware/software.

// setup paramters for RED LED pulse counting code
//Number of pulses, used to measure energy.
long pulseCount = 0; 

//Used for timer variables to calculate power usage and data publishing intervals.
unsigned long pulseTime,lastTime,lastPublish,lastUpload,lastAvg,lastCycle;


//power and energy variables
float power;
int freepower; // amount of available power from PV
int powersample[] = {0, 0, 0, 0, 0};

//Number of pulses per wh - found or set on the meter.
int ppwh = 1; //1000 pulses/kwh = 1 pulse per wh



// setup parameters for IR LED decoding sketch
String ImportExport = "X"; // variable for the direction of energy (I=In/Import, O=Out/Export)

const uint8_t intPin = 2;
#define BIT_PERIOD 860 // us
#define BUFF_SIZE 64
volatile long data[BUFF_SIZE];
volatile uint8_t in;
volatile uint8_t out;
volatile unsigned long last_us;
uint8_t dbug = 0;



float last_data;
uint8_t sFlag;
float imps;
float exps;
uint16_t idx=0;
uint8_t byt_msg = 0;
uint8_t bit_left = 0;
uint8_t bit_shft = 0;
uint8_t pSum = 0;
uint16_t BCC = 0;
uint8_t eom = 1;


void setup() 
{
Serial.begin(115200); // start serial monitor  
  while (!Serial) ; // wait for serial
  {
  delay(200);
  }
  
// required for IR pulse reading
    in = out = 0; 
    last_us = micros(); // set last_us to 'now'

attachInterrupt(digitalPinToInterrupt(2), onIR, RISING);
    
 
// required for red LED pulse reading  
attachInterrupt(digitalPinToInterrupt(3), onPulse, RISING);

// initialize timer variables to prevent instant publish/upload data has been updated
lastPublish = millis(); // set last publish to 'now'
lastUpload = millis(); // set last upload to 'now'

pinMode(importled,OUTPUT);
pinMode(exportled,OUTPUT);
pinMode(errorled,OUTPUT);

Serial.println(F("Startup complete! Waiting for first IR data."));

// readRTC(); // determine current time <- note for some reason if you call this during setup, I2C will lock up when it is called again later.

startofday = 1; // Tell the sketch that we are at the start of a new day (during startup) to begin accumulating totals.

} // end setup





void loop() 
{
  

  
if ((currenthour < 1) && (imports > 0)) // determine if it is past midnight and we have data from the meter
{
startofday = 1; // toggle flag to indicate it is the start of a new day (this will be set to 0 as soon as start of day logic has been executed)
}

// what to do at the beginning of a new day (or upon initial startup) Imports is tested to ensure we have readings.
if ((startofday == 1) && (imports > 0)) 
{
baseimport = imports;
baseexport = exports;
startofday = 0; // reset start of day flag so that this baselining only happens once
Serial.println("Reset daily counters.");
}

// turn off pulse leds after 20ms
if ( (digitalRead(importled) == HIGH) || (digitalRead(exportled) == HIGH) )
{
delay(25);
digitalWrite(importled,LOW);
digitalWrite(exportled,LOW);
}


// update average available power stats
if (( millis() - lastAvg) > 200000) // every 3.3 mins
{
poweraverages(); // take averages of available power from the last few samples 
lastAvg = millis(); // reset timer for this routine 
}


// update date/time every second and check whether we want to push out updates
if (( millis() - lastCycle) > 1000)
{
readRTC();
checktimes(); // test whether its time to publish/upload data
lastCycle = millis(); // record the last time we ran this funciton
}


  int rd = decode_buff();
  if (!rd) return;
  if (rd==3) 
  {
   rd=4; 
   /*
   Serial.println("")
   Serial.print(imports);    Serial.print("\t");
   Serial.print(exports);    Serial.print("\t");
   Serial.print(ImportExport); Serial.println("");  
   */  
  }
  
  

}




/********************* FUNCTION FOR DECODING IR MESSAGES FROM A1100 ****************/
static int decode_buff(void) {
   if (in == out) return 0;
   int next = out + 1;
   if (next >= BUFF_SIZE) next = 0;
   int p = (((data[out]) + (BIT_PERIOD/2)) / BIT_PERIOD);
   
   if (p>500) {
     idx = BCC = eom = imps = exps = sFlag = 0;   
     out = next;
     return 0;
   }
   bit_left = (4 - (pSum % 5));
   bit_shft = (bit_left<p)? bit_left : p;
   pSum = (pSum==10)? p : ((pSum+p>10)? 10: pSum + p);
   if (eom==2 && pSum>=7) {
      pSum=pSum==7?11:10;
      eom=0;   
   }

   if (bit_shft>0) {
      byt_msg >>= bit_shft;
      if (p==2) byt_msg += 0x40<<(p-bit_shft);
      if (p==3) byt_msg += 0x60<<(p-bit_shft);
      if (p==4) byt_msg += 0x70<<(p-bit_shft);   
      if (p>=5) byt_msg += 0xF0;
    }
//    Serial.print(p); Serial.print(" ");Serial.print(pSum);Serial.print(" ");    
//    Serial.print(bit_left);Serial.print(" ");Serial.print(bit_shft);Serial.print(" ");    
//    Serial.println(byt_msg,BIN);
    if (pSum >= 10) {
       idx++;
       if (idx!=328) BCC=(BCC+byt_msg)&255;
       if (idx>=95 && idx<=101)  
          imps += ((float)byt_msg-48) * pow(10 , (101 - idx));
       if (idx==103) 
          imps += ((float)byt_msg-48) / 10;
       if (idx>=114 && idx<=120) 
          exps += ((float)byt_msg-48) * pow(10 , (120-idx));
       if (idx==122) 
          exps += ((float)byt_msg-48) / 10;
       if (idx==210) 
          sFlag = (byt_msg-48)>>3; //1=Exporting ; 0=Importing       
       if (byt_msg == 3) eom=2; 
       if (idx==328) {
          if ((byt_msg>>(pSum==10?(((~BCC)&0b1000000)?0:1):2))==((~BCC)&0x7F)) {
             if (last_data != (imps + exps + sFlag)) // has data from IR port changed from last read
             {
                imports=imps;
                exports=exps;
                
                // use value of sflag to set a more intuative value for import/export
                if (sFlag == 1)
                  {ImportExport = "EXPORTING";} 
                else if (sFlag == 0)
                  {ImportExport = "IMPORTING";}
                  
                  

                
                last_data = imps + exps + sFlag;
                out = next;
                return 3;
             }
          }
          if (dbug) {
             Serial.println(""); Serial.print("---->>>>");
             Serial.print(imps); Serial.print("\t");
             Serial.print(exps); Serial.print("\t");
             Serial.print(sFlag); Serial.print("\t"); 
             Serial.print(pSum); Serial.print("\t");              
             Serial.print(byt_msg>>(pSum==10?1:2),BIN); Serial.print("\t"); //BCC read
             Serial.print((~BCC)&0x7F,BIN); Serial.print("\t"); //BCC calculated

          }
       }  
    }
    out = next;
    return 0;
}


/********************* FUNCTION WHEN IR DATA RECIEVED ****************/
void onIR()
{
   unsigned long us = micros();
   unsigned long diff = us - last_us;
   if (diff > 20 ) 
   {
      last_us = us;
      int next = in + 1;
      if (next >= BUFF_SIZE) next = 0;
      data[in] = diff;
      in = next;
   }
}


/********************* READING LED PULSE TO DETERMINE CURRENT USAGE ****************/
// The interrupt routine for reading the red LED on A1100 meter
void onPulse()
{

//used to measure time between pulses.
  lastTime = pulseTime; // set the last time to the 'now Time' of the previous execution
  pulseTime = micros(); // set pulseTime to NOW



//Calculate power
  float powercalc = (3600000000.0 / (pulseTime - lastTime))/ppwh;
        //power = (3600000000.0 / (pulseTime - lastTime))/ppwh;
  
// ignore erroneous readings caused by the interrupt triggering a second time from the same light pulse on some cheap light sensors 
/* This can be minimized by putting a 10k pulldown resistor on your sensor. The if statement below assumes that any instantaneous reading
of more than 50kw is unrealistic for a domestic environment */
    if (powercalc < 50000)
      {
      power = powercalc;
      
       
      pulseon();     // flash our own pulse LED (in case you want to attach a pulse monitor to the arduino kit instead of your meter)
        
        
    if (power > 0) // if we have a power reading, calculate available power
         {
            if (ImportExport == "IMPORTING")       // If importing power, turn power into a negative number
            {
            freepower = -power;
            }
            if (ImportExport == "EXPORTING")
            {
            freepower = (power);
            }
         }
      else
      {
      freepower = 0;
      }
      
        
      //pulseCounter
        pulseCount++;
        
       
      // checktimes(); // test whether its time to publish/upload data
      
           
      }
    else // this is only needed for debugging your LED-reading sensor
      {
      Serial.println(F("Erroneous reading!!"));
      }
}


void pulseon()
{
      /* pulse our own LEDs for import or export according to the power direction. 
      This section is used to differentiate pulses for the A1100 meter which pulses the same regardless
      of which direction the power is going in. If your meter already pulses for power in one direction, 
      you can simplify or change this code */
      if (ImportExport == "IMPORTING")
      {
      digitalWrite(importled,HIGH);
      }
      
      if (ImportExport == "EXPORTING")
      {
      digitalWrite(exportled,HIGH);
      }

}


void checktimes()
{  
  
/* This 'pulse timeout' is necessary for meter that is pulsing for the net energy of PV production and usage (eg using 2kw 
and producing 2kw = 0 pulses or 0 net energy).  The scenario may occur where a momentary spike of net usage goes to (for example) 3kwh, 
but then that spike dissapears taking us back to (near) zero. Since it wil be long time waiting for that next pulse (to recalculate energy), 
our code would otherwise assume the energy use is still 3kwh.  The code below times out at 35 seconds and forces the power reading to zero. 

If and when a pulse does occur the power before this timeout, power will be recalculated precisely. */
// timeout pulses
if ( (power > 100) && (micros() - pulseTime > 36000000) ) // if it has been longer than 36 seconds since we detected a pulse, and previous power reading was over 100w
{
power = 0; // round to zero power usage
}
  
  
// update daily running totals
todayimport = (imports - baseimport);
todayexport = (exports - baseexport);

// Print a waiting indicator to the serial port if we still haven't had our first reading from the IR to determine direction
if (imports <= 0)
{
Serial.print(".");
}
  
  
// check whether its time to push stat updates
if (((millis() - lastPublish) > (publishinterval * 1000)) && imports > 0 ) // if its been more than the specified interval since we last published current values
   {
   lastPublish = millis(); // reset timer
   // do whatever you want for displaying or pushing the stats
   // Serial.println("Publishing stats!!!!!!!!!!!!!!!!!");
   

        // date and time
        Serial.print(currentyear);
        Serial.write('-');
        Serial.print(currentmonth);
        Serial.write('-');
        Serial.print(currentday);
        Serial.write(' ');
        print2digits(currenthour);
        Serial.write(':');
        print2digits(currentminute);
   
        // meter readings
        Serial.print(" >");
        Serial.print(ImportExport);
        Serial.print(" | Power(W)/Free/Avg: ");
        Serial.print(power,0); // display power in Watts to 0 decimal places
        Serial.print(" / ");
        Serial.print(freepower);
        Serial.print(" / ");
        // take an average power sample
        int avgpower3 = ( ( powersample[0] + powersample[1] + powersample[2] ) / 3 ); // average available energy for the last 3 samples
        Serial.print( avgpower3 ); // display power in Watts to 0 decimal places. Negative numbers mean importing (buying)
        Serial.print(" | MeterIn:");
        Serial.print(imports,2);
        Serial.print(" | MeterOut:");
        Serial.print(exports,2);
        Serial.print(" | TodayIn :");
        Serial.print(todayimport,2);
        Serial.print(" | TodayOut:");
        Serial.println(todayexport,2);
        
   }

// check whether its time to push stat updates
if ( ( (millis() - lastUpload) > (uploadinterval * 300000) ) && imports > 0) // if its been more than the specified interval since we last published current values
   {
   lastUpload = millis(); // reset timer
   // include whatever code for uploading stats to another device or website
   Serial.println(" ********************** UPLOADING ********************** ");
   }

}


// function for fetching current date and time. Remove if not using an RTC module.

void readRTC()
{
  
  tmElements_t tm;


  if (RTC.read(tm)) 
  {
    
    currenthour = (tm.Hour); // set global variable to current time (hours)
    currentminute = (tm.Minute); // set global variable to current time (minutes)
    // currentsecond = (tm.Second); // set global variable to current time (minutes)
    // currenttime = ((currenthour * 100) + currentminute);
    currentday = (tm.Day); // set global variable to current (date as number)
    currentmonth = (tm.Month); // set global variable to current  (month as a number)
    currentyear =  (tmYearToCalendar(tm.Year));
    
  digitalWrite(errorled,LOW);

  } 
  else 
    {
     if (RTC.chipPresent()) 
      {
      Serial.println("The DS1307 is stopped.  Please run the SetTime");
      Serial.println("example to initialize the time and begin running.");
      Serial.println();
      } 
      else 
      {
      Serial.println("DS1307 read error!  Please check the circuitry.");
      Serial.println();
      }
     digitalWrite(errorled,HIGH); // light up the error LED to attract attention.
    delay(9000);
    }

}


// used for RTC module to append '0' to single digit data, eg hours, days, months. Remove if not using RTC module.
void print2digits(int number) {
  if (number >= 0 && number < 10) {
    Serial.write('0');
  }
  Serial.print(number);
}


void poweraverages()
{
// cycle sample values to have a running array of the last 5 samples of available power
powersample[4] = powersample[3];
powersample[3] = powersample[2];
powersample[2] = powersample[1];
powersample[1] = powersample[0];

powersample[0] = freepower; // set most recent sample to current available power 
}
