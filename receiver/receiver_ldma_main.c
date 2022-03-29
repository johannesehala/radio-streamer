/**
 * Receive lots of messages over radio and 
 * signal alarm if messages are dropped or missed.
 *
 * Possible speed gains:
 *  - let ldma do ntoh conversion (This is done already)
 *  - use higher serial speed
 *  - don't defer msg handling to thread, use ldma from receive msg 
 *    interrupt (cuz queue does two copy operations)
 *
 * Copyright Thinnect Inc. 2019
 * Copyright Proactivity-Lab, Taltech 2022
 * @license MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "retargetserial.h"

#include "cmsis_os2.h"

#include "platform.h"

#include "SignatureArea.h"
#include "DeviceSignature.h"

//#include "loggers_ext.h"
//#include "logger_fwrite.h"

#include "radio_count_to_leds.h"

#include "DeviceSignature.h"
#include "mist_comm_am.h"
#include "radio.h"

#include "ldma_handler.h"
#include "ldma_descriptors.h"

#include "endianness.h"

//#include "loglevels.h"
//#define __MODUUL__ "main"
//#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
//#include "log.h"

// Include the information header binary
#include "incbin.h"
INCBIN(Header, "header.bin");

#define MAX_PAYLOAD_SIZE    114     // According to comms_get_payload_max_length()

static osThreadId_t dr_thread_id;
static osMessageQueueId_t dr_queue_id;

static comms_layer_t* radio;
    
// Receive a message from the network
static void receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
    uint8_t plen;
    osStatus_t res;
    
    // Get payload length
    plen = (uint8_t)comms_get_payload_length(comms, msg);

    // Post to queue 
    res = osMessageQueuePut(dr_queue_id, comms_get_payload(comms, msg, plen), 0, 0);
    if(res != osOK)
    {
        PLATFORM_LedsSet(PLATFORM_LedsGet() | 0x01);
        //info3("queue put error");
    }
}

static void radio_start_done (comms_layer_t * comms, comms_status_t status, void * user)
{
    //info1("Radio started %d", status);
}

// Perform basic radio setup, register to receive RadioCountToLeds packets
static comms_layer_t* radio_setup (am_addr_t node_addr)
{
    static comms_receiver_t rcvr;
    comms_layer_t * radio = radio_init(DEFAULT_RADIO_CHANNEL, 0x22, node_addr);
    if (NULL == radio)
    {
        return NULL;
    }

    if (COMMS_SUCCESS != comms_start(radio, radio_start_done, NULL))
    {
        return NULL;
    }

    // Wait for radio to start, could use osTreadFlagWait and set from callback
    while(COMMS_STARTED != comms_status(radio))
    {
        osDelay(1);
    }

    comms_register_recv(radio, &rcvr, receive_message, NULL, AMID_RADIO_COUNT_TO_LEDS);
    //debug1("radio rdy");
    return radio;
}

/**
 * @note    Expecting msg payload first 4 bytes to be msg sequence number.
 *          Expecting msg payload size (includeing seq.nr.) to be 100 bytes.
 *
 */
void data_receive_loop ()
{
    static uint8_t msg[MAX_PAYLOAD_SIZE];
    static const uint16_t token[] = {0xDEAD, 0xBEEF};
    uint32_t msg_nr, last_msg_nr;
    
    osDelay(500);
    
    ldma_init();
    ldma_uart_start(token_descriptor_config((uint32_t *)token, 4));
    
    for(;;)
    {
        // TOKEN TEST
        //osDelay(1000);
        //if(!ldma_busy())ldma_uart_start(token_descriptor_config((uint32_t *)token, 4));
        //else PLATFORM_LedsSet(PLATFORM_LedsGet() | 0x01);
        
        if(osMessageQueueGet(dr_queue_id, &msg, NULL, 3000) == osOK)
        {
            // Check msg sequence number
            msg_nr = ntoh32(*((uint32_t*)msg));
            
            if(msg_nr != (last_msg_nr + 1));//info3("Message lost %lu", msg_nr-last_msg_nr);
            else ;//info3("msg ok");
            
            last_msg_nr = msg_nr;
            
            // Write bytes to serial using ldma
            // ldma also converts network byte order to host byte order
            if(!ldma_busy())
            {
                ldma_uart_start(msg_descriptor_config(((uint32_t*)msg)+1, 96));
                //info3("send bytes %lu - %u", ((uint16_t*)msg)+4, ntoh16(*(((uint16_t*)msg)+4)));
                PLATFORM_LedsSet(PLATFORM_LedsGet() ^ 0x04);
            }
            else
            {
                PLATFORM_LedsSet(PLATFORM_LedsGet() | 0x01);
                //info3("ldma busy error");
            }
        }
        else ;//info3("No msg yet");
        osDelay (10000);
    }
}

// HB loop - increment and send counter
void hb_loop ()
{
    am_addr_t node_addr = DEFAULT_AM_ADDR;
    uint8_t node_eui[8];
    
    dr_queue_id = osMessageQueueNew(5, MAX_PAYLOAD_SIZE, NULL);
    
    // Initialize node signature - get address and EUI64
    if (SIG_GOOD == sigInit())
    {
        node_addr = sigGetNodeId();
        sigGetEui64(node_eui);
        //infob1("ADDR:%"PRIX16" EUI64:", node_eui, sizeof(node_eui), node_addr);
    }
    else
    {
        //warn1("ADDR:%"PRIX16, node_addr); // Falling back to default addr
    }

    // Initialize radio
    radio = radio_setup(node_addr);
    if (NULL == radio)
    {
        //err1("radio");
        for (;;); // panic
    }

    for (;;)
    {
        osDelay(10*osKernelGetTickFreq()); // 10 sec
        //info1("Heartbeat");
    }
}

int logger_fwrite_boot (const char *ptr, int len)
{
    fwrite(ptr, len, 1, stdout);
    fflush(stdout);
    return len;
}

int main ()
{
    PLATFORM_Init();

    // LEDs
    PLATFORM_LedsInit();

    // Configure debug output
    RETARGET_SerialInit();
    //log_init(BASE_LOG_LEVEL, &logger_fwrite_boot, NULL);

    //info1("Radio-test "VERSION_STR" (%d.%d.%d)", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    
    // NB! PLATFORM_RadioInit() actually does nothing for tsb0 platform (see platform.c)
    PLATFORM_RadioInit(); // Radio GPIO/PRS - LNA on some MGM12P

    // Initialize OS kernel
    osKernelInitialize();

    // Create a thread
    const osThreadAttr_t hp_thread_attr = { .name = "hp" };
    osThreadNew(hb_loop, NULL, &hp_thread_attr);

    const osThreadAttr_t recv_thread_attr = { .name = "recv" };
    dr_thread_id = osThreadNew(data_receive_loop, NULL, &recv_thread_attr);

    if (osKernelReady == osKernelGetState())
    {
        // Switch to a thread-safe logger
        //logger_fwrite_init();
        //log_init(BASE_LOG_LEVEL, &logger_fwrite, NULL);

        // Start the kernel
        osKernelStart();
    }
    else
    {
        //err1("!osKernelReady");
    }

    for(;;);
}
