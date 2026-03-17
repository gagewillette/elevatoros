#ifndef ELEVATOR_H
#define ELEVATOR_H

#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/proc_fs.h>

#define MAX_WEIGHT 70
#define MAX_PASSENGERS 5
#define TOTAL_FLOORS 5

enum type { PART_TIME = 0, LAWYER = 1, BOSS = 2, VISITOR = 3 };
enum state { OFFLINE = 0, IDLE = 1, LOADING = 2, UP = 3, DOWN = 4 };

extern char *state_names[];
extern int passenger_weights[];

struct passenger {
    int start_floor;
    int dest_floor;
    int type;
    int weight;
    struct list_head list;
};

struct elevator_info {
    enum state current_state;
    int current_floor;
    int current_load;
    int passenger_count;
    int total_serviced;
    int total_waiting;
    int deactivating;
    struct list_head passengers;
    struct mutex lock;
    struct task_struct *thread;
};

struct floor_info {
    struct list_head waiters;
    int count;
};

extern struct elevator_info elevator;
extern struct floor_info floors[TOTAL_FLOORS];

int elevator_proc_init(void);
void elevator_proc_exit(void);
char passenger_type_char(int type);

#endif
