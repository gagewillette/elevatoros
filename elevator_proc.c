#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "elevator.h"

static struct proc_dir_entry *elevator_proc_entry;


// Convert passenger type enum to a single character for simple stdout
char passenger_type_char(int type)
{
  switch (type) {
    case PART_TIME:
      return 'P';
    case LAWYER:
      return 'L';
    case BOSS:
      return 'B';
    case VISITOR:
      return 'V';
    default:
      return '?';
  }
}


// this is called when /proc/elevator is read
// prints the current state of the elevator system
static int elevator_proc_show(struct seq_file *m, void *v)
{
  struct passenger *p;
  int i;

  // lock shared data while reading
  mutex_lock(&elevator.lock);

  // basic elevator info
  seq_printf(m, "Elevator state: %s\n", state_names[elevator.current_state]);
  seq_printf(m, "Current floor: %d\n", elevator.current_floor);
  seq_printf(m, "Current load: %d lbs\n", elevator.current_load);

  // print passengers currently inside the elevator
  seq_puts(m, "Elevator status:");
  if (list_empty(&elevator.passengers)) {
    seq_puts(m, " empty");
  } else {
    list_for_each_entry(p, &elevator.passengers, list) {
      seq_printf(m, " %c%d",
                 passenger_type_char(p->type),
                 p->dest_floor);
    }
  }
  seq_putc(m, '\n');
  seq_putc(m, '\n');

  // print each floor (top → bottom)
  for (i = TOTAL_FLOORS; i >= 1; i--) {
    struct floor_info *f = &floors[i - 1];

    // mark current floor with '*'
    seq_printf(m, "[%c] Floor %d: %d",
               (elevator.current_floor == i) ? '*' : ' ',
               i,
               f->count);

    // print waiting passengers on this floor
    list_for_each_entry(p, &f->waiters, list) {
      seq_printf(m, " %c%d",
                 passenger_type_char(p->type),
                 p->dest_floor);
    }

    seq_putc(m, '\n');
  }

  seq_putc(m, '\n');

  // summary stats
  seq_printf(m, "Number of passengers: %d\n", elevator.passenger_count);
  seq_printf(m, "Number of passengers waiting: %d\n", elevator.total_waiting);
  seq_printf(m, "Number of passengers serviced: %d\n", elevator.total_serviced);

  mutex_unlock(&elevator.lock);
  return 0;
}


// called when /proc/elevator is opened
// uses seq_file system
static int elevator_proc_open(struct inode *inode, struct file *file)
{
  return single_open(file, elevator_proc_show, NULL);
}


// Defines how the proc file behaves
static const struct proc_ops elevator_proc_ops = {
  .proc_open = elevator_proc_open,
  .proc_read = seq_read,
  .proc_lseek = seq_lseek,
  .proc_release = single_release,
};


// create elevator proc file on load
int elevator_proc_init(void)
{
  elevator_proc_entry = proc_create("elevator", 0, NULL, &elevator_proc_ops);

  if (!elevator_proc_entry) {
    pr_err("Failed to create /proc/elevator\n");
    return -ENOMEM;
  }

  pr_info("/proc/elevator created\n");
  return 0;
}


// remove proc file on unload
void elevator_proc_exit(void)
{
  if (elevator_proc_entry) {
    proc_remove(elevator_proc_entry);
    elevator_proc_entry = NULL;
  }

  pr_info("/proc/elevator removed\n");
}
