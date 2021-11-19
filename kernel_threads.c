
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{

  //Spawning new_tcb and allocating space for ptcb
  TCB* new_tcb = spawn_thread(CURPROC, start_main_ptcb);
  PTCB* new_ptcb = xmalloc(sizeof(PTCB));
  
  //Making needed connections between new_tcb and new_ptcb and intializing
  //the empty fields in ptcb.
  new_tcb->ptcb = new_ptcb;
  new_tcb->owner_pcb = CURPROC;

  new_ptcb->tcb = new_tcb;
  new_ptcb->task = task;
  new_ptcb->args = args;
  new_ptcb->argl = argl;

  new_ptcb->refcount = 0;
  new_ptcb->detached = 0;
  new_ptcb->exited = 0;
  new_ptcb->exit_cv = COND_INIT;
  
  //Initializing the new_ptcb_list_node, also pushing it into the list.
  rlnode_init(& new_ptcb->ptcb_list_node, new_ptcb);
  rlist_push_back(& CURPROC->ptcb_list,& new_ptcb->ptcb_list_node);

  //Increasing thread_count by 1 since we are creating a new thread.
  CURPROC->thread_count++; 

  //Waking up the created tcb
  wakeup(new_tcb); 

  //Returning the Tid_
	return (Tid_t) new_ptcb;
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

  PTCB* joined_ptcb = (PTCB *) tid;

  //There are three reasons for sys_ThreadJoin() to fail and I check them at the begining of the function
  //1)The tid to join corresponds to the thread itself. This can't happen of course since a thread can't
  //join itself.
  //2)The tid to join corresponds to a thread that is detached.
  //4)existent_node == NULL which means that the tid given is does not belong to CURPROC 

  //Checking if the given tid is a thread of the current process. If the rlist_find() succeeds 
  //it returns an a pointer to the ptcb it found. Else it returns NULL to the existent_node varriable.

  rlnode* existent_node = rlist_find(& CURPROC->ptcb_list, joined_ptcb, NULL);
  if(existent_node == NULL){
    return -1;
  }

  if(tid == sys_ThreadSelf()){
    return -1;
  }

  if(joined_ptcb->detached){
    return -1;
  }

  //Increasing refcount by 1 since we are waiting since joined_ptcb exits.
  joined_ptcb->refcount++;

  //Using kernel wait until joined ptcb exits or detaches.
  while(joined_ptcb->detached != 1 && joined_ptcb->exited !=1){
  kernel_wait(&joined_ptcb->exit_cv, SCHED_IDLE);
  }

  //Reducing refcount by 1 since we are out of the loop and that means that joined_ptch has exited.
  joined_ptcb->refcount--;

  //If still after waiting the thread is detached return -1.
  if(joined_ptcb->detached){
    return  -1;
  }

  //If joined_ptcb has exited and exit_val != NULL assign to exitval the exitval of joined_ptcb
  if(joined_ptcb->exited){
    if(exitval)
      *exitval = joined_ptcb->exitval;
  }

  //If joined_ptcb->refcount == 0 then the ptcb is not needed anymore and we can remove it 
  //from the list and after that free it.

  if(joined_ptcb->refcount == 0){
    rlist_remove(& joined_ptcb->ptcb_list_node);
    free(joined_ptcb);
  }

  return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid) 
{
  //There are 2 reason for detach to fail. 
  //1)existent_node == NULL which means that the tid given is does not belong to CURPROC 
  //2)The thread that we are trying to detach has already exited.

  PTCB* detached_ptcb = (PTCB *) tid;

  rlnode* existent_node = rlist_find(& CURPROC->ptcb_list,detached_ptcb, NULL);

  if(existent_node == NULL){
    return -1;
  }

  if(detached_ptcb->exited){
    return -1;
  }

  //Detaching the thread.
  detached_ptcb->detached = 1;

  //Broadcasting to the other ptcb waiting that the thread that corresponds to tid has been detached.
  kernel_broadcast(& detached_ptcb->exit_cv);

	return 0;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */

  PCB *curproc = CURPROC;
  TCB *curthread = cur_thread();

  //I check if thread_count == 1. If it is then you have only one thread left.
  //What's left to do is to intialize the proccess cleaning code that was previously in
  //sys_Exit() along with some additions. See bellow.

  if(CURPROC->thread_count == 1){

    //Checking if curproc is != 1 if it is 1 then start the cleanup.

    if(get_pid(curproc)!=1) {

      /* Reparent any children of the exiting process to the 
         initial task */
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) {
        rlnode* child = rlist_pop_front(& curproc->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(& initpcb->children_list, child);
      }

      /* Add exited children to the initial task's exited list 
         and signal the initial task */
      if(!is_rlist_empty(& curproc->exited_list)) {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }

      /* Put me into my parent's exited list */
      rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
      kernel_broadcast(& curproc->parent->child_exit);

    }

    assert(is_rlist_empty(& curproc->children_list));
    assert(is_rlist_empty(& curproc->exited_list));


    /* 
      Do all the other cleanup we want here, close files etc. 
     */

    /* Release the args data */
    if(curproc->args) {
      free(curproc->args);
      curproc->args = NULL;
    }

    /* Clean up FIDT */
    for(int i=0;i<MAX_FILEID;i++) {
      if(curproc->FIDT[i] != NULL) {
        FCB_decref(curproc->FIDT[i]);
        curproc->FIDT[i] = NULL;
      }
    }

    /* Disconnect my main_thread */
    curproc->main_thread = NULL;

    /* Now, mark the process as exited. */
    curproc->pstate = ZOMBIE;

  } 

  //Raising the exited flag and giving the exitval to the thread.s

  curthread->ptcb->exited = 1;
  curthread->ptcb->exitval = exitval;

  //Since we the PTCB is deleted you have to deplete the thread_count by 1.
  curproc->thread_count--;
  
  //Broadcasting that the current thread has exited to all the other threads waiting for it.
  kernel_broadcast(& cur_thread()->ptcb->exit_cv);

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);

  if(cur_thread()->ptcb->refcount == 0){
    rlist_remove(& cur_thread()->ptcb->ptcb_list_node);
    free(cur_thread()->ptcb);
  }
}


/*------------------------------Check list for the progress of the project.---------------------------------
DONE 1)Confirm that sys_Exec() and sys_Exit() are 100% correct.
DONE 2)Confirm that sys_CreateThread() is 100% correct.
DONE 3)Code sys_ThreadJoin().
DONE 4)Code sys_ThreadDetach();
DONE 5)Implement Multi-Level-Feedback algorythm to the scheduler
DONE 7)SOS!!!!!!What does the cond var do exactly? Also how do you use it?
DONE 8)Revist the last comment of ThreadJoin().
DONE 9)Ask for checking if the node exists first on sys_ThreadJoin
DONE 10)Ask for memmory trash so that we have a really good program :D .
*/