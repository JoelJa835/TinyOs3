
#include "kernel_pipe.h"
#include "util.h"
#include "kernel_sched.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

/*File_ops in order to use Read,Write,Close and Open functions when the pipe is used for reading.
*/
static file_ops reader_file_ops = {
	.Open = NULL,
	.Read = pipe_read,
	.Write = error_write,
	.Close = pipe_reader_close
};

/*File_ops in order to use Read,Write,Close and Open functions when the pipe is used for writing.
*/
static file_ops writer_file_ops = {
	.Open = NULL,
	.Read = error_read,
	.Write = pipe_write,
	.Close = pipe_writer_close
};

/*This function is used in order to create a pipe, it reserves a FCB and then intializes the empty fields.
*/
int sys_Pipe(pipe_t* pipe)
{

	/*Creating an array of size 2 of type Fid_t and FCB.
	A pipe consists of 2 Fid_ts and 2 FCB in order to enable data flow from
	parent ----> child and from
	parent <---- child.
	*/
	Fid_t reserved_Fid_t[2];
	FCB* reserved_FCB[2];

	/*Making a reservation of FCBs and Fid_ts(We are actually filling the previous arrays).
	The FCB reserve function returns 1 if the reservation is successfull.
	*/
	int reservation_complete = FCB_reserve(2,reserved_Fid_t,reserved_FCB);

	/*If the reservation is not successfull exit.
	*/
	if(reservation_complete!=1){
		return -1;
	}

	/*Allocating space for the new struct pipe_cb(see pipe.h).
	*/
	pipe_cb* new_pipe_cb = xmalloc(sizeof(pipe_cb));

	/*Intializing the varriables for each member of the struct.
	*/
	pipe->read = reserved_Fid_t[0];
	pipe->write = reserved_Fid_t[1];

	new_pipe_cb->reader = reserved_FCB[0];
	new_pipe_cb->writer = reserved_FCB[1];
	new_pipe_cb->has_space = COND_INIT;
	new_pipe_cb->has_data = COND_INIT;
	new_pipe_cb->w_position = 0;
	new_pipe_cb->r_position = 0;

	/*We are connecting the reserved_FCBs with the new_pipe_cb  
	and we are using the file ops in order to enable the required function calls.
	*/
	reserved_FCB[0]->streamobj = new_pipe_cb;
	reserved_FCB[0]->streamfunc = &reader_file_ops;
	reserved_FCB[1]->streamobj = new_pipe_cb;
	reserved_FCB[1]->streamfunc = &writer_file_ops;

	return 0;
}

/*Error function for when using read if the pipe is set-up for write-mode.
*/
int error_read(void* streamobj, char *buf, unsigned int size){
	return -1;
}

/*Error function for when using write if the pipe is set-up for read-mode.
*/
int error_write(void* streamobj, const char *buf, unsigned int size){
	return -1;
}

/*Function used for actually reading the data inside the pipe.
*/
int pipe_read(void* streamobj, char *buf, unsigned int size){

	/*Casting the void* streamobj into a pipe_cb*.
	*/
	pipe_cb* cur_pipe_cb = (pipe_cb*) streamobj;

	if(cur_pipe_cb == NULL){
		return -1;
	}

	/*Getting the current read position inside the buffer.
	Count is used in order to count how many bytes are being read.
	*/

	int cur_read = cur_pipe_cb->r_position;
	int count = 0;

	/*While the pipe is empty and the write end is closed sleep.
	*/
	while(isEmpty(cur_pipe_cb) && cur_pipe_cb->writer != NULL){	
		kernel_wait(&(cur_pipe_cb->has_data), SCHED_PIPE);
	}

	/*When the kernel wakes up if the pipe is still empty and the write end is NULL(=closed)
	this means that no data are avaible to read and won't be(obviously because the write end is closed).
	Because we read 0 chars we return 0.
	*/
	if(isEmpty(cur_pipe_cb) && cur_pipe_cb->writer == NULL){
		return 0;
	}

	/*While the pipe is not empty traverse it.
	*/
	while(!isEmpty(cur_pipe_cb)){

		/*If count == size this means that we read exactly as many characters as we can fit
		inside the buf buffer.Return.*/
		if(count == size){

			/*Also broadcasting the anyone waiting for the pipe to empty that there's some space 
			free.
			*/
			kernel_broadcast(&(cur_pipe_cb->has_space));
			return count;
		}

		/*Increasing the cur_read by 1 mod PIPE_BUFFER_SIZE(the mod part is because the buffer is circular).
		Also copying the contents of the pipe inside the buf and increasing the count of both. 
		The read_write_count varriable is used to determine whether the buffer is full or not(Check isFull() and isEmpty() functiions).
		*/
		cur_read = (cur_read + 1) % PIPE_BUFFER_SIZE;
		buf[count] = cur_pipe_cb->BUFFER[cur_read];
		cur_pipe_cb->r_position = cur_read;
		count++;
		cur_pipe_cb->read_write_count--;

	}

	/*We reached this point. This means that the pipe is empty. Broadcasting that the pipe has space 
	to anyone waiting for it.
	*/
	kernel_broadcast(&(cur_pipe_cb->has_space));

	/*Returning the number of bytes read.
	*/
	return count;
}

