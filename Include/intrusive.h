/*
 * Python/intrusive.h
 *
 * Definitions for intrusive list.
 */


#ifndef Py_INTRUSIVE_H
#define Py_INTRUSIVE_H
#ifdef __cplusplus
extern "C" {
#endif


#define CONTAINER(type, member, ptr) \
   ((type *)((char *)(ptr) - offsetof(type, member)))

struct _list {
    struct _list *next;
    struct _list *prev;
};

/* Py_LOCAL_INLINE() does not work - see issue bugs.python.org/issue5553 */

#ifdef MS_WINDOWS
#define _LOCAL(type) static type
#else
#define _LOCAL(type) static inline type
#endif

_LOCAL(void) _list_init(struct _list *l) {
    l->next = l;
    l->prev = l;
}

_LOCAL(int) _list_empty(struct _list *l) {
    return l->next == l;
}

_LOCAL(void) _list_append(struct _list *l, struct _list *item) {
    struct _list *tail = l->prev;
    tail->next = item;
    item->prev = tail;
    item->next = l;
    l->prev = item;
}

_LOCAL(void) _list_pop(struct _list *item) {
    item->next->prev = item->prev;
    item->prev->next = item->next;
    item->prev = NULL;
    item->next = NULL;
}



#ifdef __cplusplus
}
#endif
#endif /* !Py_INTRUSIVE_H */
