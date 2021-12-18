#include "tinyos.h"
#include "kernel_dev.h"


#define PIPE_BUFFER_SIZE 8192


typedef struct pipe_control_block
{
	FCB *reader,*writer;

	CondVar has_space; /*For blocking writer if no space is available*/
	CondVar has_data;	/*For blocking reader until data are available*/

	int w_position,r_position; /*write,read position in buffer*/

	char BUFFER[PIPE_BUFFER_SIZE]; /*bounded (cyclic) byte buffer;*/

	int read_write_count;

}pipe_cb;

int isFull(pipe_cb* pipe);
int isEmpty(pipe_cb* pipe);
int error_read(void* streamobj, char *buf, unsigned int size);
int error_write(void* streamobj, const char *buf, unsigned int size);
int pipe_read(void* streamobj, char *buf, unsigned int size);
int pipe_write(void* streamobj, const char *buf, unsigned int size);
int pipe_reader_close(void* streamobj);
int pipe_writer_close(void* streamobj);