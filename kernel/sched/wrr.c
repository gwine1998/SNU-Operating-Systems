/* implement TODO marked methods. */
/* the rest is probably not needed */

//cfs load balancing 보면 runqueue에 락잡는 함수가 있음
//load balancing 구현시 for_each_online_cpu
//cpumask_test_cpu() -> 옮길수 있는 cpu에서 이 태스크가 돌아갈 수 있는가?
//task_current() -> 지금 돌고있는 task 판단
//실제 task를 옮길때 deactvie_task  set_task -> activate_task()
// __acquires() <- 락 잡는것들..
// load balance 관련된 로직은 wrr.c에 집어넣고, tick에서 구현하도록 하자

#include "sched.h"

#include <linux/irq_work.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/cpumask.h>


DEFINE_SPINLOCK(wrr_loadbalance_lock); // this lock used at trigger_loadbalance.

static unsigned long last_loadbalance_time; // value that saves last_loadbalance time.
//this is static long value so it is initialized by 0.
// this value used at trigger_loadbalance.

static struct rq *find_lowest_weight_rq(void);

static void print_errmsg(const char * str)
{
    int cpu;
    cpu = get_cpu();
    put_cpu();
    printk(KERN_ALERT"%s, in cpu %d\n",str,cpu);
}


void init_wrr_rq(struct wrr_rq *wrr_rq)
{
    int cpu;

    INIT_LIST_HEAD(&(wrr_rq->queue));
    
    wrr_rq->weight_sum = 0;

    /* TODO.. by CPU number, we shouldn't use wrr_rq. may cpu number is should zero.. */

    cpu = get_cpu();
    put_cpu();
    if (cpu == WRR_NO_USE_CPU_NUM) {
        wrr_rq->usable = 0;
    }
    else {
        wrr_rq->usable = 1;
    }
    
    raw_spin_lock_init(&(wrr_rq->wrr_runtime_lock));
}

// get task of wrr_entity
static struct task_struct *get_task_of_wrr_entity(struct sched_wrr_entity *wrr_entity)
{
	return container_of(wrr_entity, struct task_struct, wrr); 
}

/*
static struct rq *rq_of_wrr_rq(struct wrr_rq *wrr_rq)
{
    return container_of(wrr_rq , struct rq, wrr);
}
*/


/* this function update max&min weight of wrr queue. need locking. */
/*
static void
update_minmax_weight_wrr(struct wrr_rq *wrr_rq)
{
    int i;
    int min_weight = WEIGHT_INITIALVALUE;
    int max_weight = WEIGHT_INITIALVALUE;
    struct list_head *weight_array = wrr_rq->weight_array;

    for(i = WRR_MINWEIGHT; i <= WRR_MAXWEIGHT; i++) 
    {
        if(!list_empty(&(weight_array[i])))
        {
            min_weight = i;
            break;
        }
    }      
    // find min_weight

    for(i = WRR_MAXWEIGHT; i >= WRR_MINWEIGHT; i--) 
    {
        if(!list_empty(&(weight_array[i]))) 
        {
            max_weight = i;
            break;
        }
    }
    // find max_weight

    wrr_rq->min_weight = min_weight;
    wrr_rq->max_weight = max_weight;
    // set weight. if list is all empty, then set initial value.
}
*/

/* this function update wrr queue. modifies count, weight sum, min and max weight.
 when insert item. need locking. */
 /*
static void
update_insert_wrr(struct wrr_rq *wrr_rq, int weight)
{
    (wrr_rq->count)++;
    (wrr_rq->weight_sum) += weight;
    update_minmax_weight_wrr(wrr_rq);
}
*/
/* this function update wrr queue. modifies count, weight sum, min and max weight.
 when delete item. need locking. */
 /*
static void
update_delete_wrr(struct wrr_rq *wrr_rq, int weight)
{
    (wrr_rq->count)--;
    (wrr_rq->weight_sum) -= weight;
    update_minmax_weight_wrr(wrr_rq);
}
*/

/* this function update new_weight to weight, and change wrr_rq's structure by then. need locking. */
/*
static void update_task_weight_wrr(struct wrr_rq *wrr_rq, struct task_struct *p, int new_weight)
{
    struct sched_wrr_entity *wrr_se;
    int old_weight;

    wrr_se = &(p->wrr);
    old_weight = wrr_se->weight;

    if(old_weight == new_weight) {  // no need to update wrr_entity.        
        return;
    }

    wrr_se->weight = new_weight;

    list_move_tail(&(wrr_se->weight_list), &(wrr_rq->weight_array[new_weight]));

    wrr_rq->weight_sum += new_weight - old_weight;

    update_minmax_weight_wrr(wrr_rq);
}
*/

