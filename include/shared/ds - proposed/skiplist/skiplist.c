/*
 * Copyright (c) 2011-16 Scott Vokes <vokes.s@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "skiplist_config.h"
#include "skiplist.h"
#include "skiplist_macros_internal.h"

struct skiplist {
    size_t count;
    struct skiplist_node *head;
    skiplist_cmp_cb *cmp;
    skiplist_alloc_cb *alloc;
    void *alloc_udata;
};

struct skiplist_node {
    int h;                  /* node height */
    void *k;                /* key */
    void *v;                /* value */

    /* Forward pointers.
     * allocated with (h)*sizeof(N*) extra bytes. */
    struct skiplist_node *next[];
};

/* Sentinel. */
static struct skiplist_node SENTINEL = { 0, NULL, NULL };
#define IS_SENTINEL(n) (n == &SENTINEL)

static struct skiplist_node *
node_alloc(struct skiplist *sl, uint8_t height, void *key, void *value);
static void *def_alloc(void *p,
    size_t osize, size_t nsize, void *udata);

/* Create a new skiplist, returns NULL on error.
 * A comparison callback is required.
 * A memory management callback is optional - if NULL,
 * malloc & free will be used internally. */
struct skiplist *skiplist_new(skiplist_cmp_cb *cmp,
        skiplist_alloc_cb *alloc, void *alloc_udata) {
    if (cmp == NULL) { return NULL; }
    if (alloc == NULL) { alloc = def_alloc; }

    struct skiplist *sl = alloc(NULL, 0, sizeof(*sl), alloc_udata);
    if (sl) {
        sl->count = 0;
        sl->cmp = cmp;
        sl->alloc = alloc;
        sl->alloc_udata = alloc_udata;

        struct skiplist_node *head = node_alloc(sl, 1, &SENTINEL, &SENTINEL);
        if (head == NULL) {
            alloc(sl, sizeof(*sl), 0, alloc_udata);
            return NULL;
        }
        sl->head = head;
    }
    return sl;
}

/* Allocate a node. The forward pointers are initialized to &SENTINEL.
 * Returns NULL on failure. */
static struct skiplist_node *node_alloc(struct skiplist *sl,
        uint8_t height, void *key, void *value) {
    assert(height > 0);
    assert(height <= SKIPLIST_MAX_HEIGHT);
    size_t size = sizeof(struct skiplist_node) +
      height * sizeof(struct skiplist_node *);
    struct skiplist_node *n = sl->alloc(NULL, 0, size, sl->alloc_udata);
    if (n == NULL) { return NULL; }
    n->h = height;
    n->k = key;
    n->v = value;
    LOG2("allocated %d-level node at %p\n", height, (void *)n);
    DO(height, n->next[i] = &SENTINEL);
    return n;
}

static void *def_alloc(void *p,
        size_t osize, size_t nsize, void *udata) {
    (void)udata;
    (void)osize;
    if (p) {
        assert(nsize == 0);
        free(p);
        return NULL;
    } else {
        assert(osize == 0);
        return malloc(nsize);
    }
}


/* Free a node. If necessary, everything it references should be
 * freed by the calling function. */
static void node_free(struct skiplist *sl, struct skiplist_node *n) {
    sl->alloc(n, sizeof(*n) + n->h * sizeof(n), 0, sl->alloc_udata);
}

/* Set the random seed used when randomly constructing skiplists. */
void skiplist_set_seed(unsigned seed) {
    srandom(seed);
}

#ifndef SKIPLIST_GEN_HEIGHT
uint8_t SKIPLIST_GEN_HEIGHT(void);
uint8_t SKIPLIST_GEN_HEIGHT(void) {
    uint8_t h = 1;
    long r = random();
    /* According to the random(3) manpages on OpenBSD and OS X,
     * "[a]ll of the bits generated by random() are usable", so
     * it should be adequate to only call random() once if the
     * default probability of 50% for each additional level
     * increase is used. */
    for (uint8_t bit=0; r & (1 << bit); bit++) {
        h++;
    }
    return (uint8_t)(h > SKIPLIST_MAX_HEIGHT ? SKIPLIST_MAX_HEIGHT : h);
}
#endif

/* Get pointers to the HEIGHT nodes that precede the position
 * for key. Used by add/set/delete/delete_all. */
