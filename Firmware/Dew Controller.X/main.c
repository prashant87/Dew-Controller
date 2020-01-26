	/*
 * File:   main.c
 * Author: Andre
 *
 * Created on 12. Januar 2020, 13:20
 */

#include "common.h"
#include "config.h"
#include "menuhelper.h"
#include "oled.h"

//-----------------------------------------------------------------------------
// Definitions
//-----------------------------------------------------------------------------
#define TEMP_AUX_MIN -30
#define TEMP_AUX_MAX 100
#define SENSOR_UPDATE_INTERVALL 50
#define SENSOR_TIMEOUT 20

#define MIN_CURRENT 0.05
#define MAX_CURRENT 3.0
#define VOLT_CRIT_HIGH 13.8
#define VOLT_WARN_HIGH 13.0
#define VOLT_WARN_LOW 11.4
#define VOLT_TURN_OFF 11.0

#define EPSILON 0.95
#define RHO 5.67e-8
#define PI 3.14
#define C_TO_K 273.15
#define INCH_TO_MM 0.0254
#define K_FACTOR 0.85
#define WIDTH 0.03

#define NUM_SAMPLES 20

#define ALPHA(x) ( (uint32_t)(x * 65535) )

#define ADC_TO_I(counts) ( (counts * 5.0) / (1023.0 * 0.05 * 50.0) )
#define ADC_TO_V(counts) ( (counts * 5.0 * (150.0 + 47.0)) / (1023.0 * 47.0) )

//-----------------------------------------------------------------------------
// Function Prototypes
//-----------------------------------------------------------------------------
void initialize(void);
void convertAnalogValues(t_globalData *data);
void checkSensor(t_globalData *data);
uint16_t ema(uint16_t in, uint16_t average, uint32_t alpha);
uint8_t getAvgChannelCurrents(t_globalData *data);
void setSwitch(uint8_t channel, uint8_t state);
void calcRequiredPower(t_globalData *data);
void testHeaters(t_globalData *data);

//-----------------------------------------------------------------------------
// Main program loop
//-----------------------------------------------------------------------------
void main(void)
{		
	t_globalData data;
	
	initialize();
	OLED_PWR = 1;
	PEN = 1;
	OLED_init();
	OLED_loadSpecialChars();
	OLED_returnHome();
	OLED_clearDisplay();
	
	SW_CH1 = 1;

	while (1) {
		CLRWDT();
		convertAnalogValues(&data);
		checkSensor(&data);
		menu(&data);
		__delay_ms(10);	
	}
}

