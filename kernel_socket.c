
#include "kernel_socket.h"
#include "kernel_sched.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

socket_cb* PORT_MAP[MAX_PORT+1] = { NULL };

/*File_ops in order to use Read,Write,Close and Open functions when using a socket.
*/
static file_ops socket_file_ops = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

/*The function that's used to create a Socket on a specific port.
*/
Fid_t sys_Socket(port_t port)
{

	if(port < NOPORT || port > MAX_PORT){
		return NOFILE;
	}

	/*The FCB and Fid_t that are used in order to create the socket.
	*/
	Fid_t reserved_Fid_t;
	FCB* reserved_FCB;

	/*Making a reservation of a FCB and a Fid_t.
	The FCB reserve function returns 1 if the reservation is successfull.
	*/
	int reservation_complete = FCB_reserve(1,&reserved_Fid_t,&reserved_FCB);

	/*If the reservation is not successfull exit.
	*/
	if(reservation_complete!=1){
		return NOFILE;
	}

	/*Allocating space for the socket control block.
	*/
	socket_cb* new_scb = xmalloc(sizeof(socket_cb));

	if(new_scb == NULL){
		FCB_unreserve(1,&reserved_Fid_t,&reserved_FCB);
		return NOFILE;
	}

	/*Intializing the member varriables of the socket_cb struckt.
	*/
	new_scb->refcount = 0;
	new_scb->fcb = reserved_FCB;
	new_scb->type = SOCKET_UNBOUND;
	new_scb->port = port;

	/*We are connecting the reserved_FCBs with the new_scb
	and we are using the file ops in order to enable the required function calls.
	*/
	reserved_FCB->streamobj = new_scb;
	reserved_FCB->streamfunc = &socket_file_ops;

	return reserved_Fid_t;
}

/*The function used to make a non-listening socket a listener.
*/

