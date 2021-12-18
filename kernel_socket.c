
#include "kernel_socket.h"
#include "kernel_sched.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

socket_cb* PORT_MAP[MAX_PORT+1] = { NULL };

static file_ops socket_file_ops = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

Fid_t sys_Socket(port_t port)
{

	if(illegal_port(port)){
		return NOFILE;
	}

	Fid_t reserved_Fid_t;
	FCB* reserved_FCB;

	int reservation_complete = FCB_reserve(1,&reserved_Fid_t,&reserved_FCB);

	if(reservation_complete!=1){
		return NOFILE;
	}

	socket_cb* new_scb = xmalloc(sizeof(socket_cb));

	if(new_scb == NULL){
		FCB_unreserve(1,&reserved_Fid_t,&reserved_FCB);
		return NOFILE;
	}

	new_scb->refcount = 0;
	new_scb->fcb = reserved_FCB;
	new_scb->type = SOCKET_UNBOUND;
	new_scb->port = port;

	reserved_FCB->streamobj = new_scb;
	reserved_FCB->streamfunc = &socket_file_ops;

	return reserved_Fid_t;
}

 	// 	-the file id is not legal
		// - the socket is not bound to a port
		// - the port bound to the socket is occupied by another listener
		// - the socket has already been initialized

int sys_Listen(Fid_t sock)
{
	FCB* listener_fcb = get_fcb(sock);

	if(listener_fcb == NULL){
		return NOFILE;
	}

	socket_cb* listener_scb = (socket_cb *) listener_fcb->streamobj;

	if(listener_scb == NULL){
		return NOFILE;
	}

	//
	if(listener_scb->type != SOCKET_UNBOUND || listener_scb->port == NOPORT){
		return NOFILE;
	}

	//An einai occupied to portmap
	if(illegal_port(listener_scb->port) || is_in_portmap(listener_scb->port))
		return NOFILE;

	PORT_MAP[listener_scb->port] = listener_scb;

	if(illegal_port(listener_scb->port)){
		return NOFILE;
	}

	listener_scb->type = SOCKET_LISTENER;

	rlnode_init(&listener_scb->listener_s.queue, NULL);
	listener_scb->listener_s.req_available = COND_INIT;

	return 0;
}
Fid_t sys_Accept(Fid_t lsock)
{
	
	FCB* lsock_fcb = get_fcb(lsock);

	if(lsock_fcb == NULL){
		return NOFILE;
	}

	socket_cb* lsock_scb = (socket_cb *) lsock_fcb->streamobj;

	if(lsock_scb == NULL){
		return NOFILE;
	}

	if(!is_in_portmap(lsock_scb->port) || lsock_scb->type != SOCKET_LISTENER){
		return NOFILE;
	}

	lsock_scb->refcount++;


	fprintf(stderr,"yep\n");
	while(is_rlist_empty(&(lsock_scb->listener_s.queue)) /* && !is_in_portmap(lsock_scb->port)*/){
		fprintf(stderr,"listener waiting for request\n");
		kernel_wait(&(lsock_scb->listener_s.req_available), SCHED_IO);
	}

	if(!is_in_portmap(lsock_scb->port)){
		return NOFILE;
	}

	if(is_rlist_empty(&(lsock_scb->listener_s.queue))){
		return NOFILE;
	}

	connection_request* admitted_request = (rlist_pop_front(&(lsock_scb->listener_s.queue)))->con_req;


	fprintf(stderr,"request admitted\n");
	socket_cb* admitted_peer = admitted_request->peer;

	if(admitted_peer->type != SOCKET_UNBOUND){
		return NOFILE;
	}

	admitted_request->admitted = 1;
	admitted_peer->type = SOCKET_PEER;


	fprintf(stderr, "before new socket\n");

	Fid_t new_peer_fidt = sys_Socket(lsock_scb->port); //Na to rwtisw.

	fprintf(stderr, "after new socket\n");

	if(new_peer_fidt == NOFILE){
		fprintf(stderr, "before return\n");
		return NOFILE;
	}


	fprintf(stderr, "reservation_complete");

	FCB* new_peer_fcb = get_fcb(new_peer_fidt);
	socket_cb* new_peer_scb = (socket_cb *) new_peer_fcb->streamobj;
	new_peer_scb->type = SOCKET_PEER;

	new_peer_scb->peer_s.peer = admitted_peer;
	admitted_peer->peer_s.peer = new_peer_scb;

	//Creating the first pipe.
	pipe_t pipe_t_1;
	pipe_t_1.write = lsock;
	pipe_t_1.read = new_peer_fidt;

	pipe_cb* pipe_cb_1 = xmalloc(sizeof(pipe_cb));
	
	if(pipe_cb_1 == NULL){
		return NOFILE;
	}

	pipe_cb_1->writer = lsock_fcb;
	pipe_cb_1->reader = new_peer_fcb;
	pipe_cb_1->has_space = COND_INIT;
	pipe_cb_1->has_data = COND_INIT;
	pipe_cb_1->w_position = 0;
	pipe_cb_1->r_position = 0;

	//Creating the second pipe.
	pipe_t pipe_t_2;
	pipe_t_2.write = new_peer_fidt;
	pipe_t_2.read = lsock;

	pipe_cb* pipe_cb_2 = xmalloc(sizeof(pipe_cb));

	if(pipe_cb_2 == NULL){
		return NOFILE;
	}

	pipe_cb_2->writer = new_peer_fcb;
	pipe_cb_2->reader = lsock_fcb;
	pipe_cb_2->has_space = COND_INIT;
	pipe_cb_2->has_data = COND_INIT;
	pipe_cb_2->w_position = 0;
	pipe_cb_2->r_position = 0;

	admitted_peer->peer_s.write_pipe = pipe_cb_1;
	admitted_peer->peer_s.read_pipe = pipe_cb_2;

	kernel_signal(&(admitted_request->connected_cv));

	lsock_scb->refcount--;

	fprintf(stderr,"Accept returned\n");

	return new_peer_fidt;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{

	FCB* peer_fcb = get_fcb(sock);

	if(peer_fcb == NULL || illegal_port(port)){
		return NOFILE;
	}

	peer_fcb->refcount++;

	connection_request* request = xmalloc(sizeof(connection_request));


	if(request == NULL){
		return NOFILE;
	}
	
	fprintf(stderr,"dadada\n");

	request->admitted = 0;
	request->peer = peer_fcb->streamobj;
	request->connected_cv = COND_INIT;

	rlnode_init(&request->queue_node, request);
	rlist_push_back(&(PORT_MAP[port]->listener_s.queue), &request->queue_node);

	kernel_signal(&(PORT_MAP[port]->listener_s.req_available));

	while(request->admitted!=1){
		kernel_timedwait(&(request->connected_cv), SCHED_IO, timeout);
		break;
	}

	peer_fcb->refcount--;

	if(request->admitted!=1 || ......){
		return NOFILE;
	}

	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	FCB* peer_fcb = get_fcb(sock);

	if(peer_fcb == NULL){
		return NOFILE;
	}

	socket_cb* peer_scb = (socket_cb *) peer_fcb->streamobj;

	if(peer_scb == NULL){
		return NOFILE;
	}

	switch(how){
	case SHUTDOWN_READ:
		pipe_reader_close(peer_scb->peer_s.read_pipe);
		break;
	case SHUTDOWN_WRITE:
		pipe_writer_close(peer_scb->peer_s.write_pipe);
		break;

	case SHUTDOWN_BOTH:
		pipe_reader_close(peer_scb->peer_s.read_pipe);
		pipe_writer_close(peer_scb->peer_s.write_pipe);
		break;

	default:

	return 0;

	}
}

int socket_read(void* streamobj, char* buf, int size){

	socket_cb* socket_scb = (socket_cb*) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}

	if(socket_scb->type == SOCKET_PEER && socket_scb->peer_s.read_pipe != NULL)
		return pipe_read(socket_scb->peer_s.read_pipe,buf,size);
	else
		return NOFILE;
}

