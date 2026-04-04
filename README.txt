Ariel Huo, Alexis Vinci 
261232502, 

this is a log of changes by assignment section

A3
1.2.1
    implemented paging
        new/changed data structures:
        - PCB: 
            - replaced start with pages array (page table that contains the page number of every three lines)
                indexed by frame number (eg. pages[3] will store the third frame of the program (lines 9, 10, 11)) 
                the array is zero indexed so there is a zeroth frame (lines 0 1 2)
            - added field num_pages
            - added field filename to track duplicate files
        - global variable ps: linked list of all available pages in memory (linked bc indexing would be nightmarish)
            - uses type Pages (new) which is a linked list of Page_Entry s (also new)


        new/changed functions
        - new: get_line in shellmemory.c -> gets the line from memory based on program counter
        - changed: every function that used parse_input with base and bounds now implements pagin with get_line 
        - changed: add_script now supports files with same name and takes an extra input (filename)
        - changed: add script supports paging logic 
        - changed: clean script modified according to changes in PCB and add_script
        - new: page_enqueue 
        - new: page_dequeue
        - changed: mem_init adds pages 1-333 (all possible pages with 1000 line memory) to ps 



A2
1.2.1:
    datatypes and access functions added in shellmemory.c
        new datatypes:
        - Line
        - PCB
        - Queue
        - avail_file_numbers
        new functions: 
        - add_script
        - run_queue
        - clean_script
    access function headers added to shellmemory.h
    modified source in interpreter.c
1.2.2
    - exec command added in interpreter.c
    - implemented if branch for FCFS in interpreter.c (exec)

1.2.3
    - added sort_queue for sjf
    - run_queue_sjf and run_queue_rr added in shellmemory.c
    - implemented for if branches SJF and RR in interpreter.c (exec)

1.2.4
    - added job length score in pcb and initializer (initializes to length)
    - added enqueue and dequeue helpers 
    - added run_queue_sjf_aging in shellmemory.c
    - implemented for if branch for AGING in interpreter.c (exec)

1.2.5
    pt1:
    - modified run_queue_rr to take input number of instructions to run before switching
    - added and implemented if branch for RR30 in interpreter.c (exec)

    pt2:
