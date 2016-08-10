#include <OneWire.h>
#include <EncoderCtl.h>
#include <ButtonCtl.h>
#include <TimerOne.h>
#include <EEPROMex.h>
#include <SoftwareSerial.h>
#include <avr/pgmspace.h>

#define FW_VERSION 40
#define FW_EC 17
/* 
 * 
 * Sous vide firmware for Arduino Uno or similar ©Nik Berry 2011-2016.
 * Was going to call it Peggy, but not even I could manage that.
 *  
 */
 
//                  Front panel
//                                        Query   Select   Units
//  ____   ____    ____    _____   ____   +----+   \|/     +--+
//  (On)  (Heat)  (Pump)  (Ready) (Safe)  | ++ |   ---     |  | °C
//   --    ----    ----    -----   ----   | ++ |   /|\     |OO| °F
//  Blue    Red   Orange   Yellow  Green  +----+  Start    +--+ 


// Analogue input pins:


/* 
 *  
 * Analogue input pin used a digital pin, connected to a DS18B20 
 * temperature sensor.
 * 
 */
 
#define TEMP_SENSOR A0

// Depth sensor inputs, used to measure water depth
// This for the 14" Jameco and 8" Sparkfun sensors (D above).

#define DMEASURE 1

// Next value only for the 8" Sparkfun sensor
// Not actually used yet

#define DCALIBRATE 2

// Digital pins:

// 0 and 1 are used for serial debugging over USB.

// Input:

#define ENCODER_A 3     // Quadrature pins
#define ENCODER_B 4     // for the rotary encoder (Selector™ Marvel Comics)
                        // Note: The EC 0 version of the front panel lacks
                        // 2 100nF caps from A and B to GND. They are green
                        // wired on the bottom of the board.
// Output:

#define TXPIN 2         // LCD serial driver Note: The EC 0 version of the shield has
                        // GND and TX transposed on the header. Be aware.
#define PUMP 8          // Pump output pin & LED
#define HEATER 9        // Heater output pin & LED
#define TEMPREADY 10    // At cooking temp LED
#define BUZZER 11       // Buzzer output pin. Used in two places:
                        // When ready to cook and food needs to be added,
                        // and when cooking is done and food needs to be removed
#define SAFE_LAMP 12    // Green LED - 'Safe to remove lid'

// Input:

#define START_SELECT 5  // Start/select button, momentary push (rotary encoder)
#define QUERY 6         // Select Query mode, momentary push
#define TEMP_UNITS 7    // Select units, °C/cm or °F/in, slider
#define LID_ON 13       // Lid on (microswitches under the lid)

#define ON 1
#define OFF 2

// Macro to control LEDS
#define lamp(x, y) (digitalWrite((x), (y) == ON ? LOW : HIGH))

// Macro to control hardware - heater, pump, buzzer
#define drive(x, y) (digitalWrite((x), (y) == OFF ? LOW : HIGH))

// Macro to convert C to F
#define ctof(x) ((x) * 9.0 / 5.0 + 32.0)

// Fix up choo-choo custom chars (first is 0,
// which would terminate string - so store as 1-8
// and convert here to 0-7.
// #define cadj(c) ((c) <= 8 ? (c) - 1 : (c))


/* 
 *  
 * States for the Finite State Machine
 * 
 */

// Normal cookimg modes

// Just waiting for Cheffy to tell me what to do
#define IDLE_WAIT 0

// Ask for cooking temperature
#define PROG_TEMP 1

// Ask for cooking time
// days
#define PROG_DAYS 2

// hours
#define PROG_HOURS 3

// minutes
#define PROG_MINUTES 4

// Temp & time known, wait till up to temp
#define PREHEAT 5

// Up to temp, wait till Cheffy ready to add food
#define FOOD_WAIT 6

// Wait till Cheffy is adding food
#define FOOD_ADDING 7

// Wait till Cheffy has added food
#define FOOD_ADDED 8

// Cook!
#define COOKING 9

// Done. Drop to holding temp and wait for
// Cheffy to remove the cooked food
#define EMPTY_WAIT 10


// Query mode can be entered from idle or cooking modes. Displays
// depth, heater on/off, pump on/off, duty cycle, and avg. power. 
#define MQUERY 11

// Diagnostic modes

// Displays depth calibration constants m and k,
// current depth, and average and instantaneous power
#define DIAG1 12

// Displays temperature calibration constants m and k,
// and raw temp and depth readings
#define DIAG2 13

// Displays the stack pointer, heap pointer, and
// free RAM
#define DIAG3 14

// Setup modes

// Set backlight brightness
#define SETUP1 15

// Set holding temperature
#define SETUP2 16

// Set minimum depth
#define SETUP3 17

// Set maximum depth
#define SETUP4 18

// Set minimum temperature
#define SETUP5 19

// Set maximum temperature
#define SETUP6 20

// Set depth calibration constants m and k.
#define SETUP7 21
#define SETUP8 22

// Set contrast (Newhaven only)
#define SETUP11 26

// Stop the house burning down modes

// Interlocks fail
#define INTERLOCK_FAIL 25

// And reason for failure
#define NOFAIL 0
#define OVERFILL 1
#define UNDERFILL 2
#define LIDOFF 3

/*
 *
 * End of states for the Finite State Machine
 * 
 */


/*
 *
 * EEPROM addresses
 * EEPROM is used to store preferences and calibration data
 *
 */
 
#define ROM_BACKLIGHT 0
#define ROM_HOLDTEMP 1
#define ROM_MINTEMP (ROM_HOLDTEMP  + sizeof(float))
#define ROM_MAXTEMP (ROM_MINTEMP + 1)
#define ROM_MINDEPTH (ROM_MAXTEMP + 1)
#define ROM_MAXDEPTH (ROM_MINDEPTH + 1)
#define ROM_CALDEPTHM (ROM_MAXDEPTH + 1)
#define ROM_CALDEPTHK (ROM_CALDEPTHM + sizeof(float))
#define ROM_MAGIC1 (ROM_CALDEPTHK + sizeof(float))
#define ROM_CONTRAST (ROM_MAGIC1 + 1)
#define ROM_MAGIC2 (ROM_CONTRAST + sizeof(float))
#define ROM_INTERLOCK (ROM_MAGIC2 + 1)


/*
 * 
 * End of EEPROM addresses
 * 
 */

// Special characters for the LCD
#define PBAR byte(4)
#define WBAR byte(5)
#define RBAR byte(6)
#define DEG byte(3)           // ° Degree symbol, but custom
#define HP_OFF byte(2)        // Custom, heater or pump off
#define PUMP_ON byte(1)       // Custom, pump on
#define HEAT_ON byte(0)       // Custom, heater on
#define LEFT_ARROW B01111111  // ← Left arrow
#define RIGHT_ARROW B01111110 // → Right arrow
#define LOGO_LEFT B00111100   // < Left hand char of logo
#define LOGO_RIGHT B00111110  // > Right hand char of logo

// Power stats (watts)
// Set these to reflect the actual hardware
#define P_HEATER 300.0
#define P_PUMP 3.5
#define P_LAMP 0.05
#define P_LIGHT 0.1
#define P_SYSTEM 2.0

// Temperature state
#define AT_TEMP 2
#define BELOW_TEMP 1
#define ABOVE_TEMP 0

// Used to check if the EEPROM has already been programmed
// with calibration/preference values
#define MAGIC_NUM1 B10111011
#define MAGIC_NUM2 B10010010

// Arguments for LCD control functions (Newhaven)
#define LCD_ON 0x41
#define LCD_OFF 0x42
#define LCD_CULOFF 0x48
#define LCD_CULON 0x47
#define LCD_CBLOFF 0x4C
#define LCD_CBLON 0x4B


/*
 * String literals really trash RAM, especially in code that
 * interacts with humans. Put them in flash, and save a mega
 * bundle of RAM. Makes code a bugger to follow, though.
 *  
 */
 
