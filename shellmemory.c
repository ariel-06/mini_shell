#ifdef DEBUG
#   define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#   define debug(...)
#   define NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "shellmemory.h"
#include "interpreter.h"
#include "shell.h"

struct memory_struct {
    char *var;
    char *value;
};

//file numbers that are available, 1 for available, 0 for unavailable
int avail_file_numbers[3] = {1, 1, 1};

struct memory_struct shellmemory[MEM_SIZE];
char current_policy[16];     // stores active scheduling policy

//new types to store script based on assumptions in 1.2.1
typedef struct line {
    //stores pointer each line
    char *content;

    //which script file does this line belong to options (0,1,2,3)
    //0 if there is no line, else corresponds to script number
    int owner;
} Line; 


Line script_memory[1000];
Queue q = {NULL, NULL};

//Multi threading variables
pthread_t workers[2];
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;           //protects the queue and script memory
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;              //workers wait on this when the queue is empty
pthread_mutex_t active_jobs_mutex = PTHREAD_MUTEX_INITIALIZER;     //protects the active job counter
pthread_cond_t active_jobs_cond = PTHREAD_COND_INITIALIZER;        //the main thread waits on this in multithread_execution
pthread_mutex_t shellmem_mutex = PTHREAD_MUTEX_INITIALIZER;        //protects shell memory

int mt_enabled = 0;          // becomes 1 when MT is activated
int scheduler_running = 0;   // controls worker loop
int active_jobs = 0;         // number of active scripts being processed




// Helper functions
int match(char *model, char *var) {
    int i, len = strlen(var), matchCount = 0;
    for (i = 0; i < len; i++) {
        if (model[i] == var[i])
            matchCount++;
    }
    if (matchCount == len) {
        return 1;
    } else
        return 0;
}

// Shell memory functions

void mem_init() {
    int i;
    for (i = 0; i < MEM_SIZE; i++) {
        shellmemory[i].var = "none";
        shellmemory[i].value = "none";
    }

}

//Caller must not hold queue_mutex, locks internally
//Signals queue_cond so that one waiting worker wakes up
void enqueue(PCB *node) {
    node->next = NULL;
    node->prev = NULL;

    pthread_mutex_lock(&queue_mutex);
    if (q.head == NULL) {
        q.head = node;
        q.tail = node;
    } else {
        q.tail->next = node;
        node->prev = q.tail;
        q.tail = node;
    }
    pthread_cond_signal(&queue_cond);   //Wake one waiting worker
    pthread_mutex_unlock(&queue_mutex);
}

//Locks queue_mutex intenally. Used by single-threaded schedulers.
PCB* dequeue() {
    pthread_mutex_lock(&queue_mutex);
    PCB *node = q.head;
    if (node == NULL) {
        pthread_mutex_unlock(&queue_mutex);
        return NULL;
    }
    if (node == q.tail) {
        q.head = NULL;
        q.tail = NULL;
    } else {
        q.head = node->next;
        q.head->prev = NULL;
    }
    node->next = NULL;
    node->prev = NULL;
    pthread_mutex_unlock(&queue_mutex);
    return node;
}

// Caller must already hold queue_mutex
static PCB* dequeue_unlocked() {
    PCB *node = q.head;
    if (node == NULL) return NULL;
    if (node == q.tail) {
        q.head = NULL;
        q.tail = NULL;
    } else {
        q.head = node->next;
        q.head->prev = NULL;
    }
    node->next = NULL;
    node->prev = NULL;
    return node;
}

int add_script (FILE *file){
    // failure check
    int pid = 0;
    for (int i = 0; i < 3; i++){
        if (avail_file_numbers[i]) {
            pid = i + 1;
            avail_file_numbers[i] = 0;
            break;
        }
    }

    if (!pid)
        return -1;

    //finds next available slot
    int start = 0;
    while(start < 1000 && script_memory[start].owner != 0)
        start++;


    PCB *pcb = (PCB*)malloc(sizeof(PCB));
    
    pcb->PID = pid;
    pcb->start_index = start;
    pcb->length = 0;
    pcb->program_counter = 0;
    pcb->next = NULL;
    pcb->prev = NULL;
    pcb->job_length_score = 0;
    pcb->priority = 0;


    char buffer[100];
    int i = start;
    while (fgets(buffer, 100, file) != NULL){
        if (i >= 1000){
            //if we reach end of memory before the whole file is stored:
            //delete from memory and return with failure
            for (int j = i; j >= start; j--){
                free(script_memory[j].content);
                script_memory[j].content = NULL;
                script_memory[j].owner = 0;
            }

            avail_file_numbers[pid - 1] = 1;
            free(pcb);
            return -1;
        }


        if (script_memory[i].owner == 0){
        //double check for overriding before storing line 
            char *line = strdup(buffer);
            script_memory[i].content = line;
            script_memory[i].owner = pid;
        }

        i++;
    }

    pcb->length = i - start;
    pcb->job_length_score = pcb->length;

    //Enqueue the PCB (acquires and releases queue_mutex internally)
    enqueue(pcb);

    // Increment active jobs after enqueue to respect locking order
    pthread_mutex_lock(&active_jobs_mutex);
    active_jobs++;
    pthread_mutex_unlock(&active_jobs_mutex);

    return pcb->PID;

}

