#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H

#include "kernel_cc.h"


#define PIPE_BUFFER_SIZE 8192

typedef struct pipe_control_block {
    CondVar has_space;      // Block writer if no space  
    CondVar has_data;       // Block reader if no data 
    
    unsigned int w_position; 
    unsigned int r_position; 
    unsigned int count;     
    
    char buffer[PIPE_BUFFER_SIZE]; 

    int write_closed;       //if = 1 writer is closed 
    int read_closed;        //if = 1 reader is closed 
    
    int refcount;           
} pipe_cb;


pipe_cb* pipe_create();


int pipe_read(pipe_cb* pipe, char* buf, unsigned int size);
int pipe_write(pipe_cb* pipe, const char* buf, unsigned int size);
int pipe_close(pipe_cb* pipe, int is_writer);

#endif