const char literal001[] PROGMEM = "                    ";
const char literal002[] PROGMEM = "Sous vide    Mark IV";
const char literal003[] PROGMEM = "by Nik Berry    v";
const char literal004[] PROGMEM = "Press Start to begin";
const char literal005[] PROGMEM = " ";
const char literal006[] PROGMEM = "  ";
const char literal007[] PROGMEM = "Add food, fit lid,";
const char literal008[] PROGMEM = { PBAR, WBAR, RBAR, ' ', 0 };
const char literal009[] PROGMEM = "Backlight:";
const char literal010[] PROGMEM = "cm";
const char literal011[] PROGMEM = "Cooking done. Press";
const char literal012[] PROGMEM = "Cooking Temp? ";
const char literal013[] PROGMEM = "Cooking Time? ";
const char literal014[] PROGMEM = "Cook temp: ";
const char literal015[] PROGMEM = "Cook time: ";
const char literal016[] PROGMEM = "Cur. temp: ";
const char literal017[] PROGMEM = "D ";
const char literal018[] PROGMEM = "Days";
const char literal019[] PROGMEM = "D cal k ";
const char literal020[] PROGMEM = "D cal m ";
const char literal021[] PROGMEM = "Depth ";
const char literal022[] PROGMEM = "Depth cal high:";
const char literal023[] PROGMEM = "Depth cal low:";
const char literal024[] PROGMEM = "Duty cycle: ";
const char literal025[] PROGMEM = "food.";
const char literal026[] PROGMEM = " Free: ";
const char literal027[] PROGMEM = " Heap: ";
const char literal028[] PROGMEM = "Heat ";
const char literal029[] PROGMEM = "Heated. Press Start";
const char literal030[] PROGMEM = "  High  ";
const char literal031[] PROGMEM = "Holding Temp:";
const char literal032[] PROGMEM = "Hold time: ";
const char literal033[] PROGMEM = "Hours";
const char literal034[] PROGMEM = " pwr ";
const char literal035[] PROGMEM = "in";
const char literal036[] PROGMEM = "  Low   ";
const char literal037[] PROGMEM = "Maximum Depth:";
const char literal038[] PROGMEM = "Maximun Temp:";
const char literal039[] PROGMEM = " Medium ";
const char literal040[] PROGMEM = "Minimum Depth:";
const char literal041[] PROGMEM = "Minimum Temp:";
const char literal042[] PROGMEM = "Minutes";
const char literal043[] PROGMEM = " Bright ";
const char literal044[] PROGMEM = "Off";
const char literal045[] PROGMEM = "Off  ";
const char literal046[] PROGMEM = "On";
const char literal047[] PROGMEM = "On  ";
const char literal048[] PROGMEM = " I/lock ";
const char literal049[] PROGMEM = "     Pre-heating";
const char literal050[] PROGMEM = "Press Start to cook.";
const char literal051[] PROGMEM = " Pump ";
const char literal052[] PROGMEM = "Ready to cook.";
const char literal053[] PROGMEM = "Remaining: ";
const char literal054[] PROGMEM = "Safe to remove lid";
const char literal055[] PROGMEM = "Stack: ";
const char literal056[] PROGMEM = "Start and remove lid";
const char literal057[] PROGMEM = " T ";
const char literal062[] PROGMEM = "then press Start.";
const char literal063[] PROGMEM = "F/W ";
const char literal064[] PROGMEM = "when ready to add";
const char literal065[] PROGMEM = "Selector to change";
const char literal066[] PROGMEM = "Programming EEPROM";
const char literal067[] PROGMEM = "";
const char literal068[] PROGMEM = { DEG, 'C', 0 };
const char literal069[] PROGMEM = { LEFT_ARROW, ' ', 0 };
const char literal070[] PROGMEM = { ' ', RIGHT_ARROW, ' ', ' ', 0 };
const char literal071[] PROGMEM = "Depth raw: ";
const char literal072[] PROGMEM = "Temp. raw: ";
const char literal073[] PROGMEM = { DEG, 'F', 0 };
const char literal074[] PROGMEM = "  Cannot continue";
const char literal075[] PROGMEM = " Interlock failure";
const char literal076[] PROGMEM = "Water level too low";
const char literal077[] PROGMEM = "Water level too high";
const char literal078[] PROGMEM = "  Detected lid off";
const char literal079[] PROGMEM = "   Please correct";
const char literal080[] PROGMEM = "Lid: ";
const char literal081[] PROGMEM = { 0xf3, 0xf3, 0 };
const char literal085[] PROGMEM = "Contrast:";
const char literal086[] PROGMEM = " ec ";
const char literal087[] PROGMEM = { 0x20, 0x47, 0x61, 0x6d, 0x65, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x20, 0 }; 
const char literal088[] PROGMEM = { 0x20, 0x20, 0x59, 0x6f, 0x75, 0x20, 0x77, 0x69, 0x6e, 0x20, 0x20, 0 };
const char literal089[] PROGMEM = "  Level ";
const char literal090[] PROGMEM = { 0x5f, 0x53, 0x54, 0x41, 0x52, 0x5f, 0 };
const char literal091[] PROGMEM = { 0x45, 0x64, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x21, 0};
const char literal092[] PROGMEM = { 0x02, 0x03, 0x41, 0x52, 0x53, 0x01, 0};
const char literal093[] PROGMEM = { 0x05, 0x06, 0};
const char literal094[] PROGMEM = "%  ";
const char* const literals[] PROGMEM =
  { literal001, literal002, literal003, literal004,
    literal005, literal006, literal007, literal008,
    literal009, literal010, literal011, literal012,
    literal013, literal014, literal015, literal016,
    literal017, literal018, literal019, literal020,
    literal021, literal022, literal023, literal024,
    literal025, literal026, literal027, literal028,
    literal029, literal030, literal031, literal032,
    literal033, literal034, literal035, literal036,
    literal037, literal038, literal039, literal040,
    literal041, literal042, literal043, literal044,
    literal045, literal046, literal047, literal048,
    literal049, literal050, literal051, literal052,
    literal053, literal054, literal055, literal056,
    literal057, literal062, literal063, literal064,
    literal065, literal066, literal067, literal068,
    literal069, literal070, literal071, literal072,
    literal073, literal074, literal075, literal076,
    literal077, literal078, literal079, literal080,
    literal081, literal085, literal086, literal087,
    literal088, literal089, literal090, literal091,
    literal092, literal093, literal094 };

// Set calibration/preference values to hard coded defaults.
// If EEPROM is already programmed, these will be overwritten.
// If not, these will be written to EEPROM
byte lcd_blight = 0;           // Low brightness
byte contrast = 2;            // Medium contrast
float holdtemp = 54.5;        // 54.5°C/130°F
byte mintemp = 40;            // 40.0°C/104°F
byte maxtemp = 80;            // 95.0°C/203°F
byte mindepth = 0;            // 10cm
byte maxdepth = 100;          // 16cm
float caldepthm = 0.09;
float caldepthk = 0.0;
byte firmware = FW_VERSION;
byte interlock = 1;

// Statistics stuff
unsigned long int number_samples, stat_heat = 0;
float avg_power = 0;
float inst_power, duty_cycle;

float cur_temp = 0.0;           // Current temp, °C
byte heat_on = 0;               // Set whenever the heater is on
byte heating = 0;               // Set whenever heating mode is enabled, whether
                                // or not the heater is actually on
byte pump_on = 0;               // Set whenever the pump is on
byte heat_req;                  // Set if the heater needs to be on
float set_temp;                 // Temperature required, as set by the cook (C)
float depth;                    // Water depth (cm)
byte units;

float y1, y2;

byte state;                     // Current state
byte save_state = IDLE_WAIT;    // Previous state, so we can go back to it, e.g. from diag mode.
byte cook_days = 0;             // Days of cooking time requested
byte cook_hours = 0;            // Hours of cooking time requested
byte cook_minutes = 0;          // Minutes of cooking time requested

unsigned long int cook_time;    // Total cooking time requested in milliseconds
unsigned long int start_time;   // Time cooking started in milliseconds
unsigned long int current_time; // Time now (during cooking and afterwards) in milliseconds
unsigned long int done_time;    // Total actual cooking or holding time in milliseconds

byte buzzer_time = 0;           // Used to time buzzer
byte buzz = 0;                  // Whether to buzz
unsigned long int counter;      // Counts seconds

byte at_temp = 0;               // Set when water is at or above the desired temperature
long int dwdmillis;             // Used by matchmillis()

// Stores a copy of what is displayed on the LCD.
// Prevents unnecessary updates when nothing has changed. 
char cl1[21], cl2[21], cl3[21], cl4[21];

// Setup software serial for the LCD
SoftwareSerial lcd(-1, TXPIN);

// Temperature sensor (DS18B20) OneWire interface
OneWire ds_temp(TEMP_SENSOR);


// And buttons and the encoder
ButtonCtl ButtonS(START_SELECT);
ButtonCtl ButtonQ(QUERY);
ButtonCtl SwitchU(TEMP_UNITS);
ButtonCtl SwitchL(LID_ON);
EncoderCtl EncoderS(ENCODER_A, ENCODER_B);

// First time in a state there is no valid selector
// value or range from the last time around the loop, so this
// is used to set a default first time round.

byte first;

/*
 * 
 * If the first time in a state in the FSM,
 * set the encoder value and limits.
 * 
 */
 
void chk_set_encoder(int v, int l, int h)
{
  if(first) {
    first = 0;
    EncoderS.set(1, v, l, h);
  }
}


/*
 * 
 * Given two a/d readings from the depth or temp sensor, and two
 * corresponding depths or temps provided by the user, calculates
 * the m and k values for the formula: depth|temp = m * reading + k.
 * 
 * Returns k and m as floats.
 * 
*/

void genform(float x1, float y1, float x2, float y2, float *m, float *k)
{
  *m = ((y1 - y2) / (x1 - x2));
  *k = y1 - x1 * *m;
}

/* 
 *  
 * Given m and k derived above, and a reading from the depth or 
 * temp sensor, returns the depth in cm, or temp in °C, as a float
 * 
 */
 
float plotform(int x, float m, float k)
{
  return (float) x * (float) m + (float) k;
}


/*
 * 
 * Four functions to copy flash-stored string literals to
 * local RAM buffers. Four are used so that one can be used
 * for each line of the LCD, without overwriting earlier
 * values (no mallocs here :)).
 * 
 */
 
char *getfstr1(int i)
{
  static char buf[21];

  strcpy_P(buf, (char*) pgm_read_word(&(literals[i])));
  return buf;
}

char *getfstr2(int i)
{
  static char buf[21];

  strcpy_P(buf, (char*) pgm_read_word(&(literals[i])));
  return buf;
}

char *getfstr3(int i)
{
  static char buf[21];

  strcpy_P(buf, (char*) pgm_read_word(&(literals[i])));
  return buf;
}

char *getfstr4(int i)
{
  static char buf[21];

  strcpy_P(buf, (char*) pgm_read_word(&(literals[i])));
  return buf;
}

// LCD control functions
// Not fully implemented or documented until
// we move from the Sparkfun LCD to the much
// nicer Newhaven one (Logo time WITH CHOO-CHOO!).

void lcd_clear()
{
  byte i;
  lcd.write(0xFE);
  lcd.write(0x51);
  for (i = 0; i < 21; i++) cl1[i] = cl2[i] = cl3[i] = cl4[i] = 0;
  delay(2);
}


//Define character, Newhaven
void lcd_chardef(byte c) {

  lcd.write(0xFE);
  lcd.write(0x54);
  lcd.write((byte) c);
}

// Moves the LCD cursor to x,y
void lcd_setcursor(int x, int y) {
  byte pos;

  switch (y) {
    case 0:
      pos = 0x00;
      break;
    case 1:
      pos = 0x40;
      break;
    case 2:
      pos = 0x14;
      break;
    case 3:
      pos = 0x54;
      break;
  }
  pos += x;
  lcd.write(0xFE);
  lcd.write(0x45);
  lcd.write(pos);
}

void lcd_backlight(byte b) {
  static byte o;
  byte r = 4;

  if(o == b) return;
  o = b;
  switch (b) {
    case 0:
      r = 4;
      break;
    case 1:
      r = 6;
      break;
    case 2:
      r = 8;
      break;
  }
  lcd.write(0xFE);
  lcd.write(0x53);
  lcd.write(r);
  delay(1);
}

void lcd_contrast(int c) {
  static byte o;
  byte r = 40;

  if(o == c) return;
  o = c;
  switch (c) {
    case 0:
      r = 30;
      break;
    case 1:
      r = 35;
      break;
    case 2:
      r = 40;
      break;
    case 3:
      r = 50;
      break;
  }
  lcd.write(0xFE);
  lcd.write(0x52);
  lcd.write(r);
  delay(1);
}