void run_queue(){ //default run queue used in source and fcfs
    PCB *current = q.head;
    while (current != NULL){
        while (current->program_counter < current->length){
            parseInput(script_memory[current->program_counter + current->start_index].content);
            current->program_counter++;

        }
        current = current->next;
    }
}

void sort_queue_sjf(){
    //copy linked list into array for sorting  
    PCB *pcb_array[3];  
    PCB *current = q.head;
    int index = 0;
    while (current != NULL && index < 3){
        pcb_array[index] = current;
        index++;
        current = current->next;
    }

    for (int i = 0; i < index; i++){
        for (int j = 0; j < index; j++){
            if (pcb_array[i]->job_length_score < pcb_array[j]->job_length_score){
                PCB *temp = pcb_array[i];
                pcb_array[i] = pcb_array[j];
                pcb_array[j] = temp;
            }
        }
    }

    //copy the sorted array back into the linked list
    for (int i = 1; i < index; i++){
        pcb_array[i]->prev = pcb_array[i - 1];
    }

    for (int i = 0; i < index - 1; i++){
        pcb_array[i]->next = pcb_array[i + 1];
    }

    q.head = pcb_array[0];
    q.tail = pcb_array[index - 1];

    q.head->prev = NULL;
    q.tail->next = NULL;

}

void run_queue_sjf() {
    // If batch script hasn't run yet, run it first
    PCB *current = q.head;
    while (current != NULL) {
        if (current->priority == 1) {
            current->priority = 0;

            while (current->program_counter < current->length) {
                parseInput(script_memory[current->program_counter + current->start_index].content);
                current->program_counter++;
            }

            // remove it from queue after completion
            dequeue();
            break;
        }
        current = current->next;
    }

    sort_queue_sjf();
    run_queue();
}

void run_queue_rr(int max_time) {
    int time = 0;
    while (q.head != NULL){
        PCB *current = dequeue();
        time = 0;
        while (current->program_counter < current->length && time < max_time){
            parseInput(script_memory[current->program_counter + current->start_index].content);
            current->program_counter++;
            time++;
        }
        time = 0;

        if (current->program_counter < current->length){
            enqueue(current);
        }
    }
}

void run_queue_sjf_aging(){
    while (q.head != NULL){

        // Force batch process to run first (only once)
        if (q.head->priority == 1) {
            PCB *to_run = dequeue();
            to_run->priority = 0;
            // Aging scheduler runs ONE line per time slice
            parseInput(script_memory[to_run->program_counter + to_run->start_index].content);
            to_run->program_counter++;
            // Decrease job_length_score of remaining processes
            PCB *current = q.head;
            while (current != NULL){
                if (current->job_length_score > 0)
                    current->job_length_score--;
                current = current->next;
            }

            if (to_run->program_counter < to_run->length){
                enqueue(to_run);
            }

            continue; // Skip normal scheduling this iteration
        }

        // Normal scheduling
        sort_queue_sjf();
        PCB *to_run = dequeue();

        PCB *current = q.head;

        // subtract 1 from job_length_score of all programs in queue
        while (current != NULL){
            if (current->job_length_score > 0)
                current->job_length_score--;
            current = current->next;
        }

        // run one line
        parseInput(script_memory[to_run->program_counter + to_run->start_index].content);
        to_run->program_counter++;

        if (to_run->program_counter < to_run->length){
            enqueue(to_run);
        } 
    }
}


void clean_script(int pid){
    // Hold queue_mutex so no other thread can concurrently modify the queue
    pthread_mutex_lock(&queue_mutex);
    PCB *current = q.head;

    for (int i = 0; i < 1000; i++){
        if (script_memory[i].owner == pid){
            script_memory[i].owner = 0;
            free(script_memory[i].content);
            script_memory[i].content = NULL;
        }
    }

    while (current != NULL){
        if (current->PID == pid){
            if (current == q.head)
                q.head = current->next;
            else 
                current->prev->next = current->next;


            if (current == q.tail)
                q.tail = current->prev;
            else 
                current->next->prev = current->prev;

            free(current);
            break;
        } else 
            current = current->next;
    }

    avail_file_numbers[pid - 1] = 1;
    pthread_mutex_unlock(&queue_mutex);
}