int sys_Listen(Fid_t sock)
{	
	/*Getting the fcb of the given Fid_t.
	*/
	FCB* listener_fcb = get_fcb(sock);

	if(listener_fcb == NULL){
		return NOFILE;
	}

	/*Getting the streamobj of the fcb and casting it a socket_cb.
	*/
	socket_cb* listener_scb = (socket_cb *) listener_fcb->streamobj;

	if(listener_scb == NULL){
		return NOFILE;
	}

	/*The socket can't be a listener if it's not unbound(that means it's peered or already a listener)
	or if it's port equals NOPORT(=0);
	*/
	if(listener_scb->type != SOCKET_UNBOUND || listener_scb->port == NOPORT){
		return NOFILE;
	}

	/*Checking if the port is within the legal limits or if the socket we are trying to make a listener is already 
	a listener(if it is that means it's inside the portmap).
	*/
	if(illegal_port(listener_scb->port) || is_in_portmap(listener_scb->port)){
		return NOFILE;
	}

	/*Adding the socket to the port map.
	*/

	PORT_MAP[listener_scb->port] = listener_scb;

	// if(illegal_port(listener_scb->port)){
	// 	return NOFILE;
	// }

	/*Making the socket type a listener.
	*/
	listener_scb->type = SOCKET_LISTENER;

	/*Initializing the varriables needed in order for a socket to become a listener.
	*/
	rlnode_init(&listener_scb->listener_s.queue, NULL);
	listener_scb->listener_s.req_available = COND_INIT;

	return 0;
}
/*A function that's used for the listening socket to accept a connection a honor it. The arguement 
that's passed is the listening socket.
*/
Fid_t sys_Accept(Fid_t lsock)
{

	/*Getting the fcb of the given Fid_t.
	*/
	FCB* lsock_fcb = get_fcb(lsock);

	if(lsock_fcb == NULL){
		return NOFILE;
	}

	/*Getting the streamobj of the fcb and casting it a socket_cb.
	*/
	socket_cb* lsock_scb = (socket_cb *) lsock_fcb->streamobj;

	if(lsock_scb == NULL){
		return NOFILE;
	}

	/*If the socket that's given is not a listener or it is not inside the portmap
	return NOFILE.
	*/
	if(!is_in_portmap(lsock_scb->port) || lsock_scb->type != SOCKET_LISTENER){
		return NOFILE;
	}

	/*We are using the listening socket, thus we are increasing the reference count by 1.
	*/
	lsock_scb->refcount++;

	/*While the request queue is empty sleep.
	*/
	while(is_rlist_empty(&(lsock_scb->listener_s.queue))){
		kernel_wait(&(lsock_scb->listener_s.req_available), SCHED_IO);
	}

	/*When you wake up if the listening socket is removed from the port map return NOFILE.
	*/
	if(!is_in_portmap(lsock_scb->port)){
		return NOFILE;
	}

	/*When you wake up if the listener queue is empty return NOFILE.
	*/
	if(is_rlist_empty(&(lsock_scb->listener_s.queue))){
		return NOFILE;
	}

	/*Pop the first request from the list.
	*/
	connection_request* admitted_request = (rlist_pop_front(&(lsock_scb->listener_s.queue)))->con_req;

	/*Get the socket that made the request to connect.
	*/	
	socket_cb* admitted_peer = admitted_request->peer;

	/*If the socket that made the request is not unbound return NOFILE.
	*/
	if(admitted_peer->type != SOCKET_UNBOUND){
		return NOFILE;
	}

	/*Create a new socket in order to peer it with the one that made the request.
	*/
	Fid_t new_peer_fidt = sys_Socket(lsock_scb->port);

	/*If the new socket creation is unsuccessfull return NOFILE.
	*/
	if(new_peer_fidt == NOFILE){
		return NOFILE;
	}

	/*Make the request admited, and the requesting socket's type SOCKET_PEER.
	*/
	admitted_request->admitted = 1;
	admitted_peer->type = SOCKET_PEER;

	/*Getting the FCB of the new socket.
	*/
	FCB* new_peer_fcb = get_fcb(new_peer_fidt);
	/*Getting the streamobj of the fcb and casting it a socket_cb.
	*/
	socket_cb* new_peer_scb = (socket_cb *) new_peer_fcb->streamobj;
	/*Making the new socket's type peer
	*/
	new_peer_scb->type = SOCKET_PEER;

	/*Connecting the new socket and the requesting socket together.
	*/
	new_peer_scb->peer_s.peer = admitted_peer;
	admitted_peer->peer_s.peer = new_peer_scb;

	/*Getting the fid_t and the fcb of the requesting socket.
	*/
	Fid_t admitted_peer_fidt = admitted_request->peer_fidt;
	FCB* admitted_peer_fcb = admitted_request->peer->fcb;

	/*Now all that's remaining is to create pipes in order to connect the
	two sockets together.2 pipes needed to do this. You can do that also
	with the sys_Pipe() function but the sys_Pipe() function is not used in order to save Fid_t's
	*/
	pipe_t pipe_t_1;
	pipe_t_1.write = new_peer_fidt;
	pipe_t_1.read = admitted_peer_fidt;

	/*Allocating space for the first pipe's cb.
	*/
	pipe_cb* pipe_cb_1 = xmalloc(sizeof(pipe_cb));
	
	if(pipe_cb_1 == NULL){
		return NOFILE;
	}

	/*Making the required initializations(check Sys_pipe() on kernel_pipe.c for more details).
	*/
	pipe_cb_1->writer = new_peer_fcb;
	pipe_cb_1->reader = admitted_peer_fcb;
	pipe_cb_1->has_space = COND_INIT;
	pipe_cb_1->has_data = COND_INIT;
	pipe_cb_1->w_position = 0;
	pipe_cb_1->r_position = 0;

	/*Doin the exact same for the second pipe.
	*/
	pipe_t pipe_t_2;
	pipe_t_2.write = admitted_peer_fidt;
	pipe_t_2.read = new_peer_fidt;

	pipe_cb* pipe_cb_2 = xmalloc(sizeof(pipe_cb));

	/*If creating the second pipe fails free the first one(we need 2 pipes for a socket).
	*/
	if(pipe_cb_2 == NULL){
		free(pipe_cb_1);
		return NOFILE;
	}

	pipe_cb_2->writer = admitted_peer_fcb;
	pipe_cb_2->reader = new_peer_fcb;
	pipe_cb_2->has_space = COND_INIT;
	pipe_cb_2->has_data = COND_INIT;
	pipe_cb_2->w_position = 0;
	pipe_cb_2->r_position = 0;

	/*After creating the 2 new pipes we need to connect them
	 with the two sockets. One pipe is used for 
	socket1(writing) --[pipe]--> socket2(reading) and the other for,
	socket1(reading) <--[pipe]-- socket2(writing).

	*/
	new_peer_scb->peer_s.write_pipe = pipe_cb_1;
	new_peer_scb->peer_s.read_pipe = pipe_cb_2;

	admitted_peer->peer_s.write_pipe = pipe_cb_2;
	admitted_peer->peer_s.read_pipe = pipe_cb_1;

	kernel_signal(&(admitted_request->connected_cv));

	lsock_scb->refcount--;

	return new_peer_fidt;
}