void lcd_setbaud(long int b) {
  byte r;
  switch (b) {
    case 2400:
      r = 11;
      break;
    case 4800:
      r = 12;
      break;
    case 9600:
      r = 13;
      break;
    case 14400:
      r = 14;
      break;
    case 19200:
      r = 15;
      break;
    case 38400:
      r = 16;
      break;
  }
  lcd.write(0x7C);
  lcd.write(r);
}


/*
 * 
 * Create various characters, icons and the logo,
 * for the Newhaven LCD.
 * 
 */


/*
 * 
 * Create the heater and pump icons for the LCD
 * 
 */

void mk_icons()
{
  byte i, j;
  static const byte icon[] PROGMEM = { B00001000, B00000100, B00011111, B00011111, // Heater
                  B00010001, B00010001, B00011111, B00001010,
                  B00000100, B00001010, B00010010, B00010111, // Pump
                  B00000101, B00000100, B00000100, B00000100,
                  B00000000, B00001110, B00010011, B00010101, // Off
                  B00010101, B00011001, B00001110, B00000000,
                  B00000110, B00001001, B00001001, B00000110, // Degree
                  B00000000, B00000000, B00000000, B00000000,
                  B00011111, B00000000, B00011110, B00010001, // p bar
                  B00011110, B00010000, B00010000, B00000000,
                  B00011111, B00000000, B00010001, B00010001, // w bar
                  B00010101, B00010101, B00001010, B00000000,
                  B00011111, B00000000, B00010110, B00011001, // r bar
                  B00010000, B00010000, B00010000, B00000000 };

  for(i = 0; i < 7; i++) {
    lcd_chardef(i);
    for(j = 0; j < 8; j++) lcd.write(pgm_read_byte(&icon[i * 8 + j]));
  }
}


/*
 * 
 * Create the sous vide logo for the LCD. The left and right
 * parts are standard symbols, and not done by mk_logo.
 * 
 */
 
void mk_logo()
{
  byte i, j;
                                                              // Part 1 is 'S'
  static const byte logo[] PROGMEM = { B00011001, B00000110, B00000000, B00001110, // Part 2
                  B00010001, B00010001, B00010001, B00001110,
                  B00000110, B00011001, B00000000, B00010001, // Part 3
                  B00010001, B00010001, B00010001, B00011111,
                  B00011001, B00000110, B00000000, B00001110, // Part 4
                  B00010000, B00001110, B00000001, B00001110,
                  B00000110, B00011001, B00000000, B00001001, // Part 5
                  B00001001, B00001001, B00001001, B00000110,
                  B00011001, B00000110, B00000000, B00010110, // Part 6
                  B00010101, B00010101, B00010101, B00010110,
                  B00011000, B00000111, B00000000, B00011100, // Part 7
                  B00010000, B00011000, B00010000, B00011100,
                  B00001000, B00010100, B00001000, B00000110, // Part 8
                  B00001001, B00001000, B00001001, B00000110,
                  B00001000, B00010100, B00001000, B00000111, // Part 9
                  B00000100, B00000110, B00000100, B00000100 };

  for(i = 0; i < 8; i++) {
    lcd_chardef(i);
    for(j = 0; j < 8; j++) lcd.write(pgm_read_byte(&logo[i * 8 + j]));
  }
}


/*
 *
 * Setup routine
 *
 */

unsigned long int setup_reset;

// OneWire address of the temperature sensor
byte *tempaddr;

