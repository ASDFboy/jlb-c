// Tasks: functions running side by side, the same as in the Rust interpreter
// (see its src/tasks.rs for the whole story).
//
//     t = START walk(hero)      starts walk(hero) as a task and goes straight on
//     AWAIT(t)                  waits for it to finish and gives back what it returned
//     DONE(t)                   TRUE once it has finished
//     CANCEL(t)                 stops it
//
// Tasks take turns: one runs at a time, and it hands over to the next after SLICE
// statements, or sooner when it WAITs or AWAITs. Each task runs on its own thread so it
// can pause anywhere, but only the thread whose turn it is touches the interpreter.
// An error in a task goes to whatever is AWAITing it (where TRY can catch it); when
// nothing is, it stops the program, like END does.

#include "jlb.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // condition variables need Windows 7 or newer
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define SLICE 1000u
#define TASK_STACK ((size_t)256 * 1024 * 1024)

typedef enum { T_READY, T_SLEEPING, T_AWAITING, T_FINISHED } TState;

typedef struct Task {
    Str *name;
    TState state;
    double wake;  // when sleeping
    int awaiting; // when awaiting
    bool cancelled;
    Value result;
    // While the task isn't running: its function calls.
    Frame *frames;
    int nframes, cap;
    // An error from a task this one is AWAITing.
    int fault;
    ErrorInfo *err;
    // Its function and inputs, until it starts.
    Function *func;
    Slot **vals;
    int nvals;
} Task;

// ---------- whose turn it is ----------

static int turn;
#ifdef _WIN32
static CRITICAL_SECTION lock;
static CONDITION_VARIABLE cv;
static void baton_init(void) {
    InitializeCriticalSection(&lock);
    InitializeConditionVariable(&cv);
}
static void baton_give(int to) {
    EnterCriticalSection(&lock);
    turn = to;
    LeaveCriticalSection(&lock);
    WakeAllConditionVariable(&cv);
}
static void baton_wait(int me) {
    EnterCriticalSection(&lock);
    while (turn != me) SleepConditionVariableCS(&cv, &lock, INFINITE);
    LeaveCriticalSection(&lock);
}
static void sleep_for(double s) {
    if (s > 0) Sleep((DWORD)(s * 1000.0 + 0.5));
}
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void baton_init(void) {
}
static void baton_give(int to) {
    pthread_mutex_lock(&lock);
    turn = to;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lock);
}
static void baton_wait(int me) {
    pthread_mutex_lock(&lock);
    while (turn != me) pthread_cond_wait(&cv, &lock);
    pthread_mutex_unlock(&lock);
}
static void sleep_for(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}
#endif

// ---------- switching ----------

/// Saves the running task's calls, loads the next one's, and gives it the turn.
static void hand_over(Interp *it, int from, int to) {
    Task *f = &it->tasks[from];
    f->frames = it->frames;
    f->nframes = it->nframes;
    f->cap = it->cap;
    Task *t = &it->tasks[to];
    it->frames = t->frames;
    it->nframes = t->nframes;
    it->cap = t->cap;
    t->frames = NULL;
    t->nframes = t->cap = 0;
    it->current = to;
    it->steps = 0;
    baton_give(to);
}

/// The next task to run after `me`, in turn order (`me` itself last). When every task
/// is WAITing, sleeps until the first one wakes. Fails when they're all stuck.
static bool pick_next(Interp *it, int me, int *next) {
    for (;;) {
        // A task's error goes straight to the program's own code.
        if (it->task_fault && it->tasks[0].state != T_FINISHED) {
            it->tasks[0].state = T_READY;
            *next = 0;
            return true;
        }
        double now = now_seconds();
        for (int i = 0; i < it->ntasks; i++) {
            Task *t = &it->tasks[i];
            if (t->state == T_SLEEPING && t->wake <= now) t->state = T_READY;
            else if (t->state == T_AWAITING && it->tasks[t->awaiting].state == T_FINISHED) t->state = T_READY;
            // A cancelled task gets a turn to end.
            if (t->cancelled && t->state != T_FINISHED) t->state = T_READY;
        }
        int n = it->ntasks;
        for (int k = 1; k <= n; k++) {
            int c = (me + k) % n;
            if (it->tasks[c].state == T_READY) {
                *next = c;
                return true;
            }
        }
        bool any = false;
        double wake = 0;
        for (int i = 0; i < n; i++) {
            if (it->tasks[i].state == T_SLEEPING && (!any || it->tasks[i].wake < wake)) {
                wake = it->tasks[i].wake;
                any = true;
            }
        }
        if (!any) return fail("every task is waiting for another one to finish (AWAIT), so none of them can go on");
        out_flush();
        sleep_for(wake - now);
    }
}

