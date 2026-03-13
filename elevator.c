#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h> 

#define MAX_WEIGHT 70
#define MAX_PASSENGERS 5
#define TOTAL_FLOORS 5

enum type { PART_TIME = 0, LAWYER = 1, BOSS = 2, VISITOR = 3 };
enum state { OFFLINE = 0, IDLE = 1, LOADING = 2, UP = 3, DOWN = 4 };
static char *state_names[] = {"OFFLINE", "IDLE", "LOADING", "UP", "DOWN"};

int passenger_weights[] = {10, 15, 20, 5};
char type_chars[] = {'P', 'L', 'B', 'V'};

// each passenger has a starting floor, destination floor, type and weight 
// To create a list of a given struct item,
// Include list_head within the struct definition
struct passenger {
    int start_floor;
    int dest_floor;
    int type;
    int weight;
    struct list_head list;
};

struct elevator_info {
    enum state current_state; // elevator is either offline, idle, loading, up, or down 
    int current_floor;
    int current_load; // total weight on elevator 
    int passenger_count;
    int total_serviced;
    int total_waiting;	// # of passengers in ll
    int deactivating; // Flag for stop_elevator
    struct list_head passengers; // List of passengers inside
    struct mutex lock; // lock is for handling shared data between floors and elevators 
    struct task_struct *thread;  // kthread for controlling elevator movement  
} elevator;


// array floors of size 5 
struct floor_info {
    struct list_head waiters; // list of passengers waiting on that floor 
    int count; // # of people standing on that floor 
} floors[TOTAL_FLOORS];

static int elevator_run(void *data) {
    struct passenger *p, *tmp;
    int moved;

    printk(KERN_INFO "Elevator thread starting...\n");

    while (!kthread_should_stop()) {
        moved = 0;

        mutex_lock(&elevator.lock);

        // --- DEACTIVATION CHECK ---
        if (elevator.deactivating && elevator.passenger_count == 0) {
            elevator.current_state = OFFLINE;
            printk(KERN_INFO "Elevator empty and deactivating. Goodbye!\n");
            mutex_unlock(&elevator.lock);
            break;
        }

        // --- UNLOADING LOGIC ---
        list_for_each_entry_safe(p, tmp, &elevator.passengers, list) {
            if (p->dest_floor == elevator.current_floor) {
                elevator.current_load -= p->weight;
                elevator.passenger_count--;
                elevator.total_serviced++;
                list_del(&p->list);
                kfree(p);
                moved = 1;
            }
        }
        if (moved) printk(KERN_INFO "Elevator unloaded passengers on floor %d\n", elevator.current_floor);

        // --- LOADING LOGIC ---
        if (!elevator.deactivating) {
            struct floor_info *f = &floors[elevator.current_floor - 1];
            list_for_each_entry_safe(p, tmp, &f->waiters, list) {
                if (elevator.passenger_count < MAX_PASSENGERS &&
                   (elevator.current_load + p->weight) <= MAX_WEIGHT) {

                    list_move_tail(&p->list, &elevator.passengers);
                    elevator.current_load += p->weight;
                    elevator.passenger_count++;
                    f->count--;
                    elevator.total_waiting--;
                    moved = 1;
                } else {
                    break; // FIFO: if the first person can't fit, nobody behind them can
                }
            }
        }
        if (moved) printk(KERN_INFO "Elevator loaded passengers on floor %d. Load: %d lbs\n", elevator.current_floor, elevator.current_load);

        // --- MOVEMENT & DELAY LOGIC ---
        if (moved) {
            elevator.current_state = LOADING;
            mutex_unlock(&elevator.lock);
            ssleep(1); // Loading delay
        } else {
            if (elevator.passenger_count == 0 && elevator.total_waiting == 0) {
                elevator.current_state = IDLE;
                mutex_unlock(&elevator.lock);
                ssleep(1); // Idle polling delay
            } else {
                // Simple Scan Algorithm: Up to 5, Down to 1
                if (elevator.current_state == UP && elevator.current_floor == TOTAL_FLOORS)
                    elevator.current_state = DOWN;
                else if (elevator.current_state == DOWN && elevator.current_floor == 1)
                    elevator.current_state = UP;
                else if (elevator.current_state == IDLE || elevator.current_state == LOADING)
                    elevator.current_state = (elevator.current_floor == TOTAL_FLOORS) ? DOWN : UP;

                mutex_unlock(&elevator.lock);

                ssleep(2); // Travel delay

                mutex_lock(&elevator.lock);
                if (elevator.current_state == UP) elevator.current_floor++;
                else if (elevator.current_state == DOWN) elevator.current_floor--;

                printk(KERN_INFO "Elevator moved to floor %d [%s]\n", elevator.current_floor, state_names[elevator.current_state]);
                mutex_unlock(&elevator.lock);
            }
        }
    }
    return 0;
}

