#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "stat.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "processInfo.h"

int swap_in_function_exists=0;
int swap_out_function_exists=0;
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);
int      sleeping_channel_count;
char *   sleeping_channel;
struct spinlock sleeping_channel_lock;
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;


int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

int
file_read(int fd, int n, char *p)
{
  struct file *f = myproc()->ofile[fd];
  if(fd<0 || fd>=NOFILE)
    return -1;
  return fileread(f, p, n);
}

int
file_write(int fd, int n, char *p)
{
  struct file *f = myproc()->ofile[fd];
  if(fd<0 || fd>=NOFILE)
    return -1;
  return filewrite(f, p, n);
}

int
file_close(int fd)
{
  struct file *f = myproc()->ofile[fd];
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}


static struct inode*
file_create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
file_open(char *path,int omode)
{
  int fd;
  struct file *f;
  struct inode *ip;
  begin_op();
  if(omode & O_CREATE){
    ip = file_create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

void Int_to_String(int n,char *c)
{
    char a[50];
    int i=0;
    while (n>0)
    {
      a[i++]=n%10+'0';
      n/=10;
    }
    for (int j = 0; j < (i+1)/2; j++)
    {
      c[j]=a[i-j-1];
      c[i-j-1]=a[j];
    }
    c[i]='\0';
}
struct Ready_Queue
{
  struct spinlock lock;
  struct proc *queue[NPROC];
  int head;
  int tail;
};

void RQueue_Push(struct proc *proc, struct Ready_Queue *q)
{
  acquire(&q->lock);
  q->queue[q->tail] = proc;
  q->tail++;
  q->tail %= NPROC;
  release(&q->lock);
}

struct proc *RQueue_Pop(struct Ready_Queue *q)
{
  // acquire(&q->lock);
  if (q->head == q->tail)
  {
    return 0;
  }
  struct proc *res = q->queue[q->head];
  q->head++;
  q->head %= NPROC;
  // release(&q->lock);
  return res;
}

struct   Ready_Queue  swap_out_queue;
struct   Ready_Queue  swap_in_queue;

void Swap_Out_Function(void)
{
  acquire(&swap_out_queue.lock);
  while (swap_out_queue.head != swap_out_queue.tail)
  {
    struct proc *proc = RQueue_Pop(&swap_out_queue);
    pde_t *pgdir = proc->pgdir;
    int done = 0;
    for (int i = 0; i < NPDENTRIES; i++)
    {
      if (done)
      {
        break;
      }

      // First we get the phy address of the 2nd level page table and convert into VA.
      pte_t *pgtab = (pte_t *)P2V(PTE_ADDR(pgdir[i]));

      for (int j = 0; j < NPTENTRIES; j++)
      {
        // If page not present -> skip
        if (!(pgtab[j] & PTE_P))
        {
          continue;
        }
        pte_t *pte = (pte_t *)P2V(PTE_ADDR(pgtab[j]));

        int pid = proc->pid;
        int va = i * (1 << 22) + j * (1 << 12);
        char file_name[50];
        char PID[50];
        char VA[50];
        Int_to_String(pid, PID);
        Int_to_String(va, VA);
        int idx = 0;
        for (int i = 0; i < strlen(PID); i++)
        {
          if (PID[i] == '\0')
            continue;
          file_name[idx++] = PID[i];
        }
        file_name[idx++] = '_';
        for (int i = 0; i < strlen(VA); i++)
        {
          if (VA[i] == '\0')
            continue;
          file_name[idx++] = VA[i];
        }
        file_name[idx++] = '.';
        file_name[idx++] = 's';
        file_name[idx++] = 'w';
        file_name[idx++] = 'p';
        file_name[idx] = '\0';

        int fd = file_open(file_name, O_CREATE | O_RDWR);
        if (fd < 0)
        {
          cprintf("Error in opening file %s\n", file_name);
          panic("File did not Open!");
        }
        if (file_write(fd, PGSIZE, (char *)PTE_ADDR(pgtab[j])) < 0)
        {
          cprintf("Error in Writinging file %s\n", file_name);
          panic("File did not Writing!");
        }
        file_close(fd);
        kfree((char *)pte);
        done = 1;
        pgtab[j] ^= PTE_P;
        pgtab[j] ^= (0x80);
        // cprintf("Swapping out %s",file_name);
      }
    }
  }
  release(&swap_out_queue.lock);
  swap_out_function_exists = 0;
  struct proc *proc = myproc();
  proc->state = UNUSED;
  proc->killed = 0;
  proc->parent = 0;
  sched();
}

void Swap_In_Function(void)
{
  acquire(&swap_in_queue.lock);
  while (swap_in_queue.head!=swap_in_queue.tail)
  {
      struct proc *proc = RQueue_Pop(&swap_in_queue);
      int pid=proc->pid;
      int va=proc->virt_add;
      char file_name[50];
      char PID[50];
      char VA[50];
      Int_to_String(pid, PID);
      Int_to_String(va, VA);
      int idx = 0;
      for (int i = 0; i < strlen(PID); i++)
      {
        if (PID[i] == '\0')
          continue;
        file_name[idx++] = PID[i];
      }
      file_name[idx++] = '_';
      for (int i = 0; i < strlen(VA); i++)
      {
        if (VA[i] == '\0')
          continue;
        file_name[idx++] = VA[i];
      }
      file_name[idx++] = '.';
      file_name[idx++] = 's';
      file_name[idx++] = 'w';
      file_name[idx++] = 'p';
      file_name[idx] = '\0';

      int fd = file_open(file_name, O_RDONLY);
      if (fd < 0)
      {
        cprintf("Error in opening file %s\n", file_name);
        panic("File did not Open!");
      }
      char * mem=kalloc();
      file_read(fd,PGSIZE,mem);
      if(mappages(proc->pgdir,(char *) va , PGSIZE, V2P(mem), PTE_W|PTE_U) < 0)
      {
        cprintf("Mapping pages failed for %s\n",file_name);
        panic("Mapping\n");
      }
      // cprintf("Swapping in %s",file_name);
      wakeup(proc->chan);
      
  }
  release(&swap_in_queue.lock);
  swap_in_function_exists = 0;
  struct proc *proc = myproc();
  proc->state = UNUSED;
  proc->killed = 0;
  proc->parent = 0;
  sched();

}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&swap_out_queue.lock,"Swap_Out");
  initlock(&swap_in_queue.lock,"Swap_In");
  initlock(&sleeping_channel_lock,"sleeping_channel");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(p->state == UNUSED)
    {
      goto found;
    }

  }
  release(&ptable.lock);
  return 0;