/*Function used for actually writing the data inside the pipe.
*/
int pipe_write(void* streamobj, const char *buf, unsigned int size){

	/*Casting the void* streamobj into a pipe_cb*.
	*/
	pipe_cb* cur_pipe_cb = (pipe_cb*) streamobj;

	if(cur_pipe_cb == NULL){
		return -1;
	}

	/*Getting the current write position inside the buffer.
	Count is used in order to count how many bytes are being read.
	*/
	int cur_write = cur_pipe_cb->w_position;
	int count = 0;

	/*While the pipe is full and the read end is closed sleep.
	*/
	while(isFull(cur_pipe_cb) && cur_pipe_cb->reader != NULL){	
		kernel_wait(&(cur_pipe_cb->has_space), SCHED_PIPE);
	}

	/*When the kernel wakes up if either of the read and write ends 
	is closed return -1.
	*/
	if(cur_pipe_cb->reader == NULL || cur_pipe_cb->writer == NULL){
		return -1;
	}

	/*While the pipe is not full traverse it.
	*/
	while(!isFull(cur_pipe_cb)){

		/*If count == size this means that we wrote exactly as many characters as there are
		inside the buf buffer.Return.*/
		if(count == size){
			/*Also broadcasting to anyone waiting for the pipe to get data that there's some data
			inside it.
			*/
			kernel_broadcast(&(cur_pipe_cb->has_data));
			return count;
		}
		/*Increasing the cur_write by 1 mod PIPE_BUFFER_SIZE(the mod part is because the buffer is circular).
		Also writing the contents from the pupe inside the buf and increasing the count of both. 
		The read_write_count varriable is used to determine whether the buffer is full or not(Check isFull() and isEmpty() functiions).
		*/
		cur_write = (cur_write + 1) % PIPE_BUFFER_SIZE;
		cur_pipe_cb->BUFFER[cur_write] = buf[count];
		cur_pipe_cb->w_position = cur_write;
		count++;
		cur_pipe_cb->read_write_count++;

	}

	/*We reached this point. This means that the pipe is full. Broadcasting that the pipe has data 
	to anyone waiting for it.
	*/
	kernel_broadcast(&(cur_pipe_cb->has_data));

	return count;
}

/*This function is used in order to close the read end of the pipe.
*/
int pipe_reader_close(void* streamobj){

	/*Casting the void* streamobj into a pipe_cb*.
	*/
	pipe_cb* cur_pipe_cb = (pipe_cb*) streamobj;

	if(cur_pipe_cb == NULL){
		return -1;
	}

	/*Making the read end NULL(=closing it)
	*/
	cur_pipe_cb->reader = NULL;

	/*If the write end is NULL as well we dont need the pipe. Let's free it.
	*/
	if(cur_pipe_cb->writer == NULL){
		free(cur_pipe_cb);
	}

	return 0;
}

/*This function is used in order to close the write end of the pipe.
*/
int pipe_writer_close(void* streamobj){

	/*Casting the void* streamobj into a pipe_cb*.
	*/
	pipe_cb* cur_pipe_cb = (pipe_cb*) streamobj;

	if(cur_pipe_cb == NULL){
		return -1;
	}

	/*Making the read end NULL(=closing it)
	*/
	cur_pipe_cb->writer = NULL;
	
	/*If the read end is NULL as well we dont need the pipe. Let's free it.
	*/
	if(cur_pipe_cb->reader == NULL){
		free(cur_pipe_cb);
	}

	return 0;
}

/*Function to check whether the pipe is full or not. It's using the read_write_varriable
inside pipe_cb besides that it's quite simple and doesn't need explanation.
*/
int isFull(pipe_cb* pipe){
	if(pipe->read_write_count == PIPE_BUFFER_SIZE){
		return 1;
	}else{
		return 0;
	}
}

/*Function to check whether the pipe is empty or not. It's using the read_write_varriable
inside pipe_cb besides that it's quite simple and doesn't need explanation.
*/
int isEmpty(pipe_cb* pipe){
	if(pipe->read_write_count == 0){
		return 1;
	}else{
		return 0;
	}
}