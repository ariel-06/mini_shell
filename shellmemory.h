#ifndef SHELLMEMORY_H
#define SHELLMEMORY_H

#include <stdio.h>
#include <pthread.h>

#define MEM_SIZE 1000

// ---- Struct definitions ----
typedef struct pcb {
    int PID;
    int start_index;
    int length;
    int program_counter;
    struct pcb *next;
    struct pcb *prev;
    int job_length_score;
    int priority;
} PCB;

typedef struct queue {
    PCB *head;
    PCB *tail;
} Queue;

extern Queue q;

// ---- Threading globals ----
extern pthread_mutex_t queue_mutex;
extern pthread_cond_t  queue_cond;
extern pthread_mutex_t active_jobs_mutex;
extern pthread_cond_t  active_jobs_cond;

extern int mt_enabled;
extern int active_jobs;
extern int scheduler_running;

// ---- Function declarations ----
void  mem_init(void);
char *mem_get_value(char *var);
void  mem_set_value(char *var, char *value);

int  add_script(FILE *f);
void clean_script(int pid);
void run_queue(void);
void run_queue_sjf(void);
void run_queue_rr(int max_time);
void run_queue_sjf_aging(void);

void start_scheduler_threads(char *policy);
void stop_scheduler_threads(void);

#endif