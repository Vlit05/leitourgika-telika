#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_pipe.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>


pipe_cb* pipe_create() //Sockets
{
    pipe_cb* pipe = (pipe_cb*)xmalloc(sizeof(pipe_cb));
    if (pipe == NULL) return NULL;

    memset(pipe, 0, sizeof(pipe_cb));
    pipe->w_position = 0;
    pipe->r_position = 0;
    pipe->count = 0;
    pipe->write_closed = 0;
    pipe->read_closed = 0;
    
    
    pipe->refcount = 0; 
    
    pipe->has_data = COND_INIT;
    pipe->has_space = COND_INIT;
    
    return pipe;
}


int pipe_read (pipe_cb* pipe, char* buf, unsigned int size)
{
    unsigned int bytes_read = 0;

    
    while (pipe->count == 0) {
        if (pipe->write_closed) {
            return 0; //EOF
        }
       
        kernel_wait(&pipe->has_data, SCHED_PIPE);
    }

    
    while (bytes_read < size && pipe->count > 0) {
        buf[bytes_read++] = pipe->buffer[pipe->r_position];
        pipe->r_position = (pipe->r_position + 1) % PIPE_BUFFER_SIZE;// otan %  0 gurizei sthn arxh 
        pipe->count--;
    }

    // jipname ta pcb 
    kernel_broadcast(&pipe->has_space);

    return bytes_read;
}


int pipe_write(pipe_cb* pipe, const char* buf, unsigned int size)
{
    unsigned int bytes_written = 0;

    
    if (pipe->read_closed) {
        return -1; 
    }

    while (bytes_written < size) {
        
        while (pipe->count == PIPE_BUFFER_SIZE) {
            if (pipe->read_closed) {
                return -1;
            }
            kernel_wait(&pipe->has_space, SCHED_PIPE);
        }

        pipe->buffer[pipe->w_position] = buf[bytes_written++];
        pipe->w_position = (pipe->w_position + 1) % PIPE_BUFFER_SIZE;
        pipe->count++;

        
        kernel_broadcast(&pipe->has_data);
    }

    return bytes_written;
}


int pipe_close (pipe_cb* pipe, int is_writer) // instead of 2 pipe close writer/reader 
{
    if (is_writer) {
        pipe->write_closed = 1;
        
        kernel_broadcast(&pipe->has_data);
    } else {
        pipe->read_closed = 1;
        
        kernel_broadcast(&pipe->has_space);
    }

    pipe->refcount--;
    
    if (pipe->refcount == 0) {
        free(pipe);
    }
    return 0;
}




static int pipe_reader_close(void* fd) {
    return pipe_close((pipe_cb*)fd, 0); // 0 = reader
}

static int pipe_writer_close (void* fd) {
    return pipe_close((pipe_cb*)fd, 1); // 1 = writer
}

static file_ops pipe_read_ops = {
    .Read = pipe_read,
    .Write = NULL,
    .Close = pipe_reader_close,
    .Open = NULL
};

static file_ops pipe_write_ops = {
    .Read = NULL,
    .Write = pipe_write,
    .Close = pipe_writer_close,
    .Open = NULL
};


int sys_Pipe(pipe_t* pipe)
{
    Fid_t fids[2];
    FCB* fcbs[2];
    pipe_cb* new_pipe;

    
    if (FCB_reserve(2, fids, fcbs) == 0) {
        return -1;
    }

    new_pipe = pipe_create();
    if (new_pipe == NULL) {
        FCB_unreserve(2, fids, fcbs);
        return -1;
    }

    
    new_pipe->refcount = 2;// read , write ends 

    
    fcbs[0]->streamobj = new_pipe;
    fcbs[0]->streamfunc = &pipe_read_ops;

    
    fcbs[1]->streamobj = new_pipe;
    fcbs[1]->streamfunc = &pipe_write_ops;

    pipe->read = fids[0];
    pipe->write = fids[1];

    return 0;
}