#define FW_VERSION "0.0.1"

#include "stm8s.h"
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "display.h"
#include "fixedpoint.h"

#define PWM_HIGH 0x3F
#define PWM_LOW 0xFF
#define PWM_VAL ((PWM_HIGH<<8) | PWM_LOW)

const uint16_t cap_vmin = (1<<10) / 100; // 10 mV
const uint16_t cap_vmax = 35<<10; // 35 V
const uint16_t cap_vstep = (1<<10) / 100; // 10mV
const uint16_t cap_vmaxpwm = 35;

const uint16_t cap_cmin = (1<<10) / 1000; // 1 mA
const uint16_t cap_cmax = 3<<10; // 3 A
const uint16_t cap_cstep = (1<<10) / 1000; // 1 mA
const uint16_t cap_cmaxpwm = 5;

const uint16_t ref_volt_10 = (33<<10) / 10; // 3.3V

uint8_t uart_write_buf[255];
uint8_t uart_write_start;
uint8_t uart_write_len;

uint8_t uart_read_buf[64];
uint8_t uart_read_len;
uint8_t read_newline;

uint16_t cal_vin;
uint16_t cal_vout_a;
uint16_t cal_vout_b;
uint16_t cal_cout_a;
uint16_t cal_cout_b;

uint8_t cfg_name[17];
uint8_t cfg_default;
uint8_t cfg_output;
uint16_t cfg_vset;
uint16_t cfg_cset;
uint16_t cfg_vshutdown;
uint16_t cfg_cshutdown;

uint16_t state_vin_raw;
uint16_t state_vout_raw;
uint16_t state_cout_raw;
uint16_t state_vin;
uint16_t state_vout;
uint16_t state_cout;
uint8_t state_constant_current; // If false, we are in constant voltage
uint8_t state_pc3;

uint16_t out_voltage;
uint16_t out_current;

uint8_t uart_write_ready(void)
{
	return (USART1_SR & USART_SR_TXE);
}

uint8_t uart_read_available(void)
{
	return (USART1_SR & USART_SR_RXNE);
}

void uart_write_ch(const char ch)
{
	if (uart_write_len < sizeof(uart_write_buf))
		uart_write_buf[uart_write_len++] = ch;
}

void uart_write_str(const char *str)
{
	uint8_t i;

	// Move the buffer to the start
	if (uart_write_start > 0) {
		for (i = 0; i < uart_write_len; i++) {
			uart_write_buf[i] = uart_write_buf[i+uart_write_start];
		}
		uart_write_start = 0;
	}

	for(i = 0; str[i] != 0 && uart_write_len < sizeof(uart_write_buf); i++) {
		uart_write_buf[uart_write_len] = str[i];
		uart_write_len++;
	}
}

void uart_write_int(uint16_t val)
{
	uint8_t ch[6];
	uint8_t i;
	uint8_t highest_nonzero = 0;

	ch[0] = '0';

	for (i = 0; i < 6 && val != 0; i++) {
		uint16_t digit = val % 10;
		ch[i] = '0' + digit;
		val /= 10;
		if (digit)
			highest_nonzero = i;
	}

	for (i = highest_nonzero+1; i > 0; i--) {
		uart_write_ch(ch[i-1]);
	}
}

void uart_write_fixed_point(uint16_t val)
{
	uint16_t tmp;
	uint32_t big;
	
	// Print the integer part
	tmp = val >> 10;
	uart_write_int(tmp);
	uart_write_ch('.');

	// Remove the integer part
	tmp <<= 10;
	big = val - tmp;

	// Take three decimal digits from the fraction part
	big *= 1000;
	big >>= 10;
	val = big;

	// Pad with zeros if the number is too small
	if (val < 100)
		uart_write_ch('0');
	if (val < 10)
		uart_write_ch('0');

	// Write the remaining fractional part
	uart_write_int(val);
}

