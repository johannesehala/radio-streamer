/**
 * @brief Receives bytes from jpnevulator serial sniffer. Waits to receive
 *        special token. Then starts logging all subsequent non-token bytes. 
 * 
 * @usage 
 *        jpnevulator -read -t /dev/ttyUSB0 | ./pars_serial_direct
 *        baud rate 115200
 *
 * @note this little-endian-big-endian business makes the code complicated to understand
 */

#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <time.h>

#define NUM_TOKEN_BYTES             4
#define NUM_DATA_ELEMENT_BYTES      2
#define NUM_FILE_NAME_CHARACTERS    100
static u_int8_t token[NUM_TOKEN_BYTES];
static int count_to_4 = 3, count_3_elements;
bool token_received(int i);
void write_to_log();

bool logging_enabled = false;
int flush_token_count = 0;
int data_element_count = 0;

FILE *fp = NULL;

void sigint_handler(int sig)
{
    fclose(fp);
    exit(sig);
}

int main(int argc, char **argv) 
{
	int i, bytes;
	bool firsttime = true;
	char filename[NUM_FILE_NAME_CHARACTERS];

    signal(SIGINT, sigint_handler);

    *argv++; // Don't read our own name.
    if (*argv == NULL)printf("No file name specified!");
    else memmove(&filename, *argv, NUM_FILE_NAME_CHARACTERS);
    
    fp = fopen(filename, "a"); // Open the file again.
    if (!fp)
    {
        printf("Failed to open %s!\n", filename);
        return 0;
    }
    else printf("Write results to %s.\n", filename);
    
    
    bytes=0;
    while(1)
	{
		if(scanf("%x ", &i))
		{
    	    // Write byte to token discovery buffer and check if we have a token.
			if(token_received(i))
			{
			    if(firsttime)
			    {
			        logging_enabled = true;
			        firsttime = false;
			    }
			    flush_token_count = 0;
			    data_element_count = 2;
			    count_3_elements = 1;
			    //printf("Token - %u bytes received before token (including token).\n", bytes);
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
    
    if(count_to_4 >= 0 )token[count_to_4--] = (u_int8_t) i; // Only first 4 bytes end up here.
    else
    {
        // Shift all bytes in token buffer to the right.
        for(k=(NUM_TOKEN_BYTES-1);k>0;k--)token[k] = token[k-1];
        token[k] = (u_int8_t) i;
    }
    return *(u_int32_t*)token == 0xDEADBEEF;
}

void write_to_log()
{
    if(logging_enabled)
    {
        if(flush_token_count == NUM_TOKEN_BYTES) // Avoid writing token to output.
        {
            if(data_element_count == NUM_DATA_ELEMENT_BYTES) // Avoid writing single bytes to output, write whole element.
            {
                if(count_3_elements < 3) // Write count_3_elements elements on one line.
                {
                    fprintf(fp, "%u ", *((u_int16_t*)token+1));
                    count_3_elements++;
                }
                else // End the line.
                {
                    fprintf(fp, "%u\n", *((u_int16_t*)token+1));
                    count_3_elements = 1;
                }
                data_element_count = 1;
            }
            else data_element_count++; // Count to NUM_DATA_ELEMENT_BYTES.
        }
        else flush_token_count++; // Counting to NUM_TOKEN_BYTES.
    }
    else ; // Waiting for first token.
}
    