/* this function update new_weight to weight, and change wrr_rq's structure by then. */
/*
void update_task_weight_wrr_by_task(struct task_struct *p, int new_weight)
{
    struct wrr_rq *wrr_rq;
    struct sched_wrr_entity *wrr_se;

    wrr_se = &(p->wrr);
    wrr_rq = wrr_se->wrr_runqueue;

    if(wrr_rq == NULL)
    {
        printk(KERN_ALERT" wrr_se doesn't have runqueue! \n");
        return;
    }

    raw_spin_lock(&(wrr_rq->wrr_runtime_lock));

    update_task_weight_wrr(wrr_rq, p, new_weight);

    raw_spin_unlock(&(wrr_rq->wrr_runtime_lock));
}
*/


/* enqueue, dequeue, requeue */
/* assume for now that the entity p->wrr is populated */
static void
enqueue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
    struct rq *lowest_rq;
    struct wrr_rq *wrr_rq;
    struct sched_wrr_entity *wrr_entity;
    struct list_head *wrr_rq_queue;
    struct list_head *wrr_entity_run_list;
    int lowest_weight;
    int weight;
    int queue_weight = rq->wrr.weight_sum;
    
    print_errmsg("enqueue start");

    rcu_read_lock();
    lowest_rq = find_lowest_weight_rq();
    rcu_read_unlock();

    wrr_rq = &(rq->wrr);
    wrr_entity = &(p->wrr);
    weight = wrr_entity->weight;
    lowest_weight = lowest_rq->wrr.weight_sum;

    if(lowest_rq && ((wrr_rq->usable == 0) || (queue_weight > lowest_weight) ))  //if lowest rq is not null, and weight of lowest rq is bigger than current, migrate task to lowest queue.
    {
        print_errmsg("enqueue in first if");
        raw_spin_lock(&(lowest_rq->lock));  // get runqueue lock.
        if(wrr_entity->on_rq) deactivate_task(rq, p, 0); // if wrr_entity is queue in other queue, deactive and dequeue.
        set_task_cpu(p, lowest_rq->cpu);
        activate_task(lowest_rq, p, 0);
        
        resched_curr(lowest_rq);
        raw_spin_unlock(&(lowest_rq->lock));
    }
    else
    {
        print_errmsg("enqueue in second if");
        wrr_rq_queue = &(wrr_rq->queue);
        wrr_entity_run_list = &(wrr_entity->run_list);
        // list values initialize.

        list_add_tail(wrr_entity_run_list, wrr_rq_queue);
        // add wrr_entity to runqueue

        wrr_rq->weight_sum += weight;
        wrr_entity->time_slice = wrr_entity->weight * WRR_TIMESLICE;
        wrr_entity->on_rq = 1;
    }

    print_errmsg("enqueue end");
    
}

static void
dequeue_task_wrr(struct rq *rq, struct task_struct *p, int flags)
{
    struct wrr_rq *wrr_rq;
    struct sched_wrr_entity *wrr_entity;
    struct list_head *wrr_rq_queue;
    struct list_head *wrr_entity_run_list;
    int weight;
    
    print_errmsg("dequeue start");
    wrr_rq = &(rq->wrr);

    wrr_entity = &(p->wrr);
    weight = wrr_entity->weight;

    wrr_rq_queue = &(wrr_rq->queue);
    wrr_entity_run_list = &(wrr_entity->run_list);

    // list values initialize.

    list_del_init(wrr_entity_run_list);

    wrr_rq->weight_sum -= weight;
    wrr_entity->on_rq = 0;
    print_errmsg("enqueue end");
}

/* need to update entity AFTER requeue. */  
// requeue current task. need locking.
static void
requeue_task_wrr(struct rq *rq, struct task_struct *p)
{
    /* todo - many other things */
    struct wrr_rq *wrr_rq;
    struct sched_wrr_entity *wrr_se;
    struct list_head *wrr_entity_run_list;

    wrr_rq = &(rq->wrr);

    if(p == NULL) return;
    if(p->policy != SCHED_WRR) return;
    wrr_se = &(p->wrr);
    wrr_entity_run_list = &(wrr_se->run_list);

    list_del_init(wrr_entity_run_list);
    list_add_tail(wrr_entity_run_list, &(wrr_rq->queue));

    wrr_se->time_slice = wrr_se->weight * WRR_TIMESLICE;
    print_errmsg("requeue end");
}

static void yield_task_wrr(struct rq *rq)
{
    requeue_task_wrr(rq, rq->curr);
}

