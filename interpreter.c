//#define DEBUG 1

#ifdef DEBUG
#   define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#   define debug(...)
// NDEBUG disables asserts
#   define NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>              // tolower, isdigit
#include <dirent.h>             // scandir
#include <unistd.h>             // chdir
#include <sys/stat.h>           // mkdir
// for run:
#include <sys/types.h>          // pid_t
#include <sys/wait.h>           // waitpid


#include "shellmemory.h"
#include "shell.h"
#include <stdarg.h>

int badcommand() {
    printf("Unknown Command\n");
    return 1;
}

// For source command only
int badcommandFileDoesNotExist() {
    printf("Bad command: File not found\n");
    return 3;
}

int badcommandMkdir() {
    printf("Bad command: my_mkdir\n");
    return 4;
}

int badcommandCd() {
    printf("Bad command: my_cd\n");
    return 5;
}

int help();
int quit();
int set(char *var, char *value);
int print(char *var);
int echo(char *string);
int my_ls();
int my_mkdir(char *name);
int my_touch(char *path);
int my_cd(char *path);
int source(char *script);
int run(char *args[], int args_size);
int exec(int count, ...);
int badcommandFileDoesNotExist();

//Helper function which reads the rest of the 
FILE* get_batch_input(){
    char line[1000];
    size_t total_size = 0;
    size_t capacity = 1024;

    char* buffer = malloc(capacity);
    if(!buffer){
        return NULL;
    }
    buffer[0] = '\0';
    //reading the rest of stdin until EOF
    while (fgets(line, sizeof(line), stdin) != NULL){
        size_t len = strlen(line);

        //Resize buffer if full
        if (total_size + len + 1 > capacity){
            capacity = capacity * 2;
            char *new_buffer = realloc(buffer, capacity);
            if (!new_buffer){
                free(buffer);
                return NULL;
            }
            buffer = new_buffer;
        }
        strcat(buffer, line);
        total_size = total_size + len;
    }
    if (total_size == 0){
        free(buffer);
        return NULL;
    }
    FILE *f = fmemopen(buffer, total_size, "r");
    return f;
}


// Interpret commands and their arguments
int interpreter(char *command_args[], int args_size) {
    int i;

    // these bits of debug output were very helpful for debugging
    // the changes we made to the parser!
    debug("#args: %d\n", args_size);
#ifdef DEBUG
    for (size_t i = 0; i < args_size; ++i) {
        debug("  %ld: %s\n", i, command_args[i]);
    }
#endif

    if (args_size < 1) {
        // This shouldn't be possible but we are defensive programmers.
        fprintf(stderr, "interpreter called with no words?\n");
        exit(1);
    }

    for (i = 0; i < args_size; i++) {   // terminate args at newlines
        command_args[i][strcspn(command_args[i], "\r\n")] = 0;
    }

    if (strcmp(command_args[0], "help") == 0) {
        //help
        if (args_size != 1)
            return badcommand();
        return help();

    } else if (strcmp(command_args[0], "quit") == 0) {
        //quit
        if (args_size != 1)
            return badcommand();
        return quit();

    } else if (strcmp(command_args[0], "set") == 0) {
        //set
        if (args_size != 3)
            return badcommand();
        return set(command_args[1], command_args[2]);

    } else if (strcmp(command_args[0], "print") == 0) {
        if (args_size != 2)
            return badcommand();
        return print(command_args[1]);

    } else if (strcmp(command_args[0], "echo") == 0) {
        if (args_size != 2)
            return badcommand();
        return echo(command_args[1]);

    } else if (strcmp(command_args[0], "my_ls") == 0) {
        if (args_size != 1)
            return badcommand();
        return my_ls();

    } else if (strcmp(command_args[0], "my_mkdir") == 0) {
        if (args_size != 2)
            return badcommand();
        return my_mkdir(command_args[1]);

    } else if (strcmp(command_args[0], "my_touch") == 0) {
        if (args_size != 2)
            return badcommand();
        return my_touch(command_args[1]);

    } else if (strcmp(command_args[0], "my_cd") == 0) {
        if (args_size != 2)
            return badcommand();
        return my_cd(command_args[1]);

    } else if (strcmp(command_args[0], "source") == 0) {
        if (args_size != 2)
            return badcommand();
        return source(command_args[1]);

    } else if (strcmp(command_args[0], "run") == 0) {
        if (args_size < 2)
            return badcommand();
        return run(&command_args[1], args_size - 1);

    } 
    else if (strcmp(command_args[0], "exec") == 0) {
        if (args_size < 3) {
            fprintf(stderr, "error: exec requires at least one program and a policy\n");
            return badcommand();
        }

        int background = 0;     //Flag to check if the background execution (#) was called
        char *policy = NULL;
        int num_programs = 0;
        
        //Background execution is called
        if (strcmp(command_args[args_size - 1], "#") == 0) {
            background = 1;
            policy = command_args[args_size - 2];
            num_programs = args_size - 3;
        } 
        else {
            background = 0;
            policy = command_args[args_size - 1];
            num_programs = args_size - 2;
        }

        if (num_programs < 1) {
            fprintf(stderr, "error: no program specified\n");
            return badcommand();
        }

        // Checking for duplicates
        for (int i = 1; i <= num_programs; i++) {
            for (int j = i + 1; j <= num_programs; j++) {
                if (strcmp(command_args[i], command_args[j]) == 0) {
                    fprintf(stderr, "error: duplicate program names\n");
                    return badcommand();
                }
            }
        }
        
        //Building the call to exec()
        int total_args = num_programs + 1 + background;     // +1 for the policy
        //Only total_args with 1 program and 1 policy
        if (total_args == 2) {
            return exec(2, command_args[1], policy);
        }
        else if (total_args == 3){
            // 2 programs and 1 policy
            if (num_programs == 2){
                    return exec(3, command_args[1], command_args[2], policy);
            }
            // 1 program, 1 policy and background
            else{
                return exec(3, command_args[1], policy, "#");
            }
        }
        else if (total_args == 4){
            // 3 programs and 1 policy
            if (num_programs == 3){
                return exec(4, command_args[1], command_args[2], command_args[3], policy);
            }
            // 2 programs, 1 policy and background
            else{
                return exec(4, command_args[1], command_args[2], policy, "#");
            }
        }
        else if (total_args == 5){
            // 3 programs, 1 policy and background
            return exec(5, command_args[1], command_args[2], command_args[3], policy, "#");
        }
        else{
            fprintf(stderr, "error: too many programs (max 3 supported)\n");
            return badcommand();
        }
    }
    else
        return badcommand();
}

