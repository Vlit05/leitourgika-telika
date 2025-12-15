#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_pipe.h" 
#include <stdlib.h>
#include <string.h>
#include <assert.h>


typedef enum {
    SOCKET_UNBOUND,
    SOCKET_LISTENER,
    SOCKET_PEER
} socket_type;

struct socket_control_block;


typedef struct connection_request {
    int admitted;                           
    struct socket_control_block* peer;      
    CondVar connected_cv;                   
    rlnode queue_node;                      
} connection_request;


typedef struct unbound_socket {
    rlnode unbound_socket; 
} unbound_socket;


typedef struct listener_socket {
    rlnode queue;                           
    CondVar req_available;                  
} listener_socket;


typedef struct peer_socket {
    struct socket_control_block* peer;      
    pipe_cb* write_pipe;                     
    pipe_cb* read_pipe;                      
} peer_socket;


typedef struct socket_control_block {
    uint refcount;
    FCB* fcb;
    socket_type type;
    port_t port;  

    union {
        unbound_socket  unbound_s;  
        listener_socket listener_s;
        peer_socket     peer_s;
    };
} socket_cb;


static socket_cb* PORT_MAP[MAX_PORT+1];
static Mutex port_map_lock = MUTEX_INIT;


static file_ops socket_ops;


int socket_read(void* obj, char* buf, unsigned int size) {
    socket_cb* sock = (socket_cb*)obj;
    if (sock->type != SOCKET_PEER || sock->peer_s.read_pipe == NULL) 
        return -1;
    return pipe_read(sock->peer_s.read_pipe, buf, size);
}

int socket_write(void* obj, const char* buf, unsigned int size) {
    socket_cb* sock = (socket_cb*)obj;
    if (sock->type != SOCKET_PEER || sock->peer_s.write_pipe == NULL) 
        return -1;
    return pipe_write(sock->peer_s.write_pipe, buf, size);
}

int socket_close(void* obj) {
    socket_cb* sock = (socket_cb*)obj;

    
    if (sock->type == SOCKET_LISTENER) {
        Mutex_Lock(&port_map_lock);
        
        if (sock->port != NOPORT && PORT_MAP[sock->port] == sock) { //remove the current socket from port_map
            PORT_MAP[sock->port] = NULL;
        }

        
        sock->type = SOCKET_UNBOUND;
       
        rlnode_init(&sock->unbound_s.unbound_socket, sock);
        
        kernel_broadcast(&sock->listener_s.req_available);

        Mutex_Unlock(&port_map_lock);
    }

    sock->refcount--; // -1 Socket 
    if (sock->refcount > 0) 
        return 0;

    
    if (sock->type == SOCKET_PEER) {
        if (sock->peer_s.write_pipe)
            pipe_close(sock->peer_s.write_pipe, 1);
        if (sock->peer_s.read_pipe)
            pipe_close(sock->peer_s.read_pipe, 0);
    }

    free(sock);
    return 0;
}

static file_ops socket_ops = {
    .Open = NULL,
    .Read = socket_read,
    .Write = socket_write,
    .Close = socket_close
};



Fid_t sys_Socket(port_t port)
{
    Fid_t fid;
    FCB* fcb;
    socket_cb* scb;

   
    if (FCB_reserve(1, &fid, &fcb) == 0) 
        return NOFILE;

    
    if (port != NOPORT && (port < 0 || port > MAX_PORT)) {
        FCB_unreserve(1, &fid, &fcb);
        return NOFILE;
    }

   

    scb = (socket_cb*)xmalloc(sizeof(socket_cb));
    if (!scb) {
        FCB_unreserve(1, &fid, &fcb);
        return NOFILE;
    }

    memset(scb, 0, sizeof(socket_cb)); //
    
    //create socket as unbound 
    scb->type = SOCKET_UNBOUND;
    scb->refcount = 1;
    scb->fcb = fcb;
    scb->port = port; 
    
    rlnode_init(&scb->unbound_s.unbound_socket, scb);

    fcb->streamobj = scb;
    fcb->streamfunc = &socket_ops;

    return fid;
}

int sys_Listen(Fid_t sock)
{
    FCB* fcb = get_fcb(sock);
    if (!fcb) 
        return -1;
    
    if (fcb->streamfunc != &socket_ops) 
        return -1;

    socket_cb* scb = (socket_cb*)fcb->streamobj;

   
    if (scb->type != SOCKET_UNBOUND) 
        return -1;

    
    if (scb->port == NOPORT) 
        return -1;

    
    Mutex_Lock(&port_map_lock);
    
    if (PORT_MAP[scb->port] != NULL) {
        Mutex_Unlock(&port_map_lock);
        return -1; // Listener exists already 
    }

    
    PORT_MAP[scb->port] = scb;
    scb->type = SOCKET_LISTENER;
    
    Mutex_Unlock(&port_map_lock);

    
    rlnode_init(&scb->listener_s.queue, NULL);
    scb->listener_s.req_available = COND_INIT;

    return 0;
}



