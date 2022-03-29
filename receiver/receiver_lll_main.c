/**
 * Generate lots of data, send lots of messages over radio and 
 * signal alarm if messages are dropped.
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

#include "loggers_ext.h"
#include "logger_fwrite.h"

#include "radio_count_to_leds.h"

#include "DeviceSignature.h"
#include "mist_comm_am.h"
#include "radio.h"

#include "endianness.h"

#include "loglevels.h"
#define __MODUUL__ "main"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

// Include the information header binary
#include "incbin.h"
INCBIN(Header, "header.bin");

#define MSG_RECEIVE_FLAG    0x01

static osThreadId_t dr_thread_id;
static osMessageQueueId_t dr_queue_id;
static osMutexId_t statistics_mutex_id;

static uint32_t radiobytes = 0;
static bool dataloss = false;

static comms_layer_t* radio;

typedef struct
{
    uint8_t bytes;
    uint8_t data_items;
    uint32_t msgnr;
    uint16_t x_first;
    uint16_t y_first;
    uint16_t z_first;
    uint16_t x_last;
    uint16_t y_last;
    uint16_t z_last;
} msg_content_t;
    
// Receive a message from the network
static void receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
    static msg_content_t msg_cont;
    static uint32_t msg_nr;
    uint16_t *payload;
    uint32_t *payload32;
    
    // Get payload length
    msg_cont.bytes = (uint8_t)comms_get_payload_length(comms, msg);
    msg_cont.data_items = (uint8_t) (msg_cont.bytes / 2);
    
    payload32 = (uint32_t*)comms_get_payload(comms, msg, msg_cont.bytes);
    msg_cont.msgnr = ntoh32(*(payload32));
    
    payload = (uint16_t*)comms_get_payload(comms, msg, msg_cont.bytes);
    
    // Read first 6 bytes and read last 6 btyes
    msg_cont.x_first = ntoh16(*payload);
    msg_cont.y_first = ntoh16(*(payload+1));
    msg_cont.z_first = ntoh16(*(payload+2));
    msg_cont.x_last = ntoh16(*(payload+msg_cont.data_items-2));
    msg_cont.y_last = ntoh16(*(payload+msg_cont.data_items-1));
    msg_cont.z_last = ntoh16(*(payload+msg_cont.data_items));
    
    if(msg_nr != (msg_cont.msgnr - 1))info("Message lost %lu", msg_cont.msgnr-msg_nr);
    msg_nr = msg_cont.msgnr;
    
    // Post to queue 
    osMessageQueuePut(dr_queue_id, &msg_cont, 0, 0);
}

static void radio_start_done (comms_layer_t * comms, comms_status_t status, void * user)
{
    info1("Radio started %d", status);
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
    debug1("radio rdy");
    return radio;
}

void data_receive_loop ()
{
    static msg_content_t msg_cont;
    bool msg_lost, data_lost;
    uint16_t last_x = -1;
    
    osDelay(500);

    for(;;)
    {
        if(osMessageQueueGet(dr_queue_id, &msg_cont, NULL, 100) == osOK)
        {
            // Increment received bytes
            if(osMutexAcquire(statistics_mutex_id, 1000) == osOK)
            {
                radiobytes += msg_cont.bytes;
                osMutexRelease(statistics_mutex_id);
            }
            else info("Mutex unavailable");
            
            // Check data for data loss (lost messages)
            msg_lost = data_lost = false;
            if(msg_cont.x_first == last_x + 1)msg_lost = true;
            if(msg_cont.x_first + msg_cont.data_items == msg_cont.x_last)data_lost = true;
            if(msg_cont.z_first == msg_cont.z_last && msg_cont.z_first == 127)data_lost = true;
            
            // NB! There will be data loss when x value wraps around (from 0xffff to 0)
            if(msg_lost | data_lost)
            {
                if(osMutexAcquire(statistics_mutex_id, 1000) == osOK)
                {
                    dataloss = true;
                    osMutexRelease(statistics_mutex_id);
                }
                else info("Mutex unavailable");
            }
            last_x = msg_cont.x_last;
        }
    }
}

void statistics_loop ()
{
    #define REPORT_INTERVAL     1 // Seconds
    static uint32_t bytes;
    static bool loss;
    
    if(osMutexAcquire(statistics_mutex_id, 1000) == osOK)
    {
        radiobytes = 0;
        dataloss = false;
        osMutexRelease(statistics_mutex_id);
    }
    else info("Mutex unavailable");
    
    for(;;)
    {
        osDelay(REPORT_INTERVAL * osKernelGetTickFreq());
        if(osMutexAcquire(statistics_mutex_id, 1000) == osOK)
        {
            bytes = radiobytes;
            loss = dataloss;
            radiobytes = 0;
            dataloss = false;
            osMutexRelease(statistics_mutex_id);
        }
        else info("Mutex unavailable");
        
        // NB! There will be data loss when x value wraps around (from 0xffff to 0)
        if(!loss)info3("During %u seconds - %lu bytes received, no loss", REPORT_INTERVAL, bytes);
        else info3("Data lost! during %u seconds - %lu bytes received", REPORT_INTERVAL, bytes);
    }
}

// HB loop - increment and send counter
void hb_loop ()
{
    am_addr_t node_addr = DEFAULT_AM_ADDR;
    uint8_t node_eui[8];
    
    dr_queue_id = osMessageQueueNew(5, sizeof(msg_content_t), NULL);
    statistics_mutex_id = osMutexNew(NULL);
    
    // Initialize node signature - get address and EUI64
    if (SIG_GOOD == sigInit())
    {
        node_addr = sigGetNodeId();
        sigGetEui64(node_eui);
        infob1("ADDR:%"PRIX16" EUI64:", node_eui, sizeof(node_eui), node_addr);
    }
    else
    {
        warn1("ADDR:%"PRIX16, node_addr); // Falling back to default addr
    }

    // Initialize radio
    radio = radio_setup(node_addr);
    if (NULL == radio)
    {
        err1("radio");
        for (;;); // panic
    }

    for (;;)
    {
        osDelay(10*osKernelGetTickFreq()); // 10 sec
        info1("Heartbeat");
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
    log_init(BASE_LOG_LEVEL, &logger_fwrite_boot, NULL);

    info1("Radio-test "VERSION_STR" (%d.%d.%d)", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    
    // NB! PLATFORM_RadioInit() actually does nothing for tsb0 platform (see platform.c)
    PLATFORM_RadioInit(); // Radio GPIO/PRS - LNA on some MGM12P

    // Initialize OS kernel
    osKernelInitialize();

    // Create a thread
    const osThreadAttr_t hp_thread_attr = { .name = "hp" };
    osThreadNew(hb_loop, NULL, &hp_thread_attr);

    const osThreadAttr_t stat_thread_attr = { .name = "stat" };
    osThreadNew(statistics_loop, NULL, &stat_thread_attr);

    const osThreadAttr_t recv_thread_attr = { .name = "recv" };
    dr_thread_id = osThreadNew(data_receive_loop, NULL, &recv_thread_attr);

    if (osKernelReady == osKernelGetState())
    {
        // Switch to a thread-safe logger
        logger_fwrite_init();
        log_init(BASE_LOG_LEVEL, &logger_fwrite, NULL);

        // Start the kernel
        osKernelStart();
    }
    else
    {
        err1("!osKernelReady");
    }

    for(;;);
}
