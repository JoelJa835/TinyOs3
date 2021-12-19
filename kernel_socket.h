
#include "kernel_pipe.h"
#include "util.h"




int socket_read(void* streamobj, char* buf, int size);
int socket_write(void* streamobj, const char* buf, int size);
int socket_close(void* streamobj);

typedef enum socket_type socket_t;
typedef struct listener_socket_struct_type listener_socket;
typedef struct unbound_socket_struct_type unbound_socket;
typedef struct peer_socket_type peer_socket;
typedef struct socket_control_block socket_cb;
typedef struct connection_request_type connection_request;
int illegal_port(port_t port);
int is_in_portmap(port_t test_port);

typedef struct connection_request_type {
	int admitted;
	Fid_t peer_fidt;
	socket_cb* peer;
	CondVar connected_cv;
	rlnode queue_node;
}connection_request;

typedef enum socket_type{
	SOCKET_LISTENER=1,
	SOCKET_UNBOUND=2,
	SOCKET_PEER=3
}socket_t;

typedef struct listener_socket_struct_type{
	rlnode queue;
	CondVar req_available;
}listener_socket;

typedef struct unbound_socket_struct_type{
	rlnode unbound_socket;
}unbound_socket;

typedef struct peer_socket_type{
	socket_cb* peer;
	pipe_cb* write_pipe;
	pipe_cb* read_pipe;
}peer_socket;

typedef struct socket_control_block
{
	uint refcount;
	FCB* fcb;
	socket_t type;
	port_t port;

	union{
		listener_socket listener_s;
		unbound_socket unbound_s;
		peer_socket peer_s;
	}

}socket_cb;