void initGlobalData(t_globalData *data)
{
	uint8_t n;
	
	data->tempC = 0;
	data->relHum = 0;
	data->dewPointC = 0;
	data->sensorVersion = 0;
	data->tempAux = 0;
	data->voltage = 0;
	data->current = 0;
	data->power = 0;
	data->dpOffset = 3.0;
	data->skyTemp = -40;
	data->fudgeFactor 1.0;
	
	for(n = 0; n < NUM_CHANNELS; n++) {
		data.chData[n].lensDia = 4;
		data.chData[n].status = OFF;
		data.chData[n].mode = AUTO;
		data.chData[n].Pmax = 0;
		data.chData[n].Preq = 0;
		data.chData[n].Patt = 0;
		data.chData[n].cur = 0;
	}
		

void systemCheck(t_globalData *data)
{
	if (data->current > MAX_CURRENT) {
		SW_CH1 = 0;
		SW_CH2 = 0;
		SW_CH3 = 0;
		SW_CH4 = 0;
		PEN = 0;
		convertAnalogValues(data);
		if (data->current > MAX_CURRENT) {
			// Alles kaputt. Wegschmeissen, neu kaufen
			error(ERR_NUKED);
		} else {
			error(ERR_OVERCURRENT);
		}		
	}
	if ((data->voltage > VOLT_CRIT_HIGH) || 
		(data->voltage <= VOLT_TURN_OFF)) {
		SW_CH1 = 0;
		SW_CH2 = 0;
		SW_CH3 = 0;
		SW_CH4 = 0;
		PEN = 0;
		OLED_PWR = 0;
		while(1);
	} else if ((data->voltage > VOLT_WARN_HIGH) && (data->voltage <= VOLT_CRIT_HIGH)) {
		error(WARN_VOLT_HIGH);
	} else if ((data->voltage > VOLT_TURN_OFF) && (data->voltage <= VOLT_WARN_LOW)) {
		error(WARN_VOLT_LOW);
	}
}

void calcRequiredPower(t_globalData *data)
{
	uint8_t n;
	float d, A, T1, T2, phi;
	float p, b, Rth;
	
	for(n = 0; n < NUM_CHANNELS; n++) {
		// Calculate thermal radiation
		d = INCH_TO_MM * data->chData[n].lensDia;	// Lens diameter in mm
		A = (PI * pow(d, 2.0)) / 4;					// Exposed area of lens
		// Assuming lens temperature has reached dew point + offset
		T1 = data->dewPointC + data->dpOffset + C_TO_K;
		T2 = data->skyTemp + C_TO_K;
		// Stefan Bolzman Law
		phi = EPSILON * RHO * A * (pow(T1, 4.0) - pow(T2, 4.0));
		// Required power is phi * experimental factor (heat loss etc...)
		data->chData[n].Preq = phi * data->fudgeFactor;		
		
		// Approx. heater temp. required
		p = 2 * PI * INCH_TO_MM * data->chData[n].lensDia;
		A = p * WIDTH;	// Area covered by heater strip
		// Thermal resistance of the lens
		Rth = (data->chData[n].lensDia / 2) * K_FACTOR * A;
		// Delta T
		data->chData[n].dt = phi * Rth - data->dewPointC;		
	}
}

void setSwitch(uint8_t channel, uint8_t state)
{
	switch (channel) {
		case 0:
			SW_CH0 = state;
			break;
		case 1:
			SW_CH1 = state;
			break;
		case 2:
			SW_CH2 = state;
			break;
		case 3:
			SW_CH3 = state;
			break;
		default:
	}
}

//-----------------------------------------------------------------------------
// Test aux. temperature sensor, query main sensor, check data from sensor
//-----------------------------------------------------------------------------
void checkSensor(t_globalData *data)
{
	t_dataPacket *dp;
	static uint32_t sensorUpdateInterval = 0;
	static uint32_t sensorTimeout = 0;
	static uint8_t state = 0;	

	// Check aux. temperature sensor
	if ((data->tempAux < TEMP_AUX_MIN) || (data->tempAux > TEMP_AUX_MAX)) {
		data->status.AUX_SENSOR_OK = 0;
	} else
		data->status.AUX_SENSOR_OK = 1;

	switch (state) {
		case 0:
			// Request data from sensor after SENSOR_UPDATE_INTERVALL
			if (timeSince(sensorUpdateInterval) >= SENSOR_UPDATE_INTERVALL) {
				sensorUpdateInterval = sensorTimeout = timeNow();
				uartSendByte('?');
				state = 1;
			}			
			break;
		case 1:
			// Wait for response
			if (timeSince(sensorTimeout) > SENSOR_TIMEOUT) {
				data->status.SENSOR_OK = 0;
				state = 0;
			} else if (uartIsDataReady()) {
				dp = getDataPacket(); // get Pointer to dataPacket
				if ((dp->header == 0xAA) && (dp->status == 1)) {
					data->tempC = dp->tempC;
					data->relHum = dp->relHum;
					data->dewPointC = dp->dewPointC;
					data->sensorVersion = dp->version;
					data->status.SENSOR_OK = 1;
				} else {
					// set error bits
					data->status.SENSOR_OK = 0;
				}
				state = 0;
			}
			break;
		default:
			state = 0;
	}
}

//-----------------------------------------------------------------------------
// Exponential moving average filter
//-----------------------------------------------------------------------------
uint16_t ema(uint16_t in, uint16_t average, uint32_t alpha)
{
  uint32_t tmp0;
  tmp0 = in * alpha + average * (65536 - alpha);
  return (tmp0 + 32768) / 65536;
}
//-----------------------------------------------------------------------------
// Start ADC conversion and return result
//-----------------------------------------------------------------------------
uint16_t adcGetConversion(uint8_t channel)
{
	ADCON0bits.CHS = channel;
	__delay_us(5);
	ADCON0bits.GO = 1;
	while (ADCON0bits.GO);
	return (uint16_t)((ADRESH << 8) + ADRESL);
}

uint8_t getAvgChannelCurrents(t_globalData *data)
{
	uint16_t adc;
	static uint16_t avg;
	static uint8_t channel = 0;
	static uint8_t samples = 0;
	static uint8_t busy = 0;
	float current;
	
	if (!busy) {
		busy = 1;
		samples = 0;
		channel = 0;
		avg = data->chData[channel].cur;
		setSwitch(channel, ON);
	} else {		
		if (samples++ < NUM_SAMPLES) {
			adc = adcGetConversion(AIN_ISENS);
			avg = ema(adc, avg, ALPHA(0.65));
		} else {
			setSwitch(channel, OFF);
			current = ADC_TO_I(adc);
			
			
			if (current < MIN_CURRENT) {
				if (data->chData[channel].status != CH_OPEN) {
					error(WARN_REMOVED);
					data->chData[channel].status = CH_OPEN);
				}
			} else if (current > MAX_CURRENT) {
				error(WARN_SHORT);
				data->chData[channel].status = CH_DISABLED
				if (!nFAULT) {
					PEN = 0;
					__delay_ms(5);
					PEN = 1;
				}
			} else {			
				data->chData[channel].current = current;
				data->chData[channel].Pmax = data->voltage * current;
				data->chData[channel].DCreq = data->chData[channel].Pmax / data->chData[channel].Preq;
			}	
			
			
			if (channel < NUM_CHANNELS - 1) {
				channel++;
				avg = data->chData[channel].cur;
				setSwitch(channel, ON);
			} else {
				busy = 0;
			}
		}
	}
	return busy;
}


