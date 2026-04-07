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
#include <limits.h>
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

struct memory_struct shellmemory[VAR_STORE_SIZE];
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
Line script_memory[FRAME_STORE_SIZE];

typedef struct page_entry {
    struct page_entry* next;
    int page_num;

} Page_Entry;

typedef struct pages {
    Page_Entry* head;
    Page_Entry* tail;
} Pages;


Frame_Owners frame_owners[FRAME_STORE_SIZE / FRAME_SIZE];

Pages ps = {NULL, NULL};

// initial ready queue (head = NULL and tail = NULL because the queue is empty)
Queue q = {NULL, NULL};

//Array that indicates how many programs use each frame
int frame_count[FRAME_STORE_SIZE / FRAME_SIZE];

//Global clock used to keep track of when frames are used
int lru_clock = 0;

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
    for (i = 0; i < VAR_STORE_SIZE; i++)
    {
        shellmemory[i].var = "none";
        shellmemory[i].value = "none";
    }


    //initializes page queue to have all available pages
    for (int i = 0; i < FRAME_STORE_SIZE/FRAME_SIZE; i++){
        for (int j = 0; j < 10; j++){
        frame_owners[i].pcbs[j] = NULL;
        }
        frame_owners[i].page_num = -1;
        frame_owners[i].last_used = 0;
        page_enqueue(i);
    }
    memset(frame_count, 0, sizeof(frame_count));
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
            pcb->program_counter = 0;
            pcb->next = NULL;
            pcb->prev = NULL;
            pcb->job_length_score = cur->job_length_score;
            pcb->priority = 0;
            pcb->num_pages = cur->num_pages;
            pcb->filename = strdup(cur->filename);
            duplicate = 1;

            for (int i = 0; i < 500; i++){
                pages[i] = cur->pages[i];
            }

            pcb->pages = malloc(sizeof(int) * 500);
            memcpy(pcb->pages, pages, sizeof(int) * 500);
            for (int i = 0; i < cur->num_pages; i++) {
                if (cur->pages[i] != -1){
                    frame_count[cur->pages[i]]++;

                    int frame = cur->pages[i];
                    for (int j = 0; j < 10; j++){
                        if (frame_owners[frame].pcbs[j] == NULL){
                            frame_owners[frame].pcbs[j] = pcb;
                            break;
                        }
                    }
                }
            }
            break;
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
        for (int i = 0; i < 500; i++){
            //indicator that none of the pages have been loaded
            pcb->pages[i] = -1;
        }
        pcb->filename = strdup(filename);

        // each line has max 100 chars
        char buffer[100];
        // tries to store each line in the file to slots in the line array
        while (fgets(buffer, 100, file) != NULL){
            if (ps.head == NULL){
                // if we reach end of line memory before the whole file is stored:
                // delete all previously stored lines and return with failure
                for (int i = 0; i < pcb->num_pages; i++){
                    for (int j = 0; j < FRAME_SIZE; j++){
                        free(script_memory[pcb->pages[i] * FRAME_SIZE + j].content);
                        script_memory[pcb->pages[i] * FRAME_SIZE + j].content = NULL;
                        script_memory[pcb->pages[i] * FRAME_SIZE + j].owner = 0;
                    }
                }

                // the pcb cannot be added so make the reserved pid available again
                avail_file_numbers[pid - 1] = 1;
                free(pcb);
                return -1;
            }
            // stop after 2 pages for demand paging
            if (pcb->num_pages >= 2){
                // buffer already has the first unloaded line, count it
                pcb->length++;
                // then count the rest
                while (fgets(buffer, 100, file) != NULL){
                    pcb->length++;
                }
                break;
            } 

            //get next available page to store
            int page = page_dequeue();
            pcb->pages[pcb->num_pages] = page;
            pcb->num_pages += 1;
            frame_count[page]++;

            // register PCB in frame_owners
            frame_owners[page].page_num = pcb->num_pages - 1;  // logical page number
            for (int j = 0; j < 10; j++){
                if (frame_owners[page].pcbs[j] == NULL){
                    frame_owners[page].pcbs[j] = pcb;
                    break;
                }
            }

            // otherwise store up to  3 lines from the file into memory
            for (int i = 0; i < FRAME_SIZE; i++){
                char *line = strdup(buffer);
                script_memory[page * FRAME_SIZE + i].content = line;
                script_memory[page * FRAME_SIZE + i].owner = pid;
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
    }
    enqueue(pcb);
    // Increment active jobs after enqueue to respect locking order
    pthread_mutex_lock(&active_jobs_mutex);
    active_jobs++;
    pthread_mutex_unlock(&active_jobs_mutex);

    // returns the pid of the program that is added. this is useful when we want to free the memory
    // that is associated with this script
    return pcb->PID;
}

