#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/fixed-point.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;



static fp_float load_average;
static struct list cpu_changed_list;
static struct lock load_average_lock;


/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

static void update_thread_priority(struct thread* t);
static void update_load_average(void);
static void update_recent_cpu(struct thread* t, fp_float cpu_scale);
static void update_changed_cpu_threads(void);
static void update_running_thread_cpu(struct thread* t);
static void update_cpu_for_all_threads(void);


/*
 updates the threads priority according to the formula.
 Use the single list so no need to change queues.
 */
static void update_thread_priority(struct thread* t) {
    int middle_term = fp_to_int(fp_div_int(t->recent_cpu, 4));
    int new_priority = PRI_MAX - middle_term - (t->nice * 2);
    if (new_priority > PRI_MAX) {
        new_priority = PRI_MAX;
    }
    if (new_priority < PRI_MIN) {
        new_priority = PRI_MIN;
    }
    t->priority = new_priority;
}


/*
 Updates the load average
 */
static void update_load_average(void) {
    fp_float lhs_scale = fp_div(int_to_fp(59), int_to_fp(60));
    fp_float lhs = fp_mul(load_average, lhs_scale);
    
    fp_float rhs_scale = fp_div(int_to_fp(1), int_to_fp(60));
    int num_ready_threads = list_size(&ready_list);
    if (thread_current() != idle_thread) {
        num_ready_threads++;
    }
    fp_float rhs = fp_mul_int(rhs_scale, num_ready_threads);
    load_average = fp_add(lhs, rhs);
}


/*
 updates the recent_cpu value for a thread
 */
static void update_recent_cpu(struct thread* t, fp_float cpu_scale) {
    fp_float lhs = fp_mul(cpu_scale, t->recent_cpu);
    t->recent_cpu = fp_add_int(lhs, t->nice);
}


/*
 updates CPU for all threads, called once every second
 */
static void update_cpu_for_all_threads(void) {
    fp_float numerator = fp_mul_int(load_average, 2);
    fp_float denominator = fp_add_int(numerator, 1);
    fp_float cpu_scale = fp_div(numerator, denominator);
    
    struct list_elem* curr = list_head(&all_list);
    struct list_elem* tail = list_tail(&all_list);
    while (true) {
        curr = list_next(curr);
        if (curr == tail) break;
        struct thread* t = list_entry(curr, struct thread, allelem);
        update_recent_cpu(t, cpu_scale);
        if (t->cpu_has_changed == false) {
            t->cpu_has_changed = true;
            list_push_back(&cpu_changed_list, &t->cpu_list_elem);
        }
    }
}


/*
 updates the priorities for the threads whos cpu changed
 */
static void update_changed_cpu_threads(void) {
    struct list_elem* curr = list_head(&cpu_changed_list);
    struct list_elem* tail = list_tail(&cpu_changed_list);
    while (true) {
        curr = list_next(curr);
        if (curr == tail) break;
        ASSERT(curr != NULL);
        struct thread* t = list_entry(curr, struct thread, cpu_list_elem);
        update_thread_priority(t);
        list_remove(&t->cpu_list_elem);
        t->cpu_has_changed = false;
    }
}

/*
 incriments the running threads recent cpu by 1
 */
static void update_running_thread_cpu(struct thread* t) {
    t->recent_cpu = fp_add_int(t->recent_cpu, 1);
    if (t->cpu_has_changed == false) {
        t->cpu_has_changed = true;
        list_push_back(&cpu_changed_list, &t->cpu_list_elem);
    }
}



/* 
 Function: thread_init
 --------------------------------------------------------------------
   Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

 
   It is not safe to call thread_current() until this function
   finishes.
 --------------------------------------------------------------------
 */
void thread_init (void) 
{
    ASSERT (intr_get_level () == INTR_OFF);
    
    /*Here we initialize the thread system, this is a good place to add any
     initialization code we think is necessary */
    lock_init (&tid_lock);
    list_init (&ready_list);
    list_init (&all_list);
    list_init (&cpu_changed_list);
    lock_init (&load_average_lock);
    load_average = 0;
    
    /* Set up a thread structure for the running thread. */
    initial_thread = running_thread (); /*recall that initial_thread is the thread running init.main.c*/
    init_thread (initial_thread, "main", PRI_DEFAULT);
    initial_thread->status = THREAD_RUNNING;
    initial_thread->tid = allocate_tid ();
}




/* 
 Function: thread_start
 --------------------------------------------------------------------
   Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. 
 --------------------------------------------------------------------
 */
void thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}






/* 
 Function: thread_tick
 --------------------------------------------------------------------
   Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context.
 --------------------------------------------------------------------
 */
void thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;
    
    
    if (thread_mlfqs) {
        //here we update load average and cpu when necessary
        if (t != idle_thread) {
            update_running_thread_cpu(thread_current());
        }
        if (timer_ticks () % TIMER_FREQ == 0) {
            //update load average
            //update cpu for all threads
            update_load_average();
            update_cpu_for_all_threads();
        }
        if (timer_ticks() % 4) {
            //here calculate priority for each thread in cpu_changed_list
            update_changed_cpu_threads();
        }
    }
    
    
    

  /* Enforce preemption. */
    if (++thread_ticks >= TIME_SLICE) {
         intr_yield_on_return (); 
    }
}





/* 
 --------------------------------------------------------------------
 Prints thread statistics. 
 --------------------------------------------------------------------
 */
void thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}





/*
 Function: thread_create
 --------------------------------------------------------------------
   Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. 
 --------------------------------------------------------------------
 */
tid_t thread_create (const char *name, int priority, thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  t->nice = thread_current()->nice;

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
    thread_unblock (t);
    
    

  return tid;
}




/* 
 Function: thread_block
 --------------------------------------------------------------------
   Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. 
 --------------------------------------------------------------------
 */
void thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}






/* 
 --------------------------------------------------------------------
 Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. 
 --------------------------------------------------------------------
 */
void thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
    if (old_level == INTR_ON && t->priority > thread_current()->priority) {
        thread_yield();
    } //might be able to remove this and call thread_yield on my own after call to thread_unblock in sema_up and thread_create, which are only places function gets called. 
    intr_set_level (old_level);
}






/* 
 --------------------------------------------------------------------
 Returns the name of the running thread. 
 --------------------------------------------------------------------
 */
const char * thread_name (void) 
{
  return thread_current ()->name;
}






/* 
 --------------------------------------------------------------------
 Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. 
 --------------------------------------------------------------------
 */
struct thread * thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}




/* 
 --------------------------------------------------------------------
 Returns the running thread's tid. 
 --------------------------------------------------------------------
 */
tid_t thread_tid (void) {
  return thread_current ()->tid;
}





/* 
 --------------------------------------------------------------------
 Deschedules the current thread and destroys it.  Never
   returns to the caller.
 --------------------------------------------------------------------
 */
void thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
    ASSERT(thread_current() != NULL);
  list_remove (&thread_current()->allelem);
    if (thread_mlfqs) {
        if (thread_current()->cpu_has_changed) {
            list_remove(&thread_current()->cpu_list_elem);
        }
    }
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}




/* 
 Function: thread_yield
 --------------------------------------------------------------------
   Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. 
 --------------------------------------------------------------------
 */
void thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ()); 

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY; 
  schedule ();
  intr_set_level (old_level); 
}



/*
 Function: thread_foreach
 --------------------------------------------------------------------
 Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. 
 --------------------------------------------------------------------
 */
void thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}




/* 
 Function: thread_set_priority
 --------------------------------------------------------------------
 Sets the current thread's priority to NEW_PRIORITY.
 --------------------------------------------------------------------
 */
void thread_set_priority (int new_priority) 
{
    if (intr_context()) {
        thread_current ()->original_priority_info.priority = new_priority;
        shed_priority();
    } else {
        enum intr_level old_level = intr_disable();
        thread_current ()->original_priority_info.priority = new_priority;
        shed_priority();
        thread_yield();
        intr_set_level(old_level);
    }
}



/* 
 --------------------------------------------------------------------
 Returns the current thread's priority. 
 --------------------------------------------------------------------
 */
int thread_get_priority (void)
{
    return thread_current()->priority;
}




/* 
 --------------------------------------------------------------------
 Sets the current thread's nice value to NICE. 
 --------------------------------------------------------------------
 */
void thread_set_nice (int nice UNUSED) 
{
    enum intr_level old_level = intr_disable();
    thread_current()->nice = nice;
    update_thread_priority(thread_current());
    thread_yield();
    intr_set_level(old_level);
}





/*
 --------------------------------------------------------------------
 Returns the current thread's nice value. 
 --------------------------------------------------------------------
 */
int thread_get_nice (void) 
{
    return thread_current()->nice;
}





/* 
 --------------------------------------------------------------------
 Returns 100 times the system load average. 
 --------------------------------------------------------------------
 */
int
thread_get_load_avg (void)
{

    lock_acquire(&load_average_lock);
    int val = fp_to_int (fp_mul_int (load_average, 100));
    lock_release(&load_average_lock);
  return val;
}





/* 
 --------------------------------------------------------------------
 Returns 100 times the current thread's recent_cpu value. 
 --------------------------------------------------------------------
 */
int thread_get_recent_cpu (void) 
{
    return fp_to_int (fp_mul_int (thread_current()->recent_cpu, 100));
}



/* 
 --------------------------------------------------------------------
 Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. 
 --------------------------------------------------------------------
 */
static void idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}