void uart_write_from_buf(void)
{
	if (uart_write_len > 0 && uart_write_ready()) {
		USART1_DR = uart_write_buf[uart_write_start];
		uart_write_start++;
		uart_write_len--;

		if (uart_write_len == 0)
			uart_write_start = 0;
	}
}

uint8_t uart_read_ch(void)
{
	return USART1_DR;
}

void uart_read_to_buf(void)
{
	// Don't read if we are writing
	if (uart_write_len > 0 || !uart_write_ready())
		return;

	if (uart_read_available()) {
		uint8_t ch = uart_read_ch();
		uart_read_buf[uart_read_len] = ch;
		uart_read_len++;

		if (ch == '\r' || ch == '\n')
			read_newline = 1;

		// Empty the read buf if we are overfilling and there is no full command in there
		if (uart_read_len == sizeof(uart_read_buf) && !read_newline) {
			uart_read_len = 0;
			uart_write_str("READ OVERFLOW\r\n");
		}
	}
}

void set_name(uint8_t *name)
{
	uint8_t idx;

	for (idx = 0; name[idx] != 0; idx++) {
		if (!isprint(name[idx]))
			name[idx] = '.'; // Eliminate non-printable chars
	}

	strncpy(cfg_name, name, sizeof(cfg_name));
	cfg_name[sizeof(cfg_name)-1] = 0;

	uart_write_str("SNAME: ");
	uart_write_str(cfg_name);
	uart_write_str("\r\n");
}

void set_output(uint8_t *s)
{
	if (s[1] != 0) {
//		uart_write_str("OUTPUT takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
		return;
	}

	if (s[0] == '0') {
		cfg_output = 0;
		uart_write_str("OUTPUT: OFF\r\n");
	} else if (s[0] == '1') {
		cfg_output = 1;
		uart_write_str("OUTPUT: ON\r\n");
	} else {
//		uart_write_str("OUTPUT takes either 0 for OFF or 1 for ON, received: \"");
		uart_write_str(s);
		uart_write_str("\"\r\n");
	}
}

uint16_t parse_fixed_point(uint8_t *s)
{
	uint8_t *t = s;
	uint16_t val = 0;
	uint16_t fraction_digits = 0;
	uint16_t whole_digits = 0;
	uint16_t fraction_factor = 1;

	for (; *s != 0; s++) {
		uart_write_ch('W');
		uart_write_ch(*s);
		uart_write_str("\r\n");
		if (*s == '.') {
			s++; // Skip the dot
			break;
		}
		if (*s >= '0' && *s <= '9') {
			val = *s - '0';
			whole_digits *= 10;
			whole_digits += val;
			if (whole_digits > 62)
				goto invalid_number;
		} else {
			goto invalid_number;
		}
	}

	whole_digits <<= 10;

	for (; *s != 0 && fraction_factor < 1000; s++) {
		uart_write_ch('F');
		uart_write_ch(*s);
		uart_write_str("\r\n");
		if (*s >= '0' && *s <= '9') {
			val = *s - '0';
			fraction_digits *= 10;
			fraction_digits += val;
			fraction_factor *= 10;
		} else {
			goto invalid_number;
		}
	}

	fraction_digits <<= 10;
	fraction_digits /= fraction_factor;

	return whole_digits + fraction_digits + 1;

invalid_number:
	uart_write_str("INVALID NUMBER '");
	uart_write_str(t);
	uart_write_ch('\'');
	uart_write_str("\r\n");
	return 0xFFFF;
}

void set_voltage(uint8_t *s)
{
	uint16_t val;

	val = parse_fixed_point(s);
	if (val == 0xFFFF)
		return;

	if (val > cap_vmax) {
		uart_write_str("VOLTAGE VALUE TOO HIGH\r\n");
		return;
	}
	if (val < cap_vmin) {
		uart_write_str("VOLTAGE VALUE TOO LOW\r\n");
		return;
	}

	uart_write_str("VOLTAGE: SET ");
	uart_write_fixed_point(val);
	uart_write_str("\r\n");
	cfg_vset = val;
}

