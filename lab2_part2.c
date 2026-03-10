        /*
    * Lab 2, Part 2 - UART Command Interface for RGB LED and SSD Display
    *
    * ECE-315 WINTER 2025 - COMPUTER INTERFACING
    * Created on: February 5, 2021
    * Modified on: July 26, 2023
    * Modified on: January 20, 2025
    * Author(s):  Shyama Gandhi, Antonio Andara Lara
    *
    * Summary:
    * This code extends Lab 1 Part 3 with UART command parsing capabilities.
    * It allows remote control of the RGB LED brightness/color and SSD display via UART commands.
    *
    * Features:
    * 1) Seven-segment display (SSD) controlled by keypad input or UART commands
    * 2) RGB LED with brightness and color control via buttons or UART commands
    * 3) UART command interface supporting:
    *    - Brightness control: "B <0-25>" or "BRIGHTNESS <0-25>"
    *    - Color control: "C <R|G|B|C|M|Y|W>" or "COLOR <R|G|B|C|M|Y|W>"
    *    - Display control: "D <xy>" or "SSD <xy>" where x,y are 0-9 or A-F
    * 4) Multi-task FreeRTOS architecture with queue-based communication
    */


    // Include FreeRTOS Libraries
    #include <FreeRTOS.h>
    #include <task.h>
    #include <queue.h>

    // Include xilinx Libraries
    #include <xparameters.h>
    #include <xgpio.h>
    #include <xscugic.h>
    #include <xil_exception.h>
    #include <xil_printf.h>
    #include <sleep.h>
    #include <xil_cache.h>

    // Other miscellaneous libraries
    #include "pmodkypd.h"
    #include "rgb_led.h"
    #include "xuartps.h"

    #ifndef RGB_RED
    #define RGB_RED      1
    #endif
    #ifndef RGB_GREEN
    #define RGB_GREEN    2
    #endif
    #ifndef RGB_BLUE
    #define RGB_BLUE     4
    #endif
    #ifndef RGB_CYAN
    #define RGB_CYAN     (RGB_GREEN | RGB_BLUE)
    #endif
    #ifndef RGB_MAGENTA
    #define RGB_MAGENTA  (RGB_RED | RGB_BLUE)
    #endif
    #ifndef RGB_YELLOW
    #define RGB_YELLOW   (RGB_RED | RGB_GREEN)
    #endif
    #ifndef RGB_WHITE
    #define RGB_WHITE    (RGB_RED | RGB_GREEN | RGB_BLUE)
    #endif
    #include <string.h>
    #include <stdio.h>
    #include <stdlib.h>

    // UART configuration
    #define UART_BASEADDR         XPAR_UART1_BASEADDR
    #define UART_RX_LINE_MAX      64

    // Device ID declarations
    #define KYPD_DEVICE_ID   	XPAR_GPIO_KYPD_BASEADDR
    /*************************** Enter your code here ****************************/
    // TODO: Define the seven-segment display (SSD) base address.

    #define SSD_DEVICE_ID      XPAR_GPIO_SSD_BASEADDR // from part 1

    #define RGB_LED_DEVICE_ID   XPAR_GPIO_LEDS_BASEADDR

    #define PUSH_BUTTON_DEVICE_ID       XPAR_GPIO_INPUTS_BASEADDR

    /*****************************************************************************/

    // keypad key table
    #define DEFAULT_KEYTABLE 	"0FED789C456B123A"

    // Declaring the devices
    PmodKYPD 	KYPDInst;

    /*************************** Enter your code here ****************************/
    // TODO: Declare the seven-segment display peripheral here.

    XGpio    SSDInst;

    XGpio    RGB_LEDInst; 

    XGpio    PUSH_BUTTONInst;

    QueueHandle_t xKeypadDisplayQueue;
    QueueHandle_t xButtonsRGBQueue; 

    typedef struct {
        u8 current_key;
        u8 previous_key;
    } KeypadState_t;

    // UART command types
    typedef enum {
        UART_CMD_LED_BRIGHTNESS,
        UART_CMD_LED_COLOR,
        UART_CMD_SSD_DISPLAY,
        UART_CMD_INVALID
    } UART_CommandType_t;

    // UART command structure with union for different parameter types
    typedef struct {
        UART_CommandType_t type;
        union {
            uint8_t brightness;   // Brightness level 0-25
            uint8_t color;       // RGB color value
            struct { u8 left; u8 right; } ssd;
        } param;
    } UART_Command_t;

    QueueHandle_t xRGBCommandQueue;
    QueueHandle_t xSSDCommandQueue;

    static XUartPs UartPs;

    /*****************************************************************************/

    // Function prototypes
    void InitializeKeypad();
    void InitializePeripherals();
    static void vKeypadTask( void *pvParameters );
    static void vRGBTask( void *pvParameters );
    static void vButtonsTask( void *pvParameters );
    static void vDisplayTask( void *pvParameters );
    u32 SSD_decode(u8 key_value, u8 cathode);
    static void vUARTTask( void *pvParameters );
    static void uart_init(void);
    static int uart_poll_rx(uint8_t *received_byte);
    static void parse_and_dispatch(const char *command_line);
    static uint8_t parse_color_char(char color_char);

    /*****************************************************************************/

    void InitializePeripherals(void)
    {
        // 1. Initialize SSD
        XGpio_Initialize(&SSDInst, SSD_DEVICE_ID);
        XGpio_SetDataDirection(&SSDInst, 1, 0x00);

        // 2. Initialize RGB LED
        XGpio_Initialize(&RGB_LEDInst, RGB_LED_DEVICE_ID);
        XGpio_SetDataDirection(&RGB_LEDInst, 2, 0x00);

        // 3. Initialize Push Button
        XGpio_Initialize(&PUSH_BUTTONInst, PUSH_BUTTON_DEVICE_ID);
        XGpio_SetDataDirection(&PUSH_BUTTONInst, 1, 0x0F);  // channel 1, configure as inputs
    }

    int main(void)
    {
        // Initialize keypad
        InitializeKeypad();

        // Initialize peripherals
        InitializePeripherals();

        xil_printf("Initialization Complete, System Ready!\n");

        // Create queues
        xKeypadDisplayQueue = xQueueCreate(1, sizeof(KeypadState_t));
        if (xKeypadDisplayQueue == NULL) {
            xil_printf("ERROR: Failed to create keypad display queue \r\n");
            return 1;
        }

        xButtonsRGBQueue = xQueueCreate(1, sizeof(u32));
        if (xButtonsRGBQueue == NULL) {
            xil_printf("ERROR: Failed to create buttons RGB queue \r\n");
            return 1;
        }

        // Create UART command queues
        xRGBCommandQueue = xQueueCreate(4, sizeof(UART_Command_t));
        if (xRGBCommandQueue == NULL) {
            xil_printf("ERROR: Failed to create RGB command queue \r\n");
            return 1;
        }

        xSSDCommandQueue = xQueueCreate(1, sizeof(KeypadState_t));
        if (xSSDCommandQueue == NULL) {
            xil_printf("ERROR: Failed to create SSD command queue \r\n");
            return 1;
        }

        uart_init();

        // Create Tasks
        xTaskCreate(vKeypadTask, "Keypad", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
        xTaskCreate(vButtonsTask, "Buttons", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
        xTaskCreate(vRGBTask, "RGB", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
        xTaskCreate(vDisplayTask, "Display", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
        xTaskCreate(vUARTTask, "UART", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY, NULL);

        vTaskStartScheduler();
        while(1);
        return 0;
    }


   static void vButtonsTask(void *pvParameters)
{
    u32 button_val;
    static u32 prev_button_val = 0;
    
    while (1) {
        button_val = XGpio_DiscreteRead(&PUSH_BUTTONInst, 1);
        
        if (button_val != prev_button_val) {
            xQueueOverwrite(xButtonsRGBQueue, &button_val);
            if (button_val != 0) {
                xil_printf("Button: 0x%02X\r\n", button_val);
            }
            prev_button_val = button_val;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
    /*****************************************************************************/

    static void vRGBTask(void *pvParameters)
    {
        uint8_t led_color = RGB_CYAN;
        const TickType_t pwm_period = 25;
        TickType_t on_delay = 0;
        TickType_t off_delay = pwm_period - on_delay;
        u32 button_value;
        static u32 previous_button_value = 0;
        UART_Command_t uart_command;

        while (1){
            // Check for button commands
            if (xQueueReceive(xButtonsRGBQueue, &button_value, 0) == pdTRUE){
                if (button_value != previous_button_value) {
                    if (button_value == 0x08 && on_delay < pwm_period) {
                        on_delay++;
                        xil_printf("xOnDelay: %d, xOffDelay: %d\n", on_delay, pwm_period - on_delay);
                    } else if (button_value == 0x01 && on_delay > 0) {
                        on_delay--;
                        xil_printf("xOnDelay: %d, xOffDelay: %d\n", on_delay, pwm_period - on_delay);
                    }
                    previous_button_value = button_value;
                }
            }

            // Check for UART commands
            if (xQueueReceive(xRGBCommandQueue, &uart_command, 0) == pdTRUE) {
                if (uart_command.type == UART_CMD_LED_BRIGHTNESS) {
                    on_delay = uart_command.param.brightness;
                    if (on_delay > pwm_period) on_delay = pwm_period;
                } else if (uart_command.type == UART_CMD_LED_COLOR) {
                    led_color = uart_command.param.color;
                }
            }

            off_delay = pwm_period - on_delay;
            // Turn LED on
            XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, led_color);
            if (on_delay == 0) {
                XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, 0);
            }
            
            vTaskDelay(on_delay);

            // Turn LED off
            XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, 0);
            vTaskDelay(off_delay);
        }
    }


    static void vDisplayTask( void *pvParameters ) {
        KeypadState_t data = {0, 0};
        KeypadState_t new_data;
        const TickType_t xDelay = pdMS_TO_TICKS(10);
        
        while (1) {
            // Check keypad queue first (keypad input takes priority)
            while (xQueueReceive(xKeypadDisplayQueue, &new_data, 0) == pdTRUE) {
                data = new_data;
            }
            // Check UART command queue (only updates if keypad queue is empty)
            if (xQueueReceive(xSSDCommandQueue, &new_data, 0) == pdTRUE) {
                data = new_data;
            }
            
            XGpio_DiscreteWrite(&SSDInst, 1, SSD_decode(data.current_key, 1));
            vTaskDelay(xDelay);
            
            XGpio_DiscreteWrite(&SSDInst, 1, SSD_decode(data.previous_key, 0));
            vTaskDelay(xDelay);
        }
    }

    

    static void vKeypadTask( void *pvParameters ) {
        u16 keystate;
        XStatus status, previous_status = KYPD_NO_KEY;
        u8 new_key, current_key = 'x', previous_key = 'x';
        KeypadState_t keypad_data;
        const TickType_t xDelay = pdMS_TO_TICKS(50);

        xil_printf("Pmod KYPD app started. Press any key on the Keypad.\r\n");

        while (1) {
            // Capture state of the keypad
            keystate = KYPD_getKeyStates(&KYPDInst);

            // Determine which single key is pressed, if any
            // if a key is pressed, store the value of the new key in new_key
            status = KYPD_getKeyPressed(&KYPDInst, keystate, &new_key);
            // Print key detect if a new key is pressed or if status has changed
            if (status == KYPD_SINGLE_KEY && previous_status == KYPD_NO_KEY) {
                xil_printf("Key Pressed: %c\r\n", (char) new_key);

                previous_key = current_key;
                current_key = new_key;

                keypad_data.current_key = current_key;
                keypad_data.previous_key = previous_key;
                xQueueOverwrite(xKeypadDisplayQueue, &keypad_data);

            } else if (status == KYPD_MULTI_KEY && status != previous_status) {
                xil_printf("Error: Multiple keys pressed\r\n");
            }

            // display the value of `status` each time it changes
            if (status != previous_status) {
                xil_printf("Status: %d\r\n", status);
            }
            
            previous_status = status;
            vTaskDelay(xDelay);
        }
    }



    void InitializeKeypad()
    {
        KYPD_begin(&KYPDInst, KYPD_DEVICE_ID);
        KYPD_loadKeyTable(&KYPDInst, (u8*) DEFAULT_KEYTABLE);
    }


    // This function is hard coded to translate key value codes to their binary representation
    u32 SSD_decode(u8 key_value, u8 cathode)
    {
        u32 result;

        // key_value represents the code of the pressed key
        switch(key_value){ // Handles the coding of the 7-seg display
            case 48: result = 0b00111111; break; // 0
            case 49: result = 0b00110000; break; // 1
            case 50: result = 0b01011011; break; // 2
            case 51: result = 0b01111001; break; // 3
            case 52: result = 0b01110100; break; // 4
            case 53: result = 0b01101101; break; // 5
            case 54: result = 0b01101111; break; // 6
            case 55: result = 0b00111000; break; // 7
            case 56: result = 0b01111111; break; // 8
            case 57: result = 0b01111100; break; // 9
            case 65: result = 0b01111110; break; // A
            case 66: result = 0b01100111; break; // B
            case 67: result = 0b00001111; break; // C
            case 68: result = 0b01110011; break; // D
            case 69: result = 0b01001111; break; // E
            case 70: result = 0b01001110; break; // F
            default: result = 0b00000000; break; // default case - all seven segments are OFF
        }

        // cathode handles which display is active (left or right)
        // by setting the MSB to 1 or 0
        if(cathode==0){
                return result;
        } else {
                return result | 0b10000000;
        }
    }

    /*****************************************************************************/
    // Initialize UART peripheral
    static void uart_init(void)
    {
        XUartPs_Config *cfg = XUartPs_LookupConfig(UART_BASEADDR);
        if (!cfg) {
            xil_printf("UART: LookupConfig failed\r\n");
            return;
        }
        if (XUartPs_CfgInitialize(&UartPs, cfg, cfg->BaseAddress) != XST_SUCCESS) {
            xil_printf("UART: CfgInitialize failed\r\n");
            return;
        }
        XUartPs_SetBaudRate(&UartPs, 115200);
    }

    // Poll UART for received byte
    static int uart_poll_rx(uint8_t *received_byte)
    {
        if (XUartPs_IsReceiveData(UartPs.Config.BaseAddress)) {
            *received_byte = (uint8_t)XUartPs_ReadReg(UartPs.Config.BaseAddress, XUARTPS_FIFO_OFFSET);
            return 1;
        }
        return 0;
    }

    // Convert color character to RGB value
    static uint8_t parse_color_char(char color_char)
    {
        if (color_char >= 'a' && color_char <= 'z') {
            color_char = (char)(color_char - 'a' + 'A');
        }
        
        switch (color_char) {
            case 'R': return RGB_RED;
            case 'G': return RGB_GREEN;
            case 'B': return RGB_BLUE;
            case 'C': return RGB_CYAN;
            case 'M': return RGB_MAGENTA;
            case 'Y': return RGB_YELLOW;
            case 'W': return RGB_WHITE;
            default:  return 0;
        }
    }

    // Parse UART command line and send to appropriate queue
    static void parse_and_dispatch(const char *command_line)
    {
        UART_Command_t command;
        KeypadState_t display_data;
        unsigned int brightness_value;
        char left_char, right_char;
        const char *param_ptr;
        uint8_t color_value;

        if (!command_line || strlen(command_line) == 0) return;

        // Parse brightness command: "B <0-25>" or "BRIGHTNESS <0-25>"
        if (strncmp(command_line, "B ", 2) == 0 || strncmp(command_line, "BRIGHTNESS ", 11) == 0) {
            param_ptr = (command_line[0] == 'B') ? command_line + 2 : command_line + 11;
            if (sscanf(param_ptr, "%u", &brightness_value) == 1 && brightness_value <= 25) {
                command.type = UART_CMD_LED_BRIGHTNESS;
                command.param.brightness = (uint8_t)brightness_value;
                xQueueSend(xRGBCommandQueue, &command, 0);
                xil_printf("UART: brightness %u\r\n", brightness_value);
            }
            return;
        }

        // Parse color command: "C <R|G|B|C|M|Y|W>" or "COLOR <R|G|B|C|M|Y|W>"
        if (strncmp(command_line, "C ", 2) == 0 || strncmp(command_line, "COLOR ", 6) == 0) {
            param_ptr = (command_line[0] == 'C') ? command_line + 2 : command_line + 6;
            while (*param_ptr == ' ') param_ptr++;
            color_value = parse_color_char((char)*param_ptr);
            if (color_value != 0) {
                command.type = UART_CMD_LED_COLOR;
                command.param.color = color_value;
                xQueueSend(xRGBCommandQueue, &command, 0);
                xil_printf("UART: color %c\r\n", (char)*param_ptr);
            }
            return;
        }

        // Parse display command: "D <xy>" or "SSD <xy>" where x and y are 0-9 or A-F
        if (command_line[0] == 'D' || strncmp(command_line, "SSD", 3) == 0) {
            param_ptr = (command_line[0] == 'D') ? command_line + 1 : command_line + 3;
            while (*param_ptr == ' ') param_ptr++;
            if (!*param_ptr) return;
            
            left_char = *param_ptr++;
            while (*param_ptr == ' ') param_ptr++;
            right_char = (*param_ptr && *param_ptr != '\r' && *param_ptr != '\n') ? *param_ptr : ' ';
            
            // Convert to uppercase
            if (left_char >= 'a' && left_char <= 'z') left_char = (char)(left_char - 'a' + 'A');
            if (right_char >= 'a' && right_char <= 'z') right_char = (char)(right_char - 'a' + 'A');
            
            display_data.previous_key = (u8)left_char;
            display_data.current_key  = (u8)(right_char == ' ' ? left_char : right_char);
            xQueueOverwrite(xSSDCommandQueue, &display_data);
            xil_printf("UART: SSD %c %c\r\n", left_char, (right_char == ' ' ? left_char : right_char));
            return;
        }
    }

    // UART task: receives commands and dispatches them
    static void vUARTTask(void *pvParameters)
    {
        static char command_buffer[UART_RX_LINE_MAX];
        static size_t buffer_length = 0;
        uint8_t received_byte;

        xil_printf("UART commands: B <0-25> brightness, C <R|G|B|C|M|Y|W> color, D <xy> or SSD <xy> display\r\n");

        for (;;) {
            if (uart_poll_rx(&received_byte)) {
                if (received_byte == '\r' || received_byte == '\n') {
                    if (buffer_length > 0) {
                        command_buffer[buffer_length] = '\0';
                        parse_and_dispatch(command_buffer);
                        buffer_length = 0;
                    }
                } else if (buffer_length < UART_RX_LINE_MAX - 1) {
                    command_buffer[buffer_length++] = (char)received_byte;
                } else {
                    // Buffer full, reset to prevent overflow
                    buffer_length = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }