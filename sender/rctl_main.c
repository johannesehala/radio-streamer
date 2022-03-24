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

#define DATA_PATCH_LEN     48 // number of data units (a data unit is 2 bytes) in one message
#define DATA_PAYLOAD_SIZE    (DATA_PATCH_LEN * 2 + 4) // Max allowed payload is 114 bytes (comms_get_paylaod_max_length())
#define DATA_START_OFFSET   2 // data start offset in msg is 4 bytes, because first 4 bytes are msg nr
#define DATA_GEN_SPEED      1 // ms, new data is generated every millisecond

static comms_msg_t msg_1, msg_2; // Message 1 and 2
static bool m_sending = false;
static osMutexId_t radio_hw_mutex; // Protects radio hardware against multiple message send operations (m_sending)
static osMutexId_t msg_1_mutex; // Protects msg 1 memory against simulataneous access (by data gen and data send tasks)
static osMutexId_t msg_2_mutex; // Protects msg 2 memory against simulataneous access (by data gen and data send tasks)
static osThreadId_t ds_thread_id;

static comms_layer_t* radio;

typedef enum
{
    MSG_1_PAYLOAD,
    MSG_2_PAYLOAD
}databuf_type_t;

#define MSG1_READY_FLAG     0x01
#define MSG2_READY_FLAG     0x02
#define MSG1_SENT_FLAG      0x04
#define MSG2_SENT_FLAG      0x08


// Receive a message from the network - NB! Not used. All messages dropped.
static void receive_message (comms_layer_t* comms, const comms_msg_t* msg, void* user)
{
    info1("Received.");
}

// Message1 has been sent
static void radio_send_done_msg1 (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
    while(osMutexAcquire(radio_hw_mutex, 1000) != osOK);
    m_sending = false;
    osMutexRelease(radio_hw_mutex);

    // Release msg 1
    osThreadFlagsSet(ds_thread_id, MSG1_SENT_FLAG);
}