void set_current(uint8_t *s)
{
	uint16_t val;

	val = parse_fixed_point(s);
	if (val == 0xFFFF)
		return;

	if (val > cap_cmax) {
		uart_write_str("CURRENT VALUE TOO HIGH\r\n");
		return;
	}
	if (val < cap_cmin) {
		uart_write_str("CURRENT VALUE TOO LOW\r\n");
		return;
	}

	uart_write_str("CURRENT: SET ");
	uart_write_fixed_point(val);
	uart_write_str("\r\n");
	cfg_cset = val;
}

void process_input()
{
	// Eliminate the CR/LF character
	uart_read_buf[uart_read_len-1] = 0;

	if (strcmp(uart_read_buf, "MODEL") == 0) {
		uart_write_str("MODEL: B3606\r\n");
	} else if (strcmp(uart_read_buf, "VERSION") == 0) {
		uart_write_str("VERSION: " FW_VERSION "\r\n");
	} else if (strcmp(uart_read_buf, "NAME") == 0) {
		uart_write_str("NAME: ");
		uart_write_str(cfg_name);
		uart_write_str("\r\n");
	} else if (strcmp(uart_read_buf, "VLIST") == 0) {
		uart_write_str("VLIST:\r\nVOLTAGE MIN: ");
		uart_write_fixed_point(cap_vmin);
		uart_write_str("\r\nVOLTAGE MAX: ");
		uart_write_fixed_point(cap_vmax);
		uart_write_str("\r\nVOLTAGE STEP: ");
		uart_write_fixed_point(cap_vstep);
		uart_write_str("\r\n");
	} else if (strcmp(uart_read_buf, "CLIST") == 0) {
		uart_write_str("CLIST:\r\nCURRENT MIN: ");
		uart_write_fixed_point(cap_cmin);
		uart_write_str("\r\nCURRENT MAX: ");
		uart_write_fixed_point(cap_cmax);
		uart_write_str("\r\nCURRENT STEP: ");
		uart_write_fixed_point(cap_cstep);
		uart_write_str("\r\n");
	} else if (strcmp(uart_read_buf, "CONFIG") == 0) {
		uart_write_str("CONFIG:\r\nOUTPUT: ");
		uart_write_str(cfg_output ? "ON" : "OFF");
		uart_write_str("\r\nVOLTAGE SET: ");
		uart_write_fixed_point(cfg_vset);
		uart_write_str("\r\nCURRENT SET: ");
		uart_write_fixed_point(cfg_cset);
		uart_write_str("\r\nVOLTAGE SHUTDOWN: ");
		if (cfg_vshutdown == 0)
			uart_write_str("DISABLED");
		else
			uart_write_fixed_point(cfg_vshutdown);
		uart_write_str("\r\nCURRENT SHUTDOWN: ");
		if (cfg_cshutdown == 0)
			uart_write_str("DISABLED");
		else
			uart_write_fixed_point(cfg_cshutdown);
		uart_write_str("\r\n");
	} else if (strcmp(uart_read_buf, "STATUS") == 0) {
		uart_write_str("STATUS:\r\nOUTPUT: ");
		uart_write_str(cfg_output ? "ON" : "OFF");
		uart_write_str("\r\nVOLTAGE IN: ");
		uart_write_fixed_point(state_vin);
		uart_write_str("\r\nVOLTAGE OUT: ");
		uart_write_fixed_point(cfg_output ? state_vout : 0);
		uart_write_str("\r\nCURRENT OUT: ");
		uart_write_fixed_point(cfg_output ? state_cout : 0);
		uart_write_str("\r\nCONSTANT: ");
		uart_write_str(state_constant_current ? "CURRENT" : "VOLTAGE");
		uart_write_str("\r\n");
	} else if (strcmp(uart_read_buf, "RSTATUS") == 0) {
		uart_write_str("RSTATUS:\r\nOUTPUT: ");
		uart_write_str(cfg_output ? "ON" : "OFF");
		uart_write_str("\r\nVOLTAGE IN ADC: ");
		uart_write_int(state_vin_raw);
		uart_write_str("\r\nVOLTAGE OUT ADC: ");
		uart_write_int(state_vout_raw);
		uart_write_str("\r\nCURRENT OUT ADC: ");
		uart_write_int(state_cout_raw);
		uart_write_str("\r\nCONSTANT: ");
		uart_write_str(state_constant_current ? "CURRENT" : "VOLTAGE");
		uart_write_str("\r\n");
	} else {
		// Process commands with arguments
		uint8_t idx;
		uint8_t space_found = 0;

		for (idx = 0; idx < uart_read_len; idx++) {
			if (uart_read_buf[idx] == ' ') {
				uart_read_buf[idx] = 0;
				space_found = 1;
				break;
			}
		}

		if (space_found) {
			if (strcmp(uart_read_buf, "SNAME") == 0) {
				set_name(uart_read_buf + idx + 1);
			} else if (strcmp(uart_read_buf, "OUTPUT") == 0) {
				set_output(uart_read_buf + idx + 1);
			} else if (strcmp(uart_read_buf, "VOLTAGE") == 0) {
				set_voltage(uart_read_buf + idx + 1);
			} else if (strcmp(uart_read_buf, "CURRENT") == 0) {
				set_current(uart_read_buf + idx + 1);
			} else {
	//			uart_write_str("UNKNOWN COMMAND!\r\n");
			}
		} else {
	//		uart_write_str("UNKNOWN COMMAND\r\n");
		}
	}

	uart_read_len = 0;
	read_newline = 0;
}