/// Lets the other tasks have a turn; comes back when it's this task's turn again.
static bool take_turns(Interp *it) {
    int me = it->current;
    int next;
    if (!pick_next(it, me, &next)) return false;
    if (next != me) {
        hand_over(it, me, next);
        baton_wait(me);
        // Whoever handed back loaded this task's calls (see hand_over).
    }
    it->steps = 0;
    if (it->tasks[me].cancelled) {
        g_fault = FAULT_CANCELLED;
        return false;
    }
    if (me == 0 && it->task_fault) {
        if (g_err) error_free(g_err);
        g_fault = it->task_fault;
        g_err = it->task_err;
        it->task_fault = FAULT_NONE;
        it->task_err = NULL;
        return false;
    }
    return true;
}

// ---------- a task's life ----------

typedef struct {
    Interp *it;
    int id;
} TaskArg;

/// A task's function has ended: keep its result (an error or END goes elsewhere, see
/// the top), then hand over for good.
static void task_finished(Interp *it, int id, bool ok, Value v) {
    Task *t = &it->tasks[id];
    if (ok) {
        t->result = v;
    } else if (g_fault == FAULT_CANCELLED) {
        g_fault = FAULT_NONE;
    } else if (g_fault == FAULT_ERROR) {
        ErrorInfo *e = take_error();
        int w = -1;
        for (int i = 0; i < it->ntasks; i++) {
            if (it->tasks[i].state == T_AWAITING && it->tasks[i].awaiting == id) {
                w = i;
                break;
            }
        }
        if (w >= 0) {
            it->tasks[w].fault = FAULT_ERROR;
            it->tasks[w].err = e;
        } else if (!it->task_fault) {
            it->task_fault = FAULT_TASK_ERROR;
            it->task_err = e;
        } else {
            error_free(e);
        }
    } else {
        int f = g_fault;
        ErrorInfo *e = take_error();
        if (!it->task_fault) {
            it->task_fault = f;
            it->task_err = e;
        } else if (e) {
            error_free(e);
        }
    }
    t->state = T_FINISHED;
    while (it->nframes) frame_pop(it);
    int next;
    if (!pick_next(it, id, &next)) {
        // Every other task is stuck: the program's code gets the error.
        ErrorInfo *e = take_error();
        if (!it->task_fault) {
            it->task_fault = FAULT_ERROR;
            it->task_err = e;
        } else {
            error_free(e);
        }
        next = 0;
    }
    hand_over(it, id, next);
}

static void task_run(TaskArg *a) {
    Interp *it = a->it;
    int id = a->id;
    free(a);
    baton_wait(id);
    Task *t = &it->tasks[id];
    Function *func = t->func;
    Slot **vals = t->vals;
    int nvals = t->nvals;
    t->func = NULL;
    t->vals = NULL;
    Value v = NULL_VALUE;
    bool ok = true;
    if (!t->cancelled) ok = call_prepared(it, func, vals, nvals, &v);
    for (int i = 0; i < nvals; i++) slot_unref(vals[i]);
    free(vals);
    func_unref(func);
    task_finished(it, id, ok, v);
}

#ifdef _WIN32
static unsigned __stdcall task_thread(void *p) {
    task_run(p);
    return 0;
}
#else
static void *task_thread(void *p) {
    task_run(p);
    return NULL;
}
#endif

static void task_init(Task *t, Str *name) {
    memset(t, 0, sizeof *t);
    t->name = name;
    t->state = T_READY;
    t->result = NULL_VALUE;
}

