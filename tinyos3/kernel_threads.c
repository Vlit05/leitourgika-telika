#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

#include "kernel_cc.h"
#include "kernel_streams.h"
#include <stdlib.h>     
#include <assert.h>     
#include <string.h>    

void my_start_main_thread();

/**
  @brief Helper function for thread exit.
  
  THIS FUNCTION MUST BE CALLED WITH THE kernel_lock HELD.
  It will release the lock via kernel_sleep.
 */

/*void thread_terminate (int exitval)
{
   
}*/

/* @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{

    if(task == NULL){
        return NOTHREAD;
    }

    TCB *temp = spawn_thread(CURPROC , my_start_main_thread);
    if(temp == NULL){
        return NOTHREAD;
    }
    PTCB *ptcbtemp;

    ptcbtemp =(PTCB*)xmalloc(sizeof(PTCB));
    if (ptcbtemp == NULL) { 
        return NOTHREAD;
    }

    ptcbtemp->args = args;
    ptcbtemp->argl = argl;
    
    ptcbtemp->detached = 0;
    ptcbtemp->exit_cv = COND_INIT;
    ptcbtemp->exited = 0;
    ptcbtemp->refcount = 0 ;
    ptcbtemp->task = task; 
    ptcbtemp->tcb = temp ;
    ptcbtemp->owner_pcb = CURPROC; 

    rlnode_init(&ptcbtemp->ptcb_list_node, ptcbtemp);
    rlist_push_back(&CURPROC->ptcb_list , &ptcbtemp->ptcb_list_node);

    CURPROC->thread_count++ ;

    temp->ptcb=ptcbtemp;
    
    wakeup(temp);
    
    return(Tid_t)ptcbtemp;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
    return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */

int sys_ThreadJoin(Tid_t tid, int* exitval)
{
    PTCB* ptcb_to_join = (PTCB*)tid;

    if (ptcb_to_join == NULL || rlist_find(&CURPROC->ptcb_list, ptcb_to_join, NULL) == NULL) { //there is no thread with the given tid in this process
        return -1; 
    }

    if(ptcb_to_join == cur_thread()->ptcb){ //the tid corresponds to the current thread 
        return -1; 
    }

    if(ptcb_to_join->detached){ // tid corresponds to a detached thread 
        return -1;
    }

    ptcb_to_join->refcount++;

    while(ptcb_to_join->exited == 0 && ptcb_to_join->detached == 0){
        kernel_wait(&ptcb_to_join->exit_cv ,SCHED_USER);
    }
    
    if(ptcb_to_join->detached == 1){
        ptcb_to_join->refcount--;
        if(ptcb_to_join->refcount == 0 && ptcb_to_join->exited == 1){
            free(ptcb_to_join); 
        }
        return -1;
    }

    if(exitval != NULL){
        *exitval = ptcb_to_join->exitval;
    }

    ptcb_to_join->refcount--;

    if(ptcb_to_join->refcount == 0){
        rlist_remove(&ptcb_to_join->ptcb_list_node);
        free(ptcb_to_join); 
    }
    
    return 0;
}
/**
  @brief Detach the given thread.
  */

int sys_ThreadDetach(Tid_t tid)
{
    PTCB* ptcb_to_detach = (PTCB*)tid;

    if (ptcb_to_detach == NULL || rlist_find(&CURPROC->ptcb_list, ptcb_to_detach, NULL) == NULL) {
        return -1; 
    }

    if(ptcb_to_detach->detached == 1) { 
        return 0;
    }

    if(ptcb_to_detach->exited == 1){
        if (ptcb_to_detach->refcount == 0) {
            rlist_remove(&ptcb_to_detach->ptcb_list_node);
            free(ptcb_to_detach);
        }
        return -1; 
    }

    ptcb_to_detach->detached = 1; 
    kernel_broadcast(&ptcb_to_detach->exit_cv);
    return 0 ;
}
/**
  @brief Terminate the current thread.
 */
void sys_ThreadExit(int exitval)
{
    PTCB* ptcb = cur_thread()->ptcb;
    PCB *curproc = CURPROC;

    ptcb->exited = 1;
    ptcb->exitval = exitval;
 
    // rlist_remove(&ptcb->ptcb_list_node); 
    curproc->thread_count--;

    kernel_broadcast(&ptcb->exit_cv);

    int should_free_ptcb = 0;
    if(ptcb->detached == 1 && ptcb->refcount == 0){
        should_free_ptcb = 1;
        // Αν είναι να το ελευθερώσουμε, ΤΩΡΑ το αφαιρούμε
        rlist_remove(&ptcb->ptcb_list_node); 
    }

    if(curproc->thread_count == 0){ // Αν είμαστε το τελευταίο νήμα
        // ... (Ο κώδικας καθαρισμού του PCB είναι σωστός ως έχει) ...
        if(sys_GetPid() != 1 && curproc->parent != NULL){ 
            PCB* initpcb = get_pcb(1);
            while(!is_rlist_empty(& curproc->children_list)) {
                rlnode* child = rlist_pop_front(& curproc->children_list);
                child->pcb->parent = initpcb;
                rlist_push_front(& initpcb->children_list, child);
            }
            if(!is_rlist_empty(& curproc->exited_list)) {
                rlist_append(& initpcb->exited_list, &curproc->exited_list);
                kernel_broadcast(& initpcb->child_exit);
            }
            rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
            kernel_broadcast(& curproc->parent->child_exit);
        }
        assert(is_rlist_empty(& curproc->children_list));
        assert(is_rlist_empty(& curproc->exited_list));
        if(curproc->args) {
            free(curproc->args);
            curproc->args = NULL;
        }
        for(int i=0;i<MAX_FILEID;i++) {
            if(curproc->FIDT[i] != NULL) {
                FCB_decref(curproc->FIDT[i]);
                curproc->FIDT[i] = NULL;
            }
        }
        curproc->main_thread = NULL;
        curproc->pstate = ZOMBIE;
    } 

    if (should_free_ptcb) {
        free(ptcb);
    }
    
    kernel_sleep(EXITED, SCHED_USER);
}