void clk_init()
{
	CLK_CKDIVR = 0x00; // Set the frequency to 16 MHz
}

void uart_init()
{
	USART1_CR1 = 0; // 8 bits, no parity
	USART1_CR2 = 0;
	USART1_CR3 = 0;

	USART1_BRR2 = 0x1;
	USART1_BRR1 = 0x1A; // 38400 baud, order important between BRRs, BRR1 must be last

	USART1_CR2 = USART_CR2_TEN | USART_CR2_REN; // Allow TX & RX

	uart_write_len = 0;
	uart_write_start = 0;
	uart_read_len = 0;
	read_newline = 0;
}

void pinout_init()
{
	// PA1 is 74HC595 SHCP, output
	// PA2 is 74HC595 STCP, output
	PA_ODR = 0;
	PA_DDR = (1<<1) | (1<<2);
	PA_CR1 = (1<<1) | (1<<2);
	PA_CR2 = (1<<1) | (1<<2);

	// PB4 is Enable control, output
	// PB5 is CV/CC sense, input
	PB_ODR = (1<<4); // For safety we start with off-state
	PB_DDR = (1<<4);
	PB_CR1 = 0;
	PB_CR2 = 0;

	// PC3 is unknown, input
	// PC4 is Iout sense, input adc, AIN2
	// PC5 is Vout control, output
	// PC6 is Iout control, output
	// PC7 is Button 1, input
	PC_ODR = 0;
	PC_DDR = (1<<5) || (1<<6);
	PC_CR1 = (1<<7); // For the button
	PC_CR2 = 0;

	// PD1 is Button 2, input
	// PD2 is Vout sense, input adc, AIN3
	// PD3 is Vin sense, input adc, AIN4
	// PD4 is 74HC595 DS, output
	PD_DDR = (1<<4);
	PD_CR1 = (1<<1) | (1<<4); // For the button
	PD_CR2 = (1<<4);
}

