
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include <stdlib.h>
#include <string.h>

void thread_terminate(int exitval);

void sys_ThreadExit(int exitval);
/*
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;


  //
  rlnode_init(&pcb->ptcb_list , NULL);
  pcb->thread_count= 0 ; 
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
  This function is provided as an argument to spawn,
  to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}

void my_start_main_thread(){


int exitval;


  Task call=cur_thread()->ptcb->task ;
  int argl = cur_thread()->ptcb->argl;
  void* args = cur_thread()->ptcb->args;
 
exitval=call(argl,args);
ThreadExit(exitval);  



}


/*
  System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  PTCB *ptcbtemp;
 
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process)
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {

    //rlnode_init(&newproc->ptcb_list, NULL);
    newproc->main_thread = spawn_thread(newproc, start_main_thread);

    ptcbtemp=(PTCB*)xmalloc(sizeof(PTCB)) ;



 // ptcb variables

   ptcbtemp->argl=newproc->argl ;
   ptcbtemp->args=newproc->args ;  
   ptcbtemp->detached=0;
   ptcbtemp->exit_cv=COND_INIT;
   ptcbtemp->exited=0;
   ptcbtemp->refcount=0;
   ptcbtemp->task=newproc->main_task ;
   ptcbtemp->tcb=newproc->main_thread ;
   ptcbtemp->owner_pcb = newproc;
 
  //rlnode_init(&ptcbtemp->ptcb_list_node,ptcbtemp) ;

//  proc variables setup
rlnode_init(&ptcbtemp->ptcb_list_node,ptcbtemp) ;
rlist_push_back(&newproc->ptcb_list,&ptcbtemp->ptcb_list_node);
newproc->thread_count = 1  ;


// tcb variables
newproc->main_thread->ptcb=ptcbtemp ;

    wakeup(ptcbtemp->tcb);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
 
  cleanup_zombie(child, status);
 
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{
  if(sys_GetPid()==1 ){
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }


  /*
    Here, we must check that we are not the init task.
    If we are, we must wait until all child processes exit.
   */
  
  /* First, store the exit status */
  PCB *curproc = CURPROC;
  curproc->exitval = exitval;

 
  sys_ThreadExit(exitval);

}






//System Info Implementation
 
typedef struct info_controll_block {
    int current_index; 
} InfoCB;


static int info_read(void* obj, char* buf, unsigned int size) {
    InfoCB* procicb = (InfoCB*)obj;
    int bytes_written = 0;
    int entry_size = sizeof(procinfo);

    
    while (bytes_written + entry_size <= size) {

        
        while (procicb->current_index < MAX_PROC && PT[procicb->current_index].pstate == FREE) {
            procicb->current_index++;
        }

       
        if (procicb->current_index >= MAX_PROC) {
            break; 
        }

      
        PCB* pcb = &PT[procicb->current_index];
        procinfo info;

        info.pid = get_pid(pcb);
        info.ppid = (pcb->parent) ? get_pid(pcb->parent) : NOPROC;
        info.alive = (pcb->pstate == ALIVE);
        info.thread_count = pcb->thread_count;
        info.main_task = pcb->main_task;
        info.argl = pcb->argl;

        
        memset(info.args, 0, PROCINFO_MAX_ARGS_SIZE);

        
        if (pcb->args != NULL && pcb->argl > 0) {
            int limit = PROCINFO_MAX_ARGS_SIZE - 1;
            int cpy_size = (pcb->argl < limit) ? pcb->argl : limit;
            memcpy(info.args, pcb->args, cpy_size);
        }

        
        memcpy(buf + bytes_written, &info, entry_size);

        bytes_written += entry_size;
        
        
        procicb->current_index++;
    }

    return bytes_written;
}

// 3. Συνάρτηση Close
static int info_close(void* obj) { 
    free(obj);
    return 0;
}


static file_ops info_ops = {
    .Read = info_read,
    .Write = NULL,       
    .Close = info_close,
    .Open = NULL
};


Fid_t sys_OpenInfo() {
    Fid_t fid;
    FCB* fcb;
    InfoCB* cb;

    
    if (FCB_reserve(1, &fid, &fcb) == 0) {
        return NOFILE;
    }

    
    cb = (InfoCB*)xmalloc(sizeof(InfoCB)); 
    if (cb == NULL) {
        FCB_unreserve(1, &fid, &fcb);
        return NOFILE;
    }

 
    cb->current_index = 0;

    // Σύνδεση
    fcb->streamobj = cb;
    fcb->streamfunc = &info_ops; 

    return fid;
}