/* Double linked list macros derived from include/linux/list.h. */


/* List head. */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name)  { &(name), &(name) }
#define LIST_HEAD(name)  struct list_head name = LIST_HEAD_INIT(name)

static inline void init_list_head(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}


/* Insert a new entry between two known consecutive entries. */
static inline void __list_add(
    struct list_head *new, struct list_head *prev, struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

/* Insert a new entry after the specified head. */
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}


/* Insert a new entry before the specified head. */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

/* Delete a list entry by making the prev/next entries
 * point to each other. */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

/* Deletes entry from list. */
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
}


/* Checks if list is empty. */
static inline bool list_is_empty(const struct list_head *list)
{
    return list->next == list;
}


/* Iterates over list head, setting pos to each entry in the list. */
#define list_for_each(pos, head) \
    for (struct list_head *pos = (head)->next; pos != (head); pos = pos->next)


/* list_for_each_entry - iterate over list of given type
 *  type:       type of the objects on the list
 *  member:     name of the list_struct within type.
 *  pos:        name of the list cursor.
 *  head:       head of the list.
 * Essentially the same as list_for_each(), except the cursor pos is pointed to
 * the containing structure at each iteration. */
#define list_for_each_entry(type, member, pos, head) \
    for (type *pos = container_of((head)->next, type, member); \
        &pos->member != (head); \
        pos = container_of(pos->member.next, type, member))