bool task_start(Interp *it, Function *func, Slot **vals, int nvals, int line, Value *out) {
    if (!it->ntasks) {
        baton_init();
        turn = 0;
        it->tcap = 8;
        it->tasks = xmalloc((size_t)it->tcap * sizeof(Task));
        task_init(&it->tasks[0], str_c("the program"));
        it->ntasks = 1;
        it->current = 0;
    }
    if (it->ntasks == it->tcap) {
        it->tcap *= 2;
        it->tasks = xrealloc(it->tasks, (size_t)it->tcap * sizeof(Task));
    }
    int id = it->ntasks;
    Str *name = func->decl->name ? str_ref(func->decl->name) : str_c("FUNC");
    Task *t = &it->tasks[id];
    task_init(t, name);
    func->rc++;
    t->func = func;
    t->vals = vals;
    t->nvals = nvals;
    it->ntasks++;
    // The task's first call, ready to go: made in its own frame list.
    Frame *saved = it->frames;
    int sn = it->nframes, sc = it->cap;
    it->frames = NULL;
    it->nframes = it->cap = 0;
    frame_push(it, func, func->env, line, func->src);
    t->frames = it->frames;
    t->nframes = it->nframes;
    t->cap = it->cap;
    it->frames = saved;
    it->nframes = sn;
    it->cap = sc;

    TaskArg *a = xmalloc(sizeof *a);
    a->it = it;
    a->id = id;
#ifdef _WIN32
    HANDLE h = (HANDLE)_beginthreadex(NULL, (unsigned)TASK_STACK, task_thread, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    bool started = h != NULL;
    if (h) CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, TASK_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    bool started = pthread_create(&th, &attr, task_thread, a) == 0;
    pthread_attr_destroy(&attr);
#endif
    if (!started) {
        free(a);
        // Undo: the task never existed.
        Frame *f = it->frames;
        int fn = it->nframes, fc = it->cap;
        it->frames = t->frames;
        it->nframes = t->nframes;
        it->cap = t->cap;
        while (it->nframes) frame_pop(it);
        free(it->frames);
        it->frames = f;
        it->nframes = fn;
        it->cap = fc;
        for (int i = 0; i < nvals; i++) slot_unref(vals[i]);
        free(vals);
        func_unref(func);
        str_unref(name);
        it->ntasks--;
        return fail("couldn't start another task (the computer is out of threads)");
    }
    out->t = V_TASK;
    out->task.name = str_ref(name);
    out->task.id = id;
    return true;
}

bool task_tick(Interp *it) {
    if (++it->steps >= SLICE) return take_turns(it);
    return true;
}

bool task_sleep(Interp *it, double secs) {
    int me = it->current;
    if (secs < 0) secs = 0;
    if (secs > 1e9) secs = 1e9;
    it->tasks[me].state = T_SLEEPING;
    it->tasks[me].wake = now_seconds() + secs;
    bool ok = take_turns(it);
    it->tasks[me].state = T_READY;
    return ok;
}

static bool task_id(Value v, const char *what, int *id) {
    if (v.t != V_TASK) return fail("%s needs a TASK (from START), not a %s", what, v_type(v));
    *id = v.task.id;
    return true;
}

bool task_await(Interp *it, Value v, Value *out) {
    if (v.t == V_TABLE) {
        Table *src = v.tb.tab;
        Table *t = table_new();
        for (size_t i = 0; i < src->n; i++) {
            Value r;
            if (!task_await(it, src->items[i]->v, &r)) {
                table_unref(t);
                return false;
            }
            table_push(t, r);
        }
        *out = v_table(t);
        return true;
    }
    int id = 0;
    if (!task_id(v, "AWAIT", &id)) return false;
    int me = it->current;
    if (id == me) return fail("a task can't AWAIT itself (it would wait forever)");
    while (it->tasks[id].state != T_FINISHED) {
        it->tasks[me].state = T_AWAITING;
        it->tasks[me].awaiting = id;
        bool ok = take_turns(it);
        it->tasks[me].state = T_READY;
        if (!ok) return false;
    }
    if (it->tasks[me].fault) {
        if (g_err) error_free(g_err);
        g_fault = it->tasks[me].fault;
        g_err = it->tasks[me].err;
        it->tasks[me].fault = FAULT_NONE;
        it->tasks[me].err = NULL;
        return false;
    }
    *out = v_retain(it->tasks[id].result);
    return true;
}

bool task_done(Interp *it, Value v, bool *out) {
    int id = 0;
    if (!task_id(v, "DONE", &id)) return false;
    *out = it->tasks[id].state == T_FINISHED;
    return true;
}

bool task_cancel(Interp *it, Value v) {
    int id = 0;
    if (!task_id(v, "CANCEL", &id)) return false;
    if (id == it->current) return fail("a task can't CANCEL itself; use RETURN to end it");
    if (it->tasks[id].state != T_FINISHED) {
        it->tasks[id].cancelled = true;
        // Let it stop now, so it's DONE when CANCEL comes back.
        Value r;
        if (!task_await(it, v, &r)) return false;
        v_release(r);
    }
    return true;
}

bool tasks_finish(Interp *it) {
    for (;;) {
        int id = -1;
        for (int i = 1; i < it->ntasks; i++) {
            if (it->tasks[i].state != T_FINISHED) {
                id = i;
                break;
            }
        }
        if (id < 0) return true;
        Value v;
        v.t = V_TASK;
        v.task.name = it->tasks[id].name;
        v.task.id = id;
        Value r;
        if (!task_await(it, v, &r)) return false;
        v_release(r);
    }
}

void tasks_cancel_all(Interp *it) {
    if (it->task_err) error_free(it->task_err);
    it->task_fault = FAULT_NONE;
    it->task_err = NULL;
    for (int i = 1; i < it->ntasks; i++)
        if (it->tasks[i].state != T_FINISHED) it->tasks[i].cancelled = true;
    if (!tasks_finish(it)) {
        ErrorInfo *e = take_error();
        if (e) error_free(e);
    }
    if (it->task_err) error_free(it->task_err);
    it->task_fault = FAULT_NONE;
    it->task_err = NULL;
    if (it->ntasks) it->tasks[0].state = T_READY;
}