int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
    FCB* fcb = get_fcb(sock);
    
    if (!fcb) 
        return -1;


    if (fcb->streamfunc != &socket_ops) 
        return -1;


    socket_cb* client_scb = (socket_cb*)fcb->streamobj;

    if (client_scb->type != SOCKET_UNBOUND) 
        return -1;
    
    if (port < 0 || port > MAX_PORT) 
        return -1;

    
    Mutex_Lock(&port_map_lock);
    socket_cb* listener = PORT_MAP[port];
    
    if (listener == NULL || listener->type != SOCKET_LISTENER) {
        Mutex_Unlock(&port_map_lock);
        return -1;
    }
    Mutex_Unlock(&port_map_lock);

    client_scb->refcount++; 

    connection_request req;
    req.admitted = 0;
    req.peer = client_scb;
    req.connected_cv = COND_INIT;
    rlnode_init(&req.queue_node, &req);

    rlist_push_back(&listener->listener_s.queue, &req.queue_node);
    kernel_signal(&listener->listener_s.req_available);

    int ret = 0;
    while (!req.admitted) {
        if (timeout == 0) {
             kernel_wait(&req.connected_cv, SCHED_PIPE);
        } else {
             int w = kernel_timedwait(&req.connected_cv, SCHED_PIPE, timeout);
             if (w == 0 && !req.admitted) {
                 rlist_remove(&req.queue_node);
                 ret = -1;
                 goto cleanup;
             }
        }
    }
    
    client_scb->type = SOCKET_PEER;

cleanup:
    client_scb->refcount--;
    return ret;
}

Fid_t sys_Accept(Fid_t lsock)
{
    FCB* lfcb = get_fcb(lsock);
    
    if (!lfcb) 
        return NOFILE;
    

    if (lfcb->streamfunc != &socket_ops) 
        return NOFILE;

    socket_cb* listener = (socket_cb*)lfcb->streamobj;
    

    if (listener->type != SOCKET_LISTENER) 
        return NOFILE;

    listener->refcount++;

    while (is_rlist_empty(&listener->listener_s.queue)) {
        kernel_wait(&listener->listener_s.req_available, SCHED_PIPE);
        
        if (listener->type != SOCKET_LISTENER) {
            listener->refcount--;
            return NOFILE;
        }
    }

    rlnode* node = rlist_pop_front(&listener->listener_s.queue);//vrisko proto request sto queue
    connection_request* req = (connection_request*)node->obj;

    Fid_t newfid;
    FCB* newfcb;
    if (FCB_reserve(1, &newfid, &newfcb) == 0) {
        listener->refcount--;
        return NOFILE; 
    }

    socket_cb* newsock = (socket_cb*)xmalloc(sizeof(socket_cb));
    if (!newsock) {
        FCB_unreserve(1, &newfid, &newfcb);
        listener->refcount--;
        return NOFILE;
    }

    memset(newsock, 0, sizeof(socket_cb));
    newsock->refcount = 1;
    newsock->type = SOCKET_PEER;
    newsock->fcb = newfcb;
    newsock->port = NOPORT; 

    socket_cb* client_sock = req->peer;

    pipe_cb* p1 = pipe_create();
    pipe_cb* p2 = pipe_create();
    
    if (!p1 || !p2) {
        if (p1) free(p1);
        if (p2) free(p2);
        free(newsock);
        FCB_unreserve(1, &newfid, &newfcb);
        listener->refcount--;
        return NOFILE;
    }

    p1->refcount = 2; 
    p2->refcount = 2;

    //Server
    newsock->peer_s.peer = client_sock;
    newsock->peer_s.read_pipe = p1;    
    newsock->peer_s.write_pipe = p2;   

    //Client 
    client_sock->peer_s.peer = newsock;
    client_sock->peer_s.read_pipe = p2;   
    client_sock->peer_s.write_pipe = p1; 

    newfcb->streamobj = newsock;
    newfcb->streamfunc = &socket_ops;

    req->admitted = 1;
    kernel_signal(&req->connected_cv);

    listener->refcount--;
    return newfid;
}

int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
    FCB* fcb = get_fcb(sock);
    
    if (!fcb) 
        return -1;
    
    if (fcb->streamfunc != &socket_ops) 
        return -1;

    socket_cb* scb = (socket_cb*)fcb->streamobj;
    
    
    if (scb->type != SOCKET_PEER) 
        return -1;

    switch (how) {
        case SHUTDOWN_READ:
            if (scb->peer_s.read_pipe) {
                pipe_close(scb->peer_s.read_pipe, 0);
                scb->peer_s.read_pipe = NULL;
            }
            break;

        case SHUTDOWN_WRITE:
            if (scb->peer_s.write_pipe) {
                pipe_close(scb->peer_s.write_pipe, 1);
                scb->peer_s.write_pipe = NULL;
            }
            break;

        case SHUTDOWN_BOTH:
            if (scb->peer_s.read_pipe) {
                pipe_close(scb->peer_s.read_pipe, 0);
                scb->peer_s.read_pipe = NULL;
            }
            if (scb->peer_s.write_pipe) {
                pipe_close(scb->peer_s.write_pipe, 1);
                scb->peer_s.write_pipe = NULL;
            }
            break;

        default:
            
            return -1;
    }

    return 0;
}