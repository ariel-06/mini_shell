#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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


//new types to store script based on assumptions in 1.2.1
typedef struct line {
    //stores pointer each line
    char *content;

    //which script file does this line belong to options (0,1,2,3)
    //0 if there is no line, else corresponds to script number
    int owner;
} Line; 

typedef struct pcb{
    int PID;
    int start_index;
    int length;
    int program_counter;
    struct pcb *next;
    struct pcb *prev;
    int job_length_score;
} PCB;

typedef struct queue { 
    PCB *head;
    PCB *tail;
} Queue;

Line script_memory[1000];
Queue q = {NULL, NULL};





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

void enqueue(PCB *node){
    if (q.head == NULL)
        q.head = node;
    else {
        q.tail->next = node;
        node->prev = q.tail;
    }
    q.tail = node;
}

PCB* dequeue(){ 
    //removes the first node in the queue and returns it
    PCB *node = q.head;
    if (node == NULL){
        return NULL; //do nothing bc the queue is already empty
    } else if (node == q.tail){
        q.head = NULL;
        q.tail = NULL;
    } else {
        q.head = q.head->next;
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

    //add the new process to end of the ready queue
    enqueue(pcb);

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
        sort_queue_sjf();
        PCB *to_run = dequeue();
        PCB *current = q.head;

    //subtracting 1 from the job_length_score of all programs in the queue 
        while (current != NULL){
            if (current->job_length_score > 0)
                current->job_length_score--;
            current = current->next;
        }

        // running a line in the program
        parseInput(script_memory[to_run->program_counter + to_run->start_index].content);
        to_run->program_counter++;
        if (to_run->program_counter < to_run->length)
        enqueue(to_run);

    }
}

void clean_script(int pid){
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
}


// Set key value pair
void mem_set_value(char *var_in, char *value_in) {
    int i;

    for (i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, var_in) == 0) {
            shellmemory[i].value = strdup(value_in);
            return;
        }
    }

    //Value does not exist, need to find a free spot.
    for (i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, "none") == 0) {
            shellmemory[i].var = strdup(var_in);
            shellmemory[i].value = strdup(value_in);
            return;
        }
    }

    return;
}

//get value based on input key
char *mem_get_value(char *var_in) {
    int i;

    for (i = 0; i < MEM_SIZE; i++) {
        if (strcmp(shellmemory[i].var, var_in) == 0) {
            return strdup(shellmemory[i].value);
        }
    }
    return NULL;
}