void pwm_init(void)
{
	/* Timer 1 Channel 1 for Iout control */
	TIM1_CR1 = 0x10; // Down direction
	TIM1_ARRH = PWM_HIGH; // Reload counter = 16384
	TIM1_ARRL = PWM_LOW;
	TIM1_PSCRH = 0; // Prescaler 0 means division by 1
	TIM1_PSCRL = 0;
	TIM1_RCR = 0; // Continuous

	TIM1_CCMR1 = 0x70;    //  Set up to use PWM mode 2.
	TIM1_CCER1 = 0x03;    //  Output is enabled for channel 1, active low
	TIM1_CCR1H = 0x00;      //  Start with the PWM signal off
	TIM1_CCR1L = 0x00;

	TIM1_BKR = 0x80;       //  Enable the main output.

	/* Timer 2 Channel 1 for Vout control */
	TIM2_ARRH = PWM_HIGH; // Reload counter = 16384
	TIM2_ARRL = PWM_LOW;
	TIM2_PSCR = 0; // Prescaler 0 means division by 1
	TIM2_CR1 = 0x10; // Down direction

	TIM2_CCMR1 = 0x70;    //  Set up to use PWM mode 2.
	TIM2_CCER1 = 0x03;    //  Output is enabled for channel 1, active low
	TIM2_CCR1H = 0x00;      //  Start with the PWM signal off
	TIM2_CCR1L = 0x00;

	// Timers are still off, will be turned on when output is turned on
}

void adc_init(void)
{
	ADC1_CR1 = 0x70; // Power down, clock/18
	ADC1_CR2 = 0x08; // Right alignment
	ADC1_CR3 = 0x00;
	ADC1_CSR = 0x00;

	ADC1_TDRL = 0x0F;

	ADC1_CR1 |= 0x01; // Turn on the ADC
}

void adc_start(uint8_t channel)
{
	uint8_t csr = ADC1_CSR;
	csr &= 0x70; // Turn off EOC, Clear Channel
	csr |= channel; // Select channel
	ADC1_CSR = csr;

	ADC1_CR1 |= 1; // Trigger conversion
}

uint8_t adc_ready()
{
	return ADC1_CSR & 0x80;
}

void config_load(void)
{
	strcpy(cfg_name, "Unnamed");
	cfg_default = 0;
	cfg_output = 0;
	cfg_vset = 5<<10; // 5V
	cfg_cset = (1<<10) / 2; // 0.5A
	cfg_vshutdown = 0;
	cfg_cshutdown = 0;

	cal_vin = 16 << 10;

	cal_vout_a = FLOAT_TO_FIXED(0.068*15.0/16.0, 10);
	cal_vout_b = FLOAT_TO_FIXED(0.031 / 0.068, 10);

	cal_cout_a = FLOAT_TO_FIXED(1.25, 10);
	cal_cout_b = FLOAT_TO_FIXED(0.031/0.068, 10);

	state_pc3 = 1;
}

void read_state(void)
{
	uint8_t tmp;

	state_constant_current = (PB_IDR & (1<<5)) ? 1 : 0;
	tmp = (PC_IDR & (1<<3)) ? 1 : 0;
	if (state_pc3 != tmp) {
		uart_write_str("PC3 is now ");
		uart_write_ch('0' + tmp);
		uart_write_str("\r\n");
		state_pc3 = tmp;
	}

	if ((ADC1_CSR & 0x0F) == 0) {
		adc_start(2);
	} else if (adc_ready()) {
		uint16_t val = ADC1_DRL;
		uint16_t valh = ADC1_DRH;
		uint8_t ch = ADC1_CSR & 0x0F;
		uint32_t tmp;

		val |= valh << 8;

		switch (ch) {
			case 2:
				state_cout_raw = val;
				// Calculation: val * cal_cout_a * 3.3 / 1024 - cal_cout_b
				tmp = val * cal_cout_a * ref_volt_10;
				tmp >>= 10;
				tmp -= cal_vout_b;
				state_cout = tmp;
				ch = 3;
				break;
			case 3:
				state_vout_raw = val;
				// Calculation: val * cal_vout_a * 3.3 / 1024 - cal_vout_b
				tmp = val * cal_vout_a * ref_volt_10;
				tmp >>= 10;
				tmp -= cal_vout_b;
				state_vout = tmp;
				ch = 4;
				break;
			case 4:
				state_vin_raw = val;
				// Calculation: val * cal_vin * 3.3 / 1024
				tmp = val * cal_vin * ref_volt_10;
				tmp >>= 10;
				state_vin = tmp;
				ch = 2;
				{
					uint8_t ch3;
					uint8_t ch2;
					uint8_t ch1;
					uint8_t ch4;

					ch4 = '0' + (val % 10);
					val /= 10;
					ch3 = '0' + (val % 10);
					val /= 10;
					ch2 = '0' + (val % 10);
					val /= 10;
					ch1 = '0' + (val % 10);

					display_show(ch1, 1, ch2, 0, ch3, 0, ch4, 0);
				}
				break;
		}

		adc_start(ch);
	}
}

