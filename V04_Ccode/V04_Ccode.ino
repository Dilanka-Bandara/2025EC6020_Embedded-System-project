#define F_CPU 16000000UL // 16 MHz clock speed
#include <avr/io.h>
#include <util/delay.h>
#include <stdio.h>
#include <stdlib.h>

// --- PIN DEFINITIONS ---
#define TRIG_PIN PB1  // Port B, Pin 1 (Arduino D9)
#define ECHO_PIN PB2  // Port B, Pin 2 (Arduino D10)
#define TILT_PIN PD2  // Port D, Pin 2 (Arduino D2)
#define DOUT_PIN PD3  // Port D, Pin 3 (Arduino D3)
#define SCK_PIN  PD4  // Port D, Pin 4 (Arduino D4)

#define OLED_ADDR 0x3C // 0x78 shifted right by 1 for AVR TWI

// --- VARIABLES ---
float calibration_factor = 420.0;
long offset = 0;

// --- FUNCTION PROTOTYPES ---
void uart_init(unsigned int baud);
int uart_putchar(char c, FILE *stream);
void gpio_init(void);
long read_ultrasonic(void);
long read_hx711(void);
void i2c_init(void);
void i2c_start(void);
void i2c_stop(void);
void i2c_write(uint8_t data);
void ssd1306_command(uint8_t cmd);
void ssd1306_data(uint8_t data);
void ssd1306_init(void);
void ssd1306_clear(void);
void ssd1306_set_cursor(uint8_t col, uint8_t page);
void ssd1306_print(const char *str);

// Setup standard output for printf
FILE uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);

int main(void) {
    // 1. Initialize Hardware Interfaces
    uart_init(9600);
    stdout = &uart_output; // Link printf to UART
    gpio_init();
    i2c_init();
    
    // 2. Initialize OLED
    ssd1306_init();
    ssd1306_clear();
    ssd1306_set_cursor(0, 0);
    ssd1306_print("System Loading...");
    
    // 3. Tare the Scale
    _delay_ms(2000);
    offset = read_hx711(); // Assuming empty scale at startup

    while (1) {
        // --- A. Read Ultrasonic ---
        long duration = read_ultrasonic();
        int distance = (int)(duration * 0.034 / 2);

        // --- B. Read Tilt Sensor ---
        int tiltState = (PIND & (1 << TILT_PIN)) ? 1 : 0;

        // --- C. Read Weight (HX711) ---
        long raw_weight = read_hx711() - offset;
        float final_weight = (float)raw_weight / calibration_factor;

        // --- D. Update UART (Serial Monitor) ---
        printf("D:%d | T:%d | W:%.1f\n", distance, tiltState, final_weight);

        // --- E. Update OLED Display ---
        char buffer[20];
        ssd1306_clear();
        
        ssd1306_set_cursor(0, 0);
        sprintf(buffer, "Dist: %d cm", distance);
        ssd1306_print(buffer);

        ssd1306_set_cursor(0, 2); // Page 2 (approx line 16)
        if(tiltState) {
            ssd1306_print("Tilt: TILTED!");
        } else {
            ssd1306_print("Tilt: Stable");
        }

        ssd1306_set_cursor(0, 5); // Page 5 (approx line 40)
        ssd1306_print("Weight/Load:");
        
        ssd1306_set_cursor(0, 7); // Page 7
        sprintf(buffer, "%.1f g", final_weight);
        ssd1306_print(buffer);

        _delay_ms(250);
    }
}

// ==========================================
// HARDWARE DRIVERS
// ==========================================

// --- UART (Serial) Driver ---
void uart_init(unsigned int baud) {
    unsigned int ubrr = F_CPU / 16 / baud - 1;
    UBRR0H = (unsigned char)(ubrr >> 8);
    UBRR0L = (unsigned char)ubrr;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0); // Enable receiver and transmitter
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); // 8-bit data format
}

int uart_putchar(char c, FILE *stream) {
    if (c == '\n') uart_putchar('\r', stream);
    while (!(UCSR0A & (1 << UDRE0))); // Wait for empty transmit buffer
    UDR0 = c;
    return 0;
}

