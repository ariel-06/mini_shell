#define MEM_SIZE 1000
void mem_init();
char *mem_get_value(char *var);
void mem_set_value(char *var, char *value);
int add_script(FILE *f);
void run_queue(void);
void clean_script(int pid);
void run_queue_sjf(void);
void run_queue_rr(int);
void run_queue_sjf_aging(void);