static void init_prevs(struct skiplist *sl, void *key,
        struct skiplist_node *head, int height,
        struct skiplist_node **prevs) {
    assert(sl);
    assert(head);
    struct skiplist_node *cur = NULL, *next = NULL;
    int lvl = height - 1, res = 0;

    cur = head;
    LOG2("sentinel is %p\n", (void *)&SENTINEL);
    LOG2("head is %p\n", (void *)head);

    do {
        assert(lvl < cur->h);
        assert(cur->h <= SKIPLIST_MAX_HEIGHT);
        next = cur->next[lvl];
        LOG2("next is %p, level is %d\n", (void *)next, lvl);
        res = IS_SENTINEL(next) ? 1 : sl->cmp(next->k, key);
        LOG2("res is %d\n", res);
        if (res < 0) {              /* < - advance. */
            cur = next;
        } else /*if (res >= 0)*/ {  /* >= - overshot, descend. */
            prevs[lvl] = cur;
            lvl--;
        }
    } while (lvl >= 0);
}

static bool grow_head(struct skiplist *sl, struct skiplist_node *nn) {
    struct skiplist_node *old_head = sl->head;
    LOG2("growing head from %d to %d\n", old_head->h, nn->h);
    struct skiplist_node *new_head = node_alloc(sl, nn->h,
        &SENTINEL, &SENTINEL);
    if (new_head == NULL) {
        fprintf(stderr, "alloc fail\n");
        return false;
    }
    DO(old_head->h, new_head->next[i] = old_head->next[i]);
    for (int i = old_head->h; i < new_head->h; i++) {
        /* The actual next[i] will be set later. */
        new_head->next[i] = nn;
    }
    sl->head = new_head;
    node_free(sl, old_head);
    return true;
}

static bool add_or_set(struct skiplist *sl, int try_replace,
        void *key, void *value, void **old) {
    assert(sl);
    struct skiplist_node *head = sl->head;
    assert(head);
    int cur_height = head->h;
    struct skiplist_node *prevs[cur_height];

    init_prevs(sl, key, head, cur_height, prevs);

    if (try_replace) {
        struct skiplist_node *next = prevs[0]->next[0];
        if (!IS_SENTINEL(next)) {
            int res = sl->cmp(next->k, key);
            if (res == 0) { /* key exists, replace value */
                if (old) { *old = next->v; }
                next->v = value;
                return true;
            } else {        /* not found */
                if (old) { *old = NULL; }
            }
        }
    }

    uint8_t new_height = SKIPLIST_GEN_HEIGHT();
    struct skiplist_node *nn = node_alloc(sl, new_height, key, value);
    if (nn == NULL) { return false; }

    if (new_height > cur_height) {
        if (!grow_head(sl, nn)) { return false; }
        DO(cur_height, if (prevs[i] == /* old */ head)
                           prevs[i] = sl->head);
        head = sl->head;
    }

    /* Insert n between prev[lvl] and prevs->next[lvl] */
    int minH = nn->h < cur_height ? nn->h : cur_height;
    for (int i = 0; i < minH; i++) {
        assert(i < prevs[i]->h);
        nn->next[i] = prevs[i]->next[i];
        assert(prevs[i]->h <= SKIPLIST_MAX_HEIGHT);
        prevs[i]->next[i] = nn;
    }
    sl->count++;
    return true;
}

bool skiplist_add(struct skiplist *sl, void *key, void *value) {
    return add_or_set(sl, 0, key, value, NULL);
}

bool skiplist_set(struct skiplist *sl, void *key, void *value, void **old) {
    return add_or_set(sl, 1, key, value, old);
}

static bool delete_one_or_all(struct skiplist *sl, void *key,
        skiplist_free_cb *cb, void *udata, void **old) {
    assert(sl);
    struct skiplist_node *head = sl->head;
    int cur_height = head->h;
    struct skiplist_node *prevs[cur_height];
    init_prevs(sl, key, head, cur_height, prevs);

    struct skiplist_node *doomed = prevs[0]->next[0];
    if (IS_SENTINEL(doomed) || 0 != sl->cmp(doomed->k, key)) {
        return false;           /* not found */
    }

    if (cb == NULL) {           /* delete one w/ key */
        DO(doomed->h, prevs[i]->next[i]=doomed->next[i]);
        if (old) { *old = doomed->v; }
        node_free(sl, doomed);
        sl->count--;
        return true;
    } else {                    /* delete all w/ key */
        int res = 0;
        int tdh = 0;            /* tallest doomed height */
        struct skiplist_node *nexts[cur_height];

        DO(cur_height, nexts[i] = &SENTINEL);

        LOG2("head is %p, sentinel is %p\n", (void *)head, (void *)&SENTINEL);
        if (SKIPLIST_LOG_LEVEL > 0)
            DO(cur_height, LOG2("prevs[%i]: %p\n", i, (void *)prevs[i]));

        /* Take the prevs, make another array of the first
         * point beyond the deleted cells at each level, and
         * link from prev to post. */
        do {
            LOG2("doomed is %p\n", (void *)doomed);
            struct skiplist_node *next = doomed->next[0];
            assert(next);
            LOG2("cur tdh: %d, next->h: %d, new tdh: %d\n",
                tdh, doomed->h, tdh > doomed->h ? tdh : doomed->h);
            tdh = tdh > doomed->h ? tdh : doomed->h;

            /* Maintain proper forward references.
             * This does some redundant work, and could instead
             * update to doomed's nexts' that have a greater
             * key. The added CMPs could be slower, though.*/
            DO(doomed->h,
                LOG2("nexts[%d] = doomed->next[%d] (%p)\n",
                    i, i, (void *)doomed->next[i]);
                nexts[i] = doomed->next[i]);
            if (SKIPLIST_LOG_LEVEL > 1)
                DO(tdh, fprintf(stderr, "nexts[%d] = %p\n", i, (void *)nexts[i]));

            cb(key, doomed->v, udata);
            sl->count--;
            node_free(sl, doomed);
            res = IS_SENTINEL(next)
              ? -1 : sl->cmp(next->k, key);
            doomed = next;
        } while (res == 0);

        LOG2("tdh is %d\n", tdh);
        DO(tdh,
            LOG2("setting prevs[%d]->next[%d] to %p\n", i, i, (void *)nexts[i]);
            prevs[i]->next[i] = nexts[i]);
        return false;
    }
}