found:
  p->numberContextSwitches=0;
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);

  acquire(&swap_out_queue.lock);
  swap_out_queue.head = 0;
  swap_out_queue.tail = 0;
  release(&swap_out_queue.lock);

  acquire(&swap_in_queue.lock);
  swap_in_queue.head = 0;
  swap_in_queue.tail = 0;
  release(&swap_in_queue.lock);

  acquire(&sleeping_channel_lock);
  sleeping_channel_count = 0;
  release(&sleeping_channel_lock);
  swap_in_function_exists = 0;
  swap_out_function_exists = 0;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Round Robin Scheduler
// PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->numberContextSwitches++;
      swtch(&(c->scheduler), p->context);
      
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Shortest Job First
// void scheduler(void)
// {
//   struct proc *p;
//   struct cpu *c = mycpu();
//   c->proc = 0;

//   for (;;)
//   {
//     sti();
//     acquire(&ptable.lock);
//     struct proc *sjf = 0;
//     for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
//     {
//       if (p->state != RUNNABLE)
//         continue;
//       if (sjf == 0)
//       {
//         sjf = p;
//       }
//       else if (sjf->burst_time > p->burst_time)
//       {
//         sjf = p;
//       }
//     }
//     p = sjf;
//     if (p != 0)
//     {
//       c->proc = p;
//       switchuvm(p);
//       p->state = RUNNING;
//       p->numberContextSwitches++;
//       swtch(&(c->scheduler), p->context);
//       switchkvm();
//       c->proc = 0;
//     }
//     release(&ptable.lock);
//   }
// }


//HYBRID
// void scheduler(void)
// {
//   struct cpu *c = mycpu();
//   c->proc = 0;

//   for (;;)
//   {
//     sti();
//     acquire(&ptable.lock);
//     struct proc *Ready_Queue[NPROC];
//     for (int i = 0; i < NPROC; i++)
//     {
//       Ready_Queue[i] = 0;
//     }

