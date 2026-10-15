/*
 * Threads: a mail, the mails it answers, and all replies to them.
 *
 * Each mail names the mail it answers by reference (SPEC.md §3.2),
 * so a thread is a tree. The client builds it from what it can
 * list: the inbox, the sent box and the saved mail. It walks up the
 * references to the first mail, then adds every mail that answers a
 * mail already in the tree. A reference to a mail it cannot find
 * becomes a missing entry, so the tree keeps its shape.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lib_internal.h"

/* a mail the thread can use */
struct node {
	char *mail_id;
	char *sender;
	char *recipient;
	char *subject;
	i64 timestamp;
	int size;
	int where;
	char *ref;
	char *reply_ref;
	int reply_known; /* reply_ref is known (set or truly none) */
	int parent; /* tree: node index, -1 for the root */
	int in_tree;
	int emitted;
};

struct pool {
	struct node *n;
	int count;
	int cap;
};

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

static void free_node(struct node *n)
{
	free(n->mail_id);
	free(n->sender);
	free(n->recipient);
	free(n->subject);
	free(n->ref);
	free(n->reply_ref);
}

static void free_pool(struct pool *p)
{
	for (int i = 0; i < p->count; i++)
		free_node(&p->n[i]);
	free(p->n);
}

static struct node *add_node(struct pool *p)
{
	if (p->count == p->cap) {
		int cap = p->cap ? p->cap * 2 : 32;
		struct node *n = realloc(p->n, (usize)cap * sizeof(*n));
		if (!n)
			return NULL;
		p->n = n;
		p->cap = cap;
	}
	struct node *n = &p->n[p->count++];
	memset(n, 0, sizeof(*n));
	n->parent = -1;
	return n;
}

static int find_id(const struct pool *p, const char *id)
{
	for (int i = 0; i < p->count; i++)
		if (p->n[i].mail_id && strcmp(p->n[i].mail_id, id) == 0)
			return i;
	return -1;
}

static int find_ref(const struct pool *p, const char *ref)
{
	for (int i = 0; i < p->count; i++)
		if (p->n[i].ref && strcmp(p->n[i].ref, ref) == 0)
			return i;
	return -1;
}

/* the node for a mail ID, new or merged */
static struct node *node_for(struct pool *p, const char *id, int *fresh)
{
	int i = find_id(p, id);
	*fresh = i < 0;
	if (i >= 0)
		return &p->n[i];
	struct node *n = add_node(p);
	if (n)
		n->mail_id = strdup(id);
	return n;
}

static int add_box(shyake_ctx *ctx, struct pool *p, const char *type,
		   int reply_known)
{
	shyake_mail_list *list = shyake_check(ctx, type);
	if (!list)
		return -1;
	int sent = strcmp(type, "sent") == 0;
	for (int i = 0; i < list->count; i++) {
		shyake_mail_entry *e = &list->entries[i];
		int fresh;
		struct node *n = node_for(p, e->mail_id, &fresh);
		if (!n)
			break;
		if (fresh) {
			n->sender = strdup(sent ? ctx->username : e->party);
			n->recipient = strdup(sent ? e->party : ctx->username);
			n->subject = dup_or_null(e->subject);
			n->timestamp = e->timestamp;
			n->size = e->size;
			n->ref = dup_or_null(e->ref);
			n->reply_ref = dup_or_null(e->reply_ref);
			n->reply_known = reply_known;
		}
		n->where |= sent ? SHYAKE_IN_SENT : SHYAKE_IN_INBOX;
	}
	shyake_free_mail_list(list);
	return 0;
}

static void add_saved(shyake_ctx *ctx, struct pool *p)
{
	shyake_saved_list *list = shyake_list_saved(ctx);
	if (!list)
		return;
	for (int i = 0; i < list->count; i++) {
		shyake_saved_entry *e = &list->entries[i];
		int fresh;
		struct node *n = node_for(p, e->mail_id, &fresh);
		if (!n)
			break;
		if (fresh) {
			n->sender = dup_or_null(e->sender);
			n->recipient = dup_or_null(e->recipient);
			n->subject = dup_or_null(e->subject);
			n->timestamp = e->timestamp;
			n->size = e->size;
			n->ref = dup_or_null(e->ref);
		}
		/* a saved copy always knows what it answers */
		if (!n->reply_known) {
			free(n->reply_ref);
			n->reply_ref = dup_or_null(e->reply_ref);
			n->reply_known = 1;
		}
		n->where |= SHYAKE_IN_SAVED;
	}
	shyake_free_saved_list(list);
}

/* below protocol 4: ask the instance what one mail answers */
static void learn_reply(shyake_ctx *ctx, struct node *n)
{
	shyake_mail_detail *d = shyake_check_one(ctx, n->mail_id);
	if (d) {
		n->reply_ref = d->reply_ref;
		d->reply_ref = NULL;
		shyake_free_mail_detail(d);
	}
	n->reply_known = 1;
}

