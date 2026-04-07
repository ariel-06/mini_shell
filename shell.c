#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>              // isspace
#include <string.h>
#include <unistd.h>             // isatty
#include "shell.h"
#include "interpreter.h"
#include "shellmemory.h"

int parseInput(char ui[]);

/*
int main(int argc, char *argv[]) {
    printf("Shell version 1.5 created Dec 2025\n");

    char prompt = '$';          // Shell prompt
    char userInput[MAX_USER_INPUT];     // user's input stored here
    // batch_mode is true when a file was given.
    int batch_mode = !isatty(STDIN_FILENO);
    int errorCode = 0;          // zero means no error, default

    //init user input
    for (int i = 0; i < MAX_USER_INPUT; i++) {
        userInput[i] = '\0';
    }

    //init shell memory
    mem_init();
    while (1) {
        if (!batch_mode) {
            printf("%c ", prompt);
        }
        // here you should check the unistd library 
        // so that you can find a way to not display $ in the batch mode
        fgets(userInput, MAX_USER_INPUT - 1, stdin);
        errorCode = parseInput(userInput);
        if (errorCode == -1)
            exit(99);           // ignore all other errors

        if (feof(stdin)) {
            return 0;
        }

        memset(userInput, 0, sizeof(userInput));
    }

    return 0;
}
*/ 

int main(int argc, char *argv[])
{
    printf("Frame Store Size = %d; Variable Store Size = %d\n", FRAME_STORE_SIZE, VAR_STORE_SIZE);

    char prompt = '$';              // Shell prompt
    char userInput[MAX_USER_INPUT]; // user's input stored here
    int errorCode = 0;              // zero means no error, default
    

    // init user input
    for (int i = 0; i < MAX_USER_INPUT; i++)
    {
        userInput[i] = '\0';
    }

    // init shell memory
    mem_init();
    while (1)
    {
        if (isatty(0)){
            printf("%c ", prompt);
        }
        
        // here you should check the unistd library
        // so that you can find a way to not display $ in the batch mode
        if (fgets(userInput, MAX_USER_INPUT - 1, stdin) == NULL) {
            // EOF reached

            return(0);
        }
        //tokenizes command input using strtok -> each command is separated using a semicolon
        //each entry separated using semicolons is treated as a new command 
        //while loop processes each entry either until there are no more entries (end of command chain)
        // or until max 10 entries are reached (assignment guideline)

        char *nextCmd = strtok(userInput, ";");
        int commandCount = 0;

        //loop continues if current entry is not null
        while (nextCmd != NULL && commandCount < 10){
            //Deals with removing trailing and leading whitespaces
            int start = 0;
            while (nextCmd[start] == ' ') {
                start++;
            }
            int end = strlen(nextCmd) - 1;
            while (end >= start && (nextCmd[end] == ' ' || nextCmd[end] == '\n')){
                end--;
            }

            int j = 0;
            for (int i = start; i <= end; i++, j++) {
                nextCmd[j] = nextCmd[i];
            }
            nextCmd[j] = '\0';

            //Makes sure we only deal with non empty entries
            if (strlen(nextCmd) > 0) {
                errorCode = parseInput(nextCmd);
                if (errorCode == -1){
                    exit(99);
                }
            }

            //moves to next command 
            nextCmd = strtok(NULL, ";");
            commandCount++;
        }
        
        memset(userInput, 0, sizeof(userInput));
    }
    return 0;

}

int wordEnding(char c) {
    // You may want to add ';' to this at some point,
    // or you may want to find a different way to implement chains.
    return c == '\0' || c == '\n' || isspace(c) || c == ';';
}

int parseInput(char inp[]) {
    char tmp[200], *words[100];
    int ix = 0, w = 0;
    int wordlen;
    int errorCode = 0;

    // This function probably isn't the best place to handle chains.
    // That is, if we really wanted to implement relatively complex
    // syntax like that of bash, we should really just tokenize everything,
    // send the ';' as a separate word to the interpreter, and let the
    // interpreter sort it out later.
    // But for this simple shell, the interpreter's job is really only to be
    // command dispatch, and this function is really acting as a complete
    // parser rather than just a tokenizer. So we'll handle it here.

    while (inp[ix] != '\n' && inp[ix] != '\0' && ix < 1000) {
        // skip white spaces
        for (; isspace(inp[ix]) && inp[ix] != '\n' && ix < 1000; ix++);

        // If the next character is a semicolon,
        // we should run what we have so far.
        if (inp[ix] == ';')
            break;

        // extract a word
        for (wordlen = 0; !wordEnding(inp[ix]) && ix < 1000; ix++, wordlen++) {
            tmp[wordlen] = inp[ix];
        }

        if (wordlen > 0) {
            tmp[wordlen] = '\0';
            words[w] = strdup(tmp);
            w++;
            if (inp[ix] == '\0')
                break;
        } else {
            break;
        }
    }
    // Ignore commands that contain no (meaningful) input by only calling the
    // interpreter if actually found words.
    if (w > 0) {
        errorCode = interpreter(words, w);
        for (size_t i = 0; i < w; ++i) {
            free(words[i]);
        }
    }
    if (inp[ix] == ';') {
        // handle the next command in the chain by recursing
        // the parser. We could equivalently wrap all of the work above
        // in a while loop, but this makes it clearer what's going on.
        // Additionally, a modern compiler is more than smart enough to
        // turn this into a loop for us! Try adding -O2 to the CFLAGS in
        // the Makefile and then read the assembly we get.
        // Or you might be interested in godbolt.org.
        return parseInput(&inp[ix + 1]);
    }
    return errorCode;
}