// --- GPIO & Sensor Drivers ---
void gpio_init(void) {
    DDRB |= (1 << TRIG_PIN);  // Trig Output
    DDRB &= ~(1 << ECHO_PIN); // Echo Input
    DDRD &= ~(1 << TILT_PIN); // Tilt Input
    DDRD |= (1 << SCK_PIN);   // HX711 Clock Output
    DDRD &= ~(1 << DOUT_PIN); // HX711 Data Input
}

long read_ultrasonic(void) {
    PORTB &= ~(1 << TRIG_PIN);
    _delay_us(2);
    PORTB |= (1 << TRIG_PIN);
    _delay_us(10);
    PORTB &= ~(1 << TRIG_PIN);

    long count = 0;
    while (!(PINB & (1 << ECHO_PIN))); // Wait for pin to go HIGH
    while (PINB & (1 << ECHO_PIN)) {   // Time the HIGH pulse
        count++;
        _delay_us(1); // Rough timing approximation
    }
    return count;
}

long read_hx711(void) {
    long count = 0;
    while (PIND & (1 << DOUT_PIN)); // Wait for DOUT to go LOW

    for (int i = 0; i < 24; i++) {
        PORTD |= (1 << SCK_PIN);
        count = count << 1;
        PORTD &= ~(1 << SCK_PIN);
        if (PIND & (1 << DOUT_PIN)) count++;
    }
    PORTD |= (1 << SCK_PIN); // 25th pulse for 128 gain
    _delay_us(1);
    PORTD &= ~(1 << SCK_PIN);

    if (count & 0x800000) count |= 0xFF000000; // Sign extend if negative
    return count;
}

// --- I2C (TWI) Driver ---
void i2c_init(void) {
    TWSR = 0x00; // Prescaler = 1
    TWBR = 0x48; // SCL clock frequency ~ 100kHz
    TWCR = (1 << TWEN); // Enable TWI
}

void i2c_start(void) {
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
}

void i2c_stop(void) {
    TWCR = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN);
}

void i2c_write(uint8_t data) {
    TWDR = data;
    TWCR = (1 << TWINT) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT)));
}

// --- Minimal SSD1306 OLED Driver ---
void ssd1306_command(uint8_t cmd) {
    i2c_start();
    i2c_write(OLED_ADDR << 1); // Write mode
    i2c_write(0x00); // Command stream
    i2c_write(cmd);
    i2c_stop();
}

void ssd1306_data(uint8_t data) {
    i2c_start();
    i2c_write(OLED_ADDR << 1);
    i2c_write(0x40); // Data stream
    i2c_write(data);
    i2c_stop();
}

void ssd1306_init(void) {
    // Standard initialization sequence for SSD1306 128x64
    ssd1306_command(0xAE); // Display OFF
    ssd1306_command(0x20); // Set Memory Addressing Mode
    ssd1306_command(0x00); // Horizontal Addressing Mode
    ssd1306_command(0x8D); // Charge Pump Setting
    ssd1306_command(0x14); // Enable Charge Pump
    ssd1306_command(0xAF); // Display ON
}

void ssd1306_clear(void) {
    for (uint8_t page = 0; page < 8; page++) {
        ssd1306_set_cursor(0, page);
        for (uint8_t col = 0; col < 128; col++) {
            ssd1306_data(0x00); // Write blank pixels
        }
    }
}

void ssd1306_set_cursor(uint8_t col, uint8_t page) {
    ssd1306_command(0xB0 + page); // Set page address (0-7)
    ssd1306_command(col & 0x0F);  // Set lower column address
    ssd1306_command(0x10 | (col >> 4)); // Set higher column address
}

// Note: Text rendering requires a 5x8 font array header file.
// For brevity, this print function simulates the structure.
extern const uint8_t font5x8[][5]; // You must link a font header

void ssd1306_print(const char *str) {
    while (*str) {
        for (uint8_t i = 0; i < 5; i++) {
            // Read font byte and send to screen
            // ssd1306_data(font5x8[*str - 32][i]); 
            ssd1306_data(0xFF); // Placeholder: Draws solid blocks without font array
        }
        ssd1306_data(0x00); // Spacing between characters
        str++;
    }
}