void setup() {
  char tmp[21];

  // Set I/O modes on pins 

  // Inputs are all handled by the ButtonCtl and 
  // EncoderCtl libraries.
  
  // Outputs
  
  pinMode(HEATER, OUTPUT);
  pinMode(SAFE_LAMP, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(PUMP, OUTPUT);
  pinMode(TXPIN, OUTPUT);
  pinMode(TEMPREADY, OUTPUT);

  // Turn 'Safe to remove lid' lamp on

  lamp(SAFE_LAMP, ON);

  // Turn heater off

  drive(HEATER, OFF);

  // Turn pump off

  drive(PUMP, OFF);

  // Turn Temp ready LED off

  lamp(TEMPREADY, OFF);

// Start debug serial, and LCD soft serial

  Serial.begin(9600);
  while(!Serial);
  lcd.begin(9600);
  lcd.write(0xfe);
  lcd.write(0x61);
  lcd.write(0x07);
  lcd.end();
  lcd.begin(57600);
 
  
  lcd_clear();
  lcd_setcursor(0, 0);
  idle_reset();
analogReference(INTERNAL);
  // If EEPROM is unprogrammed, setup defaults
  if(EEPROM.readByte(ROM_MAGIC1) != MAGIC_NUM1
  || EEPROM.readByte(ROM_MAGIC2) != MAGIC_NUM2) {
    strcpy(tmp, getfstr1(61));
    lcd_clear();
    lcd_setcursor(0, 0);
    lcd.print(tmp);

    // Write settings to EEPROM
    EEPROM.writeByte(ROM_BACKLIGHT, lcd_blight);
    EEPROM.writeFloat(ROM_HOLDTEMP, holdtemp);
    EEPROM.writeByte(ROM_MINTEMP, mintemp);
    EEPROM.writeByte(ROM_MAXTEMP, maxtemp);
    EEPROM.writeByte(ROM_MINDEPTH, mindepth);
    EEPROM.writeByte(ROM_MAXDEPTH, maxdepth);
    EEPROM.writeFloat(ROM_CALDEPTHM, caldepthm);
    EEPROM.writeFloat(ROM_CALDEPTHK, caldepthk);
    EEPROM.writeByte(ROM_CONTRAST, contrast);
    EEPROM.writeByte(ROM_INTERLOCK, interlock);
    EEPROM.writeByte(ROM_MAGIC1, MAGIC_NUM1);
    EEPROM.writeByte(ROM_MAGIC2, MAGIC_NUM2);
    delay(1000L);
  } else {
 
     // Read settings from EEPROM
    lcd_blight = EEPROM.readByte(ROM_BACKLIGHT);
    contrast = EEPROM.readByte(ROM_CONTRAST);
    holdtemp = EEPROM.readFloat(ROM_HOLDTEMP);
    mintemp = EEPROM.readByte(ROM_MINTEMP);
    maxtemp = EEPROM.readByte(ROM_MAXTEMP);
    mindepth = EEPROM.readByte(ROM_MINDEPTH);
    maxdepth = EEPROM.readByte(ROM_MAXDEPTH);
    caldepthm = EEPROM.readFloat(ROM_CALDEPTHM);
    caldepthk = EEPROM.readFloat(ROM_CALDEPTHK);
    interlock = EEPROM.readFloat(ROM_INTERLOCK);
  }
  stat_heat = 1;    // Avoid div by zero in stats calcs
  setup_reset = ~0;

  // Set up OneWire for the DS18B20
  tempaddr = setup_ow();
  
  // Needed by EncoderCtl to call EncoderCtl.update() ISRs.
  Timer1.initialize(3000L);
  Timer1.attachInterrupt(isr);
  counter = millis() / 1000;
}


/*
 * isr() contains calls to each EncoderCtl instance
 * local ISR. We only have one.
 * 
 */
 
void isr(){
  EncoderS.update();
}

// For memory monitoring diags.

uint8_t *heapptr, *stackptr;


// Interlock failure reason.
  byte ifr;

byte egg = 0;
byte spegg = 0;

/*
 *
 * Main loop
 *
 */
byte lidon;                         // Value of the lid on interlock

void loop() {
  int i;
  int l, h;
  static float x1, x2;
  byte start_selector, query;         // Current values of the buttons
  static byte ounits;
  char *line;
  byte food;
  static int oencoder;
  static byte oenc;
  byte cflag;
  static byte init = 10;

  if(spegg) sinv();
  else {
  cflag = 0;
  // Read units switch, and start_selector and query buttons
  // and lid on interlocks
  l = SwitchL.read();
  if(l >= 0) lidon = l;
  l = SwitchU.read();
  if(l >= 0) units = l;
  start_selector = ButtonS.timeup();
  query = ButtonQ.timeup();
  if(start_selector) egg = 0;
  if(query) egg++;
  if(egg == 10) {
    spegg = 1;
    lcd_contrast(2);
     return;
  }
  // Check for start_selector pressed for >= 2 seconds.
  // If in idle mode, this means enter setup.
  // Otherwise, it means reset to idle mode.

  if(start_selector >= 20) {
    start_selector = 0;
    if(state == IDLE_WAIT) set_state(SETUP1);
    else idle_reset();
  }

  // Check for query pressed for >= 2 seconds.
  // If it is, disable interlocks.

  if(query >= 20) {
    query = 0;
    interlock = 0;
    idle_reset();
  }
  
  if(init) {
    init--;
    lidon = 1;
  }
  
  if(counter != millis() / 1000) {
    counter = millis() / 1000;
    cflag = 1;
    buzzer_time++;
    if(buzzer_time == 2) {
      if(buzz) buzzer(1);
    }
    
    if(buzzer_time == 4) {
      buzzer_time = 0;
      buzzer(0);
    }
    check_mem();
  }


  // Check for query pressed to enter query mode,
  // Query pressed in idle or cooking modes means enter query mode
  // Otherwise, ignore
  if(query) {
    switch (state) {
      case MQUERY:
      case DIAG1:
      case DIAG2:
      case DIAG3:
      case SETUP1:
      case SETUP2:
      case SETUP3:
      case SETUP4:
      case SETUP5:
      case SETUP6:
      case SETUP7:
      case SETUP8:
        break;

      default:
        save_state = state;
        set_state(MQUERY);
        query = 0;
        break;
    }
  }

//  if(cflag) {
    cur_temp = get_temp();
//  }

// If the encoder value has changed, force an immediate
// update of the LCD next time it's requested.

  if(oencoder != EncoderS.get()) {
    oencoder = EncoderS.get();
    dwdmillis = -1;
  }


/*************************************************************************************
 *                                                                                   *
 *                        //======================//                                 *
 *                       // Finite state machine //                                  *
 *                      //======================//                                   *
 *                                                                                   *
 *                                    . . , , , , o o o o             __________     *
 * __+====__                                    _____     o           |Sous St.|     *
 * |   _   |                             ___===  ]OO|__n__][.         |  Vide  |     *
 * |__|_|__|_|______|_|______|_|______|_[______]_|__|_______)<        |Michigan|     *
 *   o    o    o   o    o   o    o   o   oo   oo  'oo OOO-| oo\\_     ~~~~||~~~~     *
 *--+--+--+--+--+--+--+--+--+- +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--*
 *                                                                                   *
 *************************************************************************************
 */
 
  switch (state) {
    case INTERLOCK_FAIL:
      pump_on = 0;                          // Pump off, heater off
      heating = 0;
      if(cflag) buzz = 1;                   // Alert the chef
      display_intfail();
      if(query) set_state(SETUP5); // Go to setup depth stuff
      break;
     
    case IDLE_WAIT:                         // Idle, wating for cheffy to act.
      pump_on = 0;                          // Pump off, heater off
      heating = 0;
      display_idle();
      if(start_selector) set_state(PROG_TEMP); // Cheffy presses Start, switch to choose cooking temp
      break;

    case MQUERY:
      display_query();
      if(query) set_state(save_state);     // query button in query mode leaves query mode
      if(start_selector) set_state(DIAG1); // start_selector in query mode enters diag mode
      break;

    case PROG_TEMP:                         // Programme the cooking temperature
      pump_on = 1;                          // Pump on, heater off
      heating = 0;
      if(first) {
        first = 0;
        ounits = units;
        if(units) {
          l = ctof(mintemp);
          h = ctof(maxtemp);
        } else {
          l = mintemp * 2;
          h = maxtemp * 2;
        }
        EncoderS.set(1, l, l, h);
      } else if(units != ounits) {
        ounits = units;
        // Allow the Selector to return temps from range in setup
        // in C or Fahrenheit equivalent - as set in defaults or setup
        if(units) {
          l = ctof(mintemp);
          h = ctof(maxtemp);
        } else {
          l = mintemp * 2;
          h = maxtemp * 2;
        }
        if(units) set_temp = (ctof(EncoderS.get() / 2.0 + 0.5));
        else set_temp = (EncoderS.get() - 32.0) / 9.0 * 5.0 * 2.0;
        EncoderS.set(1, set_temp, l, h);
      }

      set_temp = EncoderS.get();
      if(!units) set_temp /= 2.0;
      lamp(TEMPREADY, OFF);
      display_set_temp();
      if(units) set_temp = (set_temp - 32.0) / 9.0 * 5.0;
      if(start_selector)
        set_state(PROG_DAYS);                 // Cheffy presses Start, switch to prog days
      break;

    case PROG_DAYS:                           // Programme the cooking time, days part
      pump_on = 1;                            // Pump on, heater on
      heating = 1;

      chk_set_encoder(0, 0, 4);

      cook_hours = cook_minutes = 0;
      cook_days = EncoderS.get();
      line = cat_stime(cook_days, getfstr1(17));
      lcd_display(getfstr2(12), line, getfstr3(59), getfstr4(62), 1);
      if(start_selector) {
         if(cook_days == 4) set_state(PREHEAT); // Cheffy presses Start, days = 4, switch to pre-heat
         else set_state(PROG_HOURS); // else Cheffy presses Start, switch to prog hours
      }
      break;

    case PROG_HOURS:                          // Programme the cooking time, hours part
      pump_on = 1;                            // Pump on, heater on
      heating = 1;

      chk_set_encoder(0, 0, 23);
      
      cook_hours = EncoderS.get();
      line = cat_stime(cook_hours, getfstr1(32));
      lcd_display(getfstr1(12), line, getfstr3(60), getfstr4(62), 1);
      if(start_selector) set_state(PROG_MINUTES); // Cheffy presses Start, switch to prog minutes
      break;

    case PROG_MINUTES:                        // Programme the cooking time, minutes part
      pump_on = 1;                            // Pump on, heater on
      heating = 1;

      chk_set_encoder(0, 0, 59);

      cook_minutes = EncoderS.get();
      line = cat_stime(cook_minutes, getfstr1(41));
      lcd_display(getfstr1(12), line, getfstr3(60), getfstr4(62), 1);
      if(start_selector) set_state(PREHEAT); // Cheffy presses Start, switch to pre-heat
      break;
      
    case PREHEAT:                             // Pre-heat the water to cooking temperature
      pump_on = 1;                            // Pump on, heater on
      heating = 1;
      // Calculate cooking time in milliseconds
      cook_time = cook_days * 24UL;
      cook_time += cook_hours;
      cook_time *= 60UL;
      cook_time += cook_minutes;
      cook_time *= 60000UL;
      display_preheat();
      if(at_temp) set_state(FOOD_WAIT);       // This time, no chef input. Switch to wait for
      break;                                  // food to be added as soon as we reach temp

    case FOOD_WAIT:                           // Wait for cheffy to ask to add food.
      pump_on = 1;                            // Pump on, heater on
      heating = 1;
      if(cflag) buzz = 1;                     // Alert the chef
      display_foodwait();
      if(start_selector) set_state(FOOD_ADDING); // Cheffy presses Start, switch to adding food
      break;

    case FOOD_ADDING:                         // Wait for cheffy to add food.
      pump_on = 0;                            // and so pump off, heater off
      heating = 0;                            // Checks later on, after the state machine,
                                              // will turn on the safe LED
      display_foodadding();
      if(start_selector) set_state(FOOD_ADDED); // Cheffy presses Start, switch to food added
      break;

    case FOOD_ADDED:                          // Wait for cheffy to start cooking, AT LAST!
      pump_on = 0;                            // Lid may still be off, so pump off, heater off
      heating = 0;
      start_time = millis();                  // Note the time we started, so we'll know when to stop
      display_food_added();
      if(start_selector) set_state(COOKING);  // CHEFFY HIT START_SELECT! Switch to cooking
      break;
 
    case COOKING:                             // Cook the food until the cooking time is up
      pump_on = 1;                            // Pump on, heater on (no, really?)
      heating = 1;
      current_time = millis();                // Get the current time
      done_time = (current_time - start_time);// See how long we've been cooking
      display_cook();
      if(done_time >= cook_time) set_state(EMPTY_WAIT); // If long enough, we're done
      break;

    case EMPTY_WAIT:                          // Holding phase. Wait for cheffy to stop the hold,
                                              // and remove the yummy food
      pump_on = 1;                            // Pump on, heater on
      heating = 1;
      if(cflag) buzz = 1;

      // If the cooking temp is above our holding temp, drop to holding temp
      if(set_temp > holdtemp) set_temp = holdtemp;

      current_time = millis();
      done_time = (current_time - start_time);// Work out how long we've been holding
      display_empty();
      if(start_selector) idle_reset();
      break;

    case DIAG1:
      display_diag1();
      if(query) set_state(save_state);
      if(start_selector) set_state(DIAG2);
      break;

    case DIAG2:
      display_diag2();
      if(query) set_state(save_state);
      if(start_selector) set_state(DIAG3);
      break;

    case DIAG3:
      if(query) set_state(save_state);
      if(start_selector) set_state(DIAG1);
      display_diag3();
      break;

    case SETUP1:
      if(first) lcd_clear();
      chk_set_encoder(lcd_blight, 0, 2);

      x1 = EncoderS.get();
      display_set_bkl(x1);

      if(start_selector) {
        lcd_blight = x1;
        EEPROM.writeByte(ROM_BACKLIGHT, byte(lcd_blight)); // Write to EEPROM
        set_state(SETUP11);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP11:
      chk_set_encoder(contrast, 0, 3);

      x1 = EncoderS.get();
      display_set_con(x1);

      if(start_selector) {
        contrast = x1;
        EEPROM.writeByte(ROM_CONTRAST, byte(contrast)); // Write to EEPROM
        set_state(SETUP2);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP2:
      chk_set_encoder(holdtemp, mintemp, maxtemp);

      y1 = (float) EncoderS.get();
      display_sel(getfstr1(30), y1, 3, 1, getfstr2(63));

      if(start_selector) {
        holdtemp = y1;
        EEPROM.writeByte(ROM_HOLDTEMP, byte(holdtemp)); // Write to EEPROM
        set_state(SETUP3);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

     case SETUP3:
      chk_set_encoder(mintemp, 0, 100);

      y1 = EncoderS.get();
      display_sel(getfstr1(40), y1, 3, 1, getfstr2(63));

      if(start_selector) {
        mintemp = y1;
        EEPROM.writeByte(ROM_MINTEMP, byte(mintemp)); // Write to EEPROM
        set_state(SETUP4);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP4:
      chk_set_encoder(maxtemp, 0, 100);

      y1 = EncoderS.get();
      display_sel(getfstr1(37), y1, 3, 1, getfstr2(63));

      if(start_selector) {
        maxtemp = y1;
        EEPROM.writeByte(ROM_MAXTEMP, byte(maxtemp)); // Write to EEPROM
        set_state(SETUP5);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP5:
      chk_set_encoder(mindepth, 0, 50);

      y1 = EncoderS.get();
      display_sel(getfstr1(39), y1, 2, 0, getfstr2(9));

      if(start_selector) {
        mindepth = y1;
        EEPROM.writeByte(ROM_MINDEPTH, byte(mindepth)); // Write to EEPROM
        set_state(SETUP6);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP6:
      chk_set_encoder(maxdepth, 0, 50);

      y1 = EncoderS.get();
      display_sel(getfstr1(36), y1, 2, 0, getfstr2(9));

      if(start_selector) {
        maxdepth = y1;
        EEPROM.writeByte(ROM_MAXDEPTH, byte(maxdepth)); // Write to EEPROM
        set_state(SETUP7);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP7:
      chk_set_encoder(mindepth, 0, 50);

      y1 = EncoderS.get();
      x1 = analogRead(DMEASURE);
      display_sel(getfstr1(22), y1, 2, 1, getfstr2(9));

      if(start_selector) {
        set_state(SETUP8);
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;

    case SETUP8:
      chk_set_encoder(maxdepth, 0, 50);

      y2 = EncoderS.get();
      x2 = analogRead(DMEASURE);
      display_sel(getfstr1(21), y2, 2, 1, getfstr2(9));

      if(start_selector) {
    //    genform(x1, y1, x2 ,y2, &caldepthm, &caldepthk);  // Calculate parameters
        EEPROM.writeFloat(ROM_CALDEPTHM, caldepthm);      // and write to EEPOM
        EEPROM.writeFloat(ROM_CALDEPTHK, caldepthk);
        set_state(save_state);                    // Exit setup mode
      }
      if(query) set_state(save_state);     // query button in setup mode leaves setup mode
      break;
  }


/*************************************************************************************
 *                                                                                   *
 *                                                                                   *
 *                        //==========================//                             *
 *                       // End Finite state machine //                              *
 *                      //==========================//                               *
 *                                                                                   *
 * __________             o o o o , , , , . .                                        *
 * |  Food  |           o     _____                                    __====+__     *
 * | Heaven |         .[]__n__|OO]  ===___                             |   _   |     *
 * | Oregon |        <)_______|__|_]______[_|______|_|______|_|______|_|__|_|__|     *
 * ~~~~||~~~~     _\\oo |-OOO oo'  oo   oo   o   o    o   o    o   o    o    o       *
 *+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+ -+--+--+--+--+--+--+--+--+--+-*
 *                                                                                   *
 *************************************************************************************
 */

  depth = plotform(analogRead(DMEASURE), caldepthm, caldepthk);
 
  /*
   * 
   * Check interlocks, but only if we're not in setup
   * 
   */

  if(interlock) {
    ifr = NOFAIL;
    switch (state) {
      case SETUP1:
      case SETUP2:
      case SETUP3:
      case SETUP4:
      case SETUP5:
      case SETUP6:
      case SETUP7:
      case SETUP8:
        break;

      default:
        // Check if the lid is on
        if(!lidon) ifr = LIDOFF;

        // Check if water level is OK
        if(cflag) {
          // Read the depth of water in cm, as float
          depth = plotform(analogRead(DMEASURE), caldepthm, caldepthk);
          if(depth < (float) mindepth) ifr = UNDERFILL;
          else if(depth > (float) maxdepth) ifr = OVERFILL;
        }
        if(ifr != NOFAIL) {
          if(state != INTERLOCK_FAIL) {
            save_state = state;
            set_state(INTERLOCK_FAIL);
          }
        } else if(state == INTERLOCK_FAIL) set_state(save_state);
        break;
    }
  }

  // If we're pumping or heating, don't allow the lid to be removed.
  // Turn off the 'Safe to remove lid' lamp
  if(pump_on || heating) lamp(SAFE_LAMP, OFF);
  else lamp(SAFE_LAMP, ON);

  // If pumping, pump on
  if(pump_on) drive(PUMP, ON);
  else drive(PUMP, OFF);

  // Set the backlight and contrast as requested
  lcd_backlight(lcd_blight);
  lcd_contrast(contrast);

  // If heating, then turn the heater on only if needed
  heat_on = 0;
  if(heating) {
    // Assume at temp, as it's easier not to test for
    heat_req = AT_TEMP;

    // Set above/below as appropriate
    if(cur_temp > set_temp + 0.1) heat_req = ABOVE_TEMP;
    if(cur_temp < set_temp - 0.1) heat_req = BELOW_TEMP;

    // If below, heater on, if above, heater off
    // If at temp, leave the heater in whatever state it was.
    // This is to avoid chatter
    switch (heat_req) {
      case BELOW_TEMP:
        drive(HEATER, ON);
        heat_on = 1;
        break;

      case AT_TEMP:
      case ABOVE_TEMP:
        drive(HEATER, OFF);
        break;
    }

    // If the water is not below temp, set at_temp
    if(heat_req != BELOW_TEMP) at_temp = 1;
  } else drive(HEATER, OFF);

  if(cflag) {
    if(heat_on) stat_heat++;
    inst_power = heat_on * P_HEATER + pump_on * P_PUMP + (pump_on || heating) * P_LAMP
                 + lcd_blight * P_LIGHT + P_SYSTEM;
    avg_power *= number_samples;
    avg_power += inst_power;
    avg_power /= ++number_samples;    
    duty_cycle = (float) stat_heat / (float) number_samples * 100.0;
  }
  }
}



/* 
 *  
 * Reset stuff if we drop back to idle
 * 
 */
 
void idle_reset() {
  heat_on = 0;
  heating = 0;
  pump_on = 0;
  at_temp = 0;
  drive(HEATER, OFF);
  drive(PUMP, OFF);
  lamp(SAFE_LAMP, ON);
  lamp(TEMPREADY, OFF);
  mk_logo();
  first = 1;
  dwdmillis = -1;
  buzz = 0;
  state = IDLE_WAIT;
  lcd_clear();
}


/*
 * 
 * Set up the DS18B20 to read the temperature.
 * We use 12 bit conversion, because we like
 * guard bits.
 * 
 */
 
byte *setup_ow() {
  static byte addr[8];
  ds_temp.reset_search();
  ds_temp.search(addr);
  ds_temp.reset();
  ds_temp.select(addr);
  ds_temp.write(0x4E);
  ds_temp.write(0);
  ds_temp.write(0);
  ds_temp.write(0x7f);         // Set 12 bit conversion
  return addr;
}


/*
 * 
 * Read the temperature. If none is available (it takes
 * about 650ms to get a reading), return the last read
 * temperature. It will return -1.0 until it gets its
 * first reading. Temperature is in Celsius.
 * 
 */
 
float get_temp() {
  byte i;
  static byte dsread = 0;
  static float celsius = -1.0;
  
  if(!dsread) {
    ds_temp.reset();
    ds_temp.select(tempaddr);
    ds_temp.write(0x44, 0);
    dsread = 1;
  }
  if(!ds_temp.read()) return celsius;
  dsread = 0;
  ds_temp.reset();
  ds_temp.select(tempaddr);   
  ds_temp.write(0xBE);
  i = ds_temp.read();
  celsius = ((ds_temp.read() << 8) + i) * 0.0625;
  return celsius;
}


/* 
 * 
 * Sound buzzer.
 * For use with an actual buzzer. Call tone() for
 * use with a piezo transducer.
 * 
 */
 
 void buzzer(byte t) {
  if(t) drive(BUZZER, ON);
  else drive(BUZZER, OFF);
}


/* 
 *  
 * Switch FSM to new state
 * 
 *
 */
 
void set_state(int s) {
  at_temp = 0;
  first = 1;
  // If in idle mode, the heater/pump icons
  // will have been overwritten by the logo,
  // so put them back.
  if(state == IDLE_WAIT) mk_icons();
  if(s == IDLE_WAIT) mk_logo();
  state = s;
  dwdmillis = -1;
  buzz = 0;
  lcd_clear();
}


/* 
 *  
 * Checks if a line on the LCD would change if the string n 
 * were copied to it.
 * 
 */
 
 byte check_line(char *o, char *n) {
  int i;

  for (i = 0; i < 20 && o[i]; i++) if(o[i] != n[i]) return 1;
  return 0;
}


/* 
 *  
 *  Updates a copy of an LCD line used above
 *  
 */
 
 void copy_line(char *o, char *n) {
  int i;

  for (i = 0; i < 21; i++) o[i] = n[i];
}


/* 
 *  
 * Displays all 4 LCD lines, but only changes a line if the line 
 * has changed. The code parameter tells it whether to display 
 * heat/punp symbols at the end of lines 3 and 4.
 *  
 */
 
void lcd_display(char *l1, char *l2, char *l3, char *l4, char code) {

  if(check_line(l1, cl1)) {
    lcd_setcursor(0, 0);
    lcd.print(l1);
    copy_line(cl1, l1);
    delay(2);
  }
  if(check_line(l2, cl2)) {
    lcd_setcursor(0, 1);
    lcd.print(l2);
    copy_line(cl2, l2);
    delay(2);
  }
  if(check_line(l3, cl3)) {
    lcd_setcursor(0, 2);
    lcd.print(l3);
    copy_line(cl3, l3);
    delay(2);
  }
  if(check_line(l4, cl4)) {
    lcd_setcursor(0, 3);
    lcd.print(l4);
    copy_line(cl4, l4);
  }
  if(code) show_codes();
}


/*
 * 
 * Display heat/pump on/off symbols at the
 * end of the bottom two lines of the LCD
 * 
 */
 
 void show_codes() {
  lcd_setcursor(19, 2);
  if(heat_on) {
    lcd.write(HEAT_ON);
    cl3[20] = HEAT_ON;
  } else {;
    lcd.write(HP_OFF);
    cl3[19] = HP_OFF;
  }

  lcd_setcursor(19, 3);
  if(pump_on) {
    lcd.write(PUMP_ON);
    cl3[20] = PUMP_ON;
  } else {
    lcd.write(HP_OFF);
    cl3[19] = HP_OFF;
  }
}

void display_logo()
{
  lcd_setcursor(4, 0);
  lcd.write(byte(7));
  lcd.write(byte(B11000110));
  lcd.write('S');
  for(byte i = 0; i < 6; i++) lcd.write(byte(i));
  lcd.write(byte(B11000110));
  lcd.write(byte(6));
  lcd_setcursor(0, 0);
}

/* 
 *  
 * ,____________________.
 * |                    |
 * |Sous vide    Mark IV|
 * |by Nik Berry    v1.3|
 * |Press Start to begin|
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */

void display_idle() {
  char line[21];
  
  if(millimatch()) return;
  strcpy(line, getfstr3(2));
  strcat(line, fmt_float((float) firmware / 10.0, 1, 1));

  lcd_display(getfstr1(0), getfstr2(1), line, getfstr4(3), 0);
  display_logo();
}


/* 
 *  
 * ,____________________.
 * |Cooking Temp?       |
 * |<- NNN.NUU ->       |
 * | Selector to change |
 * |                    |
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is '°C' or '°F'
 * 
 */

void display_set_temp() {
  char str[21];

  if(millimatch()) return;
  strcpy(str, getfstr1(64));
  strcat(&str[2], fmt_temp(set_temp));
  strPcat(str, 65);
  lcd_display(getfstr2(11), str, getfstr3(60), getfstr4(62), 1);
}


/* 
 *  
 * ,____________________.
 * |     Pre-heating    |
 * |Cook temp: NNN.NUU  |
 * |Cur. temp: NNN.NUU H|
 * |                   P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is '°C' or '°F'
 * 
 */

void display_preheat() {
  char line1[21], *line2;

  if(millimatch()) return;
  strcpy(line1, cat_temp(set_temp, getfstr1(13)));
  line2 = cat_temp(cur_temp, getfstr1(15));
  lcd_display(getfstr1(48), line1, line2, getfstr4(62), 1);
}


/* 
 *  
 * ,____________________.
 * |Heated. Press Start |
 * |when ready to add   |
 * |food.              H|
 * |                   P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */
 
void display_foodwait() {
  lcd_display(getfstr1(28), getfstr2(59), getfstr3(24), getfstr4(62), 1);
}


/* 
 *  
 * ,____________________.
 * |Safe to remove lid  |
 * |Add food. fit lid,  |
 * |then press Start.  □|
 * |                   □|
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */

void display_foodadding() {
  lcd_display(getfstr1(53), getfstr2(6), getfstr3(57), getfstr4(62), 1);
}


/* 
 *  
 * ,____________________.
 * |Ready to cook.      |
 * |Press Start to cook.|
 * |Cook temp: NN.NUU  □|
 * |Cur. temp: NN.NUU  □|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is '°C' or '°F'
 * 
 */
 
void display_food_added() {
  char line1[21], *line2;

  if(millimatch()) return;
  strcpy(line1, cat_temp(set_temp, getfstr1(13)));
  line2 = cat_temp(cur_temp, getfstr1(15));
  lcd_display(getfstr1(51), getfstr2(49), line1, line2, 1);
}


/* 
 *  
 * ,____________________.
 * |Cook time: HH:MM:SS |
 * |Remaining: HH:MM:SS.|
 * |Cook temp: NN.NUU  H|
 * |Cur. temp: NN.NUU  P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is '°C' or '°F'
 * 
 */
 
 void display_cook() {
  char line1[21], line2[21], line3[21], *line4;

  if(millimatch()) return;
  strPcpy(line1, 14);
  strcat(line1, fmt_timen(cook_time));
  strPcpy(line2, 52);
  strcat(line2, fmt_timen(cook_time - done_time));;
  strcpy(line3, cat_temp(set_temp, getfstr1(13)));
  line4 = cat_temp(cur_temp, getfstr1(15));
  lcd_display(line1, line2, line3, line4, 1);
}


/* 
 *  
 * ,____________________.
 * |Cooking done. Press |
 * |Start and remove lid|
 * |Hold time: HH:MM:SSH|
 * |Cur. temp: NN.NUU  P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is '°C' or '°F'
 * 
 */
 
 void display_empty() {
  char line1[21], *line2;

  if(millimatch()) return;
  strPcpy(line1, 31);
  strcat(line1, fmt_timen(done_time - cook_time));
  line2 = cat_temp(cur_temp, getfstr1(15));
  lcd_display(getfstr1(10), getfstr2(55), line1, line2, 1);
}


/* 
 *  
 * Query display: Depth, Temperature, Heater on/off,
 * Pump on/off, Duty cycle (heat on time:total time),
 * Average power, Firmware version.
 * ,____________________.
 * |D NN.NUU T NNN.NUU  |
 * |Heat nnn Pump nnn   |
 * |Duty cycle: 1:NNN   |
 * |Avg pwr NNNW F/W N.N|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is 'cm/°C' or 'in/°F'
 * 
 */
 
void display_query() {
  char line1[21], line2[21], line3[21], line4[21];
  int d;
  
  if(millimatch()) return;
  strPcpy(line1, 16);
  strcat(line1, fmt_depth((float) depth));
  strcat(line1, cat_temp(cur_temp, getfstr1(56)));
  strPcpy(line3, 23);
  strcat(line3, fmt_float(duty_cycle, 4, 1));
  strPcat(line3, 86);
  strPcpy(line4, 7);
  strcat(line4, fmt_float(avg_power, 3, 0));
  strPcat(line4, 33);
  strcat(line4, fmt_float(inst_power, 3, 0));
  strPcpy(line2, 58);
  strcat(line2, fmt_float((float) firmware / 10.0, 1, 1));
  strPcat(line2, 78);
  strcat(line2, fmt_float((float) FW_EC, 2, 0));
  lcd_display(line1, line2, line3, line4, 1);
}


/*
 * 
 * Diagnostic display functions
 * 
 */


/* 
 *  
 * Diagnostic Depth calibration,
 * Depth, and avg/inst power usage
 * ,____________________.
 * |D cal m NNNN.NNNNN  |
 * |D cal k NNNN.NNNNN  |
 * |Depth NN.NUU       H|
 * |P: avg NNN in NNN  P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * where UU is 'cm' or 'in'
 * 
 */

void display_diag1() {
  char line1[21], line2[21];
  
  if(millimatch()) return;
  strcpy(line1, getfstr1(19));
  strcat(line1, fmt_float(caldepthm,4, 5));
  strcpy(line2, getfstr1(18));
  strcat(line2, fmt_float(caldepthk,4, 5));
  lcd_display(line1, line2, getfstr2(62), getfstr2(62), 0);
}


/* 
 *  
 * Diagnostic Temperature calibration
 * and raw Temp/Depth readings
 * ,____________________.
 * |T cal m NNNN.NNNNN  |
 * |T cal k NNNN.NNNNN  |
 * |Depth raw: NNNN    H|
 * |Temp. raw: NNN     P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */

void display_diag2() {
  char line1[21], line2[21], line3[21];

  if(millimatch()) return;
  strcpy(line1, getfstr1(75));
  strcat(line1, fmt_float((float) lidon, 1, 0));
  strPcat(line1, 47);
  strcat(line1, fmt_float((float) interlock, 1, 0));
  strPcpy(line2, 62);
  strPcpy(line3, 66);
  strcat(line3, fmt_float((float) analogRead(DMEASURE), 4, 0));
  strPcat(line3, 4);
  lcd_display(line1, line2, line3, getfstr2(62), 1);
}


/* 
 *  
 * Diagnostic RAM information
 * ,____________________.
 * |Stack: NNNN    :    |
 * | Heap: NNNN         |
 * | Free: NNNN        H|
 * |Lid: N F/W ec NN   P|
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */

void display_diag3() {
  char line1[21], line2[21], line3[21], line4[21];
  
  if(millimatch()) return;
  strPcpy(line1, 54);
  strcat(line1, fmt_float((float) ((int) stackptr), 4, 0));
  strPcpy(line2, 26);
  strcat(line2, fmt_float((float) ((int) heapptr), 4, 0));
  strPcpy(line3, 25);
  strcat(line3, fmt_float((float) (stackptr - heapptr), 4, 0));
  strcpy(line4, fmt_float((float) number_samples, 8, 0));
  lcd_display(line1, line2, line3, line4, 1);
}

/*
 * 
 * End of diagnostic display functions
 * 
 */


/*
 * Called by display routines to ensure that the LCD is updated
 * only if 1 second has elapsed since the last update.
 * Is overridden on an FSM state change, which sets dwdmillis to -1
 * 
 */
 
byte millimatch() {
  if(dwdmillis != millis() / 1000) {
    dwdmillis = millis() / 1000;
    return 0;
  } else return 1;
}


/* 
 *  
 * Function to display backlight value to be changed
 * in setup mode.
 * ,____________________.
 * |Backlight:          |
 * | <- val ->          |
 * | Selector to change |
 * |                    |
 * `~~~~~~~~~~~~~~~~~~~~'
 * Where val is Off/Low/Medium/High
 * 
 */
 
void display_set_bkl(byte v) {
  char str[21];
  
  if(millimatch()) return;
  strcpy(str, getfstr1(64));
  switch ((int) v) {
    case 0:
      strPcat(str, 35);
      break;

    case 1:
      strPcat(str, 38);
      break;

    case 2:
      strPcat(str, 29);
      break;
  }
  strPcat(str, 65);
  lcd_display(getfstr1(8), str, getfstr4(60), getfstr2(62), 1);
}


/*
 *
 * Function to display Contrast value to be changed
 * in setup mode.
 * ,____________________.
 * |Contrast:           |
 * | <- val ->          |
 * | Selector to change |
 * |                    |
 * `~~~~~~~~~~~~~~~~~~~~'
 * Where val is Flat/Low/Medium/High/Bright
 *
 */

void display_set_con(byte v) {
  char str[21];

  strcpy(str, getfstr1(64));
  switch ((int) v) {
    case 0:
      strPcat(str, 35);
      break;

    case 1:
      strPcat(str, 38);
      break;

    case 2:
      strPcat(str, 29);
      break;

    case 3:
      strPcat(str, 42);
      break;
  }
  strPcat(str, 65);
  lcd_display(getfstr1(77), str, getfstr4(60), getfstr2(62), 1);
}


/* 
 *  
 * General purpose function to display a value to be changed in
 * setup mode.
 * ,____________________.
 * |Thing to change:    |
 * | <- val ->          |
 * | Selector to change |
 * |                    |
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */
 
void display_sel(char *str, float val, byte dig, byte dec, char *u)
{
  char tmp[20];

  if(millimatch()) return;
  strcpy(tmp, getfstr4(64));
  strcat(tmp, fmt_float(val, dig, dec));
  strcat(tmp, u);
  strcat(tmp, getfstr4(65));
  lcd_display(str, tmp, getfstr2(60), getfstr3(64), 1);
 }


/* 
 *  
 * Interlock failure
 * ,____________________.
 * |  Cannot continue   |
 * | Interlock failure  |
 * |  <failure reason>  |
 * |   Please correct   |
 * `~~~~~~~~~~~~~~~~~~~~'
 * 
 */
 
void display_intfail() {
  byte i;

  if(millimatch()) return;
  switch(ifr) {
    case UNDERFILL:
      i = 71;
      break;
    
   case OVERFILL:
      i = 72;
      break;
    
   case LIDOFF:
      i = 73;
      break;
    
  }
  lcd_display(getfstr1(69), getfstr2(70), getfstr3(i), getfstr4(74), 0);
  delay(1000);
}


/*
 * 
 * String format routines. Printf, where art thou?
 *
 */


/*
 * 
 * Format a float as 'dig' digits, and 'dec' decimal places 
 * The dtostrf function is supposed to pad leading blanks,
 * but doesn't - just live with it for now. Makes the 'dig'
 * parameter useless, c'est la vie. 
 * Update: fixed-ish, using spad()
 * Update2: Not.
 *   Return:
 *     pointer to a static string
 * 
 */

char *fmt_float(float value, byte dig, byte dec) {
  static char str[12];

  dtostrf(value, (dig + dec + dec ? 1 : 0), dec, str);
  return str;
//  return spad(str, (dig + dec + dec ? 1 : 0));
}


/* 
 *  
 * Format temperature as NNN.N°F/°C
 * Expects temp to be in the relevant units.
 *   Return:
 *     pointer to a static string
 *
 */
 
char *fmt_temp(float temp) {
  static char str[8];
  int i;

  strcpy(str, fmt_float(temp, 3, 1));
  if(units) strPcat(str, 68);
  else strPcat(str, 63);
  return str;
}


/* 
 *  
 * Format depth as NN.Nin/cm.
 * Assumes depth to be in cm.
 *   Return:
 *     pointer to a static string
 *
 */

char *fmt_depth(float depth) {
  static char str[7];
  
  if(units) depth = depth / 2.54; // converts to inches
  strcpy(str, fmt_float(depth, 2, 1));
  if(units) strPcat(str, 34);
  else strPcat(str, 9);
  return str;
}


/* 
 *  
 * Format time as hh:mm:ss
 *   Return:
 *     pointer to a static string
 *
 */
  
char *fmt_timen(unsigned long int time) {
  static char str[9];
  byte h, m, s, val;

  h = time / 3600000UL;
  m = (time / 60000UL) % 60UL;
  s = (time / 1000UL) % 60UL;
  val = h / 10;
  str[0] = val + '0';
  val = h % 10;
  str[1] = val + '0';
  str[2] = ':';
  val = m / 10;
  str[3] = val + '0';
  val = m % 10;
  str[4] = val + '0';
  str[5] = ':';
  val = s / 10;
  str[6] = val + '0';
  val = s % 10;
  str[7] = val + '0';
  str[8] = 0;  
  return str;
}


/* 
 *  
 * Format time (D or H or M) as:
 *   <- TT <string> ->
 *   Return:
 *     pointer to a static string
 *
 */
  
char *cat_stime(int t, char *s) {
  static char str[17];

  strcpy(str, getfstr2(64));
  strcat(str, fmt_float((float) t, 2, 0));
  strcat(str, getfstr2(4));
  strcat(str, s);
  strcat(str, getfstr2(65));
  return str;
}


/* 
 *  
 * Format temp as <string> NNN.NUU
 * where UU is '°C' or '°F'
 * Assumes temp to be in °C.
 *   Return:
 *     pointer to a static string
 *
 */
 
char *cat_temp(float t, char *s) {
  static char str[20];

  strcpy(str, s);
  if(units) t = (t * 9.0) / 5.0 + 32.0; // converts to Fahrenheit
  strcat(str, fmt_temp(t));
  strPcat(str, 4);
  return str;
}

void strPcpy(char *s, byte i) {
  strcpy(s, getfstr1(i));
}

void strPcat(char *s, byte i) {
  strcat(s, getfstr1(i));
}


/*
 * 
 * End string formatting functions.
 * 
 */


char *spad(char *s, byte l) {
  static char p[21];
  byte n;
  
  strcpy(p, getfstr1(0));
  n = strlen(s);
  p[20 - n] = 0;
  strcat(p, s);
  return(&p[20 - l]);
}
/* 
 *  
 * Gets the values of the stack and heap pointers
 * .
 */

void check_mem() {
  stackptr = (uint8_t *)malloc(4);  // use stackptr temporarily
  heapptr = stackptr;               // save value of heap pointer
                                    // copy heap pointer from stackptr, because
                                    // we have to free the malloced bytes, and free()
                                    // zeroes its arg. So it's just clearing stackptr,
                                    // which we then set.
  free(stackptr);                   // free up the memory again (sets stackptr to 0)
  stackptr =  (uint8_t *)(SP);      // save value of stack pointer
}

/*
 *
 * From here on is an Easter egg, where you get to play a Star Wars game, flying
 * the Millenium Falcon against TIE fighters.
 *
 */
 
#define ENEMY_FIRE 55
#define HORIZONTAL 0
#define VERTICAL 1
#define FALCON_AFT byte(5)
#define FALCON_FORE byte(6)
#define TIE_FIGHTER byte(7)

const byte sws[8] PROGMEM =        { B00011111, B00000000, B00000000, B00000000,
                                     B00000000, B00000000, B00000000, B00000000 };
const byte sww[8] PROGMEM =        { B00010000, B00010000, B00010001, B00010011,
                                     B00010110, B00011100, B00001000, B00000000 };
const byte swx[8] PROGMEM =        { B00000001, B00000001, B00010001, B00011001,
                                     B00001101, B00000111, B00000010, B00000000 };
const byte tiefighter[8] PROGMEM = { B00000000, B00000000, B00011111, B00000100,
                                     B00001110, B00001110, B00000100, B00011111 };
const byte falconb[8] PROGMEM =    { B00000111, B00001101, B00011111, B00011101,
                                     B00011111, B00001101, B00001111, B00000001 };
const byte falconf[8] PROGMEM =    { B00000000, B00011000, B00011111, B00010000,
                                     B00011111, B00011000, B00000000, B00011000 };
byte speedy;
byte level;
byte enemy_fire;
byte o;
byte hit[3];
byte dead;
char lives;
byte cnt;
int score;
byte mf;
byte m[2];
byte tf[3];
char t[3][2];
byte en[3];
char e[3][2];
byte demo = 0;
byte f1 = 0;

void sinv() {
  char i, j, k;

  if(!digitalRead(TEMP_UNITS)) demo = 1;
  else demo = 0;

  cnt++;
  cnt %= 64;
  if(!cnt && speedy > 0) {
    speedy--;
  }
  
  // First time around
  if(!f1) {
    level = 0;
    EncoderS.set(0, 0, 0, 3);
    f1 = 1;
    lcd_chardef(5);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&falconb[i]));
    lcd_chardef(6);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&falconf[i]));
    lcd_chardef(7);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&tiefighter[i]));
    initgame(1);
  }
  
  // Game Over
  if(lives <= 0 && dead == 0 && f1) {
    lcd_setcursor(19, 0);
    lcd.write(' ');
    lcd_setcursor(5, 1);
    lcd.print(getfstr1(79));
    delay(100);
    if(!demo && ButtonS.time() || cnt == 0) {
      level = 0;
      initgame(1);
    }
    return;
  }

  // You Win
  if(score == 255 && level == 5) {
    pscore(score);
    lcd_setcursor(5, 1);
    lcd.print(getfstr1(80));
    delay(100);
    if(ButtonS.time() || !cnt ) {
      level = 0;
      initgame(1);
    }
    return;
  }
 
   // End of level
  if(score == 255 || level == 0) {
    if(level == 255) level = 0;
    level++;
    cnt = 100;
    lcd_clear();
    i = 3;
    if(level < 100) i++;
    if(level < 10) i++;
    lcd_setcursor(4, 0);
    lcd.print(getfstr1(81));
    lcd.print(spad(fmt_float((float) level, 1, 0), 1));
    lcd.print(getfstr1(5));
    lcd_chardef(1);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&sws[i]));
    lcd_chardef(2);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&sww[i]));
    lcd_chardef(3);
    for(i = 0; i < 8; i++) lcd.write(pgm_read_byte(&swx[i]));
    lcd_setcursor(7, 1);
    lcd.print(getfstr1(82));
    lcd_setcursor(7, 2);
    lcd.print(getfstr1(84));
    lcd_setcursor(6, 3);
    lcd.print(getfstr1(83));
    delay(6000);
    initgame(1);
  }
 
  lcd_setcursor(19, 0);
  switch(lives) {
    case 3:
     lcd.write(0xd0);
     break;
     
    case 2:
     lcd.write('=');
     break;
     
    case 1:
     lcd.write('-');
     break;
     
    case 0:
     lcd.write(' ');
     break;
     
  }

  if(!demo) {
    i = EncoderS.get();
  } else {
    i = o;
    if(!(cnt % 7)) {
      char t;
      t = 21;
      j = i;
      if(!mf) {
        for(k = 0; k < 3; k++) {
          if(en[k] && !hit[k] && e[k][HORIZONTAL] < t && e[k][HORIZONTAL] > 2) {
          t = e[k][HORIZONTAL];
          j = e[k][VERTICAL];
          } 
        }
        if(i < j) i++;
        if(i > j) i--;
      }
    }
  }

  lcd_setcursor(0, o);
  lcd.write(' ');
  lcd.write(' ');
  lcd_setcursor(0, i);
  if(!dead) {
    lcd.print(getfstr1(85));
   } else {
    switch(dead) {
      case 16:
        explode(1, 0, i);
        break;
        
      case 12:
        explode(2, 0, i);
        break;
        
      case 8:
        explode(3, 0, i);
        break;
        
      case 4:
        explode(0, 0, i);
        break;
    }
    dead--;
    if(!dead) cnt = 100;
  }
  o = i;
  if((j = ButtonQ.timeup())) {
    if(j >= 10) {
    score = 255;
      return;
    }
    lcd_contrast(contrast);
    spegg = 0;
    egg = 0;
    f1 = 0;
    level = 0;
    initgame(1);
    idle_reset();
    return;
  }

  if(!demo) {
    if(!digitalRead(START_SELECT) && !mf) {
      m[HORIZONTAL] = 2;
      m[VERTICAL] = i;
      mf = 1;
    }
  } else {
    if(!mf) {
      if(en[0] && (i == e[0][VERTICAL]) && !hit[0]
      || en[1] && (i == e[1][VERTICAL]) && !hit[1]
      || en[2] && (i == e[2][VERTICAL]) && !hit[2]) {
        m[HORIZONTAL] = 2;
        m[VERTICAL] = i;
        mf = 1;
      }
    }
  }
  for(k = 0; k < 3; k++) {
    // Generate new TIE Fighter if chance requires it
    if(!en[k] && !random(0, 25)) {
      i = random(0, 4);
      while((en[0] && e[0][VERTICAL] == i)
         || (en[1] && e[1][VERTICAL] == i)
         || (en[2] && e[2][VERTICAL] == i)) {
        i = random(0, 4);
      }
      e[k][HORIZONTAL] = 19;
      e[k][VERTICAL] = i;
      en[k] = 7;
    }
    if(en[k]) en[k]++;
    // TIE fighter checks performed every other go through
    if(en[k] && !(en[k] % 2)) {
      if(en[k] == 8) {
        if(e[k][HORIZONTAL] < 0) {
          en[k] = 0;
          lcd_setcursor(0, e[k][VERTICAL]);
          lcd.write(' ');
          score -= 5;
        }
        if(e[k][VERTICAL] == o && e[k][HORIZONTAL] <= 1) {
          lcd_setcursor(e[k][HORIZONTAL], e[k][VERTICAL]);
          lcd.print(getfstr1(85));    
          hit[k] = 1;
          if(!(dead % 4)) {
            dead = 16;
            lives --;
            initgame(0);
          }
        }
      }
//      lcd_setcursor(e[k][HORIZONTAL], e[k][VERTICAL]);
//      lcd.write(TIE_FIGHTER);
      if(e[k][HORIZONTAL] >= 0) {
        switch(hit[k]) {
          case 0:
            if(en[k] == 8) {
              if(e[k][HORIZONTAL] < 19) {
                lcd_setcursor(e[k][HORIZONTAL] + 1, e[k][VERTICAL]);
                lcd.write(' ');
              }
              lcd_setcursor(e[k][HORIZONTAL], e[k][VERTICAL]);
              lcd.write(TIE_FIGHTER);
            }
            break;

          case 1:
            en[k] = 0;
            explode(0, e[k][HORIZONTAL], e[k][VERTICAL]);
            break;
            
          case 9:
            lcd_setcursor(e[k][HORIZONTAL], e[k][VERTICAL]);
            lcd.write(TIE_FIGHTER);
          case 8:
          case 7:
          case 6:
          case 5:
          case 4:
          case 3:
          case 2:
            explode(11 - hit[k], e[k][HORIZONTAL], e[k][VERTICAL]);
            break;
        }
        if(hit[k]) hit[k]--;
      }
      if(en[k] == 8) {
        e[k][HORIZONTAL]--;
        if(en[k]) en[k] = 1;
      }
      if(en[k] && !tf[k] && !hit[k] && !random(0, enemy_fire)) {
        tf[k] = 1;
        t[k][HORIZONTAL] = e[k][HORIZONTAL];
        t[k][VERTICAL] = e[k][VERTICAL];
      }
    }
  }
  if(mf) {
    if(m[HORIZONTAL] > 2) {
      lcd_setcursor(m[HORIZONTAL] - 2, m[VERTICAL]);
      lcd.write(' ');
    }
    if(m[HORIZONTAL] < 20) {
      lcd_setcursor(m[HORIZONTAL], m[VERTICAL]);
      lcd.write('_');
    }
    for(k = 0; k < 3; k++) {
      if(en[k] && e[k][VERTICAL] == m[VERTICAL] && e[k][HORIZONTAL] <= m[HORIZONTAL]) {
        lcd_setcursor(m[HORIZONTAL] - 1, m[VERTICAL]);
        lcd.print(getfstr1(5));
        explode(1, e[k][HORIZONTAL], e[k][VERTICAL]);
        hit[k] = 9;
        score++;
        cnt = 100;
        mf = 0;
      }
     if(tf[k] && t[k][VERTICAL] == m[VERTICAL] && t[k][HORIZONTAL] <= m[HORIZONTAL]) {
        lcd_setcursor(m[HORIZONTAL] - 1, m[VERTICAL]);
        lcd.print(getfstr1(76));
        lcd_setcursor(t[k][HORIZONTAL] + 1, t[k][VERTICAL]);
        lcd.print(getfstr1(76));
        delay(70);
        lcd_setcursor(m[HORIZONTAL] - 1, m[VERTICAL]);
        lcd.print(getfstr1(5));
        lcd_setcursor(t[k][HORIZONTAL] + 1, t[k][VERTICAL]);
        lcd.print(getfstr1(5));
        lcd_setcursor(e[k][HORIZONTAL] + 1, e[k][VERTICAL]);
        lcd.write(TIE_FIGHTER);
        mf = tf[k] = 0;
      }
    }
    m[HORIZONTAL]++;
    if(m[HORIZONTAL] == 21) {
      mf = 0;
      lcd_setcursor(19, m[VERTICAL]);
      lcd.write(' ');
    }
  }
  for(k = 0; k < 3; k++) {
    if(tf[k]) {
      if(t[k][HORIZONTAL] < 18 && e[k][HORIZONTAL] != t[k][HORIZONTAL] + 1) {
        lcd_setcursor(t[k][HORIZONTAL] + 2, t[k][VERTICAL]);
        lcd.write(' ');
      }
      if(t[k][HORIZONTAL] >= 0) {
        lcd_setcursor(t[k][HORIZONTAL], t[k][VERTICAL]);
        lcd.write('_');
      }
      if(t[k][VERTICAL] == o && t[k][HORIZONTAL] <= 1) {
        lcd_setcursor(0, o);
        if(!(dead % 4)) {
          dead = 16;
          lives --;
          initgame(0);
        }
      }
      t[k][HORIZONTAL]--;
      if(t[k][HORIZONTAL] < 0) {
        tf[k] = 0;
        lcd_setcursor(0, t[k][VERTICAL]);
        lcd.write(' ');
        lcd.write(' ');
      }
    }
  }
  delay(speedy + 15);
  pscore(score);
}