bool skiplist_delete(struct skiplist *sl, void *key, void **value) {
    return delete_one_or_all(sl, key, NULL, NULL, value);
}

void skiplist_delete_all(struct skiplist *sl, void *key,
        skiplist_free_cb *cb, void *udata) {
    assert(cb);
    (void) delete_one_or_all(sl, key, cb, udata, NULL);
}

static struct skiplist_node *get_first_eq_node(struct skiplist *sl, void *key) {
    assert(sl);
    struct skiplist_node *head = sl->head;
    int height = head->h;
    int lvl = height - 1;
    struct skiplist_node *cur = head, *next = NULL;

    do {
        assert(cur->h > lvl);
        next = cur->next[lvl];

        assert(next->h <= SKIPLIST_MAX_HEIGHT);
        int res = IS_SENTINEL(next) ? 1 : sl->cmp(next->k, key);
        if (res < 0) {  /* next->key < key, advance */
            cur = next;
        } else if (res >= 0) { /* next->key >= key, descend */
            /* Descend when == to make sure it's the FIRST match. */
            if (lvl == 0) {
                if (res == 0) { return next; } /* found */
                return NULL;               /* not found */
            }
            lvl--;
        }
    } while (lvl >= 0);

    return NULL;                 /* not found */
}

bool skiplist_get(struct skiplist *sl, void *key, void **value) {
    struct skiplist_node *n = get_first_eq_node(sl, key);
    if (n) {
        if (value) { *value = n->v; }
        return true;
    } else {
        return false;
    }
}

bool skiplist_member(struct skiplist *sl, void *key) {
    return skiplist_get(sl, key, NULL);
}

bool skiplist_first(struct skiplist *sl, void **key, void **value) {
    assert(sl);
    struct skiplist_node *first = sl->head->next[0];
    if (IS_SENTINEL(first)) { return false; }
    if (key) { *key = first->k; }
    if (value) { *value = first->v; }
    return true;
}

bool skiplist_last(struct skiplist *sl, void **key, void **value) {
    assert(sl);
    struct skiplist_node *head = sl->head;
    int lvl = head->h - 1;
    struct skiplist_node *cur = head->next[lvl];
    if (IS_SENTINEL(cur)) { return false; }
    do {
        struct skiplist_node *next = cur->next[lvl];
        if (IS_SENTINEL(next)) {
            lvl--;
        } else {
            cur = next;
        }
    } while (lvl >= 0);

    assert(!IS_SENTINEL(cur));
    assert(IS_SENTINEL(cur->next[0]));
    if (key) { *key = cur->k; }
    if (value) { *value = cur->v; }
    return true;
}

bool skiplist_pop_first(struct skiplist *sl, void **key, void **value) {
    int height = 0;
    assert(sl);
    struct skiplist_node *head = sl->head;
    struct skiplist_node *first = head->next[0];
    assert(first);
    height = first->h;
    if (IS_SENTINEL(first)) { return false; }
    if (key) { *key = first->k; }
    if (value) { *value = first->v; }
    sl->count--;

    DO(height, head->next[i] = first->next[i]);
    node_free(sl, first);
    return true;
}

