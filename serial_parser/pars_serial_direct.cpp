/**
 * @brief Receives bytes from jpnevulator serial sniffer. Waits to receive
 *        special token. Then starts logging all subsequent non-token bytes. 
 * 
 * @usage 
 *        jpnevulator -read -t /dev/ttyUSB0 | ./pars_serial_direct
 *        baud rate 115200
 *
 * @note this little-endian-big-endian stuff makes it so complicated to understand
 */

#include <stdio.h>
#include <cstdlib>
#include <time.h>

#define NUM_TOKEN_BYTES             4
#define NUM_DATA_ELEMENTS_BYTES     2

static u_int8_t token[NUM_TOKEN_BYTES];
static int count_to_4 = 3;
bool token_received(int i);
void write_to_log();


bool logging_enabled = false;
int flush_token_count = 0;
int data_element_count = 0;

int main(int argc, char **argv) 
{
	int i, bytes;
	bool firsttime = true;

    bytes=0;
    
	while(1)
	{
		if(scanf("%x ", &i))
		{
		    // write byte to token discovery buffer
		    // check if we have a token
			if(token_received(i))
			{
			    if(firsttime)
			    {
			        logging_enabled = true;
			        firsttime = false;
			    }
			    flush_token_count = 0;
			    data_element_count = 2;
			    printf("Token - %u bytes received before token (including token).\n", bytes);
			    bytes = 0;
			}
			else ;
			bytes++;
			write_to_log();
		}
	}
	return 0;
}

bool token_received(int i)
{
    u_int8_t k;
    
    if(count_to_4 >= 0 )token[count_to_4--] = (u_int8_t) i; // only first 8 bytes end up here
    else
    {
        // shift all bytes in token buffer to the right
        for(k=(NUM_TOKEN_BYTES-1);k>0;k--)token[k] = token[k-1];
        token[k] = (u_int8_t) i;
    }
    return *(u_int32_t*)token == 0xDEADBEEF;
}

void write_to_log()
{
    if(logging_enabled)
    {
        if(flush_token_count == NUM_TOKEN_BYTES)
        {
            if(data_element_count == NUM_DATA_ELEMENTS_BYTES)
            {
                printf("%u\n", *((u_int16_t*)token+1));
                data_element_count = 1;
            }
            else data_element_count++;
        }
        else flush_token_count++;
    }
    else ; // waiting for first token
}
    
