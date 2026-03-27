#ifdef DEBUG
#define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug(...)
#define NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "shellmemory.h"
#include "interpreter.h"
#include "shell.h"

struct memory_struct
{
    char *var;
    char *value;
};

// file numbers that are available, 1 for available, 0 for unavailable
// each index corresponds to the pid index + 1
// extra space for nested exec inside the batch
int avail_file_numbers[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

struct memory_struct shellmemory[MEM_SIZE];
char current_policy[16]; // stores active scheduling policy

// type to store a line in a script
typedef struct line {
    // stores pointer each line
    char *content;

    // which script file does this line belong to options (0,1,2,3)
    // 0 if there is no line, else corresponds to script number
    int owner;
} Line;

// array containing all lines from all scripts that are in memory,
// max size 1000 lines as per assignment specification
Line script_memory[1000];

typedef struct page_entry {
    struct page_entry* next;
    int page_num;

} Page_Entry;

typedef struct pages {
    Page_Entry* head;
    Page_Entry* tail;
} Pages;

Pages ps = {NULL, NULL};

// initial ready queue (head = NULL and tail = NULL because the queue is empty)
Queue q = {NULL, NULL};

// Multi threading variables
pthread_t workers[2];
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;       // protects the queue and script memory
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;          // workers wait on this when the queue is empty
pthread_mutex_t active_jobs_mutex = PTHREAD_MUTEX_INITIALIZER; // protects the active job counter
pthread_cond_t active_jobs_cond = PTHREAD_COND_INITIALIZER;    // the main thread waits on this in multithread_execution
pthread_mutex_t shellmem_mutex = PTHREAD_MUTEX_INITIALIZER;    // protects shell memory

int mt_enabled = 0;        // becomes 1 when MT is activated
int scheduler_running = 0; // controls worker loop
int active_jobs = 0;       // number of active scripts being processed
int scheduler_active = 0;
int batch_running = 0;

int is_worker_thread()
{
    return (pthread_self() == workers[0] || pthread_self() == workers[1]);
}

// Helper functions
int match(char *model, char *var){
    int i, len = strlen(var), matchCount = 0;
    for (i = 0; i < len; i++)
    {
        if (model[i] == var[i])
            matchCount++;
    }
    if (matchCount == len)
    {
        return 1;
    }
    else
        return 0;
}

int page_dequeue(void) {
    if (ps.head == NULL){
        return -1; //fail bc no pages available
    }
    Page_Entry *temp = ps.head;
    ps.head = ps.head->next;
    temp->next = NULL;
    int n = temp->page_num;
    free(temp);
    return n;
}

void page_enqueue(int page_num){//we can add to the front bc it doesn't matter what order the pages are in 
    Page_Entry *new_pg = malloc(sizeof(Page_Entry));
    new_pg->page_num = page_num;
    new_pg->next = ps.head;
    if (new_pg->next == NULL)
        ps.tail = new_pg;
    ps.head = new_pg;

}

// Shell memory functions
void mem_init(){
    int i;
    for (i = 0; i < MEM_SIZE; i++)
    {
        shellmemory[i].var = "none";
        shellmemory[i].value = "none";
    }


    //initializes page queue to have all available pages
    for (int i = 0; i < 333; i++){
        page_enqueue(i);
    }
}

// Basic enqueue for doubly linked list
// Caller must not hold queue_mutex, locks internally
// Signals queue_cond so that one waiting worker wakes up
void enqueue(PCB *node){
    node->next = NULL;
    node->prev = NULL;

    pthread_mutex_lock(&queue_mutex);
    if (q.head == NULL)
    {
        q.head = node;
        q.tail = node;
    }
    else
    {
        q.tail->next = node;
        node->prev = q.tail;
        q.tail = node;
    }
    pthread_cond_signal(&queue_cond); // Wake one waiting worker
    pthread_mutex_unlock(&queue_mutex);
}

// Basic dequeue for double linked list
// Locks queue_mutex internally. Used by single-threaded schedulers.
PCB *dequeue(){
    pthread_mutex_lock(&queue_mutex);
    // removes the first node in the queue and returns it
    PCB *node = q.head;
    if (node == NULL)
    {
        pthread_mutex_unlock(&queue_mutex);
        return NULL; // do nothing bc the queue is already empty
    }
    if (node == q.tail)
    {
        q.head = NULL;
        q.tail = NULL;
    }
    else
    {
        q.head = node->next;
        q.head->prev = NULL;
    }
    node->next = NULL;
    node->prev = NULL;
    pthread_mutex_unlock(&queue_mutex);
    return node;
}

// Caller must already hold queue_mutex
PCB *dequeue_unlocked()
{
    PCB *node = q.head;
    if (node == NULL)
    {
        return NULL;
    }

    if (node == q.tail)
    {
        q.head = NULL;
        q.tail = NULL;
    }
    else
    {
        q.head = node->next;
        q.head->prev = NULL;
    }
    node->next = NULL;
    node->prev = NULL;
    return node;
}


int add_script(FILE *file, char *filename){
    /*
    adds the contents of the given file as a script in memory if there is enough space
    if there is not enough space, then nothing is added.

    inputs:
    FILE *file: pointer to the file that we want to add as a script

    returns: 0 on success, -1 on failure

    */
    // iterates through avail_file_numbers to find available pid
    int pid = 0;
    for (int i = 0; i < 10; i++)
    { // index i represents an available pid
        if (avail_file_numbers[i])
        {
            pid = i + 1;               // the available pid is i + 1
            avail_file_numbers[i] = 0; // reserve the available pid by making it no longer available
            // if an available one is found, no need to look further -> break
            break;
        }
    }

    // fail if no available pid
    if (!pid)
        return -1;

    PCB *cur = q.head;
    PCB *pcb = (PCB *)malloc(sizeof(PCB));
    int duplicate = 0;
    int pages[500] = {0};

    // check to see if the file is already a stored program, if yes use the same pages
    while (cur != NULL){
        if (strcmp(filename, cur->filename) == 0){
            pcb->PID = pid;
            pcb->length = cur->length;
            pcb->next = NULL;
            pcb->prev = NULL;
            pcb->job_length_score = cur->job_length_score;
            pcb->priority = 0;
            pcb->filename = strdup(cur->filename);
            duplicate = 1;

            for (int i = 0; i < 500; i++){
                pages[i] = cur->pages[i];
            }

            pcb->pages = malloc(sizeof(int) * 500);
            memcpy(pcb->pages, pages, sizeof(int) * 500);
        }

        cur = cur->next;
    }
    //only stores file as new lines in memory if duplciate doesn nto exist
    if (!duplicate){
        // otherwise finds next available page in the page array to store the first lines of the script
        
        // initialize pcb fields to default values
        pcb->PID = pid;           // this is the available pid we found earlier
        pcb->length = 0;          // initial length is 0; this will be updated as lines are stored
        pcb->program_counter = 0; // program counter is 0 because the script has not been ran yet
        pcb->next = NULL;
        pcb->prev = NULL;
        pcb->job_length_score = 0; // same as length, will be updated later
        pcb->priority = 0;         // priority flag to distinguish the PCB created from background execution
        pcb->num_pages = 0; 
        pcb->pages = malloc(sizeof(int) * 500);
        pcb->filename = strdup(filename);

        // each line has max 100 chars
        char buffer[100];
        // tries to store each line in the file to slots in the line array
        while (fgets(buffer, 100, file) != NULL){
            if (ps.head == NULL){
                // if we reach end of line memory before the whole file is stored:
                // delete all previously stored lines and return with failure
                for (int i = 0; i < pcb->num_pages; i++){
                    for (int j = 0; j < 3; j++){
                        free(script_memory[pcb->pages[i] * 3 + j].content);
                        script_memory[pcb->pages[i] * 3 + j].content = NULL;
                        script_memory[pcb->pages[i] * 3 + j].owner = 0;
                    }
                }

                // the pcb cannot be added so make the reserved pid available again
                avail_file_numbers[pid - 1] = 1;
                free(pcb);
                return -1;
            }
        
            //get next available page to store
            int page = page_dequeue();
            pcb->pages[pcb->num_pages] = page;
            pcb->num_pages += 1;

            // otherwise store up to  3 lines from the file into memory
            for (int i = 0; i < 3; i++){
                char *line = strdup(buffer);
                script_memory[page * 3 + i].content = line;
                script_memory[page * 3 + i].owner = pid;
                pcb->length++;

                if (i == 2){
                    break;
                } else if (fgets(buffer, 100, file) == NULL){
                    break;
                } 

            }

        }

        
        pcb->job_length_score = pcb->length;

        // add the new process to end of the ready queue (acquires and releases queue_mutex internally)
        enqueue(pcb);
    }
    // Increment active jobs after enqueue to respect locking order
    pthread_mutex_lock(&active_jobs_mutex);
    active_jobs++;
    pthread_mutex_unlock(&active_jobs_mutex);

    // returns the pid of the program that is added. this is useful when we want to free the memory
    // that is associated with this script
    return pcb->PID;
}

int get_line (PCB *program, int pc){
    int page_number = program->pages[pc / 3];
    int offset = pc % 3;

    return page_number * 3 + offset;
}

void run_queue(){ // default run queue used in source and fcfs
    /*
    starts at the head of the ready queue, runs the entire script at each node, then moves to the next script in
    the ready queue. stops when the last script (the tail) is done running

    inputs: none
    retuns: nothing
    */

    // basic iteration over a linked list
    PCB *current = q.head;
    while (current != NULL)
    {
        while (current->program_counter < current->length)
        {
            // runs the entirety of each program
            parseInput(script_memory[get_line(current, current->program_counter)].content);
            current->program_counter++;
        }
        // moves to next node
        current = current->next;
    }
}

void sort_queue_sjf(){
    /*
    sorts the ready queue in ascending order based on job_length_score
    inputs: none
    returns: nothing
    */

    // copy all pcbs in the ready queue into an array for easier sorting
    // the array has up to three elements but not all of them contain a pcb
    // keep track of last index stored to know where to run sorting algorithm until
    PCB *pcb_array[3];
    PCB *current = q.head;
    int index = 0;
    while (current != NULL && index < 3)
    {
        pcb_array[index] = current;
        index++;
        current = current->next;
    }

    // run bubble sort on the array
    for (int i = 0; i < index; i++)
    {
        for (int j = 0; j < index; j++)
        {
            if (pcb_array[i]->job_length_score < pcb_array[j]->job_length_score)
            {
                PCB *temp = pcb_array[i];
                pcb_array[i] = pcb_array[j];
                pcb_array[j] = temp;
            }
        }
    }

    // copy the sorted array back into the linked list
    for (int i = 1; i < index; i++)
    {
        // fix prev pointers
        pcb_array[i]->prev = pcb_array[i - 1];
    }

    for (int i = 0; i < index - 1; i++)
    {
        // fix next pointers
        pcb_array[i]->next = pcb_array[i + 1];
    }

    // fix head and tail pointers
    q.head = pcb_array[0];
    q.tail = pcb_array[index - 1];

    q.head->prev = NULL;
    q.tail->next = NULL;
}

void run_queue_sjf(){
    /*
    scheduling policy: SJF -> runs all scripts in the ready queue such that scripts with less
    lines runs before scripts with more lines

    inputs: none
    returns: nothing
    */

    // If batch script hasn't run yet, run it first
    PCB *current = q.head;
    while (current != NULL)
    {
        if (current->priority == 1)
        {
            current->priority = 0;

            while (current->program_counter < current->length)
            {
                parseInput(script_memory[get_line(current, current->program_counter)].content);
                current->program_counter++;
            }

            // remove it from queue after completion
            dequeue();
            break;
        }
        current = current->next;
    }

    // sorts the ready queue in ascending order based on number of lines in each script
    sort_queue_sjf();
    // runs the queue from beginning to end
    run_queue();
}

void run_queue_rr(int max_time){
    /*
    scheduling policy: RR & RR30 -> cycles through all the scritps in the ready queue,
    runs each script for max_time number of lines before moving onto the next script in the queue

    inputs:
    int max_time: the number of lines to run before switching to the next program

    returns: nothing
    */

    // init time = 0 because no line has been run yet, increments 1 for each line run after
    int time = 0;
    while (q.head != NULL)
    {
        // while the head exists -> ie while there is still a script to run
        PCB *current = dequeue();
        time = 0;
        while (current->program_counter < current->length && time < max_time)
        {
            // run script until time runs out or until there are no more lines to run
            parseInput(script_memory[get_line(current, current->program_counter)].content);
            current->program_counter++;
            time++;
        }

        // reset time to 0 for the next script to be ran
        time = 0;

        // if the current script is not done running, add it to the back of the ready queue
        if (current->program_counter < current->length)
        {
            enqueue(current);
        }
    }
}

void run_queue_sjf_aging(){
    /*
    scheduling policy: SJF with AGING -> cycles through each program of the ready queue
    which starts off in ascending order of script length. All programs that are waiting to be
    ran gets aged, and will be forwarded to the front of the queue when the job length score
    decreases below the current program being ran

    inputs: none
    returns: nothing
    */

    // runs ready queue until there are no more programs to run
    while (q.head != NULL)
    {
        // sort queue based and job_length score
        sort_queue_sjf();

        // removes and saves the script at the front of the queue with the shortest job length score
        PCB *to_run = dequeue();

        // subtracting 1 from the job_length_score of all programs in the queue (the ones that are not being run)
        PCB *current = q.head;
        while (current != NULL)
        {
            if (current->job_length_score > 0)
                current->job_length_score--;
            current = current->next;
        }

        // running a line from the script with the lowest job length score
        parseInput(script_memory[get_line(to_run, to_run->program_counter)].content);
        to_run->program_counter++;

        // enqueues the script that just ran to the front of the queue -> this is necessary because bubble
        // sort in sort_sjf will only promote scripts if they have a job length score less than the head ->
        // since in the assignment, the current script needs to continue running until it's job_length score exceeds
        // another program, it needs to be added to the front rather than the back
        if (to_run->program_counter < to_run->length)
        {
            to_run->next = q.head;
            if (q.head != NULL)
                q.head->prev = to_run;
            q.head = to_run;
            if (to_run->next == NULL)
            {
                q.tail = to_run;
            }
        }
    }
}

void clean_script(int pid){
    /*
        clears all the memory related to the script with the pid specified

        inputs:
        int pid: pid of the program to remove

        returns: nothing
    */

    // Hold queue_mutex so no other thread can concurrently modify the queue
    pthread_mutex_lock(&queue_mutex);
    PCB *current = q.head;

    // free all the lines that are stored in the line array related to this script
    // by checking the owner field
    for (int i = 0; i < 1000; i++)
    {
        if (script_memory[i].owner == pid)
        {
            script_memory[i].owner = 0;
            free(script_memory[i].content);
            script_memory[i].content = NULL;
        }
    }

    // find the pcb that is related to the pid specified
    while (current != NULL)
    {   
        // adds all the pages that it occupied back into available memory 
        // removes it from the ready queue and frees the related memory
        if (current->PID == pid)
        {
            for (int i = 0; i < current->num_pages; i++){
                page_enqueue(current->pages[i]);
            }

            if (current == q.head)
                q.head = current->next;
            else
                current->prev->next = current->next;

            if (current == q.tail)
                q.tail = current->prev;
            else
                current->next->prev = current->prev;

            free(current->pages);
            free(current->filename);
            free(current);
            
            break;
        }
        else
            current = current->next;
    }

    // makes the associated pid available again
    avail_file_numbers[pid - 1] = 1;
    pthread_mutex_unlock(&queue_mutex);
}

// Set key value pair
void mem_set_value(char *var_in, char *value_in)
{
    // Same logic (protecting memory)
    pthread_mutex_lock(&shellmem_mutex);
    for (int i = 0; i < MEM_SIZE; i++)
    {
        if (strcmp(shellmemory[i].var, var_in) == 0)
        {
            free(shellmemory[i].value);
            shellmemory[i].value = strdup(value_in);
            pthread_mutex_unlock(&shellmem_mutex);
            return;
        }
    }
    // Value does not exist, need to find a free spot.
    for (int i = 0; i < MEM_SIZE; i++)
    {
        if (strcmp(shellmemory[i].var, "none") == 0)
        {
            shellmemory[i].var = strdup(var_in);
            shellmemory[i].value = strdup(value_in);
            pthread_mutex_unlock(&shellmem_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&shellmem_mutex);
}

// get value based on input key
char *mem_get_value(char *var_in)
{
    // Same logic (protecting memory)
    pthread_mutex_lock(&shellmem_mutex);
    for (int i = 0; i < MEM_SIZE; i++)
    {
        if (strcmp(shellmemory[i].var, var_in) == 0)
        {
            char *val = strdup(shellmemory[i].value);
            pthread_mutex_unlock(&shellmem_mutex);
            return val;
        }
    }
    pthread_mutex_unlock(&shellmem_mutex);
    return NULL;
}

void *worker_function(void *arg)
{
    /*
    This is the function that the worker threads run

Loop:
   1. Lock queue_mutex and wait on queue_cond until the queue is non-empty
      or scheduler_running becomes 0.
   2. If shutting down and the queue is empty, exit the loop.
   3. Dequeue one PCB while still holding the lock, then release it.
   4. Execute according to current_policy:
        RR/RR30: run at most one time slice; re-enqueue if not done.
   5. When a job finishes, call clean_script() then decrement active_jobs
      and signal active_jobs_cond so multithread_execution() can wake.
*/
    while (1)
    {
        pthread_mutex_lock(&queue_mutex);

        // Wait while the queue is empty and not told to stop
        while (q.head == NULL && scheduler_running)
            pthread_cond_wait(&queue_cond, &queue_mutex);

        // Shutdown if no work left and scheduler is stopping
        if (!scheduler_running && q.head == NULL)
        {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        // Take the next PCB while we still hold the lock
        PCB *pcb = dequeue_unlocked();
        pthread_mutex_unlock(&queue_mutex);

        if (pcb == NULL)
            continue;

        // RR or RR30
        if (strcmp(current_policy, "RR") == 0 || strcmp(current_policy, "RR30") == 0)
        {
            // Reimplemented the RR logic
            int slice;
            if (strcmp(current_policy, "RR30") == 0)
            {
                slice = 30;
            }
            else
            {
                slice = 2;
            }
            int time = 0;
            while (pcb->program_counter < pcb->length && time < slice)
            {
                parseInput(script_memory[get_line(pcb, pcb->program_counter)].content);
                pcb->program_counter++;
                time++;
            }
            // After the first slice of the btach PCB, clear batch_running
            //  so the main thread knows it's safe to start waiting on active_jobs
            if (pcb->priority == 1)
            {
                pcb->priority = 0;
                pthread_mutex_lock(&active_jobs_mutex);
                batch_running = 0;
                pthread_cond_signal(&active_jobs_cond);
                pthread_mutex_unlock(&active_jobs_mutex);
            }
            if (pcb->program_counter < pcb->length)
            {
                enqueue(pcb); // re-queue for next slice (enqueue locks internally)
            }
            else
            {
                // Job finished
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

// Updates current_policy
// Starts worker threads if not already running
// Updating the policy even when threads are alive
void start_scheduler_threads(char *policy)
{
    strncpy(current_policy, policy, sizeof(current_policy) - 1);
    current_policy[sizeof(current_policy) - 1] = '\0';

    if (scheduler_running)
        return; // threads already alive

    scheduler_running = 1;
    pthread_create(&workers[0], NULL, worker_function, NULL);
    pthread_create(&workers[1], NULL, worker_function, NULL);
}

// Signals both workers to exit and joins them
void stop_scheduler_threads()
{
    if (!scheduler_running)
        return;

    pthread_mutex_lock(&queue_mutex);
    scheduler_running = 0;
    pthread_cond_broadcast(&queue_cond); // So idle workers wake up and exit their loops
    pthread_mutex_unlock(&queue_mutex);

    pthread_join(workers[0], NULL);
    pthread_join(workers[1], NULL);
}
