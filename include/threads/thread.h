#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

#include "threads/interrupt.h"
/* Project 1 : priority */
#include "threads/synch.h"
/* Project 1 : priority */
#ifdef VM
#include "vm/vm.h"
#endif
#ifdef EFILESYS
#include "filesys/directory.h"
#endif

/* States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING, /* Running thread. */
  THREAD_READY,   /* Not running but ready to run. */
  THREAD_BLOCKED, /* Waiting for an event to trigger. */
  THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* Project 1 : mlfqs */
#define NICE_MIN -20
#define NICE_DEFAULT 0
#define NICE_MAX 20
/* Project 1 : mlfqs */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                 /* Thread identifier. */
  enum thread_status status; /* Thread state. */
  char name[16];             /* Name (for debugging purposes). */
  int priority;              /* Priority. */

  /* Project 1 : alarm-clock */
  int64_t wakeup_tick; /* Tick till wakeup */
  /* Project 1 : alarm-clock */

  /* Project 1 : priority */
  int init_priority;
  struct lock *wait_on_lock;
  struct list donations;
  struct list_elem donation_elem;
  /* Project 1 : priority */

  /* Project 1 : mlfqs */
  int nice;
  int recent_cpu;
  struct list_elem total_elem;
  /* Project 1 : mlfqs */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /* List element. */

#ifdef USERPROG
  struct list child_list;
  struct process_desc *proc_desc;
  struct file *running_file;
  /* Owned by userprog/process.c. */
  uint64_t *pml4; /* Page map level 4 */
#endif
#ifdef VM
  /* Table for whole virtual memory owned by thread. */
  struct supplemental_page_table spt;
#endif
#ifdef EFILESYS
  struct dir *cur_dir;
#endif

  /* Owned by thread.c. */
  struct intr_frame tf; /* Information for switching */
  unsigned magic;       /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/* Project 1 : alarm-clock */
void thread_sleep(int64_t tick);
void thread_wakeup(int64_t wakeup_tick);
int64_t thread_get_min_wakeup_tick(void);
void thread_check_then_update_min_wakeup_tick(int64_t new_tick);
/* Project 1 : alarm-clock */
/* Project 1 : priority */
bool thread_cmp_more_priority(const struct list_elem *a_,
                              const struct list_elem *b_, void *aux UNUSED);
bool thread_donation_cmp_more_priority(const struct list_elem *a_,
                                       const struct list_elem *b_,
                                       void *aux UNUSED);
void thread_check_then_yield(void);
void thread_remove_donations(struct lock *lock);
void thread_refresh_donations(void);
void thread_donate_priority(void);
/* Project 1 : priority */

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);
/* Project 1 : priority */
void thread_increase_one_current_recent_cpu(void);
void thread_recalculate_mlfqs_priority_to_all_threads(void);
void thread_recalculate_recent_cpu_to_all_threads(void);
/* Project 1 : priority */

void do_iret(struct intr_frame *tf);

#endif /* threads/thread.h */
