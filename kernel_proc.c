
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/*File_ops in order to use Read,Write,Close and Open functions when using a procinfo_cb.
*/
static file_ops procinfo_ops = {
  .Open = NULL,
  .Read = procinfo_read,
  .Write = error_write,
  .Close = procinfo_close
};

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

//This is the new function that is similarly to the function exactly aboved.
//Done! I am 100% sure and received confirmation :)
void start_main_ptcb()
{
  int exitval;
  
  Task call =  cur_thread()->ptcb->task;
  int argl = cur_thread()->ptcb->argl;   
  void* args = cur_thread()->ptcb->args;

  exitval = call(argl,args);
  ThreadExit(exitval);
}

/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
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

  //Initiallizing the head of the list also setting the thread count to 1 since 
  //this is the first proccess.
  rlnode_init(& newproc->ptcb_list, NULL);
  newproc->thread_count = 1;

  if(call != NULL) {

    //Creating a new_ptcb and allocating space.
    PTCB* new_ptcb = xmalloc(sizeof(PTCB));

    //Spawning a new thread and putting it inside PCB *newproc->main_thread.
    //This thread is the main thread thus it's name.
    newproc->main_thread = spawn_thread(newproc, start_main_thread);

    //Making nessecerry connections.
    newproc->main_thread->ptcb = new_ptcb;
    newproc->main_thread->owner_pcb = newproc;

    new_ptcb->tcb = newproc->main_thread;
    new_ptcb->task = call;
    new_ptcb->args = args;
    new_ptcb->argl = argl;

    new_ptcb->refcount = 0;
    new_ptcb->detached = 0;
    new_ptcb->exited = 0;
    new_ptcb->exit_cv = COND_INIT;
    
    //Initilize the new PTCB node, and pushing it inside the PTCB list.
    rlnode_init(& new_ptcb->ptcb_list_node, new_ptcb);
    rlist_push_back(& newproc->ptcb_list,& new_ptcb->ptcb_list_node); 

    //Waking up the main_thread that I just created.
    wakeup(newproc->main_thread);
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

/*The change was to move most of it's code inside sys_ThreadExit()*/
void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  if(get_pid(curproc)==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

  sys_ThreadExit(exitval);


}

/*Open info function in order to open current processes running on tinyos3.
*/
Fid_t sys_OpenInfo()
{ 

  /*The FCB and Fid_t that are used in order to create a procinfo_cb.
  */
  Fid_t reserved_Fid_t;
  FCB* reserved_FCB;
  
  /*Making a reservation of a FCB and a Fid_t.
  The FCB reserve function returns 1 if the reservation is successfull.
  */
  int reservation_complete = FCB_reserve(1,&reserved_Fid_t,&reserved_FCB);

  if(reservation_complete!=1){
    return NOFILE;
  }

  /*Allocating space for a procinfo_cb.
  */
  procinfo_cb* new_proc_info = xmalloc(sizeof(procinfo_cb));

  if(new_proc_info == NULL){
    return NOFILE;
  }

  /*Intializing the pos on 0. This is an integer that's used to scan through the PT(process table).
  */
  new_proc_info->pos = 0;

  /*We are connecting the reserved_FCBs with the new_proc_info
  and we are using the file ops in order to enable the required function calls.
  */
  reserved_FCB->streamobj = new_proc_info;
  reserved_FCB->streamfunc = &procinfo_ops;

	return reserved_Fid_t;
}

int procinfo_read(void* streamobj, char *buf, unsigned int size){

  /*Casting the void* streamobj into a procinfo_cb *.
  */
  procinfo_cb* new_procinfo_cb = (procinfo_cb *) streamobj;

  if(new_procinfo_cb == NULL){
    return NOFILE;
  }

  /*This loop scans through the process table and finds the first non-free FCB and assigns it's
  valuues to the streamobj.
  */
  while(new_procinfo_cb->pos < MAX_PROC-1){

    PCB* cursor = &PT[new_procinfo_cb->pos];

    if(cursor->pstate != FREE){
      new_procinfo_cb->proc_info.pid = get_pid(cursor);
      new_procinfo_cb->proc_info.ppid = get_pid(cursor->parent) ;
      if(cursor->pstate == ALIVE){
        new_procinfo_cb->proc_info.alive = 1;
      }else{
        new_procinfo_cb->proc_info.alive = 0;
      }
      new_procinfo_cb->proc_info.thread_count = cursor->thread_count;
      new_procinfo_cb->proc_info.main_task = cursor->main_task;
      new_procinfo_cb->proc_info.argl = cursor->argl;
      if(cursor->argl<PROCINFO_MAX_ARGS_SIZE){
        memcpy(new_procinfo_cb->proc_info.args, cursor->args, cursor->argl);
      }else if(cursor->argl>=PROCINFO_MAX_ARGS_SIZE){
        memcpy(new_procinfo_cb->proc_info.args, cursor->args, PROCINFO_MAX_ARGS_SIZE);
      }
      new_procinfo_cb->pos++;
      break;
    }
      new_procinfo_cb->pos++;
  }

  if(new_procinfo_cb->pos == MAX_PROC-1){
     return 0;
  }

  /*Using memcopy in order to copy the values we assigned above into the given buffer in
  the format of char*.
  */
  memcpy(buf,(char *)&new_procinfo_cb->proc_info,sizeof(procinfo));

  return 1;
}

int procinfo_close(void* streamobj){

   /*Casting the void* streamobj into a procinfo_cb *.
  */
  procinfo_cb* new_procinfo_cb =(procinfo_cb*) streamobj;
  
  if(new_procinfo_cb != NULL) {  
    free(new_procinfo_cb);
    return 0;
  }

  return NOFILE;
}