#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// used fo pause_system 
int pause_flag = 0;
int pause_wait_time = 0;
int pause_start_time = 0;
struct spinlock pause_lock;

// used for FCFS scheduler
struct spinlock fcfs_lock;

// used for SJF scheduler
int rate = 5;
struct spinlock sjf_lock;

// used for performance measurements (use with "tickslock")
int sleeping_processes_mean = 0;
int runnable_processes_mean = 0;
int running_processes_mean = 0;
int num_of_processes = 0;

// used for program_time (use with "tickslock")
int program_time = 0;

// used for cpu_utilization (use with "tickslock")
int start_time = 0;
int cpu_utilization = 0;

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }

  #ifdef DEFAULT
  printf("policy: DEFAULT\n");
  #elif SJF
  printf("policy: SJF\n");
  #elif FCFS
  printf("policy: FCFS\n");
  #else
  panic("Invalid policy selected");
  #endif

  // used for cpu_utilization
  acquire(&tickslock);
  start_time = ticks;
  release(&tickslock);
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // used for FCFS scheduler
  acquire(&tickslock);
  p->last_runnable_time = ticks;
  release(&tickslock);

  // used for SJF scheduler
  p->mean_ticks = 0;
  p->last_ticks = 0;
  
  acquire(&tickslock);
  // used for performance measurements
  p->sleeping_time = 0;
  p->runnable_time = 0;
  p->running_time = 0;

  p->sleeping_time_start = 0;
  p->runnable_time_start = 0;
  p->running_time_start = 0;
  release(&tickslock);

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;

  // used for FCFS scheduler
  p->last_runnable_time = 0;

  // used for SJF scheduler
  p->mean_ticks = 0;
  p->last_ticks = 0;

  acquire(&tickslock);
  // used for performance measurements
  p->sleeping_time = 0;
  p->runnable_time = 0;
  p->running_time = 0;

  p->sleeping_time_start = 0;
  p->runnable_time_start = 0;
  p->running_time_start = 0;
  release(&tickslock);
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  // print ids

  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  // used for performance measurements
  acquire(&tickslock);
  np->runnable_time_start = ticks;
  release(&tickslock);

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  // used for performance measurements
  acquire(&tickslock);
  p->sleeping_time = p->sleeping_time;
  p->runnable_time = p->runnable_time;
  p->running_time = p->running_time;

  sleeping_processes_mean = (sleeping_processes_mean * num_of_processes + p->sleeping_time) / (num_of_processes + 1);
  runnable_processes_mean = (runnable_processes_mean * num_of_processes + p->runnable_time) / (num_of_processes + 1);
  running_processes_mean = (running_processes_mean * num_of_processes + p->running_time) / (num_of_processes + 1);
  num_of_processes++;
  program_time += p->running_time;
  cpu_utilization = (100 * program_time) / ((ticks - start_time));
  release(&tickslock);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  #ifdef DEFAULT
  default_scheduler();
  #elif SJF
  sjf_scheduler();
  #elif FCFS
  fcfs_scheduler();
  #endif
}

void
default_scheduler(void){
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // pause_system - syscall check - start
    acquire(&pause_lock);
    int pause_flag_value = pause_flag;
    int pause_start_time_value = pause_start_time;
    int seconds = pause_wait_time;
    release(&pause_lock);
    
    if(pause_flag_value == 1) {
      int time_passed = 0;
      while (time_passed < seconds)
      {
        acquire(&tickslock);
        time_passed = (ticks - pause_start_time_value) / 10;
        release(&tickslock);
      }
      acquire(&pause_lock);
      pause_flag = 0;
      release(&pause_lock);
    }
    // pause_system - syscall check - end

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;

        if(p->pid > 2) {
          // used for performance measurements
          acquire(&tickslock);
          c->proc->runnable_time += ticks - c->proc->runnable_time_start;
          p->running_time_start = ticks;
          release(&tickslock);
        }

        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Shortest Job First scheduler
void
sjf_scheduler(void){
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // pause_system - syscall check - start
    acquire(&pause_lock);
    int pause_flag_value = pause_flag;
    int pause_start_time_value = pause_start_time;
    int seconds = pause_wait_time;
    release(&pause_lock);
    
    if(pause_flag_value == 1) {
      int time_passed = 0;
      while (time_passed < seconds)
      {
        acquire(&tickslock);
        time_passed = (ticks - pause_start_time_value) / 10;
        release(&tickslock);
      }
      acquire(&pause_lock);
      pause_flag = 0;
      release(&pause_lock);
    }
    // pause_system - syscall check - end

    int min_sjf_time = 0;
    int shortest_job = -1;
    int i = 0;
    struct proc *sjf_p = 0;

    // locking other CPUs until the next process to run will be selcted
    acquire(&sjf_lock);
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        if(shortest_job != -1) {
          if(min_sjf_time > p->mean_ticks) {
            shortest_job = i;
            min_sjf_time = p->mean_ticks;
          }
        }
        else {
          shortest_job = i;
          min_sjf_time = p->mean_ticks;
        }
      }
      release(&p->lock);
      i++;
    }
    if(shortest_job != -1) {
      sjf_p = &proc[shortest_job];
      acquire(&sjf_p->lock);
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      sjf_p->state = RUNNING;
      c->proc = sjf_p;

      if(c->proc->pid > 2) {
        // used for performance measurements
        acquire(&tickslock);
        c->proc->runnable_time += ticks - c->proc->runnable_time_start;
        c->proc->running_time_start = ticks;
        release(&tickslock);
      }
      
      release(&sjf_lock);
      swtch(&c->context, &sjf_p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      release(&sjf_p->lock);
    }
    else {
      release(&sjf_lock);
    }
  }
}

