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