int load_page(PCB *pcb, int page_num) {
    // open the file
    FILE *file = fopen(pcb->filename, "rt");
    if (file == NULL) return -1;

    // skip to the right position in the file
    char buffer[100];
    int lines_to_skip = page_num * FRAME_SIZE;
    for (int i = 0; i < lines_to_skip; i++){
        if (fgets(buffer, 100, file) == NULL){
            fclose(file);
            return -1; // page_num is out of bounds
        }
    }

    // get a free frame, evicting if necessary
    int frame;
    if (ps.head != NULL){
        // free frame available
        frame = page_dequeue();
        printf("Page fault!\n");
    } else {
        // no free frame, need to evict
        frame = evict_frame(); 
    }

    // load up to FRAME_SIZE lines into the frame
    for (int i = 0; i < FRAME_SIZE; i++){
        if (fgets(buffer, 100, file) == NULL) break;
        script_memory[frame * FRAME_SIZE + i].content = strdup(buffer);
        script_memory[frame * FRAME_SIZE + i].owner = pcb->PID;
    }

    fclose(file);

    // update page table
    pcb->pages[page_num] = frame;
    pcb->num_pages++;

    // update frame_owners
    frame_owners[frame].page_num = page_num;
    frame_owners[frame].last_used = lru_clock++;
    for (int i = 0; i < 10; i++){
        if (frame_owners[frame].pcbs[i] == NULL){
            frame_owners[frame].pcbs[i] = pcb;
            break;
        }
    }

    // update refcount
    frame_count[frame]++;

    // update all other PCBs with the same filename
    PCB *cur = q.head;
    while (cur != NULL){
        if (cur != pcb && strcmp(cur->filename, pcb->filename) == 0){
            // update their page table too
            cur->pages[page_num] = frame;
            cur->num_pages++;
            // register them in frame_owners too
            for (int j = 0; j < 10; j++){
                if (frame_owners[frame].pcbs[j] == NULL){
                    frame_owners[frame].pcbs[j] = cur;
                    break;
                }
            }
            frame_count[frame]++;
        }
        cur = cur->next;
    }

    return 0;
}

int evict_frame() {
    // find the least recently used frame
    int frame = -1;
    int min_time = INT_MAX;
    for (int i = 0; i < FRAME_STORE_SIZE / FRAME_SIZE; i++){
        if (frame_owners[i].page_num != -1 && frame_owners[i].last_used < min_time){
            min_time = frame_owners[i].last_used;
            frame = i;
        }
    }

    // print required message
    printf("Page fault!\nVictim page contents:\n");
    for (int i = 0; i < FRAME_SIZE; i++){
        if (script_memory[frame * FRAME_SIZE + i].content != NULL){
            printf("%s", script_memory[frame * FRAME_SIZE + i].content);
        }
    }
    printf("End of victim page contents.\n");

    // update all owner PCBs' page tables to -1
    int page_num = frame_owners[frame].page_num;
    for (int i = 0; i < 10; i++){
        if (frame_owners[frame].pcbs[i] != NULL){
            frame_owners[frame].pcbs[i]->pages[page_num] = -1;
            frame_owners[frame].pcbs[i] = NULL;
        }
    }

    // clear the frame from script_memory
    for (int i = 0; i < FRAME_SIZE; i++){
        free(script_memory[frame * FRAME_SIZE + i].content);
        script_memory[frame * FRAME_SIZE + i].content = NULL;
        script_memory[frame * FRAME_SIZE + i].owner = 0;
    }

    // reset frame metadata
    frame_owners[frame].page_num = -1;
    frame_count[frame] = 0;

    return frame;
}

int get_line(PCB *program, int pc){
    int page_number = pc / FRAME_SIZE;
    int frame = program->pages[page_number];
    
    if (frame == -1){
        // page fault — page is not loaded
        return -1;  // signal a page fault to the caller
    }

    // update LRU timestamp
    frame_owners[frame].last_used = lru_clock++;

    int offset = pc % FRAME_SIZE;
    return frame * FRAME_SIZE + offset;
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
            int line = get_line(current, current->program_counter);
            if (line == -1){
                load_page(current, current->program_counter / FRAME_SIZE);
                // no re-enqueue needed, just continue running
                continue;
            }
            parseInput(script_memory[line].content);
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
        PCB *current = dequeue();
        time = 0;
        int page_fault = 0;  // flag to track page fault

        while (current->program_counter < current->length && time < max_time)
        {
            int line = get_line(current, current->program_counter);
            if (line == -1){
                load_page(current, current->program_counter / FRAME_SIZE);
                enqueue(current);
                page_fault = 1;
                break;
            } 
            parseInput(script_memory[line].content);
            current->program_counter++;
            time++;
        }

        time = 0;

        // only re-enqueue if not already re-enqueued due to page fault
        if (!page_fault && current->program_counter < current->length)
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
        clears all the memory related to the script with the pid specified if no other duplicate scripts

        inputs:
        int pid: pid of the program to remove

        returns: nothing
    */
    pthread_mutex_lock(&queue_mutex);

    PCB *current = q.head;
    while (current != NULL)
    {   
        if (current->PID == pid)
        {
            // decrement refcount for each loaded frame
            // but do NOT free the frames or return them to the free pool
            for (int i = 0; i < current->num_pages; i++){
                int frame = current->pages[i];
                if (frame != -1){  // only decrement if page is actually loaded
                    frame_count[frame]--;
                    // remove this PCB from frame_owners
                    for (int j = 0; j < 10; j++){
                        if (frame_owners[frame].pcbs[j] == current){
                            frame_owners[frame].pcbs[j] = NULL;
                            break;
                        }
                    }
                }
            }

            // remove PCB from the ready queue
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

    avail_file_numbers[pid - 1] = 1;
    pthread_mutex_unlock(&queue_mutex);
}

// Set key value pair
void mem_set_value(char *var_in, char *value_in)
{
    // Same logic (protecting memory)
    pthread_mutex_lock(&shellmem_mutex);
    for (int i = 0; i < VAR_STORE_SIZE; i++)
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
    for (int i = 0; i < VAR_STORE_SIZE; i++)
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
    for (int i = 0; i < VAR_STORE_SIZE; i++)
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