void spos(byte p, byte c) {
  byte k;

  for(k = 0; k < 3; k++) {
    if(e[k][VERTICAL] == 3 && e[k][HORIZONTAL] ==  p - 1) return;
    if(t[k][VERTICAL] == 3 && t[k][HORIZONTAL] ==  p) return;
  }
  lcd_setcursor(p, 3);
  lcd.write(byte(c));
}

void pscore(int s) {
  static byte os = 0;
  byte i;
  char *tmp;

  if(os >= 10 && s < 10) spos(18, ' ');
  if(os >= 100 && s < 100) spos(17, ' ');
  tmp = spad(fmt_float((float) s, 5, 0), 5);
  for(i = 0; i < 5; i++) if(tmp[i] != ' ') {
    spos(15 + i, tmp[i]);
  }
  os = s;
}

void initgame(byte f)
{
  lcd_clear();
  if(f) {
    lives = 3;
    score = 0;
    if(level >= 5) {
      speedy = 0;
      enemy_fire = 14;
    } else {
      speedy = 90 - level * 10;
      enemy_fire = ENEMY_FIRE - (level - 1) * 10;
    }
    cnt = 0;
  }
  hit[0] = hit[1] = hit[2] = mf = tf[1] = tf[2] = tf[3] = en[0] = en[1] = en[2] =  0;
  dead = 0;
  o = 0;
  EncoderS.set(0, 0, 0, 3);
}

