# Part 1 – UART Communication using Polling

## High-Level Solution

Part 1 of this lab implements UART communication in polling mode with a SHA-256 hashing and verification system. Four FreeRTOS tasks run concurrently: UART_RX polls the UART receive FIFO and enqueues incoming bytes to a queue; UART_TX dequeues characters from an output queue and writes them to the UART transmit FIFO; a CLI task displays a menu, reads user input from the receive queue, and sends hash or verify requests to a command queue; a Crypto task dequeues requests, computes SHA-256, and returns results via a result queue. The two UART directions are time-multiplexed by the scheduler so that polling does not block other tasks. Menu prompts, user input flow, and hash or verification results are printed to the SDK terminal.

## Task Flow Diagram

See the task flow diagram below. main() initializes the UART (baud 115200), creates the four tasks (UART_RX, UART_TX, CLI, Crypto) and four queues (q_rx_byte, q_tx, q_cmd, q_result), and starts the scheduler. The UART supplies received bytes to the UART_RX task; UART_RX enqueues them to q_rx_byte. The CLI task reads from q_rx_byte, sends requests to q_cmd, and sends output strings to q_tx. The Crypto task reads from q_cmd, computes SHA-256, and writes results to q_result. The UART_TX task reads from q_tx and drives the UART transmit FIFO. Menu, prompts, and results are printed to the terminal via q_tx and UART_TX.

```
    UART (115200)  ----[RX poll]---->  UART_RX  ---->  q_rx_byte  ---->  CLI
         ^                                                                    |
         |                                                                    v
    [TX poll]  <----  UART_TX  <----  q_tx  <----  (menu, results)      q_cmd
         |                                                                    |
         |                                                                    v
                                                                         Crypto
                                                                              |
                                                                         q_result  ----> (back to CLI)
```

## Code Explanation

**Initialization (main):** UART is set up with uart_init() using the Xilinx XUartPs driver (config lookup, baud rate 115200). Four tasks are created: UART_RX, UART_TX, CLI, and Crypto, each with priority 2. Four queues are created—q_rx_byte (bytes from UART), q_tx (characters to send), q_cmd (crypto_request_t), and q_result (crypto_result_t). The FreeRTOS scheduler is then started.

**UART_RX task:** Runs in a loop. It uses uart_poll_rx() to check for a received byte; when data is present, it enqueues the byte to q_rx_byte. A vTaskDelay of 1 ms is used between polls so other tasks can run.

**UART_TX task:** Blocks on q_tx. When a character is received, it writes the byte to the UART transmit FIFO using uart_tx_byte() (which polls until the FIFO is not full). A short delay is used after each send.

**CLI task:** Prints the initial message and then runs in a loop. It prints the menu (1. Hash a string, 2. Verify hash), reads the user option with receive_byte() (from q_rx_byte), then for hash reads the input string with receive_string(), or for verify reads the string and the expected hash. It sends a crypto_request_t to q_cmd, then polls q_result until the Crypto task posts a crypto_result_t. It prints the calculated hash and, for verify, whether the hashes match, by sending characters to q_tx. After each operation it waits for a key press and flushes q_rx_byte before reprinting the menu.

**Crypto task:** Runs in a loop. When it receives a crypto_request_t from q_cmd, it calls sha256_string() on the input, converts the binary hash to a hex string with hash_to_string(), and for verify compares that string to the expected hash and sets the match flag. It sends the crypto_result_t to q_result.

## Deliverables

- **Terminal:** Menu displayed on startup and after each operation. User can select hash or verify; prompts and computed hash or verification result (and match status) printed to the SDK terminal.
- **UART:** Receive and transmit handled in polling mode at 115200 baud with no UART interrupts; all I/O passes through the four tasks and queues.