/* walk up from the focus; returns the root node index */
static int find_root(shyake_ctx *ctx, struct pool *p, int focus)
{
	int cur = focus;
	for (int steps = 0; steps <= p->count; steps++) {
		struct node *n = &p->n[cur];
		if (!n->reply_known)
			learn_reply(ctx, n);
		if (!n->reply_ref)
			return cur;
		int up = find_ref(p, n->reply_ref);
		if (up < 0) {
			/* the answered mail is gone: a missing entry */
			char *ref = strdup(n->reply_ref);
			struct node *m = add_node(p);
			if (!m) {
				free(ref);
				return cur;
			}
			m->ref = ref;
			m->reply_known = 1;
			return p->count - 1;
		}
		if (up == focus)
			return cur; /* a loop: cannot happen with real hashes */
		cur = up;
	}
	return cur;
}

/* add every reply to a mail in the tree */
static void grow_tree(struct pool *p, int root)
{
	p->n[root].in_tree = 1;
	int added = 1;
	while (added) {
		added = 0;
		for (int i = 0; i < p->count; i++) {
			struct node *n = &p->n[i];
			if (n->in_tree || !n->reply_ref)
				continue;
			int up = find_ref(p, n->reply_ref);
			if (up >= 0 && p->n[up].in_tree) {
				n->in_tree = 1;
				n->parent = up;
				added = 1;
			}
		}
	}
}

/* sort key of a missing mail: just before its first reply */
static i64 sort_time(const struct pool *p, int i)
{
	if (p->n[i].mail_id)
		return p->n[i].timestamp;
	i64 t = INT64_MAX;
	for (int j = 0; j < p->count; j++)
		if (p->n[j].in_tree && p->n[j].parent == i &&
		    p->n[j].timestamp < t)
			t = p->n[j].timestamp;
	return t;
}

/* before: older first, then by ID; a missing mail goes first */
static int before(const struct pool *p, int a, int b)
{
	i64 ta = sort_time(p, a), tb = sort_time(p, b);
	if (ta != tb)
		return ta < tb;
	if (!p->n[a].mail_id || !p->n[b].mail_id)
		return !p->n[a].mail_id;
	return strcmp(p->n[a].mail_id, p->n[b].mail_id) < 0;
}

static shyake_thread *emit(struct pool *p, int focus)
{
	int total = 0;
	for (int i = 0; i < p->count; i++)
		total += p->n[i].in_tree;
	shyake_thread *t = calloc(1, sizeof(*t));
	int *slot = malloc((usize)p->count * sizeof(int));
	if (!t || !slot)
		goto fail;
	t->entries = calloc((usize)total, sizeof(shyake_thread_entry));
	if (!t->entries)
		goto fail;

	/* a mail only after the mail it answers, else oldest first */
	for (int k = 0; k < total; k++) {
		int best = -1;
		for (int i = 0; i < p->count; i++) {
			struct node *n = &p->n[i];
			if (!n->in_tree || n->emitted ||
			    (n->parent >= 0 && !p->n[n->parent].emitted))
				continue;
			if (best < 0 || before(p, i, best))
				best = i;
		}
		struct node *n = &p->n[best];
		n->emitted = 1;
		slot[best] = k;
		shyake_thread_entry *e = &t->entries[k];
		e->mail_id = n->mail_id;
		e->sender = n->sender;
		e->recipient = n->recipient;
		e->subject = n->subject;
		e->ref = n->ref;
		e->reply_ref = n->reply_ref;
		n->mail_id = n->sender = n->recipient = n->subject = NULL;
		n->ref = n->reply_ref = NULL;
		e->timestamp = e->mail_id ? n->timestamp : 0;
		e->size = n->size;
		e->where = n->where;
		e->parent = n->parent >= 0 ? slot[n->parent] : -1;
		if (best == focus)
			t->focus = k;
	}
	t->count = total;
	free(slot);
	return t;
fail:
	free(slot);
	if (t)
		free(t->entries);
	free(t);
	return NULL;
}

void shyake_free_thread(shyake_thread *t)
{
	if (!t)
		return;
	for (int i = 0; i < t->count; i++) {
		shyake_thread_entry *e = &t->entries[i];
		free(e->mail_id);
		free(e->sender);
		free(e->recipient);
		free(e->subject);
		free(e->ref);
		free(e->reply_ref);
	}
	free(t->entries);
	free(t);
}

shyake_thread *shyake_thread_of(shyake_ctx *ctx, const char *mail_id,
				int saved_only)
{
	if (!ctx || !mail_id)
		return NULL;
	clear_error(ctx);

	struct pool p = { 0 };
	int complete = 1;
	if (!saved_only) {
		if (!ctx->instance_url || !ctx->instance_url[0]) {
			set_error(ctx, "No instance is configured.");
			return NULL;
		}
		int level = instance_protocol(ctx);
		complete = level >= 4;
		if (add_box(ctx, &p, "inbox", complete) != 0 ||
		    add_box(ctx, &p, "sent", complete) != 0) {
			free_pool(&p);
			return NULL;
		}
	}
	add_saved(ctx, &p);

	int focus = find_id(&p, mail_id);
	if (focus < 0) {
		set_error(ctx,
			  saved_only ? "Saved mail %s not found." :
				       "Mail %s not found.",
			  mail_id);
		free_pool(&p);
		return NULL;
	}
	int root = find_root(ctx, &p, focus);
	grow_tree(&p, root);
	shyake_thread *t = emit(&p, focus);
	free_pool(&p);
	if (!t) {
		set_error(ctx, "Out of memory.");
		return NULL;
	}
	t->complete = complete;
	clear_error(ctx);
	return t;
}