//     int idx = 0;
//     struct proc *p;
//     for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
//     {
//       if (p->state == RUNNABLE)
//       {
//         Ready_Queue[idx++] = p;
//       }
//     }
//     if (idx != 0)
//     {
//       struct proc *temp;
//       for (int i = 0; i < idx; i++)
//       {
//         for (int j = i + 1; j < idx; j++)
//         {
//           if (Ready_Queue[i]->burst_time > Ready_Queue[j]->burst_time)
//           {
//             temp = Ready_Queue[i];
//             Ready_Queue[i] = Ready_Queue[j];
//             Ready_Queue[j] = temp;
//           }
//         }
//       }
//       int time_quanta = 1;
//       for (int i = 0; i < idx; i++)
//       {
//         if (Ready_Queue[i]->burst_time > 0)
//         {
//           time_quanta = Ready_Queue[i]->burst_time;
//           break;
//         }
//       }
//       for (int i = 0; i < idx; i++)
//       {
//         p = Ready_Queue[i];
//         p->time_quanta=time_quanta;
//         if (p->state != RUNNABLE)
//         {
//           continue;
//         }
//         c->proc = p;
//         switchuvm(p);
//         p->state = RUNNING;
//         p->numberContextSwitches++;
//         swtch(&(c->scheduler), p->context);
//         switchkvm();
//         c->proc = 0;
//       }
//     }

//     release(&ptable.lock);
//   }
// }

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();
  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


/////////////////////////////////////////////////////////////////////////////////////////

int thread_create(void(*fcn)(void*), void *arg, void*stack)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Page Table is same.
  np->pgdir = curproc->pgdir;   

  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  // Stack Size is of 1 page
  np->tf->esp = (uint)stack + 4096;

  // Load Arguments in Stack
  np->tf->esp -= 4;
  *((uint*)(np->tf->esp)) = (uint)arg;

  // Save Return Address in stack
  np->tf->esp -= 4;
  *((uint*)(np->tf->esp)) = 0xffffffff;

  // Set instruction Pointer
  np->tf->eip = (uint) fcn;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

int thread_join(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        // freevm(p->pgdir);    // Dont free the page directory
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

int thread_exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

int getNumProc(void)
{
  int active = 0;

  acquire(&ptable.lock);

  struct proc *process;

  for (process = ptable.proc; process < &ptable.proc[NPROC]; process++)
  {
    if (process->state != UNUSED)
    {
      active++;
    }
  }

  release(&ptable.lock);
  return active;
}

int getMaxPid(void)
{
  int max_active = -1;

  acquire(&ptable.lock);

  struct proc *process;

  for (process = ptable.proc; process < &ptable.proc[NPROC]; process++)
  {
    if (process->state != UNUSED)
    {
      if (max_active < process->pid)
      {
        max_active = process->pid;
      }
    }
  }

  release(&ptable.lock);
  return max_active;
}

int getProcInfo(int pid, struct processInfo * procInfo)
{
  struct proc *p;
  int idx = 0;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      procInfo->numberContextSwitches = p->numberContextSwitches;
      procInfo->ppid = p->parent->pid;
      procInfo->psize = p->sz;
      release(&ptable.lock);
      return 0;
    }
    idx++;
  }
  release(&ptable.lock);

  return -1;
}

int set_burst_time(int n)
{
  struct proc *cur_proc = myproc();
  cur_proc->burst_time = n;
  return 0;
}

int get_burst_time(void)
{
  struct proc *cur_proc = myproc();
  return cur_proc->burst_time;
}

void ps(void)
{
  acquire(&ptable.lock);
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == SLEEPING)
    {
      cprintf("%s\t%d\tSLEEPING\n", p->name, p->pid);
    }
    if (p->state == RUNNING)
    {
      cprintf("%s\t%d\tRUNNING\n", p->name, p->pid);
    }
    if (p->state == RUNNABLE)
    {
      cprintf("%s\t%d\tRUNNABLE\n", p->name, p->pid);
    }
  }
  release(&ptable.lock);
}

void create_kernel_process(const char *name, void (*entrypoint)())
{
  struct proc *np;
  if ((np = allocproc()) == 0)
  {
    panic("Process Allocation Failed!\n");
  }
  if ((np->pgdir = setupkvm()) == 0)
  {
    panic("Page Table Setup Failed!\n");
  }
  np->tf->eip = (uint)entrypoint;

  safestrcpy(np->name, name, sizeof(np->name));

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);
}