/* 
 --------------------------------------------------------------------
 Function used as the basis for a kernel thread. 
 --------------------------------------------------------------------
 */
static void kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}




/* 
 --------------------------------------------------------------------
 Returns the running thread. 
 --------------------------------------------------------------------
 */
struct thread * running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}





/*
 --------------------------------------------------------------------
 Returns true if T appears to point to a valid thread.
 --------------------------------------------------------------------
 */
static bool is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}





/* 
 --------------------------------------------------------------------
 Does basic initialization of T as a blocked thread named
   NAME. 
 --------------------------------------------------------------------
 */
static void init_thread (struct thread *t, const char *name, int priority)
{
    enum intr_level old_level;
    
    ASSERT (t != NULL);
    ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT (name != NULL);
    
    memset (t, 0, sizeof *t);
    t->status = THREAD_BLOCKED;
    strlcpy (t->name, name, sizeof t->name);
    t->stack = (uint8_t *) t + PGSIZE;
    t->priority = priority;
    t->magic = THREAD_MAGIC;
    
    if (thread_mlfqs) {
        if (t == initial_thread) {
            //initial thread initialization
            t->nice = 0;
            t->recent_cpu = 0;
        } else {
            //regular thread init
            t->nice = thread_current()->nice;
            t->recent_cpu = thread_current()->recent_cpu;
        }
        update_thread_priority(t);
    }
    
    list_init(&(t->locks_held));
    t->original_priority_info.priority = priority;
    t->original_priority_info.holder = NULL; //indicates orig priority package.
    list_push_front(&(t->locks_held), &(t->original_priority_info.elem));
    t->lock_waiting_on = NULL;

    
    old_level = intr_disable ();
    list_push_back (&all_list, &t->allelem);
    intr_set_level (old_level);
}





/* 
 --------------------------------------------------------------------
 Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. 
 --------------------------------------------------------------------
 */
static void * alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}





/* 
 --------------------------------------------------------------------
 Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. 
 --------------------------------------------------------------------
 */
static struct thread * next_thread_to_run (void) 
{
    
    if (list_empty (&ready_list)) {
        return idle_thread;
    } else {
        struct thread* next_thread = get_highest_priority_thread(&ready_list, true);
        ASSERT(next_thread != NULL);
        return next_thread;
    }
}






/* 
 --------------------------------------------------------------------
 Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. 
 --------------------------------------------------------------------
 */
void thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}





/* 
 --------------------------------------------------------------------
 Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. 
 --------------------------------------------------------------------
 */
static void schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}





/* 
 --------------------------------------------------------------------
 Returns a tid to use for a new thread. 
 --------------------------------------------------------------------
 */
static tid_t allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}


/*
 --------------------------------------------------------------------
 LP: Donate priority function. Note, because this function is only
 called from sema_down, and sema_down disables interrupts, we do not
 have to do it here as well. 
 --------------------------------------------------------------------
 */
void donate_priority(void) {
    struct thread* currThread = thread_current();
    while (currThread->lock_waiting_on != NULL) {
        struct lock* currLock = currThread->lock_waiting_on;
        if (currThread->priority > currLock->priority) {
            currLock->priority = currThread->priority;
        }
        if (currThread->priority > currLock->holder->priority) {
            currLock->holder->priority = currThread->priority;
        }
        currThread = currThread->lock_waiting_on->holder;
    }
}

/*
 --------------------------------------------------------------------
 LP: Final steps to release a lock and remove any donated
 priority associated with the lock. 
 --------------------------------------------------------------------
 */
void shed_priority() {
    struct list_elem* curr = list_head(&(thread_current()->locks_held));
    struct list_elem* tail = list_tail(&(thread_current()->locks_held));
    int highest_remaining_priority = PRI_MIN;
    while (true) {
        curr = list_next(curr);
        if(curr == tail) break;
        struct lock* currLock = list_entry(curr, struct lock, elem);
        if (currLock->priority > highest_remaining_priority) {
            highest_remaining_priority = currLock->priority;
        }
    }
    thread_current()->priority = highest_remaining_priority;
}


/*
 --------------------------------------------------------------------
 LP: finds the highest priority thread in the list, removes it from the
 list, and then returns a pointer to the thread.
 --------------------------------------------------------------------
 */
struct thread* get_highest_priority_thread(struct list* list, bool should_remove) {
    
    struct list_elem* curr = list_head(list);
    struct list_elem* tail = list_tail(list);
    struct thread* currHighest = NULL;
    while (true) {
        curr = list_next(curr);
        if (curr == tail) break;
        struct thread* currThread = list_entry(curr, struct thread, elem);
        if (currHighest == NULL) currHighest = currThread;
        if (currThread->priority > currHighest->priority) {
            currHighest = currThread;
        }
    }
    if (currHighest != NULL && should_remove) list_remove(&(currHighest->elem));
    return currHighest;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