#define PI 3.1415927
#define CY 12
#define CX 8
#define NUM 48

void explode(byte d, byte h, byte v) {
  int i;
  float r, t;
  int x, y;
  static byte o, p;
  static float cobj[NUM][2];
  byte e[40];
  
  memset(e, 0, 40);
  if(d == 1) {
    do_exp(o - 1, p - 1, ' ');
    do_exp(o, p - 1, ' ');
    do_exp(o - 1, p, ' ');
    do_exp(o + 1 , p + 1, ' ');
    do_exp(o, p + 1, ' ');
    o = h;
    p = v;
    for(i = 0; i < NUM; i++) {
      t = ((float) i * 360.0 / (float) NUM) * PI / 180.0;
      r = (float) ((random(8, 23)) * 20) / 100.0;
      if(t < PI ) r *= 1.4;
      cobj[i][0] = r;
      cobj[i][1] = t;
      x = (r * sin(t) + 0.5) + CX;
      y = (r * cos(t) + 0.5) + CY;
      if(x >= 6 && x < 11 && y >= 0 && y < 8) e[y] |= 0x10 >> (x - 6);
      if(x >= 0 && x < 5 && y >= 9 && y < 17) e[8 + y - 9] |= 0x10 >> x;
      if(x >= 6 && x < 11 && y >= 9 && y < 17) e[16 + y - 9] |= 0x10 >> (x - 6);
      if(x >= 12 && x < 17 && y >= 9 && y < 17) e[24 + y - 9] |= 0x10 >> (x - 12);
      if(x >= 6 && x < 11 && y >= 18 && y < 26) e[32 + y - 18] |= 0x10 >> (x - 6);
    }
    set_exp(e);
    do_exp(o, p - 1, 0);
    do_exp(o - 1, p, 1);
    do_exp(o, p, 2);
    do_exp(o + 1 , p, 3);
    do_exp(o, p + 1, 4);
  } else if(d) {
    for(i = 0; i < NUM; i++) {
      r = cobj[i][0];
      t = cobj[i][1];
      x = (r * sin(t) * (float) d / 2.0 + 0.5) + CX;
      y = (r * cos(t) * (float) d / 2.0 + 0.5) + CY;
      if(x >= 0 && x < 5 && y >= 0 && y < 8) e[y] |= 0x10 >> x;
      if(x >= 6 && x < 11 && y >= 0 && y < 8) e[8 + y] |= 0x10 >> (x - 6);
      if(x >= 0 && x < 5 && y >= 9 && y < 17) e[16 + y - 9] |= 0x10 >> x;
      if(x >= 0 && x < 5 && y >= 18 && y < 26) e[24 + y - 18] |= 0x10 >> x;
      if(x >= 6 && x < 11 && y >= 18 && y < 26) e[32 + y - 18] |= 0x10 >> (x - 6);
    }
    set_exp(e);
    do_exp(o, p - 1, ' ');
    do_exp(o - 1, p, ' ');
    do_exp(o, p, ' ');
    do_exp(o + 1 , p, ' ');
    do_exp(o, p + 1, ' ');
    do_exp(o - 1, p - 1, 0);
    do_exp(o, p - 1, 1);
    do_exp(o - 1, p, 2);
    do_exp(o + 1 , p + 1, 3);
    do_exp(o, p + 1, 4);
  } else {    
    do_exp(o - 1, p - 1, ' ');
    do_exp(o, p - 1, ' ');
    do_exp(o - 1, p, ' ');
    do_exp(o + 1 , p + 1, ' ');
    do_exp(o, p + 1, ' ');
  }
}

void set_exp(byte *c) {
  byte i;
  
  lcd_chardef(0);
  for(i = 0; i < 8; i++) lcd.write(c[i]);
  lcd_chardef(1);
  for(i = 0; i < 8; i++) lcd.write(c[8 + i]);
  lcd_chardef(2);
  for(i = 0; i < 8; i++) lcd.write(c[16 + i]);
  lcd_chardef(3);
  for(i = 0; i < 8; i++) lcd.write(c[24 + i]);
  lcd_chardef(4);
  for(i = 0; i < 8; i++) lcd.write(c[32 + i]);
}

void do_exp(byte x, byte y, byte c) {
  byte k;

  if(x < 0 || x > 19 || y < 0 || y > 3) return;
  for(k = 0; k < 3; k++) 
    if(en[k] && e[k][HORIZONTAL] == x && e[k][VERTICAL] == y) return;
  lcd_setcursor(x, y);
  lcd.write(byte(c));
}

