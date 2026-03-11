/*
  Rokenbok controller emulator - explicit pulse mapping version

  Idea:
  - After SYNC, count clock pulses.
  - For each pulse number, decide whether DATA should be active
    based on the current command.
  - This avoids bit shifting bugs while debugging the protocol.

  Notes:
  - Adjust the pulse numbers below if your command mapping is off.
  - This version assumes "1" means drive DATA high for that pulse.
  - If the protocol is active-low, invert the output logic.
*/

const uint8_t syncPin = 2;   // external interrupt
const uint8_t vccInPin = 3;  // optional power detect
const uint8_t clkPin  = 4;   // pin change interrupt
const uint8_t dataPin = 8;
const uint8_t ledPin  = 13;

// Current command from PC
volatile char currentCommand = '!';

// Frame state
volatile bool frameActive = false;
volatile uint8_t pulseCount = 0;

// Track D4 for pin-change edge detection
volatile uint8_t lastPortDState = 0;

// --------------------------------------------------
// Helper: return true if DATA should be active on this pulse
// --------------------------------------------------
bool shouldPulseData(char cmd, uint8_t pulse)
{
  switch (cmd)
  {
    case '0': // select
      return (pulse == 0);

    case 'w': // forward
      return (pulse == 6);

    case 's': // backward
      return (pulse == 7);

    case 'd': // right
      return (pulse == 8);

    case 'a': // left
      return (pulse == 9);

    case 'i': // lift down
      return (pulse == 10);

    case 'k': // lift up
      return (pulse == 11);

    case '4': // grab close
      return (pulse == 12);

    case '6': // grab open
      return (pulse == 13);

    case '!': // idle
    default:
      return false;
  }
}

void setup()
{
  pinMode(syncPin, INPUT);
  pinMode(vccInPin, INPUT);
  pinMode(clkPin, INPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(ledPin, OUTPUT);

  PORTB |= (1 << PB0);   // set D8 high
  PORTB &= ~(1 << PB0);  // set D8 low

  Serial.begin(250000);

  attachInterrupt(digitalPinToInterrupt(syncPin), syncISR, RISING);

  // Enable pin change interrupt for D4
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT20);
  lastPortDState = PIND;
  DDRB |= (1 << DDB0); //set pin 8 as output
}

void loop()
{
  // Read commands from PC
  while (Serial.available() > 0)
  {
    currentCommand = Serial.read();
  }
}

void syncISR()
{
  pulseCount = 0;
  frameActive = true;
  PORTB &= ~(1 << PB0);
}

// Called on each rising edge of clock
inline void handleClockFallingEdge()
{
  if (!frameActive)
    return;

  //default LOW first
  PORTB &= ~(1 << PB0);
  pulseCount++;
}

inline void handleClockRisingEdge()
{
  if (shouldPulseData(currentCommand, pulseCount))
  {
    PORTB |= (1 << PB0);
  }

  // End frame after expected number of pulses
  if (pulseCount >= 17)
  {
    frameActive = false;
    digitalWrite(dataPin, LOW);
  }
}

ISR(PCINT2_vect)
{
  uint8_t currentPortDState = PIND;

  bool oldClk = lastPortDState & (1 << PD4);
  bool newClk = currentPortDState & (1 << PD4);

  // Rising edge detect on D4
  if (!oldClk && newClk)
  {
    handleClockRisingEdge();
  }
  else
  {
    handleClockFallingEdge();
  }

  lastPortDState = currentPortDState;
}