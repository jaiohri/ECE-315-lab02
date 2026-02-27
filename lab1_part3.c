        /*
    * Lab 1, Part 3 - Seven-Segment Display & Keypad
    *
    * ECE-315 WINTER 2025 - COMPUTER INTERFACING
    * Created on: February 5, 2021
    * Modified on: July 26, 2023
    * Modified on: January 20, 2025
    * Author(s):  Shyama Gandhi, Antonio Andara Lara
    *
    * Summary:
    * 1) Declare & initialize the 7-seg display (SSD).
    * 2) Use xDelay to alternate between two digits fast enough to prevent flicker.
    * 3) Output pressed keypad digits on both SSD digits: current_key on right, previous_key on left.
    * 4) Print status changes and experiment with xDelay to find minimum flicker-free frequency.
    *
    * Deliverables:
    * - Demonstrate correct display of current and previous keys with no flicker.
    * - Print to the SDK terminal every time that theh variable `status` changes.
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
    /* ========== PART 2 ADDITIONS START (includes & defines) ========== */
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

    // UART base address (from BSP xparameters)
    #define UART_BASEADDR         XPAR_UART1_BASEADDR
    #define UART_RX_LINE_MAX      64
    /* ========== PART 2 ADDITIONS END (includes & defines) ========== */

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

    /* ========== PART 2 ADDITIONS START (command structure & queues) ========== */
    typedef enum {
        UART_CMD_LED_BRIGHTNESS,
        UART_CMD_LED_COLOR,
        UART_CMD_SSD_DISPLAY,
        UART_CMD_INVALID
    } UART_CommandType_t;

    typedef struct {
        UART_CommandType_t type;
        union {
            uint8_t brightness;   // 0..25 maps to xOnDelay
            uint8_t color;        // RGB_* value from rgb_led.h
            struct { u8 left; u8 right; } ssd;
        } param;
    } UART_Command_t;

    QueueHandle_t xRGBCommandQueue;
    QueueHandle_t xSSDCommandQueue;

    static XUartPs UartPs;
    /* ========== PART 2 ADDITIONS END (command structure & queues) ========== */

    /*****************************************************************************/

    // Function prototypes
    void InitializeKeypad();
    void InitializePeripherals();
    static void vKeypadTask( void *pvParameters );
    static void vRGBTask( void *pvParameters );
    static void vButtonsTask( void *pvParameters );
    static void vDisplayTask( void *pvParameters );
    u32 SSD_decode(u8 key_value, u8 cathode);
    /* ========== PART 2 ADDITIONS START (prototypes) ========== */
    static void vUARTTask( void *pvParameters );
    static void uart_init(void);
    static int uart_poll_rx(uint8_t *b);
    static void parse_and_dispatch(const char *line);
    /* ========== PART 2 ADDITIONS END (prototypes) ========== */

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

        /* ========== PART 2 ADDITIONS START (main: queues, uart_init, UART task) ========== */
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
        /* ========== PART 2 ADDITIONS END (main: queues, uart_init, UART task) ========== */

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
        /* ========== PART 2 ADDITIONS START (vRGBTask: UART command poll) ========== */
        uint8_t color = RGB_CYAN;
        const TickType_t xPeriod = 25;
        TickType_t xOnDelay = 0;
        TickType_t xOffDelay = xPeriod - xOnDelay;
        u32 button_val;
        static u32 prev_button_val = 0;
        UART_Command_t uart_cmd;

        while (1){
            /* Non-blocking poll: button commands (original Lab 1 Part 3 behavior) */
            if (xQueueReceive(xButtonsRGBQueue, &button_val, 0) == pdTRUE){
                if (button_val != prev_button_val) {
                    if (button_val == 0x08 && xOnDelay < xPeriod) {
                        xOnDelay++;
                        xil_printf("xOnDelay: %d, xOffDelay: %d\n", xOnDelay, xPeriod - xOnDelay);
                    } else if (button_val == 0x01 && xOnDelay > 0) {
                        xOnDelay--;
                        xil_printf("xOnDelay: %d, xOffDelay: %d\n", xOnDelay, xPeriod - xOnDelay);
                    }
                    prev_button_val = button_val;
                }
            }

            if (xQueueReceive(xRGBCommandQueue, &uart_cmd, 0) == pdTRUE) {
                if (uart_cmd.type == UART_CMD_LED_BRIGHTNESS) {
                    xOnDelay = uart_cmd.param.brightness;
                    if (xOnDelay > xPeriod) xOnDelay = xPeriod;
                } else if (uart_cmd.type == UART_CMD_LED_COLOR) {
                    color = uart_cmd.param.color;
                }
            }
            /* ========== PART 2 ADDITIONS END (vRGBTask: UART command poll) ========== */

            xOffDelay = xPeriod - xOnDelay;
            /* LED on for xOnDelay ticks */
            XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, color);
            if (xOnDelay == 0) {
                XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, 0);
            }
            
            vTaskDelay(xOnDelay);

            /* LED off for xOffDelay ticks */
            XGpio_DiscreteWrite(&RGB_LEDInst, RGB_CHANNEL, 0);
            vTaskDelay(xOffDelay);
        }
    }


    static void vDisplayTask( void *pvParameters ) {
        KeypadState_t data = {0, 0};
        KeypadState_t new_data;
        const TickType_t xDelay = pdMS_TO_TICKS(10);
        
        while (1) {
            /* ========== PART 2 ADDITIONS START (vDisplayTask: SSD queue poll) ========== */
            while (xQueueReceive(xKeypadDisplayQueue, &new_data, 0) == pdTRUE) {
                data = new_data;
            }
            if (xQueueReceive(xSSDCommandQueue, &new_data, 0) == pdTRUE) {
                data = new_data;
            }
            /* ========== PART 2 ADDITIONS END (vDisplayTask: SSD queue poll) ========== */
            
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
    /* ========== PART 2 ADDITIONS START (UART init, parsing, vUARTTask) ========== */
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

    static int uart_poll_rx(uint8_t *b)
    {
        if (XUartPs_IsReceiveData(UartPs.Config.BaseAddress)) {
            *b = (uint8_t)XUartPs_ReadReg(UartPs.Config.BaseAddress, XUARTPS_FIFO_OFFSET);
            return 1;
        }
        return 0;
    }

    /* Parse one line and send command to appropriate queue (Task 2). */
    static void parse_and_dispatch(const char *line)
    {
        UART_Command_t cmd;
        KeypadState_t ssd_data;
        unsigned int n;
        char a, b;

        if (!line || strlen(line) == 0) return;

        /* BRIGHTNESS n or B n (n 0-25) */
        if (strncmp(line, "B ", 2) == 0 || strncmp(line, "BRIGHTNESS ", 11) == 0) {
            const char *p = (line[0] == 'B') ? line + 2 : line + 11;
            if (sscanf(p, "%u", &n) == 1 && n <= 25) {
                cmd.type = UART_CMD_LED_BRIGHTNESS;
                cmd.param.brightness = (uint8_t)n;
                xQueueSend(xRGBCommandQueue, &cmd, 0);
                xil_printf("UART: brightness %u\r\n", n);
            }
            return;
        }

        /* COLOR x or C x (R G B C M Y W) */
        if (strncmp(line, "C ", 2) == 0 || strncmp(line, "COLOR ", 6) == 0) {
            const char *p = (line[0] == 'C') ? line + 2 : line + 6;
            while (*p == ' ') p++;
            a = (char)*p;
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            switch (a) {
                case 'R': cmd.param.color = RGB_RED;    break;
                case 'G': cmd.param.color = RGB_GREEN;  break;
                case 'B': cmd.param.color = RGB_BLUE;   break;
                case 'C': cmd.param.color = RGB_CYAN;   break;
                case 'M': cmd.param.color = RGB_MAGENTA; break;
                case 'Y': cmd.param.color = RGB_YELLOW; break;
                case 'W': cmd.param.color = RGB_WHITE;  break;
                default: return;
            }
            cmd.type = UART_CMD_LED_COLOR;
            xQueueSend(xRGBCommandQueue, &cmd, 0);
            xil_printf("UART: color %c\r\n", a);
            return;
        }

        /* SSD xy or D xy - two chars for left and right digit (0-9, A-F). Accept "D 20", "D20", "SSD 20" */
        if (line[0] == 'D' || strncmp(line, "SSD", 3) == 0) {
            const char *p = (line[0] == 'D') ? line + 1 : line + 3;
            while (*p == ' ') p++;
            if (!*p) return;
            a = *p++;
            while (*p == ' ') p++;
            b = (*p && *p != '\r' && *p != '\n') ? *p : (char)' ';
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            ssd_data.previous_key = (u8)a;
            ssd_data.current_key  = (u8)(b == ' ' ? a : b);
            xQueueOverwrite(xSSDCommandQueue, &ssd_data);
            xil_printf("UART: SSD %c %c\r\n", a, b == ' ' ? a : b);
            return;
        }
    }

    static void vUARTTask(void *pvParameters)
    {
        static char line_buf[UART_RX_LINE_MAX];
        static size_t len = 0;
        uint8_t byte;

        xil_printf("UART commands: B <0-25> brightness, C <R|G|B|C|M|Y|W> color, D <xy> or SSD <xy> display\r\n");

        for (;;) {
            if (uart_poll_rx(&byte)) {
                if (byte == '\r' || byte == '\n') {
                    if (len > 0) {
                        line_buf[len] = '\0';
                        parse_and_dispatch(line_buf);
                        len = 0;
                    }
                } else if (len < UART_RX_LINE_MAX - 1) {
                    line_buf[len++] = (char)byte;
                } else {
                    len = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    /* ========== PART 2 ADDITIONS END (UART init, parsing, vUARTTask) ========== */