int help() {

    // note the literal tab characters here for alignment
    char help_string[] = "COMMAND			DESCRIPTION\n \
help			Displays all the commands\n \
quit			Exits / terminates the shell with “Bye!”\n \
set VAR STRING		Assigns a value to shell memory\n \
print VAR		Displays the STRING assigned to VAR\n \
source SCRIPT.TXT		Executes the file SCRIPT.TXT\n ";
    printf("%s\n", help_string);
    return 0;
}

int quit() {
    printf("Bye!\n");
    exit(0);
}

int set(char *var, char *value) {
    mem_set_value(var, value);
    return 0;
}

int print(char *var) {
    char *value = mem_get_value(var);
    if (value) {
        printf("%s\n", value);
        free(value);
    } else {
        printf("Variable does not exist\n");
    }
    return 0;
}

int echo(char *string){
    if (string == NULL || string[0] == '\0'){
        return 0;
    }
    if (string[0] != '$') {
        printf("%s\n", string);
        return 0;
    }
    char *varName = string + 1;
    char *value = mem_get_value(varName);

    if (strcmp(value, "Variable does not exist") == 0) {
        return 0;
    }
    print(varName);
    return 0;
}

int my_ls(){
    DIR *cwd = opendir(".");
    if (cwd == NULL){
        return 1;
    }
    //assume less than 100 entries in cwd
    char* filesetc[100];
    int index = 0;
    struct dirent *temp;
    
    while ((temp = readdir(cwd)) != NULL && index < 100) {
        filesetc[index] = strdup(temp->d_name);
        index++;
    }
    closedir(cwd);

    for (int i = 0; i < index - 1; i++){
        for (int j = i + 1; j < index; j++){
            if (strcmp(filesetc[i], filesetc[j]) > 0){
                char *tmp = filesetc[i];
                filesetc[i] = filesetc[j];
                filesetc[j] = tmp;
            }
        }
    }

    for (int i = 0; i < index; i++) {
        printf("%s\n", filesetc[i]);
        free(filesetc[i]);
    }

    return 0;

}

int my_mkdir(char *dirname) {
    char *name = dirname;

    if (dirname[0] == '$') {
        name = mem_get_value(dirname + 1);
        if (strcmp(name, "Variable does not exist") == 0) {
            printf("Bad command: my_mkdir\n");
            return 1;
        }

        int i = 0;
        while (name[i] != '\0'){
            char c = name[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))){
                printf("Bad command: my_mkdir\n");
                return 1;
            }
            i++;
        }
    }
    
    if (mkdir(name, 0777) != 0) {
        printf("Bad command: my_mkdir\n");
        return 1;
    }

    return 0;
}

int my_touch (char *filename){
    FILE *f = fopen(filename, "a");

    if (f == NULL){
        return 1;
    }

    fclose(f);
    return 0;
}

int my_cd(char *dirname){
    if (chdir(dirname) != 0) {
        printf("Bad command: my_cd\n");
        return 1;
    }
    return 0;
}

int source(char *script) {
    int errCode = 0;
    char line[MAX_USER_INPUT];
    FILE *p = fopen(script, "rt");      // the program is in a file

    int pid = add_script(p);
    run_queue();
    clean_script(pid);

    /*
    fgets(line, MAX_USER_INPUT - 1, p);
    while (1) {
        errCode = parseInput(line);     // which calls interpreter()
        memset(line, 0, sizeof(line));

        if (feof(p)) {
            break;
        }
        fgets(line, MAX_USER_INPUT - 1, p);
    }
    */
    
    fclose(p);

    return errCode;
}

