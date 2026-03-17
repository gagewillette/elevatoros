#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "elevator.h"

// function prototypes
static int start_elevator_impl(void);
static int issue_request_impl(int start_floor, int dest_floor, int type);
static int stop_elevator_impl(void);

char *state_names[] = {"OFFLINE", "IDLE", "LOADING", "UP", "DOWN"};

int passenger_weights[] = {10, 15, 20, 5};

struct elevator_info elevator;
struct floor_info floors[TOTAL_FLOORS];


// the kernel thread that controls the elevator movement  
static int elevator_run(void *data) {
    struct passenger *p, *tmp;

    printk(KERN_INFO "Elevator thread starting...\n");

    while (!kthread_should_stop()) {
        int moved = 0; // any work done by elevator
        int loaded = 0; // any loading done
        int unloaded = 0; // any unloading done

        mutex_lock(&elevator.lock);

        // if stopping & elevator is empty then shut down
        if (elevator.deactivating && elevator.passenger_count == 0) {
            elevator.current_state = OFFLINE;
            printk(KERN_INFO "Elevator empty and deactivating. Goodbye!\n");
            mutex_unlock(&elevator.lock);
            break;
        }
        
        // unload passengers whose destination is this floor
        list_for_each_entry_safe(p, tmp, &elevator.passengers, list) {
            if (p->dest_floor == elevator.current_floor) {
                elevator.current_load -= p->weight;
                elevator.passenger_count--;
                elevator.total_serviced++;
                list_del(&p->list);
                kfree(p);
                unloaded = 1;
                moved = 1;
            }
        }
        
        // print unloading events
        if (unloaded) printk(KERN_INFO "Elevator unloaded passengers on floor %d\n", elevator.current_floor);
        
        // FIFO load passengers waiting on this floor
        if (!elevator.deactivating) {
            struct floor_info *f = &floors[elevator.current_floor - 1];

            list_for_each_entry_safe(p, tmp, &f->waiters, list) {

                // check capacity & wait constraints
                if (elevator.passenger_count < MAX_PASSENGERS && (elevator.current_load + p->weight) <= MAX_WEIGHT) {
                    // move passenger from floor onto elevator
                    list_move_tail(&p->list, &elevator.passengers);

                    elevator.current_load += p->weight;
                    elevator.passenger_count++;

                    f->count--;
                    elevator.total_waiting--;

                    loaded = 1; // set flags
                    moved = 1;
                } else {
                    break;
                }
            }
        }
        
        // print loading events
        if (loaded) {
            printk(KERN_INFO "Elevator loaded passengers on floor %d. Load: %d lbs\n",
                   elevator.current_floor, elevator.current_load);
        }

        // if work done, stay on floor briefly
        if (moved) {
            elevator.current_state = LOADING;
            mutex_unlock(&elevator.lock);
            ssleep(1);
            continue;
        }

        // if no passengers or waiting then set to IDLE
        if (elevator.passenger_count == 0 && elevator.total_waiting == 0) {
            elevator.current_state = IDLE;
            mutex_unlock(&elevator.lock);
            ssleep(1);
            continue;
        }
  
        // determine new direction
        if (elevator.current_state == UP && elevator.current_floor == TOTAL_FLOORS)
            elevator.current_state = DOWN;
        else if (elevator.current_state == DOWN && elevator.current_floor == 1)
            elevator.current_state = UP;
        else if (elevator.current_state == IDLE || elevator.current_state == LOADING)
            elevator.current_state =
                (elevator.current_floor == TOTAL_FLOORS) ? DOWN : UP;


        mutex_unlock(&elevator.lock);

        ssleep(2);

        mutex_lock(&elevator.lock);

        // move elevator one floor
        if (elevator.current_state == UP)
            elevator.current_floor++;
        else if (elevator.current_state == DOWN)
            elevator.current_floor--;

        printk(KERN_INFO "Elevator moved to floor %d [%s]\n",
               elevator.current_floor, state_names[elevator.current_state]);

        mutex_unlock(&elevator.lock);
    }

    return 0;
}

// helper function for hard-coded tests 
void create_test_passenger(int start, int dest, int type) {
    struct passenger *p = kmalloc(sizeof(struct passenger), GFP_KERNEL);
    if (!p) return;

    p->start_floor = start;
    p->dest_floor = dest;
    p->type = type;
    p->weight = passenger_weights[type];

    list_add_tail(&p->list, &floors[start - 1].waiters);
    floors[start - 1].count++;
    elevator.total_waiting++;
}