static bool yield_to_task_wrr(struct rq *rq, struct task_struct *p, bool preempt)
{
	return true;
}

static struct task_struct *
pick_next_task_wrr(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
    struct wrr_rq *wrr_rq = &(rq->wrr);
    if(list_empty(&(wrr_rq->queue))) {
        return NULL;
    }
    else {
        return get_task_of_wrr_entity(list_first_entry(&(wrr_rq->queue), struct sched_wrr_entity, run_list));
    }
}

static void put_prev_task_wrr(struct rq *rq, struct task_struct *p)
{
    return;
}

static void switched_to_wrr(struct rq *rq, struct task_struct *p)
{
    // TODO
    // populate p->wrr (which is entity)
    // assign task to laziest cpu
    struct sched_wrr_entity *wrr_entity;

    if(p == NULL) return;
    if(p->policy != SCHED_WRR) return;
    // if (list_empty(&(p->wrr.run_list))) return;

    wrr_entity = &(p->wrr);
    wrr_entity->weight = 10;
    wrr_entity->time_slice = wrr_entity->weight * WRR_TIMESLICE;
    print_errmsg("switch end");
}

static void
prio_changed_wrr(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void set_curr_task_wrr(struct rq *rq)
{
}

static unsigned int
get_rr_interval_wrr(struct rq *rq, struct task_struct *task)
{
    return task->wrr.weight * WRR_TIMESLICE;
}

static void
check_preempt_curr_wrr(struct rq *rq, struct task_struct *p, int flags)
{
}

/* scheduler_tick callback function */
/* called by timer. */
static void task_tick_wrr(struct rq *rq, struct task_struct *p, int queued) // TODO rev
{
    struct wrr_rq *wrr_rq;
    struct task_struct *curr;
    struct sched_wrr_entity *wrr_entity;
    struct list_head *wrr_entity_run_list;

    wrr_rq = &(rq->wrr);

    if(p == NULL)   // if task_struct don't accessible..
    {
        return;
    }
    else if(p->policy != SCHED_WRR) // policy is not SCHED_WRR, don't need to do job.
    {
        return;
    }

    curr = rq->curr;
    wrr_entity = &(curr->wrr);
    wrr_entity_run_list = &(wrr_entity->run_list);
    
    //printk(KERN_ALERT"task_tick_wrr_called %d\n",wrr_entity->time_slice);

    if(--(wrr_entity->time_slice) > 0)  // if current task time_slice is not zero.. don't need to re_schedule.
    {
        return;
    }
    else
    {
        print_errmsg("task_tick_wrr in else");
        //printk(KERN_ALERT"before requeue %d\n",wrr_entity->time_slice);
        wrr_entity->time_slice = wrr_entity->weight * WRR_TIMESLICE;
        requeue_task_wrr(rq, curr);
        print_errmsg("task_tick_wrr after requeue");
        //printk(KERN_ALERT"after requeue %d\n",wrr_entity->time_slice);
        resched_curr(rq); // TODO : is this ok for when holding lock, resched_curr is correct?
        print_errmsg("task_tick_wrr after resched");
        // should we use set_tsk_need_resched(p)?
        //printk(KERN_ALERT"after resched %d\n",wrr_entity->time_slice);
    }
    return;
}

/*
 * from fair.c
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
static void task_fork_wrr(struct task_struct *p)
{
    if (p == NULL) return;

    (p->wrr).weight = ((p->real_parent)->wrr).weight;
    (p->wrr).time_slice = (p->wrr).weight * WRR_TIMESLICE;

    return;
}

static void task_dead_wrr(struct task_struct *p)
{
    /* TODO */
    return;
}

// return value NULL mean that there is no cpu to migration. need RCU lock.
static struct rq *find_lowest_weight_rq(void)
{
    int curr_cpu;
    int curr_weight;
    int lowest_weight;
    struct rq *curr_rq;
    struct rq *lowest_rq;
    struct wrr_rq *curr_wrr_rq;

    lowest_rq = NULL; // not initialized..
    lowest_weight = -1;

    curr_cpu = -1;
    for_each_online_cpu(curr_cpu)
    {
        curr_rq = cpu_rq(curr_cpu);
        curr_wrr_rq = &(curr_rq->wrr);
        if(curr_wrr_rq->usable != 0) // if cpu is usable
        {
            curr_weight = curr_wrr_rq->weight_sum;
            if(lowest_weight == -1) // not initialized..
            {
                lowest_rq = curr_rq;
                lowest_weight = curr_weight;
            }
            else
            {
                if(lowest_weight > curr_weight)
                {
                    lowest_rq = curr_rq;
                    lowest_weight = curr_weight;
                }
            }
        }
    }