bool skiplist_pop_last(struct skiplist *sl, void **key, void **value) {
    assert(sl);
    struct skiplist_node *head = sl->head;
    struct skiplist_node *prevs[head->h];
    int lvl = head->h - 1;
    struct skiplist_node *cur = head;
    if (sl->count == 0) { return false; }

    /* Get all the nodes that are (node -> last -> &SENTINEL) so
     * node can skip directly to the sentinel. */
    do {
        if (IS_SENTINEL(cur->next[lvl])) {
            prevs[lvl--] = cur;
        } else {
            struct skiplist_node *next = cur->next[lvl]->next[lvl];
            if (IS_SENTINEL(next)) {
                prevs[lvl--] = cur;
            } else {
                cur = cur->next[lvl];
            }
        }
    } while (lvl >= 0);

    cur = cur->next[0];
    assert(!IS_SENTINEL(cur));
    assert(IS_SENTINEL(cur->next[0]));

    /* skip over the last non-SENTINEL nodes. */
    DO(cur->h, assert(prevs[i]->next[i] == cur));
    DO(cur->h, prevs[i]->next[i] = &SENTINEL);

    if (key) { *key = cur->k; }
    if (value) { *value = cur->v; }
    sl->count--;

    assert(!IS_SENTINEL(cur));
    node_free(sl, cur);
    return true;
}

size_t skiplist_count(struct skiplist *sl) {
    assert(sl);
    return sl->count;
}

bool skiplist_empty(struct skiplist *sl) {
    return (skiplist_count(sl) == 0);
}

static void walk_and_apply(struct skiplist_node *cur,
        skiplist_iter_cb *cb, void *udata) {
    while (!IS_SENTINEL(cur)) {
        enum skiplist_iter_res res;
        res = cb(cur->k, cur->v, udata);
        if (res != SKIPLIST_ITER_CONTINUE) { break; }
        cur = cur->next[0];
    }
}

void skiplist_iter(struct skiplist *sl, skiplist_iter_cb *cb, void *udata) {
    assert(sl);
    assert(cb);
    walk_and_apply(sl->head->next[0], cb, udata);
}

void skiplist_iter_from(struct skiplist *sl, void *key,
        skiplist_iter_cb *cb, void *udata) {
    assert(sl);
    assert(cb);
    struct skiplist_node *cur = get_first_eq_node(sl, key);
    LOG2("first node is %p\n", (void *)cur);
    if (cur == NULL) { return; }
    walk_and_apply(cur, cb, udata);
}

size_t skiplist_clear(struct skiplist *sl,
        skiplist_free_cb *cb, void *udata) {
    assert(sl);
    struct skiplist_node *cur = sl->head->next[0];
    size_t ct = 0;
    while (!IS_SENTINEL(cur)) {
        struct skiplist_node *doomed = cur;
        if (cb) { cb(doomed->k, doomed->v, udata); }
        cur = doomed->next[0];
        node_free(sl, doomed);
        ct++;
    }
    DO(sl->head->h, sl->head->next[i] = &SENTINEL);
    return ct;
}

size_t skiplist_free(struct skiplist *sl,
        skiplist_free_cb *cb, void *udata) {
    assert(sl);
    size_t ct = skiplist_clear(sl, cb, udata);
    node_free(sl, sl->head);
    sl->alloc(sl, sizeof(*sl), 0, sl->alloc_udata);
    return ct;
}

#if SKIPLIST_DEBUG
void skiplist_debug(struct skiplist *sl, FILE *f,
        skiplist_fprintf_kv_cb *cb, void *udata) {
    assert(sl);
    int max_lvl = sl->head->h;
    int counts[max_lvl];
    DO(max_lvl, counts[i] = 0);
    if (f) { fprintf(f, "max level is %d\n", max_lvl); }

    struct skiplist_node *head = sl->head;
    assert(head);
    if (f) {
        fprintf(f, "head is %p\nsentinel is %p\n",
            (void *)head, (void *)&SENTINEL);
    }
    struct skiplist_node *n = NULL;

    int ct = 0, prev_ct = 0;
    for (int i = max_lvl - 1; i>=0; i--) {
        if (f) { fprintf(f, "-- L %d:", i); }
        for (n = head->next[i]; n != &SENTINEL; n = n->next[i]) {
            if (f) {
                fprintf(f, " -> %p(%d%s",
                    (void *)n, n->h, cb == NULL ? "" : ":");
                if (cb) { cb(f, n->k, n->v, udata); }
                fprintf(f, ")");
            }

            if (f && n->h > max_lvl) {
                fprintf(stderr, "\nERROR: node %p's ->h > head->h (%d, %d)\n",
                    (void *)n, n->h, max_lvl);
            }
            assert(n->h <= max_lvl);
            ct++;
        }
        if (prev_ct != 0) { assert(ct >= prev_ct); }
        prev_ct = ct;
        counts[i] = ct;
        ct = 0;
        if (f) { fprintf(f, " -> &SENTINEL(%p)\n", (void *)&SENTINEL); }
    }

    if (f) {
        DO(max_lvl,
            if (counts[i] > 0)
                fprintf(f, "-- Count @ %d: %d\n", i, counts[i]));
    }
}
#endif