// initializes data and spawns the kthread 
static int __init elevator_init(void) {
    int i;

    // initialize global states 
    elevator.current_state = IDLE; 
    elevator.current_floor = 1;
    elevator.current_load = 0;
    elevator.passenger_count = 0;
    elevator.total_serviced = 0;
    elevator.total_waiting = 0;
    elevator.deactivating = 0;
    INIT_LIST_HEAD(&elevator.passengers);
    mutex_init(&elevator.lock);

    // initialize all floors 
    for (i = 0; i < TOTAL_FLOORS; i++) {
        INIT_LIST_HEAD(&floors[i].waiters);
        floors[i].count = 0;
    }

  	
    // init elevator state & thread vars
    elevator.current_state = OFFLINE;
    elevator.thread = NULL;

    if (elevator_proc_init()) {
      printk(KERN_ERR "Failed to initialize /proc/elevator\n");
      mutex_destroy(&elevator.lock);
      return -ENOMEM;
    }

    printk(KERN_INFO "Elevator module loaded successfully.\n");

    // this is test data that calls the respective syscalls locally.
    // this is only meant to test the funcitonality and should remove on prod
    // TODO: remove
    start_elevator_impl();
    issue_request_impl(1, 3, BOSS);
    issue_request_impl(1, 5, VISITOR);
    issue_request_impl(2, 4, LAWYER);
    issue_request_impl(4, 1, PART_TIME);
    // TODO: read the above and remove ts

    return 0;
}

// initialize elevator state and launch the
// kernel thread  for controlling elevator movement
static int start_elevator_impl(void)
{
    int ret = 0;

    mutex_lock(&elevator.lock);

    // prevent starting the elevator if it is already active 
    if (elevator.current_state != OFFLINE || elevator.thread != NULL) {
        mutex_unlock(&elevator.lock);
        return 1;
    }

    // Initialize elevator state 
    elevator.current_state = IDLE;
    elevator.current_floor = 1;
    elevator.current_load = 0;
    elevator.passenger_count = 0;
    elevator.total_serviced = 0;
    elevator.total_waiting = 0;
    elevator.deactivating = 0;

    INIT_LIST_HEAD(&elevator.passengers);

    mutex_unlock(&elevator.lock);

    // start elevator control thread 
    elevator.thread = kthread_run(elevator_run, NULL, "elevator_thread");

    if (IS_ERR(elevator.thread)) {
        ret = PTR_ERR(elevator.thread);
        elevator.thread = NULL;

        mutex_lock(&elevator.lock);
        elevator.current_state = OFFLINE;
        mutex_unlock(&elevator.lock);

        return ret;
    }

    return 0;
}

/*
 * Handles a new passenger request.
 *
 * A passenger is dynamically allocated and added to the waiting
 * queue of the specified floor. The elevator thread will later
 * load the passenger when it reaches that floor.
 */
static int issue_request_impl(int start_floor, int dest_floor, int type)
{
    struct passenger *p;

    // Validate request parameters 
    if (start_floor < 1 || start_floor > TOTAL_FLOORS ||
        dest_floor < 1 || dest_floor > TOTAL_FLOORS ||
        start_floor == dest_floor ||
        type < PART_TIME || type > VISITOR) {
        return 1;
    }

    // Allocate passenger structure 
    p = kmalloc(sizeof(struct passenger), GFP_KERNEL);
    if (!p)
        return -ENOMEM;

    // Initialize passenger data
    p->start_floor = start_floor;
    p->dest_floor = dest_floor;
    p->type = type;
    p->weight = passenger_weights[type];

    mutex_lock(&elevator.lock);

    // Add passenger to the waiting list for the start floor
    list_add_tail(&p->list, &floors[start_floor - 1].waiters);

    floors[start_floor - 1].count++;
    elevator.total_waiting++;

    mutex_unlock(&elevator.lock);

    return 0;
}


/*
 * Signals the elevator system to stop accepting new work
 * and begin shutdown.
 *
 * Elevator is set to deactivating state, it will continute
 * executing untill all passengers are off and it can deactive
 */
static int stop_elevator_impl(void)
{
    mutex_lock(&elevator.lock);

    // If already shutting down, ignore duplicate requests
    if (elevator.deactivating) {
        mutex_unlock(&elevator.lock);
        return 1;
    }

    // Signal elevator thread to begin shutdown 
    elevator.deactivating = 1;

    mutex_unlock(&elevator.lock);

    return 0;
}

// clean up memory and stop the thread 
static void __exit elevator_exit(void) {
    struct passenger *p, *tmp;
    int i;

    // exit the proc output
    elevator_proc_exit();

    // tell the thread to stop and wait for it to finish 
    if (elevator.thread) kthread_stop(elevator.thread);

    mutex_lock(&elevator.lock);
    // free all passengers currently inside the elevator 
    list_for_each_entry_safe(p, tmp, &elevator.passengers, list) {
        list_del(&p->list);
        kfree(p);
    }
    // free all passengers waiting on floors 
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