//-----------------------------------------------------------------------------
// Convert ADC counts to actual measurements
//-----------------------------------------------------------------------------
void convertAnalogValues(t_globalData *data)
{
	static uint16_t avgT, avgV, avgI;
	uint16_t adc;
	float f;
	
	adc = adcGetConversion(AIN_TEMP);
	avgT = ema(adc, avgT, ALPHA(0.65));
	adc = adcGetConversion(AIN_VSENS);
	avgV = ema(adc, avgV, ALPHA(0.65));
	adc = adcGetConversion(AIN_ISENS);
	avgI = ema(adc, avgI, ALPHA(0.65));
	data->tempAux = (avgT * 0.1191) - 34.512;
	data->voltage = ((float)avgV * 5.0 * (150.0 + 47.0)) / (1023.0 * 47.0);
	data->current = (avgI * 5.0) / (1023.0 * 0.05 * 50.0);
	data->power = data->voltage * data->current;
}

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------
void initialize(void)
{
	OSCFRQ = 0b00000010; // 4 MHz
	OSCCON1 = 0b01100000; // HINTOSC (1-32MHz), CDIV = 1
	while (!OSCCON3bits.ORDY); // Wait until clock switch is done

	// Peripheral Pin Select (PPS)
	RX1DTPPSbits.RX1DTPPS = 0x17;   //RC7->EUSART1:RX1;
	RC6PPS = 0x0F; //RC4->EUSART1:TX1;    

	// Analog/digital IO
	ANSELA = 0b01000000; // RA6: aux. temp. sensor analog input
	ANSELB = 0b00000000; // PORTB is digital only
	ANSELC = 0b00000011; // RC0: VSENS, RC1: ISENS

	// Data direction registers
	TRISA = 0b11110000; // Outputs: RA0..3: SW_CH1..4, Inputs: RA4..7
	TRISB = 0b00000000;
	TRISC = 0b10000111; // Inputs: RC7: RX, RC2: nFAULT, RC1: ISENS, RC0: VSENS

	// ADC
	ADCON0bits.ADON = 1;
	ADCON1 = 0b11100000; // Right justified, Fosc/64, Vref+=Vdd

	// Timer0: 10ms 
	T0CON0 = 0b10000000; // Enabled, 8-bit mode, postscaler 1:1
	T0CON1 = 0b01000111; // Clock Fosc/4, sync to Fosc, prescaler 1:128
	TMR0 = TMR0_PRELOAD;

	// Timer1: 100ms
	T1CON = 0b00110011; // Prescaler 1:8, 16-bit rd/wr, enabled
	T1CLK = 0b00000001; // Clock Fosc/4
	TMR1 = TMR1_PRELOAD;

	// Interrupts
	PIE0 = 0b00110000; // TMR0IE, IOCIE
	PIE3 = 0b00100000; // RC1IE
	PIE4 = 0b00000001; // TMR1IE
	INTCON = 0b11000000;    // GIE, PEIE

	// Interrupt-on-change
	IOCAP = 0b10110000; // Pos. edge on RA7, RA5, RA4 (PB, ROT_B, ROT_A)
	IOCAN = 0b10110000; // Neg. edge on RA7, RA5, RA4
	IOCCN = 0b00000100; // Neg. edge on RC2 (nFAULT)

	// EUSART
	// SYNC = 0, BRGH = 0, BRG16 = 1, SPBRG = 25 -> 9615 Baud (0.16% error)
	BAUD1CON = 0b00001000; // BRG16 = 1
	SPBRGL = 25;
	RC1STA = 0b10010000; // SPEN = 1, CREN = 1
	TX1STA = 0b00100000; // TXEN = 1
}