// Message2 has been sent
static void radio_send_done_msg2 (comms_layer_t * comms, comms_msg_t * msg, comms_error_t result, void * user)
{
    logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snt %u", result);
    while(osMutexAcquire(radio_hw_mutex, 1000) != osOK);
    m_sending = false;
    osMutexRelease(radio_hw_mutex);
    
    // Release msg 2
    osThreadFlagsSet(ds_thread_id, MSG2_SENT_FLAG);
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

/**
 * @brief Generate dummy values and write to msg payload area. 
 *        x-axis values incremented
 *        y-axis values decremented
 *        z-axis values constant
 * 
 * @note    DATA_START_OFFSET is 4 bytes, since pointer is 16-bit (2bytes) we offset by avalue of 2
 * 
 * @return  payload_index incremented by number of bytes written to payload
 */
uint8_t write_new_data(comms_msg_t *msg, uint16_t payload_index)
{
    static uint16_t counter_x = 0, counter_y = 0xffff, counter_z = 127;

    // Cast pointer to msg payload area as 16 bit integer, cuz we are writing 16-bit values to payload
    uint16_t *payload = (uint16_t*)comms_get_payload(radio, msg, DATA_PAYLOAD_SIZE);
    *(payload+payload_index+DATA_START_OFFSET) = hton16(counter_x); // New x axis value
    *(payload+payload_index+1+DATA_START_OFFSET) = hton16(counter_y); // New y axis value
    *(payload+payload_index+2+DATA_START_OFFSET) = hton16(counter_z); // New z axis value

    counter_x++;
    counter_y--;    
    // counter_z unchanged
    
    return (uint8_t) payload_index + 3; 
}

void write_msg_number(comms_msg_t *msg, uint32_t msg_nr)
{
    uint32_t *payload = (uint32_t*)comms_get_payload(radio, msg, DATA_PAYLOAD_SIZE);
    *(payload) = hton32(msg_nr);
}

void data_gen_loop ()
{    
    static uint16_t buf_index = 0; // Number of bytes written to msg payload
    static comms_msg_t *msg=&msg_1;
    static databuf_type_t fill_data_buf = MSG_1_PAYLOAD;
    uint32_t msg_count=0;
    
    osDelay(1500);
    
    // Write msg number to first msg
    write_msg_number(msg, msg_count++);
    
    for(;;)
    {
        osDelay(1);
        if(buf_index < DATA_PATCH_LEN) // If buffer is not full
        {
            // ... write data to buffer
            
            if(fill_data_buf == MSG_1_PAYLOAD) // If using msg 1
            {
                // ... get access to msg 1 
                if(osMutexAcquire(msg_1_mutex, 1000) == osOK)
                {
                    // and write new data and release resource
                    buf_index = write_new_data(msg, buf_index);
                    osMutexRelease(msg_1_mutex);
                }
                else 
                {
                    // Msg 1 not available
                    PLATFORM_LedsSet(PLATFORM_LedsGet()^2);
                }
            }
            else if(fill_data_buf == MSG_2_PAYLOAD)// If using msg 2
            {
                // ... get access to msg 2
                if(osMutexAcquire(msg_2_mutex, 1000) == osOK)
                {
                    // and write new data and release resource
                    buf_index = write_new_data(msg, buf_index);
                    osMutexRelease(msg_2_mutex);
                }
                else 
                {
                    // Msg 2 not available
                    PLATFORM_LedsSet(PLATFORM_LedsGet()^2);
                }
            }
            else ; // Shouldn't get here
        }
        else
        {
            // Buffer full, send this buffer over radio
            // Switch to next buffer
            if(fill_data_buf == MSG_1_PAYLOAD) // If using msg 1
            {
                // ... get access to msg 2 
                if(osMutexAcquire(msg_2_mutex, 1000) == osOK)
                {
                    fill_data_buf = MSG_2_PAYLOAD;
                    msg = &msg_2;
                    write_msg_number(msg, msg_count++);
                    buf_index = write_new_data(msg, 0);
                    osMutexRelease(msg_2_mutex);
                    // Trigger message send
                    osThreadFlagsSet(ds_thread_id, MSG1_READY_FLAG);
                }
                else 
                {
                    // Msg 2 not available
                    PLATFORM_LedsSet(PLATFORM_LedsGet()^1);
                }
            }
            else if(fill_data_buf == MSG_2_PAYLOAD) // If using msg 2
            {
                // ... get access to msg 1 
                if(osMutexAcquire(msg_1_mutex, 1000) == osOK)
                {
                    fill_data_buf = MSG_1_PAYLOAD;
                    msg = &msg_1; 
                    write_msg_number(msg, msg_count++); 
                    buf_index = write_new_data(msg, 0);
                    osMutexRelease(msg_1_mutex);
                    // Trigger message send
                    osThreadFlagsSet(ds_thread_id, MSG2_READY_FLAG);
                }
                else 
                {
                    // Msg 1 not available
                    PLATFORM_LedsSet(PLATFORM_LedsGet()^1);
                }
            }
            else ; // Shouldn't get here
        }
    }
}

void data_send_loop ()
{
    static uint8_t flags = MSG1_READY_FLAG;
    static comms_msg_t *m_msg;
    static uint32_t cnt = 0;
    comms_error_t result;

    osDelay(500);

    for(;;)
    {
        // Wait for thread flag
        osThreadFlagsClear(MSG1_READY_FLAG | MSG2_READY_FLAG);
        flags = osThreadFlagsWait(MSG1_READY_FLAG | MSG2_READY_FLAG, osFlagsWaitAny, osWaitForever);
        
        if(flags == MSG1_READY_FLAG)
        {
            if(osMutexAcquire(msg_1_mutex, 1000) == osOK) // Get access to msg 1
            {
                m_msg = &msg_1;
                // Mutex is released by send done function!
            }
            else 
            {
                // Msg 1 not available
                PLATFORM_LedsSet(PLATFORM_LedsGet()^4);
            }
        }
        else if (flags == MSG2_READY_FLAG)
        {
            if(osMutexAcquire(msg_2_mutex, 1000) == osOK) // Get access to msg 2
            {
                m_msg = &msg_2;
                // Mutex is released by send done function!
            }
            else 
            {
                // Msg 2 not available
                PLATFORM_LedsSet(PLATFORM_LedsGet()^4);
            }
        }
        else ; // Multiple flags or unknown flags set or error
        
        while(osMutexAcquire(radio_hw_mutex, 1000) != osOK); // Get access to radio hardware
        if(false == m_sending)
        {
            comms_set_packet_type(radio, m_msg, AMID_RADIO_COUNT_TO_LEDS);
            comms_am_set_destination(radio, m_msg, AM_BROADCAST_ADDR);
            comms_set_payload_length(radio, m_msg, DATA_PAYLOAD_SIZE);

            if(flags == MSG1_READY_FLAG)result = comms_send(radio, m_msg, radio_send_done_msg1, NULL);
            else if(flags == MSG2_READY_FLAG)result = comms_send(radio, m_msg, radio_send_done_msg2, NULL);
            else 
            {
                // Normally we should release radio_hw_mutex mutexes here,
                // but this time we want the system to hang if
                // if it can't handle the data load.
                result = COMMS_FAIL;
            }
            logger(result == COMMS_SUCCESS ? LOG_DEBUG1: LOG_WARN1, "snd %u", result);
            if (COMMS_SUCCESS == result)
            {
                m_sending = true;
            }
        }
        osMutexRelease(radio_hw_mutex);

        // Wait for message to be sent, then release the mutex
        osThreadFlagsClear(MSG1_SENT_FLAG | MSG2_SENT_FLAG);
        flags = osThreadFlagsWait(MSG1_SENT_FLAG | MSG2_SENT_FLAG, osFlagsWaitAny, 1000);
        if(flags == MSG1_SENT_FLAG)
        {
            osMutexRelease(msg_1_mutex);
        }
        else if (flags == MSG2_SENT_FLAG)
        {
            osMutexRelease(msg_2_mutex);
        }
        else info2("%u",flags); // Multiple flags or unknown flags set or error
        info3("m %lu", cnt++);
    }
}

// HB loop - increment and send counter
void hb_loop ()
{
    am_addr_t node_addr = DEFAULT_AM_ADDR;
    uint8_t node_eui[8];
    
    radio_hw_mutex = osMutexNew(NULL);
    msg_1_mutex = osMutexNew(NULL);
    msg_2_mutex = osMutexNew(NULL);

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

    // Message init should be done before any other comms_ function is called
    comms_init_message(radio, &msg_1);
    comms_init_message(radio, &msg_2);

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
    
    const osThreadAttr_t gen_thread_attr = { .name = "gen" };
    osThreadNew(data_gen_loop, NULL, &gen_thread_attr);
    
    const osThreadAttr_t send_thread_attr = { .name = "send" };
    ds_thread_id = osThreadNew(data_send_loop, NULL, &send_thread_attr);

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