void create_test_passenger(int start, int dest, int type) {
    struct passenger *p = kmalloc(sizeof(struct passenger), GFP_KERNEL);
    if (!p) return;

    p->start_floor = start;
    p->dest_floor = dest;
    p->type = type;
    p->weight = passenger_weights[type];

    // Add to the floor list
    list_add_tail(&p->list, &floors[start - 1].waiters);
    floors[start - 1].count++;
    elevator.total_waiting++;
}

/* --- 4. MODULE INITIALIZATION --- */
static int __init elevator_init(void) {
    int i;

    elevator.current_state = IDLE; // Set to IDLE for testing movement
    elevator.current_floor = 1;
    elevator.current_load = 0;
    elevator.passenger_count = 0;
    elevator.total_serviced = 0;
    elevator.total_waiting = 0;
    elevator.deactivating = 0;
    INIT_LIST_HEAD(&elevator.passengers);
    mutex_init(&elevator.lock);

    for (i = 0; i < TOTAL_FLOORS; i++) {
        INIT_LIST_HEAD(&floors[i].waiters);
        floors[i].count = 0;
    }


    mutex_lock(&elevator.lock);

    // Test Case: Complex Pickup/Dropoff
    // 1. A Boss on Floor 1 going to Floor 3 (20 lbs)
    create_test_passenger(1, 3, BOSS);

    // 2. A Visitor on Floor 1 going to Floor 5 (5 lbs)
    // Elevator should pick up both on Floor 1
    create_test_passenger(1, 5, VISITOR);

    // 3. A Lawyer on Floor 2 going to Floor 4 (15 lbs)
    // Elevator should pick up while passing Floor 2
    create_test_passenger(2, 4, LAWYER);

    // 4. A Part-timer on Floor 4 going to Floor 1 (10 lbs)
    // Elevator should ignore this until it finishes going UP
    create_test_passenger(4, 1, PART_TIME);

    mutex_unlock(&elevator.lock);

    elevator.thread = kthread_run(elevator_run, NULL, "elevator_thread");
    if (IS_ERR(elevator.thread)) {
        return PTR_ERR(elevator.thread);
    }

    printk(KERN_INFO "Elevator module loaded successfully.\n");
    return 0;
}

static void __exit elevator_exit(void) {
    struct passenger *p, *tmp;
    int i;

    if (elevator.thread) kthread_stop(elevator.thread);

    mutex_lock(&elevator.lock);
    list_for_each_entry_safe(p, tmp, &elevator.passengers, list) {
        list_del(&p->list);
        kfree(p);
    }
    for (i = 0; i < TOTAL_FLOORS; i++) {
        list_for_each_entry_safe(p, tmp, &floors[i].waiters, list) {
            list_del(&p->list);
            kfree(p);
        }
    }
    mutex_unlock(&elevator.lock);
    mutex_destroy(&elevator.lock);

    printk(KERN_INFO "Elevator module unloaded.\n");
}

MODULE_LICENSE("GPL");
module_init(elevator_init);
module_exit(elevator_exit);
