#include "history/live_title_async.h"
#include "core/memory.h"
#include "core/mem_budget.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef enum { TITLE_EMPTY, TITLE_QUEUED, TITLE_RUNNING, TITLE_DONE } TitleState;
typedef struct {
    u64 token;
    u32 users;
    TitleState state;
    LiveTitleResult value;
} TitleSlot;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t worker;
    Arena arena;
    TitleSlot *slots;
    u64 next_token;
    bool started, stop, exit_registered;
    void (*wake)(void);
} titles = {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static bool title_cancelled(void *ctx) {
    u64 token = *(const u64 *)ctx;
    pthread_mutex_lock(&titles.lock);
    bool cancelled = titles.stop ||
        titles.slots[token % LIVE_TITLE_ASYNC_CAP].token != token;
    pthread_mutex_unlock(&titles.lock);
    return cancelled;
}

static void *title_worker(void *unused) {
    (void)unused;
#ifdef __APPLE__
    (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    for (;;) {
        pthread_mutex_lock(&titles.lock);
        i32 index = -1;
        while (!titles.stop) {
            /* FIFO over the bounded set, independent of pane/window order. */
            for (i32 i = 0; i < LIVE_TITLE_ASYNC_CAP; i++)
                if (titles.slots[i].state == TITLE_QUEUED &&
                    (index < 0 || titles.slots[i].token < titles.slots[index].token)) index = i;
            if (index >= 0) break;
            pthread_cond_wait(&titles.changed, &titles.lock);
        }
        if (titles.stop) { pthread_mutex_unlock(&titles.lock); break; }
        TitleSlot *slot = &titles.slots[index];
        slot->state = TITLE_RUNNING;
        u64 token = slot->token;
        LiveTitleResult answer = slot->value;
        pthread_mutex_unlock(&titles.lock);

        /* The parser's mutable caches have one production caller: this worker.
         * No filesystem traversal runs while holding the mailbox. */
        answer.found = chat_live_title_cancellable(answer.tool, answer.fg_pid, answer.cwd,
                                      answer.title, sizeof answer.title, title_cancelled, &token);

        pthread_mutex_lock(&titles.lock);
        void (*wake)(void) = NULL;
        if (!titles.stop && slot->token == token) {
            slot->value = answer;
            slot->state = TITLE_DONE;
            wake = titles.wake;
        }
        pthread_mutex_unlock(&titles.lock);
        if (wake) wake();
    }
    return NULL;
}

void live_title_async_init(void (*wake)(void)) {
    pthread_mutex_lock(&titles.lock);
    titles.wake = wake;
    titles.stop = false;
    if (!titles.exit_registered)
        titles.exit_registered = atexit(live_title_async_shutdown) == 0;
    pthread_mutex_unlock(&titles.lock);
}

u64 live_title_async_request(ChatTool tool, i32 fg_pid, const char *cwd) {
    if (tool != CHAT_TOOL_CLAUDE && tool != CHAT_TOOL_CODEX &&
        tool != CHAT_TOOL_KIMI && tool != CHAT_TOOL_COMMANDCODE && tool != CHAT_TOOL_CLINE &&
        tool != CHAT_TOOL_PI && tool != CHAT_TOOL_OMP) return 0;
    if (!cwd) cwd = "";
    usize n = strlen(cwd);
    if (n >= LIVE_TITLE_CWD_CAP) return 0; /* never query a truncated directory */
    pthread_mutex_lock(&titles.lock);
    if (titles.stop) { pthread_mutex_unlock(&titles.lock); return 0; }
    if (!titles.slots) {
        titles.arena = arena_create(sizeof(TitleSlot) * LIVE_TITLE_ASYNC_CAP);
        titles.slots = arena_alloc(&titles.arena, sizeof(TitleSlot) * LIVE_TITLE_ASYNC_CAP);
        if (!titles.slots) {
            arena_destroy(&titles.arena);
            pthread_mutex_unlock(&titles.lock); return 0;
        }
        memset(titles.slots, 0, sizeof(TitleSlot) * LIVE_TITLE_ASYNC_CAP);
        mem_budget_transient_adjust(MEM_HISTORY, (isize)titles.arena.size);
    }
    if (!titles.started) {
        if (pthread_create(&titles.worker, NULL, title_worker, NULL) != 0) {
            pthread_mutex_unlock(&titles.lock); return 0;
        }
        titles.started = true;
    }
    i32 vacant = -1;
    for (i32 i = 0; i < LIVE_TITLE_ASYNC_CAP; i++) {
        TitleSlot *slot = &titles.slots[i];
        /* Panes of one tool in one directory ask the same question unless the
         * probe attributes by pid (only Claude), so they share one run instead
         * of queueing the same store scan once per pane. */
        if ((slot->state == TITLE_QUEUED || slot->state == TITLE_RUNNING) &&
            slot->value.tool == tool &&
            (!live_title_tool_keys_pid(tool) || slot->value.fg_pid == fg_pid) &&
            !strcmp(slot->value.cwd, cwd)) {
            if (slot->users == (u32)-1) { pthread_mutex_unlock(&titles.lock); return 0; }
            slot->users++;
            u64 token = slot->token;
            pthread_mutex_unlock(&titles.lock); return token;
        }
        /* A completed answer still belongs to its consumers until released. */
        if (slot->state == TITLE_EMPTY) {
            if (vacant < 0 || slot->token < titles.slots[vacant].token) vacant = i;
        }
    }
    if (vacant < 0) { pthread_mutex_unlock(&titles.lock); return 0; }
    TitleSlot *slot = &titles.slots[vacant];
    memset(slot, 0, sizeof *slot);
    /* Encode the slot as well as its generation: polling 128 panes must not
     * search 128 slots per pane on every event-loop turn. */
    slot->token = ++titles.next_token * LIVE_TITLE_ASYNC_CAP + (u64)vacant;
    if (!slot->token) slot->token = ++titles.next_token * LIVE_TITLE_ASYNC_CAP + (u64)vacant;
    slot->users = 1;
    slot->state = TITLE_QUEUED;
    slot->value.tool = tool;
    slot->value.fg_pid = fg_pid;
    memcpy(slot->value.cwd, cwd, n + 1);
    u64 token = slot->token;
    pthread_cond_signal(&titles.changed);
    pthread_mutex_unlock(&titles.lock);
    return token;
}

LiveTitleStatus live_title_async_result(u64 token, LiveTitleResult *out) {
    if (!token || !out) return LIVE_TITLE_EXPIRED;
    LiveTitleStatus status = LIVE_TITLE_EXPIRED;
    pthread_mutex_lock(&titles.lock);
    if (titles.slots) {
        TitleSlot *slot = &titles.slots[token % LIVE_TITLE_ASYNC_CAP];
        if (slot->token == token) {
            status = slot->state == TITLE_DONE ? LIVE_TITLE_READY : LIVE_TITLE_PENDING;
            if (status == LIVE_TITLE_READY) *out = slot->value;
        }
    }
    pthread_mutex_unlock(&titles.lock);
    return status;
}

bool live_title_async_matches(u64 token, ChatTool tool, i32 fg_pid, const char *cwd) {
    if (!token) return false;
    if (!cwd) cwd = "";
    pthread_mutex_lock(&titles.lock);
    TitleSlot *slot = titles.slots ? &titles.slots[token % LIVE_TITLE_ASYNC_CAP] : NULL;
    bool matches = slot && slot->token == token && slot->value.tool == tool &&
                   (!live_title_tool_keys_pid(tool) || slot->value.fg_pid == fg_pid) &&
                   !strcmp(slot->value.cwd, cwd);
    pthread_mutex_unlock(&titles.lock);
    return matches;
}

void live_title_async_release(u64 *request) {
    if (!request || !*request) return;
    u64 token = *request;
    *request = 0;
    pthread_mutex_lock(&titles.lock);
    if (titles.slots) {
        TitleSlot *slot = &titles.slots[token % LIVE_TITLE_ASYNC_CAP];
        if (slot->token == token && --slot->users == 0) {
            /* Reclaim queued and completed jobs immediately. A running probe
             * owns a value copy; its generation check discards the answer even
             * if this slot is reused before that probe returns. */
            memset(slot, 0, sizeof *slot);
        }
    }
    pthread_mutex_unlock(&titles.lock);
}

void live_title_async_shutdown(void) {
    pthread_mutex_lock(&titles.lock);
    titles.stop = true;
    titles.wake = NULL;
    bool join = titles.started;
    pthread_t worker = titles.worker;
    pthread_cond_signal(&titles.changed);
    pthread_mutex_unlock(&titles.lock);
    if (join) pthread_join(worker, NULL);
    pthread_mutex_lock(&titles.lock);
    titles.started = false;
    titles.slots = NULL;
    if (titles.arena.base)
        mem_budget_transient_adjust(MEM_HISTORY, -(isize)titles.arena.size);
    arena_destroy(&titles.arena);
    pthread_mutex_unlock(&titles.lock);
}