// First Comes First Served scheduler
void
fcfs_scheduler(void){
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // pause_system - syscall check - start
    acquire(&pause_lock);
    int pause_flag_value = pause_flag;
    int pause_start_time_value = pause_start_time;
    int seconds = pause_wait_time;
    release(&pause_lock);
    
    if(pause_flag_value == 1) {
      int time_passed = 0;
      while (time_passed < seconds)
      {
        acquire(&tickslock);
        time_passed = (ticks - pause_start_time_value) / 10;
        release(&tickslock);
      }
      acquire(&pause_lock);
      pause_flag = 0;
      release(&pause_lock);
    }
    // pause_system - syscall check - end

    int min_fcfs_time = 0;
    int first_come = -1;
    int i = 0;
    struct proc *fcfs_p = 0;

    // locking other CPUs until the next process to run will be selcted
    acquire(&fcfs_lock);
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
          if(first_come != -1) {
            if(min_fcfs_time > p->last_runnable_time) {
              first_come = i;
              min_fcfs_time = p->last_runnable_time;
            }
          }
          else {
            first_come = i;
            min_fcfs_time = p->last_runnable_time;
          }
      }
      release(&p->lock);
      i++;
    }
    if(first_come != -1) {
      fcfs_p = &proc[first_come];
      acquire(&fcfs_p->lock);
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      fcfs_p->state = RUNNING;
      c->proc = fcfs_p;

      if(c->proc->pid > 2) {
        // used for performance measurements
        acquire(&tickslock);
        c->proc->runnable_time += ticks - c->proc->runnable_time_start;
        c->proc->running_time_start = ticks;
        release(&tickslock);
      }
      
      release(&fcfs_lock);
      swtch(&c->context, &fcfs_p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      release(&fcfs_p->lock);
    }
    else {
      release(&fcfs_lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;

  if(p->pid > 2) {
    // used for performance measurements
    acquire(&tickslock);
    p->running_time += ticks - p->running_time_start;
    p->runnable_time_start = ticks;
    release(&tickslock);
  }

  // used for SJF and FCFS
  int last_ticks_value = 0;
  acquire(&tickslock);
  p->last_ticks = ticks - p->last_ticks;
  last_ticks_value = p->last_ticks;
  p->last_ticks = ticks;

  p->last_runnable_time = ticks;
  release(&tickslock);
  p->mean_ticks = ((10 - rate) * p->mean_ticks + last_ticks_value * (rate)) / 10;

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  if(p->pid > 2) {
    // used for performance measurements
    acquire(&tickslock);
    p->running_time += ticks - p->running_time_start;
    p->sleeping_time_start = ticks;
    release(&tickslock);
  }

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        
        if(p->pid > 2) {
          // used for performance measurements ("tickslock" already acquired)
          p->sleeping_time += ticks - p->sleeping_time_start;
          p->runnable_time_start = ticks;
        }
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;

        if(p->pid > 2) {
          // used for performance measurements
          acquire(&tickslock);
          p->sleeping_time += ticks - p->sleeping_time_start;
          p->runnable_time_start = ticks;
          release(&tickslock);
        }
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// added for printing pids of shell and init - files updated:
// kernel folder: defs.h, proc.c, syscall.c, syscall.h, sysproc.c 
// user folder: user.h, usys.pl, printpids.c
// Makefile: UPROGS
int 
print_pids(void)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      printf("%s %d\n", p->name, p->pid);
      release(&p->lock);
  }
  return 0;
}

// pause system - pause all processes
int 
pause_system(int seconds)
{
  // setting pause_flag that processes will pause only on the next context switch
  acquire(&pause_lock);
  pause_flag = 1;
  pause_wait_time = seconds; 
  acquire(&tickslock);
  pause_start_time = ticks;
  release(&tickslock);
  release(&pause_lock);

  // returning to scheduler to pause in there
  yield();
  return 0;
}

// kill system - kill all processes but init and shell
int 
kill_system(void)
{
  // going over all the processes and sending them kill
  struct proc *p;
  int pid;
  for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);
      pid = p->pid;
      release(&p->lock);

      // init = 1, shell = 2
      if (pid > 2){
        kill(pid);
      }
  }
  return 0;
}

// print_stats - print performance measurements
int
print_stats(void)
{
  acquire(&tickslock);
  int sleeping_processes_mean_value = sleeping_processes_mean;
  int runnable_processes_mean_value = runnable_processes_mean;
  int running_processes_mean_value = running_processes_mean;
  int num_of_processes_value = num_of_processes;
  int program_time_value = program_time;
  int start_time_value = start_time;
  int cpu_utilization_value = cpu_utilization;
  release(&tickslock);

  printf("sleeping_processes_mean: %d mils\n", sleeping_processes_mean_value);
  printf("runnable_processes_mean: %d mils\n", runnable_processes_mean_value);
  printf("running_processes_mean: %d mils\n", running_processes_mean_value);
  printf("num_of_processes: %d\n", num_of_processes_value);
  printf("program_time: %d mils\n", program_time_value);
  printf("start_time: %d mils\n", start_time_value);
  printf("cpu_utilization: %d%\n", cpu_utilization_value);
  return 0;
}

// get_utilization - returns cpu_utilization
int
get_utilization(void)
{
  acquire(&tickslock);
  int utilization = cpu_utilization;
  release(&tickslock);
  return utilization;
}