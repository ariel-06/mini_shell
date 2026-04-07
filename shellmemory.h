#ifndef SHELLMEMORY_H
#define SHELLMEMORY_H

#include <stdio.h>
#include <pthread.h>

//#define MEM_SIZE 1000
//#define FRAME_STORE_SIZE 333;
//#define FRAME_SIZE 3;

// Struct definitions

//type to store information about a script, 
//acts as a node in a linked list (ready queue)
typedef struct pcb {
    int PID;
    int start_index;
    int length;
    int program_counter;
    struct pcb *next;
    struct pcb *prev;
    int job_length_score;
    int priority;
    char* filename;
    int* pages; 
    int num_pages;
} PCB;

typedef struct {
    PCB *pcbs[10];
    int page_num;
    int last_used; //timestamp of last access
} Frame_Owners;

extern Frame_Owner frame_owners[];

//ready queue, implemented as a classic doubly linked list
typedef struct queue {
    PCB *head;
    PCB *tail;
} Queue;

extern Queue q;

// Threading globals
extern pthread_mutex_t queue_mutex;
extern pthread_cond_t  queue_cond;
extern pthread_mutex_t active_jobs_mutex;
extern pthread_cond_t  active_jobs_cond;

extern int mt_enabled;
extern int active_jobs;
extern int scheduler_running;
extern int quit_called;
extern int scheduler_active;
extern int batch_running;
extern pthread_t workers[2];
int is_worker_thread(void);

// Function declarations
void  mem_init(void);
char *mem_get_value(char *var);
void  mem_set_value(char *var, char *value);

int  add_script(FILE *f, char* filename);
void clean_script(int pid);
void run_queue(void);
void run_queue_sjf(void);
void run_queue_rr(int max_time);
void run_queue_sjf_aging(void);

void start_scheduler_threads(char *policy);
void stop_scheduler_threads(void);

#endif
