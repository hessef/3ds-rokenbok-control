/*
  Rokenbok controller emulator - multiple input version
*/

const uint8_t syncPin = 2;   // external interrupt
const uint8_t vccInPin = 3;  // optional power detect
const uint8_t clkPin  = 4;   // pin change interrupt
const uint8_t dataPin = 8;
const uint8_t ledPin  = 13;

//command mask
uint8_t rxBuffer[2];
uint8_t rxIndex = 0;
uint16_t commandMask;

// Frame state
volatile bool frameActive = false;
volatile uint8_t pulseCount = 0;

// Track D4 for pin-change edge detection
volatile uint8_t lastPortDState = 0;

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
  while (Serial.available())
  {
    rxBuffer[rxIndex++] = Serial.read();
    if (rxIndex == 2)
    {
      commandMask = ((uint16_t)rxBuffer[0] << 8) | rxBuffer[1];
      rxIndex = 0;
    }
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

  pulseCount++;
}

inline void handleClockRisingEdge()
{
  if (commandMask & (1 << pulseCount))
  {
    PORTB |= (1 << PB0);
  }
  else
  {
    PORTB &= ~(1 << PB0);
  }

  // End frame after expected number of pulses
  if (pulseCount >= 17)
  {
    frameActive = false;
    PORTB &= ~(1 << PB0);
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