/*Function that's used for making a connection request to the given port.
*/
int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{

	/*Getting the fcb of the given Fid_t.
	*/
	FCB* peer_fcb = get_fcb(sock);

	if(peer_fcb == NULL){
		return NOFILE;
	}

	/*Getting the streamobj of the fcb and casting it a socket_cb.
	*/
	socket_cb* peer_scb = (socket_cb *) peer_fcb->streamobj;

	if(peer_fcb == NULL){
		return NOFILE;
	}

	/*If the listening port is illegal or if's not in portmap return NOFILE.
	*/
	if(illegal_port(port) || !is_in_portmap(port)){
		return NOFILE;
	}

	/*Increase the ref count by 1.
	*/
	peer_fcb->refcount++;

	/*If the socket that wants to connect is not unbound return NOFILE.
	*/
	if(peer_scb->type != SOCKET_UNBOUND){
		return NOFILE;
	}

	/*Allocating space for the new request.
	*/
	connection_request* request = xmalloc(sizeof(connection_request));

	if(request == NULL){
		return NOFILE;
	}

	/*Intializing the member variables of the request struct.
	*/
	request->admitted = 0;
	request->peer = peer_fcb->streamobj;
	request->connected_cv = COND_INIT;
	request->peer_fidt = sock;

	rlnode_init(&request->queue_node, request);

	/*Pushing the request we just made into the listener's queue.
	*/
	rlist_push_back(&(PORT_MAP[port]->listener_s.queue), &request->queue_node);

	/*Also signaling to anyone sleeping that a new request is available.
	*/
	kernel_signal(&(PORT_MAP[port]->listener_s.req_available));

	/*While the request is not admitted sleep.
	*/
	while(request->admitted!=1){
		kernel_timedwait(&(request->connected_cv), SCHED_IO, timeout);
		break;
	}

	/*Decreasing ref count by 1.
	*/
	peer_fcb->refcount--;

	/*After you wake if the request is still not admitted or if the socket type is not SOCKET_PEER
	return NOFILE.
	*/
	if(request->admitted!=1 || (request->peer->type != SOCKET_PEER) ){
		return NOFILE;
	}

	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{	
	/*Getting the fcb of the given Fid_t.
	*/
	FCB* peer_fcb = get_fcb(sock);

	if(peer_fcb == NULL){
		return NOFILE;
	}

	/*Getting the streamobj of the fcb and casting it a socket_cb.
	*/
	socket_cb* peer_scb = (socket_cb *) peer_fcb->streamobj;

	if(peer_scb == NULL){
		return NOFILE;
	}

	/*Shutdown can only be used if the socket is peered. Else return NOFILE.
	*/
	if(peer_scb->type == SOCKET_PEER){
		switch(how){
			/*Shutting down read.
			*/
			case SHUTDOWN_READ:
				if(peer_scb->peer_s.read_pipe != NULL){
					pipe_reader_close(peer_scb->peer_s.read_pipe);
				}
				peer_scb->peer_s.read_pipe = NULL;
				break;
			/*Shutting down write.
			*/
			case SHUTDOWN_WRITE:
				if(peer_scb->peer_s.write_pipe != NULL){
					pipe_writer_close(peer_scb->peer_s.write_pipe);
				}
				peer_scb->peer_s.write_pipe = NULL;
				break;
			/*Shutting down both.
			*/
			case SHUTDOWN_BOTH:
				if(peer_scb->peer_s.write_pipe != NULL){
					pipe_writer_close(peer_scb->peer_s.write_pipe);
				}
				if(peer_scb->peer_s.read_pipe != NULL){
					pipe_reader_close(peer_scb->peer_s.read_pipe);
				}
				peer_scb->peer_s.write_pipe = NULL;
				peer_scb->peer_s.read_pipe = NULL;
				break;
			/*We have only 3 modes, if the user of the function gives anything else for 
			input it's an error thus the function returns NOFILE.
			*/
			default:
				return NOFILE;
		}
	}else{
		return NOFILE;
	}
	return 0;
}

int socket_read(void* streamobj, char* buf, int size){

	/*Casting the void* streamobj to socket_cb*.
	*/
	socket_cb* socket_scb = (socket_cb*) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}

	/*If the socket is type is SOCKET_PEER and the read end is not closed read from the pipe.
	*/
	if(socket_scb->type == SOCKET_PEER && socket_scb->peer_s.read_pipe != NULL){
		return pipe_read(socket_scb->peer_s.read_pipe,buf,size);;
	}else{
		return NOFILE;
	}
}