int socket_write(void* streamobj, const char* buf, int size){

	socket_cb* socket_scb = (socket_cb*) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}

	if(socket_scb->type == SOCKET_PEER && socket_scb->peer_s.write_pipe != NULL)
		return pipe_write(socket_scb->peer_s.write_pipe,buf,size);
	else
		return NOFILE;
}
int socket_close(void* streamobj){

	socket_cb* socket_scb = (socket_cb* ) streamobj;

	if(socket_scb == NULL){
		return NOFILE;
	}
	
	//added this if
	if(socket_scb->refcount == 0){
		if(socket_scb->type != SOCKET_LISTENER){
		free(socket_scb);
		}else{
			PORT_MAP[is_in_portmap(socket_scb->port)] = NULL;
			free(socket_scb);
		}
	}

	//return NOFILE WHY ?
	return 0;
}

int illegal_port(port_t port){
	if(port < NOPORT || port > MAX_PORT){
		return 1;
	}else{
		return 0;
	}
}

int is_in_portmap(port_t test_port){

	for(int i=1; i<MAX_PORT; i++){
		socket_cb* portmap_scb = PORT_MAP[i];

		if( (portmap_scb != NULL) && (portmap_scb->port == test_port) ){
			return i;
		}
	}

	return 0;
}	


/**
Comment in kernel_sched.h for the new SCHED_LISTENER;
Comment in pipes;
change variable names in first 2 functions;
take a look at kernel_socket unbound for the struct and in sys_Socket	
ta malloc sta pipes
*/