uint8_t output_state(void)
{
	return (PB_ODR & (1<<4)) ? 0 : 1;
}

void control_voltage(void)
{
	uint32_t tmp;
	uint16_t ctr;

	tmp = cfg_vset;
	tmp *= PWM_VAL;
	tmp /= cap_vmaxpwm;
	tmp >>= 10; // vset is fixed point, remove the decimal point or we overflow

	ctr = tmp;

	uart_write_str("VPWM ");
	uart_write_int(ctr);
	uart_write_ch(' ');

	TIM1_CCR1H = ctr >> 8;
	TIM1_CCR1L = ctr & 0xFF;
	TIM1_CR1 |= 0x01; // Enable timer

	uart_write_int(TIM1_CCR1H);
	uart_write_ch(' ');
	uart_write_int(TIM1_CCR1L);
	uart_write_str("\r\n");

	out_voltage = cfg_vset;
}

void control_current(void)
{
	uint32_t tmp;
	uint16_t ctr;

	tmp = cfg_cset;
	tmp *= PWM_VAL;
	tmp /= cap_cmaxpwm;
	tmp >>= 10; // cset is fixed point, remove the decimal point or we overflow

	ctr = tmp;

	uart_write_str("CPWM ");
	uart_write_int(ctr);
	uart_write_ch(' ');

	TIM2_CCR1H = ctr >> 8;
	TIM2_CCR1L = ctr & 0xFF;
	TIM2_CR1 |= 0x01; // Enable timer

	uart_write_int(TIM2_CCR1H);
	uart_write_ch(' ');
	uart_write_int(TIM2_CCR1L);
	uart_write_str("\r\n");

	out_current = cfg_cset;
}

void control_outputs(void)
{
	if (cfg_output) {
		// Only tune the PWMs if we are outputing anything
		if (out_voltage != cfg_vset) {
			control_voltage();
		}

		if (out_current != cfg_cset) {
			control_current();
		}
	}

	if (cfg_output != output_state()) {
		// Startup and shutdown orders need to be in reverse order
		if (cfg_output) {
			// We turned on the PWMs above already
			PB_ODR &= ~(1<<4);
		} else {
			PB_ODR |= (1<<4);

			TIM1_CCR1H = 0;
			TIM1_CCR1L = 0;
			TIM1_CR1 &= 0xFE; // Disable timer

			TIM2_CCR1H = 0;
			TIM2_CCR1L = 0;
			TIM2_CR1 &= 0xFE; // Disable timer

			out_voltage = 0;
			out_current = 0;
		}
	}
}

int main()
{
	unsigned long i = 0;

	pinout_init();
	clk_init();
	uart_init();
	pwm_init();
	adc_init();
	display_init();

	config_load();

	if (cfg_default)
		cfg_output = 1;

	control_outputs();

	uart_write_str("\r\nB3606 starting: Version " FW_VERSION "\r\n");

	do {
		uart_write_from_buf();

		read_state();
		control_outputs();
		display_refresh();

		uart_read_to_buf();
		if (read_newline) {
			process_input();
		}
	} while(1);
}