int socket_write(void* streamobj, const char* buf, int size){

	/*Casting the void* streamobj to socket_cb*.
	*/
	socket_cb* socket_scb = (socket_cb*) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}

	/*If the socket is type is SOCKET_PEER and the write end is not closed write to the pipe.
	*/
	if(socket_scb->type == SOCKET_PEER && socket_scb->peer_s.write_pipe != NULL){
		return pipe_write(socket_scb->peer_s.write_pipe,buf,size);
	}
	else{
		return NOFILE;
	}
}

int socket_close(void* streamobj){

	/*Casting the void* streamobj to socket_cb*.
	*/
	socket_cb* socket_scb = (socket_cb* ) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}
	
	// /*Only if the refcount is 0 you can close the socket.
	// */
	// if(socket_scb->refcount == 0){
	// 	if(socket_scb->type != SOCKET_LISTENER){
	// 		if(socket_scb->type == SOCKET_PEER){
	// 			pipe_reader_close(socket_scb->peer_s.read_pipe);
	// 			pipe_writer_close(socket_scb->peer_s.write_pipe);
	// 			socket_scb->peer_s.read_pipe = NULL;
	// 			socket_scb->peer_s.write_pipe = NULL;
	// 		}
	// 		free(socket_scb);
	// 	}else{
	// 		PORT_MAP[is_in_portmap(socket_scb->port)] = NULL;
	// 		free(socket_scb);
	// 	}
	// }

	/*Only if the refcount is 0 you can close the socket.
	*/
	if(socket_scb->refcount == 0){
	 	if(socket_scb->type == SOCKET_UNBOUND){
	 		free(socket_scb);
	 	}else if(socket_scb->type == SOCKET_PEER){
	 		pipe_reader_close(socket_scb->peer_s.read_pipe);
	 		pipe_writer_close(socket_scb->peer_s.write_pipe);
	 		socket_scb->peer_s.read_pipe = NULL;
	 		socket_scb->peer_s.write_pipe = NULL;
	 		free(socket_scb);
	 	}else if(socket_scb->type == SOCKET_LISTENER){
	 		PORT_MAP[is_in_portmap(socket_scb->port)] = NULL;
	 		free(socket_scb);
	 	}else{
	 		return NOFILE;
		}
	}

	return 0;
}

/*Function to check if the given port is within the legal limits.
*/
int illegal_port(port_t port){

	if(port <= NOPORT || port > MAX_PORT){
		return 1;
	}else{
		return 0;
	}
}

/*Function to check if the given port is inside the port map.
*/
int is_in_portmap(port_t test_port){

	for(int i=1; i<MAX_PORT; i++){
		socket_cb* portmap_scb = PORT_MAP[i];

		if( (portmap_scb != NULL) && (portmap_scb->port == test_port) ){
			return i;
		}
	}

	return 0;
}	