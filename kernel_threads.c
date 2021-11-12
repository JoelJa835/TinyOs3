
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

/** 
  @brief Create a new thread in the current process.
  DONE 85%-90% sure.. :)"
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{

  TCB* new_tcb = spawn_thread(CURPROC, start_main_ptcb);
  PTCB* new_ptcb = xmalloc(sizeof(PTCB));

  new_tcb->ptcb = new_ptcb;
  new_ptcb->tcb = new_tcb;
  rlnode_init(& new_ptcb->ptcb_list_node, new_ptcb);
  rlist_push_back(& CURPROC->ptcb_list, &new_ptcb->ptcb_list_node);

  //Edit:When you create a new thead you have to increase the thread_count by 1.
  CURPROC->thread_count = CURPROC->thread_count + 1; 

  wakeup(new_tcb); 

	return (Tid_t) new_ptcb;
}

/**
  @brief Return the Tid of the current thread.
  DONE 100% sure. :)
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */
  //1.elegxoi gia na doume an mporei an ginei to thread join also increase tid + 1
  //2.call kernel_wait() to wait for join maybe inside a loop (?)
  //3.ti simbainei an ginei wakeup if it wakes up tid - 1
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

  //Checking if the current thread is a thread of the current process. If the rlist_find() succeeds 
  //it returns an a pointer to the ptcb it found. Else it returns NULL to the existent_node varriable.
  rlnode* existent_node = rlist_find(& CURPROC->ptcb_list,& cur_thread()->ptcb, existent_node);

  //There are three reasons for sys_Thread Join to fail and I check them at the begining of the function
  //1)existent_node == NULL OR
  //2)The tid to join corresponds to the thread itself. This can't happen of course since a thread can't
  //join itself.
  //3)The tid to join corresponds to a thread that is detached

  if(existent_node != NULL || tid == sys_ThreadSelf() || ((PTCB *) tid)->detached == 1){
    return -1;
  }

  // ((PTCB* tid)->refcount = (PTCB* tid)->refcount + 1)
  // kernel_wait((PTCB* tid)->exit_cv, SCHED_USER);

  // if((PTCB* tid)->exited == 1){
  //   ((PTCB* tid)->refcount = (PTCB* tid)->refcount - 1)
  // }

  
	return -1;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid) 
{
  //1.use kernel_broadcast to wakeup all threads if thread joins fails
	return -1;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  //check if it is the last thread (copy sys_exit code) and then use kernel broadcast at 
  //the list that wait this thread to end, Reduce thread count -1. Also kernel_sleep and so on..
  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  PCB *curproc = CURPROC;

  //I check if thread_count == 1. If it is then you have only one thread left.
  //What's left to do is to intialize the proccess cleaning code that was previously in
  //sys_Exit() along with some additions. See bellow.

  if(CURPROC->thread_count == 1){

    if(get_pid(curproc)==1) {

      while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

    } else {

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

    //That's the new code segment that I added to the sys_Exit() code.
    //Pop the last PTCB from the list and make thread count = 0,
    //also you have to assert that ptcb list is empty
    //I think it's correct though it needs confirmation (?)

    rlist_pop_front(& curproc->ptcb_list);
    curproc->thread_count = 0;
    assert(is_rlist_empty(& curproc->ptcb_list));

    /* Disconnect my main_thread */
    curproc->main_thread = NULL;

    /* Now, mark the process as exited. */
    curproc->pstate = ZOMBIE;

    //Also ask if you have to kernel_broadcast here as well(?). Probably not because it is the last
    //thread and no-one is waiting for it but ask just in case.

  } else {

    //Question: What happens to the popped ptcb? Does it have to be deleted or it is deleted by simply popping it?
    //If so then what's the puprpose of exitval if the ptcb "Object" is deleted (?) 
     rlnode* popped_node = rlist_pop_front(& curproc->ptcb_list);
     popped_node->ptcb->exited = 1; 

    //Since we the PTCB is deleted you have to deplete the thread count by 1.
     curproc->thread_count = curproc->thread_count - 1;
     //Question: kernel_broadcoast has to be used 100% to inform the waiting threads that the thread has exited
     //but how do you used the function? One idea is impleemented bellow. (?)
     //Edit: Probably informs all the threads that have this ptcb on their wait list that it exited.
     //I still have to learn WHERE to initiate the COND_VAR because I suspect that 
     //the varriable currently it has nothing in it.
     kernel_broadcast(& popped_node->ptcb->exit_cv);

  }


  // Moved kernel_sleep to the end because you have to kernel sleep regardless the thread_count 
  //I think it's correct I just have to confirm it. (?)
  /* Bye-bye cruel world */
    kernel_sleep(EXITED, SCHED_USER);
}


/*------------------------------Check list for the progress of the project.---------------------------------
1)Confirm that sys_Exec() and sys_Exit() are 100% correct.
2)Confirm that sys_CreateThread() is 100% correct.
3)Code sys_ThreadJoin()
4)Code sys_ThreadDetach();
5)Implement Multi-Level-Feedback algorythm to the scheduler
6)Clean up my messy comments :[
7)SOS!!!!!!What does the cond var do exactly? Also how do you use it?

*/