int run(char *args[], int arg_size) {
    // copy the args into a new NULL-terminated array.
    char **adj_args = calloc(arg_size + 1, sizeof(char *));
    for (int i = 0; i < arg_size; ++i) {
        adj_args[i] = args[i];
    }

    // always flush output streams before forking.
    fflush(stdout);
    // attempt to fork the shell
    pid_t pid = fork();
    if (pid < 0) {
        // fork failed. Report the error and move on.
        perror("fork() failed");
        return 1;
    } else if (pid == 0) {
        // we are the new child process.
        execvp(adj_args[0], adj_args);
        perror("exec failed");
        // The parent and child are sharing stdin, and according to
        // a part of the glibc documentation that you are **not**
        // expected to know for this course, a shared input handle
        // should be fflushed (if it is needed) or closed
        // (if it is not). Handling this exec error total_args is not even
        // necessary, but let's do it right.
        // (Failure to do this can result in the parent process
        // reading the remaining input twice in batch mode.)
        fclose(stdin);
        exit(1);
    } else {
        // we are the parent process.
        waitpid(pid, NULL, 0);
    }

    return 0;
}

int exec (int count, ...){
    va_list args;
    va_start(args, count);
    //Copying all arguments into an array
    char *arg_array[5];
    for (int i = 0; i < count; i++){
        arg_array[i] = va_arg(args, char*);
    }

    va_end(args);
    int background = 0;
    if (strcmp(arg_array[count - 1], "#") == 0) {
        background = 1;
    }

    int mode_index;
    if (background == 1){
        mode_index = count - 2;
    }
    else{
        mode_index = count - 1;
    }
    char * mode = arg_array[mode_index];
    int num_programs = mode_index; 
    
    int pids[3] = {0};
    int pid_index = 0;
    int batch_pid = -1;
    PCB* batch_script = NULL;
    //If we have a #, create batch PCB and inserting it at the front of the queue
    if (background){
        FILE *batch_file = get_batch_input();
        if (batch_file != NULL){
            batch_pid = add_script(batch_file);
            fclose(batch_file);

            if (batch_pid == -1){
                fprintf(stderr, "failed to load batch script process\n");
                return -1;
            }
            // Find the PCB in queue with that PID and set priority, should just be the head but just in case
            PCB *current = q.head;
            while (current != NULL) {
                if (current->PID == batch_pid) {
                    current->priority = 1;
                    batch_script = current;
                    break;
                }
                current = current->next;
            }
        }
    }
    //Loading program files

    for (int i = 0; i < num_programs; i++){
        FILE *f = fopen(arg_array[i], "rt");
        if (f == NULL){
            fprintf(stderr, "file failed to open\n");
            //Clean up previously loaded scripts
            for (int j = 0; j < pid_index; j++){
                clean_script(pids[j]);
            }
            if (batch_pid != -1)
                clean_script(batch_pid);
            return -1;
        }
        pids[pid_index] = add_script(f);
        fclose(f);
        
        if (pids[pid_index] == -1) {
            fprintf(stderr, "not enough memory for file: %s\n", arg_array[i]);
            for (int j = 0; j < pid_index; j++){
                clean_script(pids[j]);
            }
            if (batch_pid != -1)
                clean_script(batch_pid);
            return -1;
        }
        pid_index++;
    }

    if (strcmp(mode, "FCFS") == 0){
        run_queue();
    } else if (strcmp(mode, "SJF") == 0){
        run_queue_sjf();
    } else if (strcmp(mode, "RR") == 0){
        run_queue_rr(2);
    } else if (strcmp(mode, "AGING") == 0) {
        run_queue_sjf_aging();
    } else if (strcmp(mode, "RR30") == 0){
        run_queue_rr(30);
    } else {
        fprintf(stderr, "invalid scheduling mode: %s\n", mode);
        for (int j = 0; j < pid_index; j++)
            clean_script(pids[j]);
        if (batch_script != 1){
            clean_script(batch_pid);
        }
        return -1;
    }

    for (int i = 0; i < pid_index; i++) {
        clean_script(pids[i]);
    }
    if (batch_pid) {
        clean_script(batch_script->PID);
    }

    return 0;
}


/*
int run(char **args){
    
    Uses a "fork-exec-wait" to run other commands. It forks the shell and calls execvp to execute the given command.

    It simply passes user input as command-line arguments to external programs, so assumes valid input. Assumes a max of 4 input arguments after run.
    
    inputs: **args: pointer to the command line arguments after run
    
    Returns 0 on success and 1 on failure. Output of the specified process.
    
    //Setting up pid and forking
    pid_t pid = fork();
    //If the fork failed return an error code
    if (pid < 0){
        return 1;
    }
    
    //If we're in the child process execute the child process
    if (pid == 0){
        execvp(args[0], args);    //execvp since it looks up the command in arg[0] and accepts argument (args) as an array of arguments
        return 1;
    }
    //Else we're in the parent process and we need to wait
    else{
	    wait(NULL);
    }
    return 0;
}
*/