    return lowest_rq;
}





// return value NULL mean that there is no cpu to migration. need RCU lock.
static struct rq *find_highest_weight_rq(void)
{
    int curr_cpu;
    int curr_weight;
    int highest_weight;
    struct rq *curr_rq;
    struct rq *highest_rq;
    struct wrr_rq *curr_wrr_rq;
    
    highest_rq = NULL;
    highest_weight = -1; // not initialized..

    curr_cpu = -1;
    for_each_online_cpu(curr_cpu)
    {
        curr_rq = cpu_rq(curr_cpu);
        curr_wrr_rq = &(curr_rq->wrr);
        if(curr_wrr_rq->usable != 0) // if cpu is usable
        {
            curr_weight = curr_wrr_rq->weight_sum;
            if(highest_weight == -1)
            {
                highest_rq = curr_rq;
                highest_weight = curr_weight;
            }
            else
            {
                if(highest_weight < curr_weight)
                {
                    highest_rq = curr_rq;
                    highest_weight = curr_weight;
                }
            }
        }
    }

    return highest_rq;
}

// if migration available, return 1 else return 0.
static int is_migration_available(struct rq *rq_from, struct rq *rq_to, struct task_struct *mig_task)
{
    int weight_rq_from = (rq_from->wrr).weight_sum;
    int weight_rq_to = (rq_to->wrr).weight_sum;
    int weight_mig = (mig_task->wrr).weight;

    // current task is running
    if (rq_from->curr == mig_task)
        return 0;
    
    // migrated task is not runable at other cpu.
    if (!cpumask_test_cpu(rq_to->cpu, &(mig_task->cpus_allowed)))
        return 0;

    if(weight_rq_from - weight_mig <= weight_rq_to + weight_mig) return 0;
    else return 1;
}

// this function called in core.c for load balancing.
void trigger_load_balance_wrr(struct rq *rq)
{
    // TODO
    /* check if 2000ms passed since last load balancing */

    struct rq *rq_highest_weight;
    struct rq *rq_lowest_weight;
    struct wrr_rq *wrr_rq_highest_weight;
    struct wrr_rq *wrr_rq_lowest_weight;
    struct task_struct *migrated_task = NULL;
    struct task_struct *current_task;
    struct list_head *wrr_rq_runqueue;

    unsigned long current_time;
    int weight_highest;
    int weight_lowest;
    int curr_weight;
    int mig_weight = 0;

    struct sched_wrr_entity *curr_entity;



    // check is 2000ms passed
    spin_lock(&wrr_loadbalance_lock);
    current_time = jiffies;
    if (current_time < last_loadbalance_time + 2*HZ)
    {
        spin_unlock(&wrr_loadbalance_lock);
        return;
    }
    last_loadbalance_time = current_time;
    spin_unlock(&wrr_loadbalance_lock);

    print_errmsg("loadbalance after time check");
    rcu_read_lock();
    rq_highest_weight = find_highest_weight_rq();
    rq_lowest_weight = find_lowest_weight_rq();
    rcu_read_unlock();

    if(rq_highest_weight == NULL) return;
    if(rq_lowest_weight == NULL) return;
    // can't find runqueue to migrate.  
    // (1 cpu exist or there is no usable cpu. only cpu wrr_rq->usable is 0)

    if(rq_highest_weight->wrr.weight_sum == rq_lowest_weight->wrr.weight_sum) return;
    // 1 cpu exist case

    double_rq_lock(rq_highest_weight, rq_lowest_weight);
    
    wrr_rq_highest_weight = &(rq_highest_weight->wrr);
    wrr_rq_lowest_weight = &(rq_lowest_weight->wrr);

    weight_highest = wrr_rq_highest_weight->weight_sum;
    weight_lowest = wrr_rq_lowest_weight->weight_sum;

    mig_weight = 0;
    migrated_task = NULL;
    wrr_rq_runqueue = &(wrr_rq_highest_weight->queue);

    // now find candidate for migration.
    list_for_each_entry(curr_entity, wrr_rq_runqueue, run_list)
    {
        current_task = get_task_of_wrr_entity(curr_entity);
        curr_weight = (current_task->wrr).weight;

        if(is_migration_available(rq_highest_weight, rq_lowest_weight, current_task) && (mig_weight < curr_weight))
        {
            mig_weight = curr_weight;
            migrated_task = current_task;
        }
    }

    if (migrated_task == NULL)  // there is no migrate task.
    {
        double_rq_unlock(rq_highest_weight, rq_lowest_weight);
        return;
    }

    // deactive task before moving to other cpu.

    deactivate_task(rq_highest_weight, migrated_task, 0);
    set_task_cpu(migrated_task, rq_lowest_weight->cpu);
    activate_task(rq_lowest_weight, migrated_task, 0);
    resched_curr(rq_lowest_weight);

    double_rq_unlock(rq_highest_weight, rq_lowest_weight);

    /* if this cpu == highest and weight differnce is enough then */
    /*     search for suitable task for migration */
    /*     (e.g. not running and compatiable with lowest cpu) */
    /*     if no such task then */
    /*         load balancing failed. return */
    /*     else then */
    /*         migrate task */
    print_errmsg("loadbalance end");
}