// Set key value pair
void mem_set_value(char *var_in, char *value_in) {
    // Same logic (protecting memory)
    pthread_mutex_lock(&shellmem_mutex);
    for (int i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, var_in) == 0) {
            free(shellmemory[i].value);
            shellmemory[i].value = strdup(value_in);
            pthread_mutex_unlock(&shellmem_mutex);
            return;
        }
    }
    for (int i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, "none") == 0) {
            shellmemory[i].var   = strdup(var_in);
            shellmemory[i].value = strdup(value_in);
            pthread_mutex_unlock(&shellmem_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&shellmem_mutex);
}

//get value based on input key
char *mem_get_value(char *var_in) {
    // Same logic (protecting memory)
    pthread_mutex_lock(&shellmem_mutex);
    for (int i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, var_in) == 0) {
            char *val = strdup(shellmemory[i].value);
            pthread_mutex_unlock(&shellmem_mutex);
            return val;
        }
    }
    pthread_mutex_unlock(&shellmem_mutex);
    return NULL;
}

/* 
Loop:
   1. Lock queue_mutex and wait on queue_cond until the queue is non-empty
      or scheduler_running becomes 0.
   2. If shutting down and the queue is empty, exit the loop.
   3. Dequeue one PCB while still holding the lock, then release it.
   4. Execute according to current_policy:
        FCFS/SJF: run the entire job to completion in one shot.
        RR/RR30: run at most one time slice; re-enqueue if not done.
   5. When a job finishes, call clean_script() then decrement active_jobs
      and signal active_jobs_cond so multithread_execution() can wake.
*/
void *worker_function(void *arg) {
    while (1) {
        pthread_mutex_lock(&queue_mutex);

        // Wait while the queue is empty and not told to stop
        while (q.head == NULL && scheduler_running)
            pthread_cond_wait(&queue_cond, &queue_mutex);

        //Shutdown if no work left and scheduler is stopping
        if (!scheduler_running && q.head == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        //Take the next PCB while we still hold the lock
        PCB *pcb = dequeue_unlocked();
        pthread_mutex_unlock(&queue_mutex);

        if (pcb == NULL) continue;

        //FCFS or SJF
        if (strcmp(current_policy, "FCFS") == 0 || strcmp(current_policy, "SJF") == 0) {
            while (pcb->program_counter < pcb->length) {
                parseInput(script_memory[pcb->start_index + pcb->program_counter].content);
                pcb->program_counter++;
            }
            int pid = pcb->PID;
            clean_script(pid);

            // Decreasing the active_jobs counter while preserving the lock order
            pthread_mutex_lock(&active_jobs_mutex);
            active_jobs--;
            pthread_cond_signal(&active_jobs_cond);
            pthread_mutex_unlock(&active_jobs_mutex);
        }

        // RR or RR30
        else if (strcmp(current_policy, "RR") == 0 || strcmp(current_policy, "RR30") == 0) {
            int slice;
            if (strcmp(current_policy, "RR30") == 0) {
                slice = 30;
            } else {
                slice = 2;
            }
            int time  = 0;
            while (pcb->program_counter < pcb->length && time < slice) {
                parseInput(script_memory[pcb->start_index + pcb->program_counter].content);
                pcb->program_counter++;
                time++;
            }

            if (pcb->program_counter < pcb->length) {
                enqueue(pcb); // re-queue for next slice (enqueue locks internally)
            } else {
                //Job finished
                int pid = pcb->PID;
                clean_script(pid); 

                pthread_mutex_lock(&active_jobs_mutex);
                active_jobs--;
                pthread_cond_signal(&active_jobs_cond);
                pthread_mutex_unlock(&active_jobs_mutex);
            }
        }
    }

    return NULL;
}

//Updates current_policy
//Starts worker threads if not already running
//Updating the policy even when threads are alive
void start_scheduler_threads(char *policy) {
    strncpy(current_policy, policy, sizeof(current_policy) - 1);
    current_policy[sizeof(current_policy) - 1] = '\0';

    if (scheduler_running)
        return; // threads already alive

    scheduler_running = 1;
    pthread_create(&workers[0], NULL, worker_function, NULL);
    pthread_create(&workers[1], NULL, worker_function, NULL);
}

//Signals both workers to exit and joins them
void stop_scheduler_threads() {
    if (!scheduler_running)
        return;

    pthread_mutex_lock(&queue_mutex);
    scheduler_running = 0;
    pthread_cond_broadcast(&queue_cond);    //So idle workers wake up and exit their loops
    pthread_mutex_unlock(&queue_mutex);

    pthread_join(workers[0], NULL);
    pthread_join(workers[1], NULL);
}