/**
 * cpumask_any_but_online - return a "random" in a cpumask, but not this one.
 * @mask: the cpumask to search
 * @cpu: the cpu to ignore.
 *
 * Often used to find any cpu but smp_processor_id() in a mask.
 * Returns >= nr_cpu_ids if no cpus set.
 */
// need rcu lock. this function copied & modified from lib/cpumask.c
static int cpumask_any_but_online(const struct cpumask *mask, unsigned int cpu)
{
	unsigned int i;

	cpumask_check(cpu);
	for_each_cpu_and(i, mask, cpu_online_mask) // iterate at availble & online cpu.
		if (i != cpu)
			break;
	return i;
}

/* copied from rt.c's implementation */
// return cpu that is suitable to get new task 
static int
select_task_rq_wrr(struct task_struct *p, int cpu, int sd_flag, int flags)
{

    // TODO : handle error when there is no cpu to insert new task.
    struct rq *target_rq;
    int target;

    print_errmsg("select_task start");
	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK) {
        return cpu;
    }

    if(p->nr_cpus_allowed == 1) {
        rcu_read_lock();
        target = cpumask_any_but_online(&p->cpus_allowed, WRR_NO_USE_CPU_NUM);
        rcu_read_unlock();
        if(target >= nr_cpu_ids)
        {
            printk(KERN_ALERT"there is no cpu that task is allowed!\n");
            target = cpumask_first(&(p->cpus_allowed));
        }   // need to handle error
        else if (target == WRR_NO_USE_CPU_NUM)
        {
            printk(KERN_ALERT"task's only available cpu is cpu zero! this is not allowed!\n");
            target = cpumask_first(&(p->cpus_allowed));
        }   // need to handle error
        return target;
    }

    rcu_read_lock();
    target_rq = find_lowest_weight_rq();
    rcu_read_unlock();

    target = (target_rq == NULL) ? cpu : cpu_of(target_rq);

    if (cpumask_test_cpu(target, &p->cpus_allowed) == 0) {  // if task coudln't assign to target, just get random task
        rcu_read_lock();
        target = cpumask_any_but_online(&p->cpus_allowed, WRR_NO_USE_CPU_NUM);
        rcu_read_unlock();
        if(target >= nr_cpu_ids)
        {
            printk(KERN_ALERT"there is no cpu that task is allowed!\n");
            target = cpumask_first(&(p->cpus_allowed));
        }   // need to handle error
        
    }
    return target;

    print_errmsg("select_task end");
}

static void task_woken_wrr(struct rq *rq, struct task_struct *p)
{
}

static void rq_online_wrr(struct rq *rq)
{
}

static void rq_offline_wrr(struct rq *rq)
{
}

static void switched_from_wrr(struct rq *rq, struct task_struct *p)
{
}

static void update_curr_wrr(struct rq *rq)
{
}

const struct sched_class wrr_sched_class = {

    .next               = &fair_sched_class,
    .enqueue_task       = enqueue_task_wrr,
    .dequeue_task       = dequeue_task_wrr,
    .yield_task         = yield_task_wrr,
    .yield_to_task = yield_to_task_wrr,
    .check_preempt_curr = check_preempt_curr_wrr,
    .pick_next_task     = pick_next_task_wrr,
    .put_prev_task      = put_prev_task_wrr,
    .set_curr_task      = set_curr_task_wrr,
    .task_tick          = task_tick_wrr,
    .task_fork          = task_fork_wrr,
    .task_dead          = task_dead_wrr,
    .get_rr_interval    = get_rr_interval_wrr,
    .prio_changed       = prio_changed_wrr,
    .switched_to        = switched_to_wrr,
    .update_curr        = update_curr_wrr,


    .select_task_rq   = select_task_rq_wrr,
    .set_cpus_allowed = set_cpus_allowed_common,
    .rq_online        = rq_online_wrr,
    .rq_offline       = rq_offline_wrr,
    .task_woken       = task_woken_wrr,
    .switched_from    = switched_from_wrr

};
