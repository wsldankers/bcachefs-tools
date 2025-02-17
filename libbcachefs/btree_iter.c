// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "bkey_methods.h"
#include "bkey_buf.h"
#include "btree_cache.h"
#include "btree_iter.h"
#include "btree_key_cache.h"
#include "btree_locking.h"
#include "btree_update.h"
#include "debug.h"
#include "error.h"
#include "extents.h"
#include "journal.h"
#include "recovery.h"
#include "replicas.h"
#include "subvolume.h"

#include <linux/prefetch.h>
#include <trace/events/bcachefs.h>

static void btree_trans_verify_sorted(struct btree_trans *);
inline void bch2_btree_path_check_sort(struct btree_trans *, struct btree_path *, int);

static inline void btree_path_list_remove(struct btree_trans *, struct btree_path *);
static inline void btree_path_list_add(struct btree_trans *, struct btree_path *,
				       struct btree_path *);

static inline unsigned long btree_iter_ip_allocated(struct btree_iter *iter)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	return iter->ip_allocated;
#else
	return 0;
#endif
}

static struct btree_path *btree_path_alloc(struct btree_trans *, struct btree_path *);

/*
 * Unlocks before scheduling
 * Note: does not revalidate iterator
 */
static inline int bch2_trans_cond_resched(struct btree_trans *trans)
{
	if (need_resched() || race_fault()) {
		bch2_trans_unlock(trans);
		schedule();
		return bch2_trans_relock(trans) ? 0 : -EINTR;
	} else {
		return 0;
	}
}

static inline int __btree_path_cmp(const struct btree_path *l,
				   enum btree_id	r_btree_id,
				   bool			r_cached,
				   struct bpos		r_pos,
				   unsigned		r_level)
{
	/*
	 * Must match lock ordering as defined by __bch2_btree_node_lock:
	 */
	return   cmp_int(l->btree_id,	r_btree_id) ?:
		 cmp_int((int) l->cached,	(int) r_cached) ?:
		 bpos_cmp(l->pos,	r_pos) ?:
		-cmp_int(l->level,	r_level);
}

static inline int btree_path_cmp(const struct btree_path *l,
				 const struct btree_path *r)
{
	return __btree_path_cmp(l, r->btree_id, r->cached, r->pos, r->level);
}

static inline struct bpos bkey_successor(struct btree_iter *iter, struct bpos p)
{
	/* Are we iterating over keys in all snapshots? */
	if (iter->flags & BTREE_ITER_ALL_SNAPSHOTS) {
		p = bpos_successor(p);
	} else {
		p = bpos_nosnap_successor(p);
		p.snapshot = iter->snapshot;
	}

	return p;
}

static inline struct bpos bkey_predecessor(struct btree_iter *iter, struct bpos p)
{
	/* Are we iterating over keys in all snapshots? */
	if (iter->flags & BTREE_ITER_ALL_SNAPSHOTS) {
		p = bpos_predecessor(p);
	} else {
		p = bpos_nosnap_predecessor(p);
		p.snapshot = iter->snapshot;
	}

	return p;
}

static inline bool is_btree_node(struct btree_path *path, unsigned l)
{
	return l < BTREE_MAX_DEPTH &&
		(unsigned long) path->l[l].b >= 128;
}

static inline struct bpos btree_iter_search_key(struct btree_iter *iter)
{
	struct bpos pos = iter->pos;

	if ((iter->flags & BTREE_ITER_IS_EXTENTS) &&
	    bkey_cmp(pos, POS_MAX))
		pos = bkey_successor(iter, pos);
	return pos;
}

static inline bool btree_path_pos_before_node(struct btree_path *path,
					      struct btree *b)
{
	return bpos_cmp(path->pos, b->data->min_key) < 0;
}

static inline bool btree_path_pos_after_node(struct btree_path *path,
					     struct btree *b)
{
	return bpos_cmp(b->key.k.p, path->pos) < 0;
}

static inline bool btree_path_pos_in_node(struct btree_path *path,
					  struct btree *b)
{
	return path->btree_id == b->c.btree_id &&
		!btree_path_pos_before_node(path, b) &&
		!btree_path_pos_after_node(path, b);
}

/* Btree node locking: */

void bch2_btree_node_unlock_write(struct btree_trans *trans,
			struct btree_path *path, struct btree *b)
{
	bch2_btree_node_unlock_write_inlined(trans, path, b);
}

void __bch2_btree_node_lock_write(struct btree_trans *trans, struct btree *b)
{
	struct btree_path *linked;
	unsigned readers = 0;

	trans_for_each_path(trans, linked)
		if (linked->l[b->c.level].b == b &&
		    btree_node_read_locked(linked, b->c.level))
			readers++;

	/*
	 * Must drop our read locks before calling six_lock_write() -
	 * six_unlock() won't do wakeups until the reader count
	 * goes to 0, and it's safe because we have the node intent
	 * locked:
	 */
	if (!b->c.lock.readers)
		atomic64_sub(__SIX_VAL(read_lock, readers),
			     &b->c.lock.state.counter);
	else
		this_cpu_sub(*b->c.lock.readers, readers);

	six_lock_write(&b->c.lock, NULL, NULL);

	if (!b->c.lock.readers)
		atomic64_add(__SIX_VAL(read_lock, readers),
			     &b->c.lock.state.counter);
	else
		this_cpu_add(*b->c.lock.readers, readers);
}

bool __bch2_btree_node_relock(struct btree_trans *trans,
			      struct btree_path *path, unsigned level)
{
	struct btree *b = btree_path_node(path, level);
	int want = __btree_lock_want(path, level);

	if (!is_btree_node(path, level))
		goto fail;

	if (race_fault())
		goto fail;

	if (six_relock_type(&b->c.lock, want, path->l[level].lock_seq) ||
	    (btree_node_lock_seq_matches(path, b, level) &&
	     btree_node_lock_increment(trans, b, level, want))) {
		mark_btree_node_locked(trans, path, level, want);
		return true;
	}
fail:
	trace_btree_node_relock_fail(trans->fn, _RET_IP_,
				     path->btree_id,
				     &path->pos,
				     (unsigned long) b,
				     path->l[level].lock_seq,
				     is_btree_node(path, level) ? b->c.lock.state.seq : 0);
	return false;
}

bool bch2_btree_node_upgrade(struct btree_trans *trans,
			     struct btree_path *path, unsigned level)
{
	struct btree *b = path->l[level].b;

	if (!is_btree_node(path, level))
		return false;

	switch (btree_lock_want(path, level)) {
	case BTREE_NODE_UNLOCKED:
		BUG_ON(btree_node_locked(path, level));
		return true;
	case BTREE_NODE_READ_LOCKED:
		BUG_ON(btree_node_intent_locked(path, level));
		return bch2_btree_node_relock(trans, path, level);
	case BTREE_NODE_INTENT_LOCKED:
		break;
	}

	if (btree_node_intent_locked(path, level))
		return true;

	if (race_fault())
		return false;

	if (btree_node_locked(path, level)
	    ? six_lock_tryupgrade(&b->c.lock)
	    : six_relock_type(&b->c.lock, SIX_LOCK_intent, path->l[level].lock_seq))
		goto success;

	if (btree_node_lock_seq_matches(path, b, level) &&
	    btree_node_lock_increment(trans, b, level, BTREE_NODE_INTENT_LOCKED)) {
		btree_node_unlock(path, level);
		goto success;
	}

	return false;
success:
	mark_btree_node_intent_locked(trans, path, level);
	return true;
}

static inline bool btree_path_get_locks(struct btree_trans *trans,
					struct btree_path *path,
					bool upgrade)
{
	unsigned l = path->level;
	int fail_idx = -1;

	do {
		if (!btree_path_node(path, l))
			break;

		if (!(upgrade
		      ? bch2_btree_node_upgrade(trans, path, l)
		      : bch2_btree_node_relock(trans, path, l)))
			fail_idx = l;

		l++;
	} while (l < path->locks_want);

	/*
	 * When we fail to get a lock, we have to ensure that any child nodes
	 * can't be relocked so bch2_btree_path_traverse has to walk back up to
	 * the node that we failed to relock:
	 */
	if (fail_idx >= 0) {
		__bch2_btree_path_unlock(path);
		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);

		do {
			path->l[fail_idx].b = BTREE_ITER_NO_NODE_GET_LOCKS;
			--fail_idx;
		} while (fail_idx >= 0);
	}

	if (path->uptodate == BTREE_ITER_NEED_RELOCK)
		path->uptodate = BTREE_ITER_UPTODATE;

	bch2_trans_verify_locks(trans);

	return path->uptodate < BTREE_ITER_NEED_RELOCK;
}

static struct bpos btree_node_pos(struct btree_bkey_cached_common *_b,
				  bool cached)
{
	return !cached
		? container_of(_b, struct btree, c)->key.k.p
		: container_of(_b, struct bkey_cached, c)->key.pos;
}

/* Slowpath: */
bool __bch2_btree_node_lock(struct btree_trans *trans,
			    struct btree_path *path,
			    struct btree *b,
			    struct bpos pos, unsigned level,
			    enum six_lock_type type,
			    six_lock_should_sleep_fn should_sleep_fn, void *p,
			    unsigned long ip)
{
	struct btree_path *linked;
	unsigned reason;

	/* Check if it's safe to block: */
	trans_for_each_path(trans, linked) {
		if (!linked->nodes_locked)
			continue;

		/*
		 * Can't block taking an intent lock if we have _any_ nodes read
		 * locked:
		 *
		 * - Our read lock blocks another thread with an intent lock on
		 *   the same node from getting a write lock, and thus from
		 *   dropping its intent lock
		 *
		 * - And the other thread may have multiple nodes intent locked:
		 *   both the node we want to intent lock, and the node we
		 *   already have read locked - deadlock:
		 */
		if (type == SIX_LOCK_intent &&
		    linked->nodes_locked != linked->nodes_intent_locked) {
			reason = 1;
			goto deadlock;
		}

		if (linked->btree_id != path->btree_id) {
			if (linked->btree_id < path->btree_id)
				continue;

			reason = 3;
			goto deadlock;
		}

		/*
		 * Within the same btree, non-cached paths come before cached
		 * paths:
		 */
		if (linked->cached != path->cached) {
			if (!linked->cached)
				continue;

			reason = 4;
			goto deadlock;
		}

		/*
		 * Interior nodes must be locked before their descendants: if
		 * another path has possible descendants locked of the node
		 * we're about to lock, it must have the ancestors locked too:
		 */
		if (level > __fls(linked->nodes_locked)) {
			reason = 5;
			goto deadlock;
		}

		/* Must lock btree nodes in key order: */
		if (btree_node_locked(linked, level) &&
		    bpos_cmp(pos, btree_node_pos((void *) linked->l[level].b,
						 linked->cached)) <= 0) {
			BUG_ON(trans->in_traverse_all);
			reason = 7;
			goto deadlock;
		}
	}

	return btree_node_lock_type(trans, path, b, pos, level,
				    type, should_sleep_fn, p);
deadlock:
	trace_trans_restart_would_deadlock(trans->fn, ip,
			trans->in_traverse_all, reason,
			linked->btree_id,
			linked->cached,
			&linked->pos,
			path->btree_id,
			path->cached,
			&pos);
	btree_trans_restart(trans);
	return false;
}

/* Btree iterator locking: */

#ifdef CONFIG_BCACHEFS_DEBUG

static void bch2_btree_path_verify_locks(struct btree_path *path)
{
	unsigned l;

	if (!path->nodes_locked) {
		BUG_ON(path->uptodate == BTREE_ITER_UPTODATE &&
		       btree_path_node(path, path->level));
		return;
	}

	for (l = 0; btree_path_node(path, l); l++)
		BUG_ON(btree_lock_want(path, l) !=
		       btree_node_locked_type(path, l));
}

void bch2_trans_verify_locks(struct btree_trans *trans)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		bch2_btree_path_verify_locks(path);
}
#else
static inline void bch2_btree_path_verify_locks(struct btree_path *path) {}
#endif

/* Btree path locking: */

/*
 * Only for btree_cache.c - only relocks intent locks
 */
bool bch2_btree_path_relock_intent(struct btree_trans *trans,
				   struct btree_path *path)
{
	unsigned l;

	for (l = path->level;
	     l < path->locks_want && btree_path_node(path, l);
	     l++) {
		if (!bch2_btree_node_relock(trans, path, l)) {
			__bch2_btree_path_unlock(path);
			btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
			trace_trans_restart_relock_path_intent(trans->fn, _RET_IP_,
						   path->btree_id, &path->pos);
			btree_trans_restart(trans);
			return false;
		}
	}

	return true;
}

__flatten
static bool bch2_btree_path_relock(struct btree_trans *trans,
			struct btree_path *path, unsigned long trace_ip)
{
	bool ret = btree_path_get_locks(trans, path, false);

	if (!ret) {
		trace_trans_restart_relock_path(trans->fn, trace_ip,
						path->btree_id, &path->pos);
		btree_trans_restart(trans);
	}
	return ret;
}

bool __bch2_btree_path_upgrade(struct btree_trans *trans,
			       struct btree_path *path,
			       unsigned new_locks_want)
{
	struct btree_path *linked;

	EBUG_ON(path->locks_want >= new_locks_want);

	path->locks_want = new_locks_want;

	if (btree_path_get_locks(trans, path, true))
		return true;

	/*
	 * XXX: this is ugly - we'd prefer to not be mucking with other
	 * iterators in the btree_trans here.
	 *
	 * On failure to upgrade the iterator, setting iter->locks_want and
	 * calling get_locks() is sufficient to make bch2_btree_path_traverse()
	 * get the locks we want on transaction restart.
	 *
	 * But if this iterator was a clone, on transaction restart what we did
	 * to this iterator isn't going to be preserved.
	 *
	 * Possibly we could add an iterator field for the parent iterator when
	 * an iterator is a copy - for now, we'll just upgrade any other
	 * iterators with the same btree id.
	 *
	 * The code below used to be needed to ensure ancestor nodes get locked
	 * before interior nodes - now that's handled by
	 * bch2_btree_path_traverse_all().
	 */
	if (!path->cached && !trans->in_traverse_all)
		trans_for_each_path(trans, linked)
			if (linked != path &&
			    linked->cached == path->cached &&
			    linked->btree_id == path->btree_id &&
			    linked->locks_want < new_locks_want) {
				linked->locks_want = new_locks_want;
				btree_path_get_locks(trans, linked, true);
			}

	return false;
}

void __bch2_btree_path_downgrade(struct btree_path *path,
				 unsigned new_locks_want)
{
	unsigned l;

	EBUG_ON(path->locks_want < new_locks_want);

	path->locks_want = new_locks_want;

	while (path->nodes_locked &&
	       (l = __fls(path->nodes_locked)) >= path->locks_want) {
		if (l > path->level) {
			btree_node_unlock(path, l);
		} else {
			if (btree_node_intent_locked(path, l)) {
				six_lock_downgrade(&path->l[l].b->c.lock);
				path->nodes_intent_locked ^= 1 << l;
			}
			break;
		}
	}

	bch2_btree_path_verify_locks(path);
}

void bch2_trans_downgrade(struct btree_trans *trans)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		bch2_btree_path_downgrade(path);
}

/* Btree transaction locking: */

bool bch2_trans_relock(struct btree_trans *trans)
{
	struct btree_path *path;

	if (unlikely(trans->restarted))
		return false;

	trans_for_each_path(trans, path)
		if (path->should_be_locked &&
		    !bch2_btree_path_relock(trans, path, _RET_IP_)) {
			trace_trans_restart_relock(trans->fn, _RET_IP_,
					path->btree_id, &path->pos);
			BUG_ON(!trans->restarted);
			return false;
		}
	return true;
}

void bch2_trans_unlock(struct btree_trans *trans)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		__bch2_btree_path_unlock(path);

	/*
	 * bch2_gc_btree_init_recurse() doesn't use btree iterators for walking
	 * btree nodes, it implements its own walking:
	 */
	BUG_ON(!trans->is_initial_gc &&
	       lock_class_is_held(&bch2_btree_node_lock_key));
}

/* Btree iterator: */

#ifdef CONFIG_BCACHEFS_DEBUG

static void bch2_btree_path_verify_cached(struct btree_trans *trans,
					  struct btree_path *path)
{
	struct bkey_cached *ck;
	bool locked = btree_node_locked(path, 0);

	if (!bch2_btree_node_relock(trans, path, 0))
		return;

	ck = (void *) path->l[0].b;
	BUG_ON(ck->key.btree_id != path->btree_id ||
	       bkey_cmp(ck->key.pos, path->pos));

	if (!locked)
		btree_node_unlock(path, 0);
}

static void bch2_btree_path_verify_level(struct btree_trans *trans,
				struct btree_path *path, unsigned level)
{
	struct btree_path_level *l;
	struct btree_node_iter tmp;
	bool locked;
	struct bkey_packed *p, *k;
	struct printbuf buf1 = PRINTBUF;
	struct printbuf buf2 = PRINTBUF;
	struct printbuf buf3 = PRINTBUF;
	const char *msg;

	if (!bch2_debug_check_iterators)
		return;

	l	= &path->l[level];
	tmp	= l->iter;
	locked	= btree_node_locked(path, level);

	if (path->cached) {
		if (!level)
			bch2_btree_path_verify_cached(trans, path);
		return;
	}

	if (!btree_path_node(path, level))
		return;

	if (!bch2_btree_node_relock(trans, path, level))
		return;

	BUG_ON(!btree_path_pos_in_node(path, l->b));

	bch2_btree_node_iter_verify(&l->iter, l->b);

	/*
	 * For interior nodes, the iterator will have skipped past deleted keys:
	 */
	p = level
		? bch2_btree_node_iter_prev(&tmp, l->b)
		: bch2_btree_node_iter_prev_all(&tmp, l->b);
	k = bch2_btree_node_iter_peek_all(&l->iter, l->b);

	if (p && bkey_iter_pos_cmp(l->b, p, &path->pos) >= 0) {
		msg = "before";
		goto err;
	}

	if (k && bkey_iter_pos_cmp(l->b, k, &path->pos) < 0) {
		msg = "after";
		goto err;
	}

	if (!locked)
		btree_node_unlock(path, level);
	return;
err:
	bch2_bpos_to_text(&buf1, path->pos);

	if (p) {
		struct bkey uk = bkey_unpack_key(l->b, p);
		bch2_bkey_to_text(&buf2, &uk);
	} else {
		pr_buf(&buf2, "(none)");
	}

	if (k) {
		struct bkey uk = bkey_unpack_key(l->b, k);
		bch2_bkey_to_text(&buf3, &uk);
	} else {
		pr_buf(&buf3, "(none)");
	}

	panic("path should be %s key at level %u:\n"
	      "path pos %s\n"
	      "prev key %s\n"
	      "cur  key %s\n",
	      msg, level, buf1.buf, buf2.buf, buf3.buf);
}

static void bch2_btree_path_verify(struct btree_trans *trans,
				   struct btree_path *path)
{
	struct bch_fs *c = trans->c;
	unsigned i;

	EBUG_ON(path->btree_id >= BTREE_ID_NR);

	for (i = 0; i < (!path->cached ? BTREE_MAX_DEPTH : 1); i++) {
		if (!path->l[i].b) {
			BUG_ON(!path->cached &&
			       c->btree_roots[path->btree_id].b->c.level > i);
			break;
		}

		bch2_btree_path_verify_level(trans, path, i);
	}

	bch2_btree_path_verify_locks(path);
}

void bch2_trans_verify_paths(struct btree_trans *trans)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		bch2_btree_path_verify(trans, path);
}

static void bch2_btree_iter_verify(struct btree_iter *iter)
{
	struct btree_trans *trans = iter->trans;

	BUG_ON(iter->btree_id >= BTREE_ID_NR);

	BUG_ON(!!(iter->flags & BTREE_ITER_CACHED) != iter->path->cached);

	BUG_ON((iter->flags & BTREE_ITER_IS_EXTENTS) &&
	       (iter->flags & BTREE_ITER_ALL_SNAPSHOTS));

	BUG_ON(!(iter->flags & __BTREE_ITER_ALL_SNAPSHOTS) &&
	       (iter->flags & BTREE_ITER_ALL_SNAPSHOTS) &&
	       !btree_type_has_snapshots(iter->btree_id));

	if (iter->update_path)
		bch2_btree_path_verify(trans, iter->update_path);
	bch2_btree_path_verify(trans, iter->path);
}

static void bch2_btree_iter_verify_entry_exit(struct btree_iter *iter)
{
	BUG_ON((iter->flags & BTREE_ITER_FILTER_SNAPSHOTS) &&
	       !iter->pos.snapshot);

	BUG_ON(!(iter->flags & BTREE_ITER_ALL_SNAPSHOTS) &&
	       iter->pos.snapshot != iter->snapshot);

	BUG_ON(bkey_cmp(iter->pos, bkey_start_pos(&iter->k)) < 0 ||
	       bkey_cmp(iter->pos, iter->k.p) > 0);
}

static int bch2_btree_iter_verify_ret(struct btree_iter *iter, struct bkey_s_c k)
{
	struct btree_trans *trans = iter->trans;
	struct btree_iter copy;
	struct bkey_s_c prev;
	int ret = 0;

	if (!bch2_debug_check_iterators)
		return 0;

	if (!(iter->flags & BTREE_ITER_FILTER_SNAPSHOTS))
		return 0;

	if (bkey_err(k) || !k.k)
		return 0;

	BUG_ON(!bch2_snapshot_is_ancestor(trans->c,
					  iter->snapshot,
					  k.k->p.snapshot));

	bch2_trans_iter_init(trans, &copy, iter->btree_id, iter->pos,
			     BTREE_ITER_NOPRESERVE|
			     BTREE_ITER_ALL_SNAPSHOTS);
	prev = bch2_btree_iter_prev(&copy);
	if (!prev.k)
		goto out;

	ret = bkey_err(prev);
	if (ret)
		goto out;

	if (!bkey_cmp(prev.k->p, k.k->p) &&
	    bch2_snapshot_is_ancestor(trans->c, iter->snapshot,
				      prev.k->p.snapshot) > 0) {
		struct printbuf buf1 = PRINTBUF, buf2 = PRINTBUF;

		bch2_bkey_to_text(&buf1, k.k);
		bch2_bkey_to_text(&buf2, prev.k);

		panic("iter snap %u\n"
		      "k    %s\n"
		      "prev %s\n",
		      iter->snapshot,
		      buf1.buf, buf2.buf);
	}
out:
	bch2_trans_iter_exit(trans, &copy);
	return ret;
}

void bch2_assert_pos_locked(struct btree_trans *trans, enum btree_id id,
			    struct bpos pos, bool key_cache)
{
	struct btree_path *path;
	unsigned idx;
	struct printbuf buf = PRINTBUF;

	trans_for_each_path_inorder(trans, path, idx) {
		int cmp = cmp_int(path->btree_id, id) ?:
			cmp_int(path->cached, key_cache);

		if (cmp > 0)
			break;
		if (cmp < 0)
			continue;

		if (!(path->nodes_locked & 1) ||
		    !path->should_be_locked)
			continue;

		if (!key_cache) {
			if (bkey_cmp(pos, path->l[0].b->data->min_key) >= 0 &&
			    bkey_cmp(pos, path->l[0].b->key.k.p) <= 0)
				return;
		} else {
			if (!bkey_cmp(pos, path->pos))
				return;
		}
	}

	bch2_dump_trans_paths_updates(trans);
	bch2_bpos_to_text(&buf, pos);

	panic("not locked: %s %s%s\n",
	      bch2_btree_ids[id], buf.buf,
	      key_cache ? " cached" : "");
}

#else

static inline void bch2_btree_path_verify_level(struct btree_trans *trans,
						struct btree_path *path, unsigned l) {}
static inline void bch2_btree_path_verify(struct btree_trans *trans,
					  struct btree_path *path) {}
static inline void bch2_btree_iter_verify(struct btree_iter *iter) {}
static inline void bch2_btree_iter_verify_entry_exit(struct btree_iter *iter) {}
static inline int bch2_btree_iter_verify_ret(struct btree_iter *iter, struct bkey_s_c k) { return 0; }

#endif

/* Btree path: fixups after btree updates */

static void btree_node_iter_set_set_pos(struct btree_node_iter *iter,
					struct btree *b,
					struct bset_tree *t,
					struct bkey_packed *k)
{
	struct btree_node_iter_set *set;

	btree_node_iter_for_each(iter, set)
		if (set->end == t->end_offset) {
			set->k = __btree_node_key_to_offset(b, k);
			bch2_btree_node_iter_sort(iter, b);
			return;
		}

	bch2_btree_node_iter_push(iter, b, k, btree_bkey_last(b, t));
}

static void __bch2_btree_path_fix_key_modified(struct btree_path *path,
					       struct btree *b,
					       struct bkey_packed *where)
{
	struct btree_path_level *l = &path->l[b->c.level];

	if (where != bch2_btree_node_iter_peek_all(&l->iter, l->b))
		return;

	if (bkey_iter_pos_cmp(l->b, where, &path->pos) < 0)
		bch2_btree_node_iter_advance(&l->iter, l->b);
}

void bch2_btree_path_fix_key_modified(struct btree_trans *trans,
				      struct btree *b,
				      struct bkey_packed *where)
{
	struct btree_path *path;

	trans_for_each_path_with_node(trans, b, path) {
		__bch2_btree_path_fix_key_modified(path, b, where);
		bch2_btree_path_verify_level(trans, path, b->c.level);
	}
}

static void __bch2_btree_node_iter_fix(struct btree_path *path,
				       struct btree *b,
				       struct btree_node_iter *node_iter,
				       struct bset_tree *t,
				       struct bkey_packed *where,
				       unsigned clobber_u64s,
				       unsigned new_u64s)
{
	const struct bkey_packed *end = btree_bkey_last(b, t);
	struct btree_node_iter_set *set;
	unsigned offset = __btree_node_key_to_offset(b, where);
	int shift = new_u64s - clobber_u64s;
	unsigned old_end = t->end_offset - shift;
	unsigned orig_iter_pos = node_iter->data[0].k;
	bool iter_current_key_modified =
		orig_iter_pos >= offset &&
		orig_iter_pos <= offset + clobber_u64s;

	btree_node_iter_for_each(node_iter, set)
		if (set->end == old_end)
			goto found;

	/* didn't find the bset in the iterator - might have to readd it: */
	if (new_u64s &&
	    bkey_iter_pos_cmp(b, where, &path->pos) >= 0) {
		bch2_btree_node_iter_push(node_iter, b, where, end);
		goto fixup_done;
	} else {
		/* Iterator is after key that changed */
		return;
	}
found:
	set->end = t->end_offset;

	/* Iterator hasn't gotten to the key that changed yet: */
	if (set->k < offset)
		return;

	if (new_u64s &&
	    bkey_iter_pos_cmp(b, where, &path->pos) >= 0) {
		set->k = offset;
	} else if (set->k < offset + clobber_u64s) {
		set->k = offset + new_u64s;
		if (set->k == set->end)
			bch2_btree_node_iter_set_drop(node_iter, set);
	} else {
		/* Iterator is after key that changed */
		set->k = (int) set->k + shift;
		return;
	}

	bch2_btree_node_iter_sort(node_iter, b);
fixup_done:
	if (node_iter->data[0].k != orig_iter_pos)
		iter_current_key_modified = true;

	/*
	 * When a new key is added, and the node iterator now points to that
	 * key, the iterator might have skipped past deleted keys that should
	 * come after the key the iterator now points to. We have to rewind to
	 * before those deleted keys - otherwise
	 * bch2_btree_node_iter_prev_all() breaks:
	 */
	if (!bch2_btree_node_iter_end(node_iter) &&
	    iter_current_key_modified &&
	    b->c.level) {
		struct bset_tree *t;
		struct bkey_packed *k, *k2, *p;

		k = bch2_btree_node_iter_peek_all(node_iter, b);

		for_each_bset(b, t) {
			bool set_pos = false;

			if (node_iter->data[0].end == t->end_offset)
				continue;

			k2 = bch2_btree_node_iter_bset_pos(node_iter, b, t);

			while ((p = bch2_bkey_prev_all(b, t, k2)) &&
			       bkey_iter_cmp(b, k, p) < 0) {
				k2 = p;
				set_pos = true;
			}

			if (set_pos)
				btree_node_iter_set_set_pos(node_iter,
							    b, t, k2);
		}
	}
}

void bch2_btree_node_iter_fix(struct btree_trans *trans,
			      struct btree_path *path,
			      struct btree *b,
			      struct btree_node_iter *node_iter,
			      struct bkey_packed *where,
			      unsigned clobber_u64s,
			      unsigned new_u64s)
{
	struct bset_tree *t = bch2_bkey_to_bset(b, where);
	struct btree_path *linked;

	if (node_iter != &path->l[b->c.level].iter) {
		__bch2_btree_node_iter_fix(path, b, node_iter, t,
					   where, clobber_u64s, new_u64s);

		if (bch2_debug_check_iterators)
			bch2_btree_node_iter_verify(node_iter, b);
	}

	trans_for_each_path_with_node(trans, b, linked) {
		__bch2_btree_node_iter_fix(linked, b,
					   &linked->l[b->c.level].iter, t,
					   where, clobber_u64s, new_u64s);
		bch2_btree_path_verify_level(trans, linked, b->c.level);
	}
}

/* Btree path level: pointer to a particular btree node and node iter */

static inline struct bkey_s_c __btree_iter_unpack(struct bch_fs *c,
						  struct btree_path_level *l,
						  struct bkey *u,
						  struct bkey_packed *k)
{
	if (unlikely(!k)) {
		/*
		 * signal to bch2_btree_iter_peek_slot() that we're currently at
		 * a hole
		 */
		u->type = KEY_TYPE_deleted;
		return bkey_s_c_null;
	}

	return bkey_disassemble(l->b, k, u);
}

static inline struct bkey_s_c btree_path_level_peek_all(struct bch_fs *c,
							struct btree_path_level *l,
							struct bkey *u)
{
	return __btree_iter_unpack(c, l, u,
			bch2_btree_node_iter_peek_all(&l->iter, l->b));
}

static inline struct bkey_s_c btree_path_level_peek(struct bch_fs *c,
						    struct btree_path *path,
						    struct btree_path_level *l,
						    struct bkey *u)
{
	struct bkey_s_c k = __btree_iter_unpack(c, l, u,
			bch2_btree_node_iter_peek(&l->iter, l->b));

	path->pos = k.k ? k.k->p : l->b->key.k.p;
	return k;
}

static inline struct bkey_s_c btree_path_level_prev(struct bch_fs *c,
						    struct btree_path *path,
						    struct btree_path_level *l,
						    struct bkey *u)
{
	struct bkey_s_c k = __btree_iter_unpack(c, l, u,
			bch2_btree_node_iter_prev(&l->iter, l->b));

	path->pos = k.k ? k.k->p : l->b->data->min_key;
	return k;
}

static inline bool btree_path_advance_to_pos(struct btree_path *path,
					     struct btree_path_level *l,
					     int max_advance)
{
	struct bkey_packed *k;
	int nr_advanced = 0;

	while ((k = bch2_btree_node_iter_peek_all(&l->iter, l->b)) &&
	       bkey_iter_pos_cmp(l->b, k, &path->pos) < 0) {
		if (max_advance > 0 && nr_advanced >= max_advance)
			return false;

		bch2_btree_node_iter_advance(&l->iter, l->b);
		nr_advanced++;
	}

	return true;
}

/*
 * Verify that iterator for parent node points to child node:
 */
static void btree_path_verify_new_node(struct btree_trans *trans,
				       struct btree_path *path, struct btree *b)
{
	struct bch_fs *c = trans->c;
	struct btree_path_level *l;
	unsigned plevel;
	bool parent_locked;
	struct bkey_packed *k;

	if (!IS_ENABLED(CONFIG_BCACHEFS_DEBUG))
		return;

	if (!test_bit(JOURNAL_REPLAY_DONE, &c->journal.flags))
		return;

	plevel = b->c.level + 1;
	if (!btree_path_node(path, plevel))
		return;

	parent_locked = btree_node_locked(path, plevel);

	if (!bch2_btree_node_relock(trans, path, plevel))
		return;

	l = &path->l[plevel];
	k = bch2_btree_node_iter_peek_all(&l->iter, l->b);
	if (!k ||
	    bkey_deleted(k) ||
	    bkey_cmp_left_packed(l->b, k, &b->key.k.p)) {
		struct printbuf buf1 = PRINTBUF;
		struct printbuf buf2 = PRINTBUF;
		struct printbuf buf3 = PRINTBUF;
		struct printbuf buf4 = PRINTBUF;
		struct bkey uk = bkey_unpack_key(b, k);

		bch2_dump_btree_node(c, l->b);
		bch2_bpos_to_text(&buf1, path->pos);
		bch2_bkey_to_text(&buf2, &uk);
		bch2_bpos_to_text(&buf3, b->data->min_key);
		bch2_bpos_to_text(&buf3, b->data->max_key);
		panic("parent iter doesn't point to new node:\n"
		      "iter pos %s %s\n"
		      "iter key %s\n"
		      "new node %s-%s\n",
		      bch2_btree_ids[path->btree_id],
		      buf1.buf, buf2.buf, buf3.buf, buf4.buf);
	}

	if (!parent_locked)
		btree_node_unlock(path, plevel);
}

static inline void __btree_path_level_init(struct btree_path *path,
					   unsigned level)
{
	struct btree_path_level *l = &path->l[level];

	bch2_btree_node_iter_init(&l->iter, l->b, &path->pos);

	/*
	 * Iterators to interior nodes should always be pointed at the first non
	 * whiteout:
	 */
	if (level)
		bch2_btree_node_iter_peek(&l->iter, l->b);
}

static inline void btree_path_level_init(struct btree_trans *trans,
					 struct btree_path *path,
					 struct btree *b)
{
	BUG_ON(path->cached);

	btree_path_verify_new_node(trans, path, b);

	EBUG_ON(!btree_path_pos_in_node(path, b));
	EBUG_ON(b->c.lock.state.seq & 1);

	path->l[b->c.level].lock_seq = b->c.lock.state.seq;
	path->l[b->c.level].b = b;
	__btree_path_level_init(path, b->c.level);
}

/* Btree path: fixups after btree node updates: */

/*
 * A btree node is being replaced - update the iterator to point to the new
 * node:
 */
void bch2_trans_node_add(struct btree_trans *trans, struct btree *b)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		if (!path->cached &&
		    btree_path_pos_in_node(path, b)) {
			enum btree_node_locked_type t =
				btree_lock_want(path, b->c.level);

			if (path->nodes_locked &&
			    t != BTREE_NODE_UNLOCKED) {
				btree_node_unlock(path, b->c.level);
				six_lock_increment(&b->c.lock, t);
				mark_btree_node_locked(trans, path, b->c.level, t);
			}

			btree_path_level_init(trans, path, b);
		}
}

/*
 * A btree node has been modified in such a way as to invalidate iterators - fix
 * them:
 */
void bch2_trans_node_reinit_iter(struct btree_trans *trans, struct btree *b)
{
	struct btree_path *path;

	trans_for_each_path_with_node(trans, b, path)
		__btree_path_level_init(path, b->c.level);
}

/* Btree path: traverse, set_pos: */

static int lock_root_check_fn(struct six_lock *lock, void *p)
{
	struct btree *b = container_of(lock, struct btree, c.lock);
	struct btree **rootp = p;

	return b == *rootp ? 0 : -1;
}

static inline int btree_path_lock_root(struct btree_trans *trans,
				       struct btree_path *path,
				       unsigned depth_want,
				       unsigned long trace_ip)
{
	struct bch_fs *c = trans->c;
	struct btree *b, **rootp = &c->btree_roots[path->btree_id].b;
	enum six_lock_type lock_type;
	unsigned i;

	EBUG_ON(path->nodes_locked);

	while (1) {
		b = READ_ONCE(*rootp);
		path->level = READ_ONCE(b->c.level);

		if (unlikely(path->level < depth_want)) {
			/*
			 * the root is at a lower depth than the depth we want:
			 * got to the end of the btree, or we're walking nodes
			 * greater than some depth and there are no nodes >=
			 * that depth
			 */
			path->level = depth_want;
			for (i = path->level; i < BTREE_MAX_DEPTH; i++)
				path->l[i].b = NULL;
			return 1;
		}

		lock_type = __btree_lock_want(path, path->level);
		if (unlikely(!btree_node_lock(trans, path, b, SPOS_MAX,
					      path->level, lock_type,
					      lock_root_check_fn, rootp,
					      trace_ip))) {
			if (trans->restarted)
				return -EINTR;
			continue;
		}

		if (likely(b == READ_ONCE(*rootp) &&
			   b->c.level == path->level &&
			   !race_fault())) {
			for (i = 0; i < path->level; i++)
				path->l[i].b = BTREE_ITER_NO_NODE_LOCK_ROOT;
			path->l[path->level].b = b;
			for (i = path->level + 1; i < BTREE_MAX_DEPTH; i++)
				path->l[i].b = NULL;

			mark_btree_node_locked(trans, path, path->level, lock_type);
			btree_path_level_init(trans, path, b);
			return 0;
		}

		six_unlock_type(&b->c.lock, lock_type);
	}
}

noinline
static int btree_path_prefetch(struct btree_trans *trans, struct btree_path *path)
{
	struct bch_fs *c = trans->c;
	struct btree_path_level *l = path_l(path);
	struct btree_node_iter node_iter = l->iter;
	struct bkey_packed *k;
	struct bkey_buf tmp;
	unsigned nr = test_bit(BCH_FS_STARTED, &c->flags)
		? (path->level > 1 ? 0 :  2)
		: (path->level > 1 ? 1 : 16);
	bool was_locked = btree_node_locked(path, path->level);
	int ret = 0;

	bch2_bkey_buf_init(&tmp);

	while (nr && !ret) {
		if (!bch2_btree_node_relock(trans, path, path->level))
			break;

		bch2_btree_node_iter_advance(&node_iter, l->b);
		k = bch2_btree_node_iter_peek(&node_iter, l->b);
		if (!k)
			break;

		bch2_bkey_buf_unpack(&tmp, c, l->b, k);
		ret = bch2_btree_node_prefetch(c, trans, path, tmp.k, path->btree_id,
					       path->level - 1);
	}

	if (!was_locked)
		btree_node_unlock(path, path->level);

	bch2_bkey_buf_exit(&tmp, c);
	return ret;
}

static int btree_path_prefetch_j(struct btree_trans *trans, struct btree_path *path,
				 struct btree_and_journal_iter *jiter)
{
	struct bch_fs *c = trans->c;
	struct bkey_s_c k;
	struct bkey_buf tmp;
	unsigned nr = test_bit(BCH_FS_STARTED, &c->flags)
		? (path->level > 1 ? 0 :  2)
		: (path->level > 1 ? 1 : 16);
	bool was_locked = btree_node_locked(path, path->level);
	int ret = 0;

	bch2_bkey_buf_init(&tmp);

	while (nr && !ret) {
		if (!bch2_btree_node_relock(trans, path, path->level))
			break;

		bch2_btree_and_journal_iter_advance(jiter);
		k = bch2_btree_and_journal_iter_peek(jiter);
		if (!k.k)
			break;

		bch2_bkey_buf_reassemble(&tmp, c, k);
		ret = bch2_btree_node_prefetch(c, trans, path, tmp.k, path->btree_id,
					       path->level - 1);
	}

	if (!was_locked)
		btree_node_unlock(path, path->level);

	bch2_bkey_buf_exit(&tmp, c);
	return ret;
}

static noinline void btree_node_mem_ptr_set(struct btree_trans *trans,
					    struct btree_path *path,
					    unsigned plevel, struct btree *b)
{
	struct btree_path_level *l = &path->l[plevel];
	bool locked = btree_node_locked(path, plevel);
	struct bkey_packed *k;
	struct bch_btree_ptr_v2 *bp;

	if (!bch2_btree_node_relock(trans, path, plevel))
		return;

	k = bch2_btree_node_iter_peek_all(&l->iter, l->b);
	BUG_ON(k->type != KEY_TYPE_btree_ptr_v2);

	bp = (void *) bkeyp_val(&l->b->format, k);
	bp->mem_ptr = (unsigned long)b;

	if (!locked)
		btree_node_unlock(path, plevel);
}

static noinline int btree_node_iter_and_journal_peek(struct btree_trans *trans,
						     struct btree_path *path,
						     unsigned flags,
						     struct bkey_buf *out)
{
	struct bch_fs *c = trans->c;
	struct btree_path_level *l = path_l(path);
	struct btree_and_journal_iter jiter;
	struct bkey_s_c k;
	int ret = 0;

	__bch2_btree_and_journal_iter_init_node_iter(&jiter, c, l->b, l->iter, path->pos);

	k = bch2_btree_and_journal_iter_peek(&jiter);

	bch2_bkey_buf_reassemble(out, c, k);

	if (flags & BTREE_ITER_PREFETCH)
		ret = btree_path_prefetch_j(trans, path, &jiter);

	bch2_btree_and_journal_iter_exit(&jiter);
	return ret;
}

static __always_inline int btree_path_down(struct btree_trans *trans,
					   struct btree_path *path,
					   unsigned flags,
					   unsigned long trace_ip)
{
	struct bch_fs *c = trans->c;
	struct btree_path_level *l = path_l(path);
	struct btree *b;
	unsigned level = path->level - 1;
	enum six_lock_type lock_type = __btree_lock_want(path, level);
	bool replay_done = test_bit(JOURNAL_REPLAY_DONE, &c->journal.flags);
	struct bkey_buf tmp;
	int ret;

	EBUG_ON(!btree_node_locked(path, path->level));

	bch2_bkey_buf_init(&tmp);

	if (unlikely(!replay_done)) {
		ret = btree_node_iter_and_journal_peek(trans, path, flags, &tmp);
		if (ret)
			goto err;
	} else {
		bch2_bkey_buf_unpack(&tmp, c, l->b,
				 bch2_btree_node_iter_peek(&l->iter, l->b));

		if (flags & BTREE_ITER_PREFETCH) {
			ret = btree_path_prefetch(trans, path);
			if (ret)
				goto err;
		}
	}

	b = bch2_btree_node_get(trans, path, tmp.k, level, lock_type, trace_ip);
	ret = PTR_ERR_OR_ZERO(b);
	if (unlikely(ret))
		goto err;

	mark_btree_node_locked(trans, path, level, lock_type);
	btree_path_level_init(trans, path, b);

	if (likely(replay_done && tmp.k->k.type == KEY_TYPE_btree_ptr_v2) &&
	    unlikely(b != btree_node_mem_ptr(tmp.k)))
		btree_node_mem_ptr_set(trans, path, level + 1, b);

	if (btree_node_read_locked(path, level + 1))
		btree_node_unlock(path, level + 1);
	path->level = level;

	bch2_btree_path_verify_locks(path);
err:
	bch2_bkey_buf_exit(&tmp, c);
	return ret;
}

static int btree_path_traverse_one(struct btree_trans *, struct btree_path *,
				   unsigned, unsigned long);

static int bch2_btree_path_traverse_all(struct btree_trans *trans)
{
	struct bch_fs *c = trans->c;
	struct btree_path *path;
	unsigned long trace_ip = _RET_IP_;
	int i, ret = 0;

	if (trans->in_traverse_all)
		return -EINTR;

	trans->in_traverse_all = true;
retry_all:
	trans->restarted = false;
	trans->traverse_all_idx = U8_MAX;

	trans_for_each_path(trans, path)
		path->should_be_locked = false;

	btree_trans_verify_sorted(trans);

	for (i = trans->nr_sorted - 2; i >= 0; --i) {
		struct btree_path *path1 = trans->paths + trans->sorted[i];
		struct btree_path *path2 = trans->paths + trans->sorted[i + 1];

		if (path1->btree_id == path2->btree_id &&
		    path1->locks_want < path2->locks_want)
			__bch2_btree_path_upgrade(trans, path1, path2->locks_want);
		else if (!path1->locks_want && path2->locks_want)
			__bch2_btree_path_upgrade(trans, path1, 1);
	}

	bch2_trans_unlock(trans);
	cond_resched();

	if (unlikely(trans->memory_allocation_failure)) {
		struct closure cl;

		closure_init_stack(&cl);

		do {
			ret = bch2_btree_cache_cannibalize_lock(c, &cl);
			closure_sync(&cl);
		} while (ret);
	}

	/* Now, redo traversals in correct order: */
	trans->traverse_all_idx = 0;
	while (trans->traverse_all_idx < trans->nr_sorted) {
		path = trans->paths + trans->sorted[trans->traverse_all_idx];

		/*
		 * Traversing a path can cause another path to be added at about
		 * the same position:
		 */
		if (path->uptodate) {
			ret = btree_path_traverse_one(trans, path, 0, _THIS_IP_);
			if (ret == -EINTR || ret == -ENOMEM)
				goto retry_all;
			if (ret)
				goto err;
			BUG_ON(path->uptodate);
		} else {
			trans->traverse_all_idx++;
		}
	}

	/*
	 * BTREE_ITER_NEED_RELOCK is ok here - if we called bch2_trans_unlock()
	 * and relock(), relock() won't relock since path->should_be_locked
	 * isn't set yet, which is all fine
	 */
	trans_for_each_path(trans, path)
		BUG_ON(path->uptodate >= BTREE_ITER_NEED_TRAVERSE);
err:
	bch2_btree_cache_cannibalize_unlock(c);

	trans->in_traverse_all = false;

	trace_trans_traverse_all(trans->fn, trace_ip);
	return ret;
}

static inline bool btree_path_good_node(struct btree_trans *trans,
					struct btree_path *path,
					unsigned l, int check_pos)
{
	if (!is_btree_node(path, l) ||
	    !bch2_btree_node_relock(trans, path, l))
		return false;

	if (check_pos < 0 && btree_path_pos_before_node(path, path->l[l].b))
		return false;
	if (check_pos > 0 && btree_path_pos_after_node(path, path->l[l].b))
		return false;
	return true;
}

static inline unsigned btree_path_up_until_good_node(struct btree_trans *trans,
						     struct btree_path *path,
						     int check_pos)
{
	unsigned i, l = path->level;

	while (btree_path_node(path, l) &&
	       !btree_path_good_node(trans, path, l, check_pos)) {
		btree_node_unlock(path, l);
		path->l[l].b = BTREE_ITER_NO_NODE_UP;
		l++;
	}

	/* If we need intent locks, take them too: */
	for (i = l + 1;
	     i < path->locks_want && btree_path_node(path, i);
	     i++)
		if (!bch2_btree_node_relock(trans, path, i))
			while (l <= i) {
				btree_node_unlock(path, l);
				path->l[l].b = BTREE_ITER_NO_NODE_UP;
				l++;
			}

	return l;
}

/*
 * This is the main state machine for walking down the btree - walks down to a
 * specified depth
 *
 * Returns 0 on success, -EIO on error (error reading in a btree node).
 *
 * On error, caller (peek_node()/peek_key()) must return NULL; the error is
 * stashed in the iterator and returned from bch2_trans_exit().
 */
static int btree_path_traverse_one(struct btree_trans *trans,
				   struct btree_path *path,
				   unsigned flags,
				   unsigned long trace_ip)
{
	unsigned depth_want = path->level;
	int ret = 0;

	if (unlikely(trans->restarted)) {
		ret = -EINTR;
		goto out;
	}

	/*
	 * Ensure we obey path->should_be_locked: if it's set, we can't unlock
	 * and re-traverse the path without a transaction restart:
	 */
	if (path->should_be_locked) {
		ret = bch2_btree_path_relock(trans, path, trace_ip) ? 0 : -EINTR;
		goto out;
	}

	if (path->cached) {
		ret = bch2_btree_path_traverse_cached(trans, path, flags);
		goto out;
	}

	if (unlikely(path->level >= BTREE_MAX_DEPTH))
		goto out;

	path->level = btree_path_up_until_good_node(trans, path, 0);

	/*
	 * Note: path->nodes[path->level] may be temporarily NULL here - that
	 * would indicate to other code that we got to the end of the btree,
	 * here it indicates that relocking the root failed - it's critical that
	 * btree_path_lock_root() comes next and that it can't fail
	 */
	while (path->level > depth_want) {
		ret = btree_path_node(path, path->level)
			? btree_path_down(trans, path, flags, trace_ip)
			: btree_path_lock_root(trans, path, depth_want, trace_ip);
		if (unlikely(ret)) {
			if (ret == 1) {
				/*
				 * No nodes at this level - got to the end of
				 * the btree:
				 */
				ret = 0;
				goto out;
			}

			__bch2_btree_path_unlock(path);
			path->level = depth_want;

			if (ret == -EIO)
				path->l[path->level].b =
					BTREE_ITER_NO_NODE_ERROR;
			else
				path->l[path->level].b =
					BTREE_ITER_NO_NODE_DOWN;
			goto out;
		}
	}

	path->uptodate = BTREE_ITER_UPTODATE;
out:
	BUG_ON((ret == -EINTR) != !!trans->restarted);
	bch2_btree_path_verify(trans, path);
	return ret;
}

int __must_check bch2_btree_path_traverse(struct btree_trans *trans,
					  struct btree_path *path, unsigned flags)
{
	if (path->uptodate < BTREE_ITER_NEED_RELOCK)
		return 0;

	return  bch2_trans_cond_resched(trans) ?:
		btree_path_traverse_one(trans, path, flags, _RET_IP_);
}

static void btree_path_copy(struct btree_trans *trans, struct btree_path *dst,
			    struct btree_path *src)
{
	unsigned i;

	memcpy(&dst->pos, &src->pos,
	       sizeof(struct btree_path) - offsetof(struct btree_path, pos));

	for (i = 0; i < BTREE_MAX_DEPTH; i++)
		if (btree_node_locked(dst, i))
			six_lock_increment(&dst->l[i].b->c.lock,
					   __btree_lock_want(dst, i));

	bch2_btree_path_check_sort(trans, dst, 0);
}

static struct btree_path *btree_path_clone(struct btree_trans *trans, struct btree_path *src,
					   bool intent)
{
	struct btree_path *new = btree_path_alloc(trans, src);

	btree_path_copy(trans, new, src);
	__btree_path_get(new, intent);
	return new;
}

inline struct btree_path * __must_check
bch2_btree_path_make_mut(struct btree_trans *trans,
			 struct btree_path *path, bool intent,
			 unsigned long ip)
{
	if (path->ref > 1 || path->preserve) {
		__btree_path_put(path, intent);
		path = btree_path_clone(trans, path, intent);
		path->preserve = false;
#ifdef CONFIG_BCACHEFS_DEBUG
		path->ip_allocated = ip;
#endif
		btree_trans_verify_sorted(trans);
	}

	path->should_be_locked = false;
	return path;
}

struct btree_path * __must_check
bch2_btree_path_set_pos(struct btree_trans *trans,
		   struct btree_path *path, struct bpos new_pos,
		   bool intent, unsigned long ip)
{
	int cmp = bpos_cmp(new_pos, path->pos);
	unsigned l = path->level;

	EBUG_ON(trans->restarted);
	EBUG_ON(!path->ref);

	if (!cmp)
		return path;

	path = bch2_btree_path_make_mut(trans, path, intent, ip);

	path->pos = new_pos;

	bch2_btree_path_check_sort(trans, path, cmp);

	if (unlikely(path->cached)) {
		btree_node_unlock(path, 0);
		path->l[0].b = BTREE_ITER_NO_NODE_CACHED;
		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
		goto out;
	}

	l = btree_path_up_until_good_node(trans, path, cmp);

	if (btree_path_node(path, l)) {
		BUG_ON(!btree_node_locked(path, l));
		/*
		 * We might have to skip over many keys, or just a few: try
		 * advancing the node iterator, and if we have to skip over too
		 * many keys just reinit it (or if we're rewinding, since that
		 * is expensive).
		 */
		if (cmp < 0 ||
		    !btree_path_advance_to_pos(path, &path->l[l], 8))
			__btree_path_level_init(path, l);
	}

	if (l != path->level) {
		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
		__bch2_btree_path_unlock(path);
	}
out:
	bch2_btree_path_verify(trans, path);
	return path;
}

/* Btree path: main interface: */

static struct btree_path *have_path_at_pos(struct btree_trans *trans, struct btree_path *path)
{
	struct btree_path *next;

	next = prev_btree_path(trans, path);
	if (next && !btree_path_cmp(next, path))
		return next;

	next = next_btree_path(trans, path);
	if (next && !btree_path_cmp(next, path))
		return next;

	return NULL;
}

static struct btree_path *have_node_at_pos(struct btree_trans *trans, struct btree_path *path)
{
	struct btree_path *next;

	next = prev_btree_path(trans, path);
	if (next && next->level == path->level && path_l(next)->b == path_l(path)->b)
		return next;

	next = next_btree_path(trans, path);
	if (next && next->level == path->level && path_l(next)->b == path_l(path)->b)
		return next;

	return NULL;
}

static inline void __bch2_path_free(struct btree_trans *trans, struct btree_path *path)
{
	__bch2_btree_path_unlock(path);
	btree_path_list_remove(trans, path);
	trans->paths_allocated &= ~(1ULL << path->idx);
}

void bch2_path_put(struct btree_trans *trans, struct btree_path *path, bool intent)
{
	struct btree_path *dup;

	EBUG_ON(trans->paths + path->idx != path);
	EBUG_ON(!path->ref);

	if (!__btree_path_put(path, intent))
		return;

	/*
	 * Perhaps instead we should check for duplicate paths in traverse_all:
	 */
	if (path->preserve &&
	    (dup = have_path_at_pos(trans, path))) {
		dup->preserve = true;
		path->preserve = false;
		goto free;
	}

	if (!path->preserve &&
	    (dup = have_node_at_pos(trans, path)))
		goto free;
	return;
free:
	if (path->should_be_locked &&
	    !btree_node_locked(dup, path->level))
		return;

	dup->should_be_locked |= path->should_be_locked;
	__bch2_path_free(trans, path);
}

void bch2_trans_updates_to_text(struct printbuf *buf, struct btree_trans *trans)
{
	struct btree_insert_entry *i;

	pr_buf(buf, "transaction updates for %s journal seq %llu",
	       trans->fn, trans->journal_res.seq);
	pr_newline(buf);
	pr_indent_push(buf, 2);

	trans_for_each_update(trans, i) {
		struct bkey_s_c old = { &i->old_k, i->old_v };

		pr_buf(buf, "update: btree %s %pS",
		       bch2_btree_ids[i->btree_id],
		       (void *) i->ip_allocated);
		pr_newline(buf);

		pr_buf(buf, "  old ");
		bch2_bkey_val_to_text(buf, trans->c, old);
		pr_newline(buf);

		pr_buf(buf, "  new ");
		bch2_bkey_val_to_text(buf, trans->c, bkey_i_to_s_c(i->k));
		pr_newline(buf);
	}

	pr_indent_pop(buf, 2);
}

noinline __cold
void bch2_dump_trans_updates(struct btree_trans *trans)
{
	struct printbuf buf = PRINTBUF;

	bch2_trans_updates_to_text(&buf, trans);
	bch_err(trans->c, "%s", buf.buf);
	printbuf_exit(&buf);
}

noinline __cold
void bch2_dump_trans_paths_updates(struct btree_trans *trans)
{
	struct btree_path *path;
	struct printbuf buf = PRINTBUF;
	unsigned idx;

	trans_for_each_path_inorder(trans, path, idx) {
		printbuf_reset(&buf);

		bch2_bpos_to_text(&buf, path->pos);

		printk(KERN_ERR "path: idx %u ref %u:%u%s%s btree=%s l=%u pos %s locks %u %pS\n",
		       path->idx, path->ref, path->intent_ref,
		       path->should_be_locked ? " S" : "",
		       path->preserve ? " P" : "",
		       bch2_btree_ids[path->btree_id],
		       path->level,
		       buf.buf,
		       path->nodes_locked,
#ifdef CONFIG_BCACHEFS_DEBUG
		       (void *) path->ip_allocated
#else
		       NULL
#endif
		       );
	}

	printbuf_exit(&buf);

	bch2_dump_trans_updates(trans);
}

static struct btree_path *btree_path_alloc(struct btree_trans *trans,
					   struct btree_path *pos)
{
	struct btree_path *path;
	unsigned idx;

	if (unlikely(trans->paths_allocated ==
		     ~((~0ULL << 1) << (BTREE_ITER_MAX - 1)))) {
		bch2_dump_trans_paths_updates(trans);
		panic("trans path oveflow\n");
	}

	idx = __ffs64(~trans->paths_allocated);
	trans->paths_allocated |= 1ULL << idx;

	path = &trans->paths[idx];

	path->idx		= idx;
	path->ref		= 0;
	path->intent_ref	= 0;
	path->nodes_locked	= 0;
	path->nodes_intent_locked = 0;

	btree_path_list_add(trans, pos, path);
	return path;
}

struct btree_path *bch2_path_get(struct btree_trans *trans,
				 enum btree_id btree_id, struct bpos pos,
				 unsigned locks_want, unsigned level,
				 unsigned flags, unsigned long ip)
{
	struct btree_path *path, *path_pos = NULL;
	bool cached = flags & BTREE_ITER_CACHED;
	bool intent = flags & BTREE_ITER_INTENT;
	int i;

	BUG_ON(trans->restarted);
	btree_trans_verify_sorted(trans);
	bch2_trans_verify_locks(trans);

	trans_for_each_path_inorder(trans, path, i) {
		if (__btree_path_cmp(path,
				     btree_id,
				     cached,
				     pos,
				     level) > 0)
			break;

		path_pos = path;
	}

	if (path_pos &&
	    path_pos->cached	== cached &&
	    path_pos->btree_id	== btree_id &&
	    path_pos->level	== level) {
		__btree_path_get(path_pos, intent);
		path = bch2_btree_path_set_pos(trans, path_pos, pos, intent, ip);
	} else {
		path = btree_path_alloc(trans, path_pos);
		path_pos = NULL;

		__btree_path_get(path, intent);
		path->pos			= pos;
		path->btree_id			= btree_id;
		path->cached			= cached;
		path->uptodate			= BTREE_ITER_NEED_TRAVERSE;
		path->should_be_locked		= false;
		path->level			= level;
		path->locks_want		= locks_want;
		path->nodes_locked		= 0;
		path->nodes_intent_locked	= 0;
		for (i = 0; i < ARRAY_SIZE(path->l); i++)
			path->l[i].b		= BTREE_ITER_NO_NODE_INIT;
#ifdef CONFIG_BCACHEFS_DEBUG
		path->ip_allocated		= ip;
#endif
		btree_trans_verify_sorted(trans);
	}

	if (!(flags & BTREE_ITER_NOPRESERVE))
		path->preserve = true;

	if (path->intent_ref)
		locks_want = max(locks_want, level + 1);

	/*
	 * If the path has locks_want greater than requested, we don't downgrade
	 * it here - on transaction restart because btree node split needs to
	 * upgrade locks, we might be putting/getting the iterator again.
	 * Downgrading iterators only happens via bch2_trans_downgrade(), after
	 * a successful transaction commit.
	 */

	locks_want = min(locks_want, BTREE_MAX_DEPTH);
	if (locks_want > path->locks_want) {
		path->locks_want = locks_want;
		btree_path_get_locks(trans, path, true);
	}

	return path;
}

inline struct bkey_s_c bch2_btree_path_peek_slot(struct btree_path *path, struct bkey *u)
{

	struct bkey_s_c k;

	if (!path->cached) {
		struct btree_path_level *l = path_l(path);
		struct bkey_packed *_k;

		EBUG_ON(path->uptodate != BTREE_ITER_UPTODATE);

		_k = bch2_btree_node_iter_peek_all(&l->iter, l->b);
		k = _k ? bkey_disassemble(l->b, _k, u) : bkey_s_c_null;

		EBUG_ON(k.k && bkey_deleted(k.k) && bpos_cmp(k.k->p, path->pos) == 0);

		if (!k.k || bpos_cmp(path->pos, k.k->p))
			goto hole;
	} else {
		struct bkey_cached *ck = (void *) path->l[0].b;

		EBUG_ON(ck &&
			(path->btree_id != ck->key.btree_id ||
			 bkey_cmp(path->pos, ck->key.pos)));

		/* BTREE_ITER_CACHED_NOFILL|BTREE_ITER_CACHED_NOCREATE? */
		if (unlikely(!ck || !ck->valid))
			return bkey_s_c_null;

		EBUG_ON(path->uptodate != BTREE_ITER_UPTODATE);

		*u = ck->k->k;
		k = bkey_i_to_s_c(ck->k);
	}

	return k;
hole:
	bkey_init(u);
	u->p = path->pos;
	return (struct bkey_s_c) { u, NULL };
}

/* Btree iterators: */

int __must_check
__bch2_btree_iter_traverse(struct btree_iter *iter)
{
	return bch2_btree_path_traverse(iter->trans, iter->path, iter->flags);
}

int __must_check
bch2_btree_iter_traverse(struct btree_iter *iter)
{
	int ret;

	iter->path = bch2_btree_path_set_pos(iter->trans, iter->path,
					btree_iter_search_key(iter),
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));

	ret = bch2_btree_path_traverse(iter->trans, iter->path, iter->flags);
	if (ret)
		return ret;

	iter->path->should_be_locked = true;
	return 0;
}

/* Iterate across nodes (leaf and interior nodes) */

struct btree *bch2_btree_iter_peek_node(struct btree_iter *iter)
{
	struct btree_trans *trans = iter->trans;
	struct btree *b = NULL;
	int ret;

	EBUG_ON(iter->path->cached);
	bch2_btree_iter_verify(iter);

	ret = bch2_btree_path_traverse(trans, iter->path, iter->flags);
	if (ret)
		goto err;

	b = btree_path_node(iter->path, iter->path->level);
	if (!b)
		goto out;

	BUG_ON(bpos_cmp(b->key.k.p, iter->pos) < 0);

	bkey_init(&iter->k);
	iter->k.p = iter->pos = b->key.k.p;

	iter->path = bch2_btree_path_set_pos(trans, iter->path, b->key.k.p,
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));
	iter->path->should_be_locked = true;
	BUG_ON(iter->path->uptodate);
out:
	bch2_btree_iter_verify_entry_exit(iter);
	bch2_btree_iter_verify(iter);

	return b;
err:
	b = ERR_PTR(ret);
	goto out;
}

struct btree *bch2_btree_iter_next_node(struct btree_iter *iter)
{
	struct btree_trans *trans = iter->trans;
	struct btree_path *path = iter->path;
	struct btree *b = NULL;
	unsigned l;
	int ret;

	BUG_ON(trans->restarted);
	EBUG_ON(iter->path->cached);
	bch2_btree_iter_verify(iter);

	/* already at end? */
	if (!btree_path_node(path, path->level))
		return NULL;

	/* got to end? */
	if (!btree_path_node(path, path->level + 1)) {
		btree_node_unlock(path, path->level);
		path->l[path->level].b = BTREE_ITER_NO_NODE_UP;
		path->level++;
		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
		return NULL;
	}

	if (!bch2_btree_node_relock(trans, path, path->level + 1)) {
		__bch2_btree_path_unlock(path);
		path->l[path->level].b = BTREE_ITER_NO_NODE_GET_LOCKS;
		path->l[path->level + 1].b = BTREE_ITER_NO_NODE_GET_LOCKS;
		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
		trace_trans_restart_relock_next_node(trans->fn, _THIS_IP_,
					   path->btree_id, &path->pos);
		btree_trans_restart(trans);
		ret = -EINTR;
		goto err;
	}

	b = btree_path_node(path, path->level + 1);

	if (!bpos_cmp(iter->pos, b->key.k.p)) {
		btree_node_unlock(path, path->level);
		path->l[path->level].b = BTREE_ITER_NO_NODE_UP;
		path->level++;
	} else {
		/*
		 * Haven't gotten to the end of the parent node: go back down to
		 * the next child node
		 */
		path = iter->path =
			bch2_btree_path_set_pos(trans, path, bpos_successor(iter->pos),
					   iter->flags & BTREE_ITER_INTENT,
					   btree_iter_ip_allocated(iter));

		path->level = iter->min_depth;

		for (l = path->level + 1; l < BTREE_MAX_DEPTH; l++)
			if (btree_lock_want(path, l) == BTREE_NODE_UNLOCKED)
				btree_node_unlock(path, l);

		btree_path_set_dirty(path, BTREE_ITER_NEED_TRAVERSE);
		bch2_btree_iter_verify(iter);

		ret = bch2_btree_path_traverse(trans, path, iter->flags);
		if (ret)
			goto err;

		b = path->l[path->level].b;
	}

	bkey_init(&iter->k);
	iter->k.p = iter->pos = b->key.k.p;

	iter->path = bch2_btree_path_set_pos(trans, iter->path, b->key.k.p,
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));
	iter->path->should_be_locked = true;
	BUG_ON(iter->path->uptodate);
out:
	bch2_btree_iter_verify_entry_exit(iter);
	bch2_btree_iter_verify(iter);

	return b;
err:
	b = ERR_PTR(ret);
	goto out;
}

/* Iterate across keys (in leaf nodes only) */

inline bool bch2_btree_iter_advance(struct btree_iter *iter)
{
	struct bpos pos = iter->k.p;
	bool ret = (iter->flags & BTREE_ITER_ALL_SNAPSHOTS
		    ? bpos_cmp(pos, SPOS_MAX)
		    : bkey_cmp(pos, SPOS_MAX)) != 0;

	if (ret && !(iter->flags & BTREE_ITER_IS_EXTENTS))
		pos = bkey_successor(iter, pos);
	bch2_btree_iter_set_pos(iter, pos);
	return ret;
}

inline bool bch2_btree_iter_rewind(struct btree_iter *iter)
{
	struct bpos pos = bkey_start_pos(&iter->k);
	bool ret = (iter->flags & BTREE_ITER_ALL_SNAPSHOTS
		    ? bpos_cmp(pos, POS_MIN)
		    : bkey_cmp(pos, POS_MIN)) != 0;

	if (ret && !(iter->flags & BTREE_ITER_IS_EXTENTS))
		pos = bkey_predecessor(iter, pos);
	bch2_btree_iter_set_pos(iter, pos);
	return ret;
}

static inline struct bkey_i *btree_trans_peek_updates(struct btree_trans *trans,
						      enum btree_id btree_id,
						      struct bpos pos)
{
	struct btree_insert_entry *i;

	trans_for_each_update(trans, i)
		if ((cmp_int(btree_id,	i->btree_id) ?:
		     bpos_cmp(pos,	i->k->k.p)) <= 0) {
			if (btree_id ==	i->btree_id)
				return i->k;
			break;
		}

	return NULL;
}

static noinline
struct bkey_s_c btree_trans_peek_journal(struct btree_trans *trans,
					 struct btree_iter *iter,
					 struct bkey_s_c k)
{
	struct bkey_i *next_journal =
		bch2_journal_keys_peek(trans->c, iter->btree_id, 0,
				       iter->path->pos);

	if (next_journal &&
	    bpos_cmp(next_journal->k.p,
		     k.k ? k.k->p : iter->path->l[0].b->key.k.p) <= 0) {
		iter->k = next_journal->k;
		k = bkey_i_to_s_c(next_journal);
	}

	return k;
}

/*
 * Checks btree key cache for key at iter->pos and returns it if present, or
 * bkey_s_c_null:
 */
static noinline
struct bkey_s_c btree_trans_peek_key_cache(struct btree_iter *iter, struct bpos pos)
{
	struct btree_trans *trans = iter->trans;
	struct bch_fs *c = trans->c;
	struct bkey u;
	int ret;

	if (!bch2_btree_key_cache_find(c, iter->btree_id, pos))
		return bkey_s_c_null;

	if (!iter->key_cache_path)
		iter->key_cache_path = bch2_path_get(trans, iter->btree_id, pos,
						     iter->flags & BTREE_ITER_INTENT, 0,
						     iter->flags|BTREE_ITER_CACHED,
						     _THIS_IP_);

	iter->key_cache_path = bch2_btree_path_set_pos(trans, iter->key_cache_path, pos,
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));

	ret = bch2_btree_path_traverse(trans, iter->key_cache_path, iter->flags|BTREE_ITER_CACHED);
	if (unlikely(ret))
		return bkey_s_c_err(ret);

	iter->key_cache_path->should_be_locked = true;

	return bch2_btree_path_peek_slot(iter->key_cache_path, &u);
}

static struct bkey_s_c __bch2_btree_iter_peek(struct btree_iter *iter, struct bpos search_key)
{
	struct btree_trans *trans = iter->trans;
	struct bkey_i *next_update;
	struct bkey_s_c k, k2;
	int ret;

	EBUG_ON(iter->path->cached || iter->path->level);
	bch2_btree_iter_verify(iter);

	while (1) {
		iter->path = bch2_btree_path_set_pos(trans, iter->path, search_key,
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));

		ret = bch2_btree_path_traverse(trans, iter->path, iter->flags);
		if (unlikely(ret)) {
			/* ensure that iter->k is consistent with iter->pos: */
			bch2_btree_iter_set_pos(iter, iter->pos);
			k = bkey_s_c_err(ret);
			goto out;
		}

		iter->path->should_be_locked = true;

		k = btree_path_level_peek_all(trans->c, &iter->path->l[0], &iter->k);

		if (unlikely(iter->flags & BTREE_ITER_WITH_KEY_CACHE) &&
		    k.k &&
		    (k2 = btree_trans_peek_key_cache(iter, k.k->p)).k) {
			ret = bkey_err(k2);
			if (ret) {
				k = k2;
				bch2_btree_iter_set_pos(iter, iter->pos);
				goto out;
			}

			k = k2;
			iter->k = *k.k;
		}

		if (unlikely(iter->flags & BTREE_ITER_WITH_JOURNAL))
			k = btree_trans_peek_journal(trans, iter, k);

		next_update = iter->flags & BTREE_ITER_WITH_UPDATES
			? btree_trans_peek_updates(trans, iter->btree_id, search_key)
			: NULL;
		if (next_update &&
		    bpos_cmp(next_update->k.p,
			     k.k ? k.k->p : iter->path->l[0].b->key.k.p) <= 0) {
			iter->k = next_update->k;
			k = bkey_i_to_s_c(next_update);
		}

		if (k.k && bkey_deleted(k.k)) {
			/*
			 * If we've got a whiteout, and it's after the search
			 * key, advance the search key to the whiteout instead
			 * of just after the whiteout - it might be a btree
			 * whiteout, with a real key at the same position, since
			 * in the btree deleted keys sort before non deleted.
			 */
			search_key = bpos_cmp(search_key, k.k->p)
				? k.k->p
				: bpos_successor(k.k->p);
			continue;
		}

		if (likely(k.k)) {
			break;
		} else if (likely(bpos_cmp(iter->path->l[0].b->key.k.p, SPOS_MAX))) {
			/* Advance to next leaf node: */
			search_key = bpos_successor(iter->path->l[0].b->key.k.p);
		} else {
			/* End of btree: */
			bch2_btree_iter_set_pos(iter, SPOS_MAX);
			k = bkey_s_c_null;
			goto out;
		}
	}
out:
	bch2_btree_iter_verify(iter);

	return k;
}

/**
 * bch2_btree_iter_peek: returns first key greater than or equal to iterator's
 * current position
 */
struct bkey_s_c bch2_btree_iter_peek_upto(struct btree_iter *iter, struct bpos end)
{
	struct btree_trans *trans = iter->trans;
	struct bpos search_key = btree_iter_search_key(iter);
	struct bkey_s_c k;
	struct bpos iter_pos;
	int ret;

	if (iter->update_path) {
		bch2_path_put(trans, iter->update_path,
			      iter->flags & BTREE_ITER_INTENT);
		iter->update_path = NULL;
	}

	bch2_btree_iter_verify_entry_exit(iter);

	while (1) {
		k = __bch2_btree_iter_peek(iter, search_key);
		if (!k.k || bkey_err(k))
			goto out;

		/*
		 * iter->pos should be mononotically increasing, and always be
		 * equal to the key we just returned - except extents can
		 * straddle iter->pos:
		 */
		if (!(iter->flags & BTREE_ITER_IS_EXTENTS))
			iter_pos = k.k->p;
		else if (bkey_cmp(bkey_start_pos(k.k), iter->pos) > 0)
			iter_pos = bkey_start_pos(k.k);
		else
			iter_pos = iter->pos;

		if (bkey_cmp(iter_pos, end) > 0) {
			bch2_btree_iter_set_pos(iter, end);
			k = bkey_s_c_null;
			goto out;
		}

		if (iter->update_path &&
		    bkey_cmp(iter->update_path->pos, k.k->p)) {
			bch2_path_put(trans, iter->update_path,
				      iter->flags & BTREE_ITER_INTENT);
			iter->update_path = NULL;
		}

		if ((iter->flags & BTREE_ITER_FILTER_SNAPSHOTS) &&
		    (iter->flags & BTREE_ITER_INTENT) &&
		    !(iter->flags & BTREE_ITER_IS_EXTENTS) &&
		    !iter->update_path) {
			struct bpos pos = k.k->p;

			if (pos.snapshot < iter->snapshot) {
				search_key = bpos_successor(k.k->p);
				continue;
			}

			pos.snapshot = iter->snapshot;

			/*
			 * advance, same as on exit for iter->path, but only up
			 * to snapshot
			 */
			__btree_path_get(iter->path, iter->flags & BTREE_ITER_INTENT);
			iter->update_path = iter->path;

			iter->update_path = bch2_btree_path_set_pos(trans,
						iter->update_path, pos,
						iter->flags & BTREE_ITER_INTENT,
						_THIS_IP_);
		}

		/*
		 * We can never have a key in a leaf node at POS_MAX, so
		 * we don't have to check these successor() calls:
		 */
		if ((iter->flags & BTREE_ITER_FILTER_SNAPSHOTS) &&
		    !bch2_snapshot_is_ancestor(trans->c,
					       iter->snapshot,
					       k.k->p.snapshot)) {
			search_key = bpos_successor(k.k->p);
			continue;
		}

		if (bkey_whiteout(k.k) &&
		    !(iter->flags & BTREE_ITER_ALL_SNAPSHOTS)) {
			search_key = bkey_successor(iter, k.k->p);
			continue;
		}

		break;
	}

	iter->pos = iter_pos;

	iter->path = bch2_btree_path_set_pos(trans, iter->path, k.k->p,
				iter->flags & BTREE_ITER_INTENT,
				btree_iter_ip_allocated(iter));
	BUG_ON(!iter->path->nodes_locked);
out:
	if (iter->update_path) {
		if (iter->update_path->uptodate &&
		    !bch2_btree_path_relock(trans, iter->update_path, _THIS_IP_)) {
			k = bkey_s_c_err(-EINTR);
		} else {
			BUG_ON(!(iter->update_path->nodes_locked & 1));
			iter->update_path->should_be_locked = true;
		}
	}
	iter->path->should_be_locked = true;

	if (!(iter->flags & BTREE_ITER_ALL_SNAPSHOTS))
		iter->pos.snapshot = iter->snapshot;

	ret = bch2_btree_iter_verify_ret(iter, k);
	if (unlikely(ret)) {
		bch2_btree_iter_set_pos(iter, iter->pos);
		k = bkey_s_c_err(ret);
	}

	bch2_btree_iter_verify_entry_exit(iter);

	return k;
}

/**
 * bch2_btree_iter_next: returns first key greater than iterator's current
 * position
 */
struct bkey_s_c bch2_btree_iter_next(struct btree_iter *iter)
{
	if (!bch2_btree_iter_advance(iter))
		return bkey_s_c_null;

	return bch2_btree_iter_peek(iter);
}

/**
 * bch2_btree_iter_peek_prev: returns first key less than or equal to
 * iterator's current position
 */
struct bkey_s_c bch2_btree_iter_peek_prev(struct btree_iter *iter)
{
	struct btree_trans *trans = iter->trans;
	struct bpos search_key = iter->pos;
	struct btree_path *saved_path = NULL;
	struct bkey_s_c k;
	struct bkey saved_k;
	const struct bch_val *saved_v;
	int ret;

	EBUG_ON(iter->path->cached || iter->path->level);
	EBUG_ON(iter->flags & BTREE_ITER_WITH_UPDATES);

	if (iter->flags & BTREE_ITER_WITH_JOURNAL)
		return bkey_s_c_err(-EIO);

	bch2_btree_iter_verify(iter);
	bch2_btree_iter_verify_entry_exit(iter);

	if (iter->flags & BTREE_ITER_FILTER_SNAPSHOTS)
		search_key.snapshot = U32_MAX;

	while (1) {
		iter->path = bch2_btree_path_set_pos(trans, iter->path, search_key,
						iter->flags & BTREE_ITER_INTENT,
						btree_iter_ip_allocated(iter));

		ret = bch2_btree_path_traverse(trans, iter->path, iter->flags);
		if (unlikely(ret)) {
			/* ensure that iter->k is consistent with iter->pos: */
			bch2_btree_iter_set_pos(iter, iter->pos);
			k = bkey_s_c_err(ret);
			goto out;
		}

		k = btree_path_level_peek(trans->c, iter->path,
					  &iter->path->l[0], &iter->k);
		if (!k.k ||
		    ((iter->flags & BTREE_ITER_IS_EXTENTS)
		     ? bpos_cmp(bkey_start_pos(k.k), search_key) >= 0
		     : bpos_cmp(k.k->p, search_key) > 0))
			k = btree_path_level_prev(trans->c, iter->path,
						  &iter->path->l[0], &iter->k);

		bch2_btree_path_check_sort(trans, iter->path, 0);

		if (likely(k.k)) {
			if (iter->flags & BTREE_ITER_FILTER_SNAPSHOTS) {
				if (k.k->p.snapshot == iter->snapshot)
					goto got_key;

				/*
				 * If we have a saved candidate, and we're no
				 * longer at the same _key_ (not pos), return
				 * that candidate
				 */
				if (saved_path && bkey_cmp(k.k->p, saved_k.p)) {
					bch2_path_put(trans, iter->path,
						      iter->flags & BTREE_ITER_INTENT);
					iter->path = saved_path;
					saved_path = NULL;
					iter->k	= saved_k;
					k.v	= saved_v;
					goto got_key;
				}

				if (bch2_snapshot_is_ancestor(iter->trans->c,
							      iter->snapshot,
							      k.k->p.snapshot)) {
					if (saved_path)
						bch2_path_put(trans, saved_path,
						      iter->flags & BTREE_ITER_INTENT);
					saved_path = btree_path_clone(trans, iter->path,
								iter->flags & BTREE_ITER_INTENT);
					saved_k = *k.k;
					saved_v = k.v;
				}

				search_key = bpos_predecessor(k.k->p);
				continue;
			}
got_key:
			if (bkey_whiteout(k.k) &&
			    !(iter->flags & BTREE_ITER_ALL_SNAPSHOTS)) {
				search_key = bkey_predecessor(iter, k.k->p);
				if (iter->flags & BTREE_ITER_FILTER_SNAPSHOTS)
					search_key.snapshot = U32_MAX;
				continue;
			}

			break;
		} else if (likely(bpos_cmp(iter->path->l[0].b->data->min_key, POS_MIN))) {
			/* Advance to previous leaf node: */
			search_key = bpos_predecessor(iter->path->l[0].b->data->min_key);
		} else {
			/* Start of btree: */
			bch2_btree_iter_set_pos(iter, POS_MIN);
			k = bkey_s_c_null;
			goto out;
		}
	}

	EBUG_ON(bkey_cmp(bkey_start_pos(k.k), iter->pos) > 0);

	/* Extents can straddle iter->pos: */
	if (bkey_cmp(k.k->p, iter->pos) < 0)
		iter->pos = k.k->p;

	if (iter->flags & BTREE_ITER_FILTER_SNAPSHOTS)
		iter->pos.snapshot = iter->snapshot;
out:
	if (saved_path)
		bch2_path_put(trans, saved_path, iter->flags & BTREE_ITER_INTENT);
	iter->path->should_be_locked = true;

	bch2_btree_iter_verify_entry_exit(iter);
	bch2_btree_iter_verify(iter);

	return k;
}

/**
 * bch2_btree_iter_prev: returns first key less than iterator's current
 * position
 */
struct bkey_s_c bch2_btree_iter_prev(struct btree_iter *iter)
{
	if (!bch2_btree_iter_rewind(iter))
		return bkey_s_c_null;

	return bch2_btree_iter_peek_prev(iter);
}

struct bkey_s_c bch2_btree_iter_peek_slot(struct btree_iter *iter)
{
	struct btree_trans *trans = iter->trans;
	struct bpos search_key;
	struct bkey_s_c k;
	int ret;

	EBUG_ON(iter->path->level);
	bch2_btree_iter_verify(iter);
	bch2_btree_iter_verify_entry_exit(iter);

	/* extents can't span inode numbers: */
	if ((iter->flags & BTREE_ITER_IS_EXTENTS) &&
	    unlikely(iter->pos.offset == KEY_OFFSET_MAX)) {
		if (iter->pos.inode == KEY_INODE_MAX)
			return bkey_s_c_null;

		bch2_btree_iter_set_pos(iter, bpos_nosnap_successor(iter->pos));
	}

	search_key = btree_iter_search_key(iter);
	iter->path = bch2_btree_path_set_pos(trans, iter->path, search_key,
					iter->flags & BTREE_ITER_INTENT,
					btree_iter_ip_allocated(iter));

	ret = bch2_btree_path_traverse(trans, iter->path, iter->flags);
	if (unlikely(ret))
		return bkey_s_c_err(ret);

	if ((iter->flags & BTREE_ITER_CACHED) ||
	    !(iter->flags & (BTREE_ITER_IS_EXTENTS|BTREE_ITER_FILTER_SNAPSHOTS))) {
		struct bkey_i *next_update;

		if ((iter->flags & BTREE_ITER_WITH_UPDATES) &&
		    (next_update = btree_trans_peek_updates(trans,
						iter->btree_id, search_key)) &&
		    !bpos_cmp(next_update->k.p, iter->pos)) {
			iter->k = next_update->k;
			k = bkey_i_to_s_c(next_update);
			goto out;
		}

		if (unlikely(iter->flags & BTREE_ITER_WITH_JOURNAL) &&
		    (next_update = bch2_journal_keys_peek(trans->c, iter->btree_id,
							  0, iter->pos)) &&
		    !bpos_cmp(next_update->k.p, iter->pos)) {
			iter->k = next_update->k;
			k = bkey_i_to_s_c(next_update);
			goto out;
		}

		if (unlikely(iter->flags & BTREE_ITER_WITH_KEY_CACHE) &&
		    (k = btree_trans_peek_key_cache(iter, iter->pos)).k) {
			if (!bkey_err(k))
				iter->k = *k.k;
			goto out;
		}

		k = bch2_btree_path_peek_slot(iter->path, &iter->k);
	} else {
		struct bpos next;

		if (iter->flags & BTREE_ITER_INTENT) {
			struct btree_iter iter2;
			struct bpos end = iter->pos;

			if (iter->flags & BTREE_ITER_IS_EXTENTS)
				end.offset = U64_MAX;

			bch2_trans_copy_iter(&iter2, iter);
			k = bch2_btree_iter_peek_upto(&iter2, end);

			if (k.k && !bkey_err(k)) {
				iter->k = iter2.k;
				k.k = &iter->k;
			}
			bch2_trans_iter_exit(trans, &iter2);
		} else {
			struct bpos pos = iter->pos;

			k = bch2_btree_iter_peek(iter);
			iter->pos = pos;
		}

		if (unlikely(bkey_err(k)))
			return k;

		next = k.k ? bkey_start_pos(k.k) : POS_MAX;

		if (bkey_cmp(iter->pos, next) < 0) {
			bkey_init(&iter->k);
			iter->k.p = iter->pos;

			if (iter->flags & BTREE_ITER_IS_EXTENTS) {
				bch2_key_resize(&iter->k,
						min_t(u64, KEY_SIZE_MAX,
						      (next.inode == iter->pos.inode
						       ? next.offset
						       : KEY_OFFSET_MAX) -
						      iter->pos.offset));
				EBUG_ON(!iter->k.size);
			}

			k = (struct bkey_s_c) { &iter->k, NULL };
		}
	}
out:
	iter->path->should_be_locked = true;

	bch2_btree_iter_verify_entry_exit(iter);
	bch2_btree_iter_verify(iter);
	ret = bch2_btree_iter_verify_ret(iter, k);
	if (unlikely(ret))
		return bkey_s_c_err(ret);

	return k;
}

struct bkey_s_c bch2_btree_iter_next_slot(struct btree_iter *iter)
{
	if (!bch2_btree_iter_advance(iter))
		return bkey_s_c_null;

	return bch2_btree_iter_peek_slot(iter);
}

struct bkey_s_c bch2_btree_iter_prev_slot(struct btree_iter *iter)
{
	if (!bch2_btree_iter_rewind(iter))
		return bkey_s_c_null;

	return bch2_btree_iter_peek_slot(iter);
}

/* new transactional stuff: */

static inline void btree_path_verify_sorted_ref(struct btree_trans *trans,
						struct btree_path *path)
{
	EBUG_ON(path->sorted_idx >= trans->nr_sorted);
	EBUG_ON(trans->sorted[path->sorted_idx] != path->idx);
	EBUG_ON(!(trans->paths_allocated & (1ULL << path->idx)));
}

static inline void btree_trans_verify_sorted_refs(struct btree_trans *trans)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	unsigned i;

	for (i = 0; i < trans->nr_sorted; i++)
		btree_path_verify_sorted_ref(trans, trans->paths + trans->sorted[i]);
#endif
}

static void btree_trans_verify_sorted(struct btree_trans *trans)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	struct btree_path *path, *prev = NULL;
	unsigned i;

	trans_for_each_path_inorder(trans, path, i) {
		if (prev && btree_path_cmp(prev, path) > 0) {
			bch2_dump_trans_paths_updates(trans);
			panic("trans paths out of order!\n");
		}
		prev = path;
	}
#endif
}

static inline void btree_path_swap(struct btree_trans *trans,
				   struct btree_path *l, struct btree_path *r)
{
	swap(l->sorted_idx, r->sorted_idx);
	swap(trans->sorted[l->sorted_idx],
	     trans->sorted[r->sorted_idx]);

	btree_path_verify_sorted_ref(trans, l);
	btree_path_verify_sorted_ref(trans, r);
}

inline void bch2_btree_path_check_sort(struct btree_trans *trans, struct btree_path *path,
				       int cmp)
{
	struct btree_path *n;

	if (cmp <= 0) {
		n = prev_btree_path(trans, path);
		if (n && btree_path_cmp(n, path) > 0) {
			do {
				btree_path_swap(trans, n, path);
				n = prev_btree_path(trans, path);
			} while (n && btree_path_cmp(n, path) > 0);

			goto out;
		}
	}

	if (cmp >= 0) {
		n = next_btree_path(trans, path);
		if (n && btree_path_cmp(path, n) > 0) {
			do {
				btree_path_swap(trans, path, n);
				n = next_btree_path(trans, path);
			} while (n && btree_path_cmp(path, n) > 0);
		}
	}
out:
	btree_trans_verify_sorted(trans);
}

static inline void btree_path_list_remove(struct btree_trans *trans,
					  struct btree_path *path)
{
	unsigned i;

	EBUG_ON(path->sorted_idx >= trans->nr_sorted);

	array_remove_item(trans->sorted, trans->nr_sorted, path->sorted_idx);

	for (i = path->sorted_idx; i < trans->nr_sorted; i++)
		trans->paths[trans->sorted[i]].sorted_idx = i;

	path->sorted_idx = U8_MAX;

	btree_trans_verify_sorted_refs(trans);
}

static inline void btree_path_list_add(struct btree_trans *trans,
				       struct btree_path *pos,
				       struct btree_path *path)
{
	unsigned i;

	btree_trans_verify_sorted_refs(trans);

	path->sorted_idx = pos ? pos->sorted_idx + 1 : 0;

	if (trans->in_traverse_all &&
	    trans->traverse_all_idx != U8_MAX &&
	    trans->traverse_all_idx >= path->sorted_idx)
		trans->traverse_all_idx++;

	array_insert_item(trans->sorted, trans->nr_sorted, path->sorted_idx, path->idx);

	for (i = path->sorted_idx; i < trans->nr_sorted; i++)
		trans->paths[trans->sorted[i]].sorted_idx = i;

	btree_trans_verify_sorted_refs(trans);
}

void bch2_trans_iter_exit(struct btree_trans *trans, struct btree_iter *iter)
{
	if (iter->path)
		bch2_path_put(trans, iter->path,
			      iter->flags & BTREE_ITER_INTENT);
	if (iter->update_path)
		bch2_path_put(trans, iter->update_path,
			      iter->flags & BTREE_ITER_INTENT);
	if (iter->key_cache_path)
		bch2_path_put(trans, iter->key_cache_path,
			      iter->flags & BTREE_ITER_INTENT);
	iter->path = NULL;
	iter->update_path = NULL;
	iter->key_cache_path = NULL;
}

static void __bch2_trans_iter_init(struct btree_trans *trans,
				   struct btree_iter *iter,
				   unsigned btree_id, struct bpos pos,
				   unsigned locks_want,
				   unsigned depth,
				   unsigned flags,
				   unsigned long ip)
{
	EBUG_ON(trans->restarted);

	if (!(flags & (BTREE_ITER_ALL_SNAPSHOTS|BTREE_ITER_NOT_EXTENTS)) &&
	    btree_node_type_is_extents(btree_id))
		flags |= BTREE_ITER_IS_EXTENTS;

	if (!(flags & __BTREE_ITER_ALL_SNAPSHOTS) &&
	    !btree_type_has_snapshots(btree_id))
		flags &= ~BTREE_ITER_ALL_SNAPSHOTS;

	if (!(flags & BTREE_ITER_ALL_SNAPSHOTS) &&
	    btree_type_has_snapshots(btree_id))
		flags |= BTREE_ITER_FILTER_SNAPSHOTS;

	if (!test_bit(JOURNAL_REPLAY_DONE, &trans->c->journal.flags))
		flags |= BTREE_ITER_WITH_JOURNAL;

	if (!btree_id_cached(trans->c, btree_id)) {
		flags &= ~BTREE_ITER_CACHED;
		flags &= ~BTREE_ITER_WITH_KEY_CACHE;
	} else if (!(flags & BTREE_ITER_CACHED))
		flags |= BTREE_ITER_WITH_KEY_CACHE;

	iter->trans	= trans;
	iter->path	= NULL;
	iter->update_path = NULL;
	iter->key_cache_path = NULL;
	iter->btree_id	= btree_id;
	iter->min_depth	= depth;
	iter->flags	= flags;
	iter->snapshot	= pos.snapshot;
	iter->pos	= pos;
	iter->k.type	= KEY_TYPE_deleted;
	iter->k.p	= pos;
	iter->k.size	= 0;
#ifdef CONFIG_BCACHEFS_DEBUG
	iter->ip_allocated = ip;
#endif

	iter->path = bch2_path_get(trans, btree_id, iter->pos,
				   locks_want, depth, flags, ip);
}

void bch2_trans_iter_init(struct btree_trans *trans,
			  struct btree_iter *iter,
			  unsigned btree_id, struct bpos pos,
			  unsigned flags)
{
	__bch2_trans_iter_init(trans, iter, btree_id, pos,
			       0, 0, flags, _RET_IP_);
}

void bch2_trans_node_iter_init(struct btree_trans *trans,
			       struct btree_iter *iter,
			       enum btree_id btree_id,
			       struct bpos pos,
			       unsigned locks_want,
			       unsigned depth,
			       unsigned flags)
{
	__bch2_trans_iter_init(trans, iter, btree_id, pos, locks_want, depth,
			       BTREE_ITER_NOT_EXTENTS|
			       __BTREE_ITER_ALL_SNAPSHOTS|
			       BTREE_ITER_ALL_SNAPSHOTS|
			       flags, _RET_IP_);
	BUG_ON(iter->path->locks_want	 < min(locks_want, BTREE_MAX_DEPTH));
	BUG_ON(iter->path->level	!= depth);
	BUG_ON(iter->min_depth		!= depth);
}

void bch2_trans_copy_iter(struct btree_iter *dst, struct btree_iter *src)
{
	*dst = *src;
	if (src->path)
		__btree_path_get(src->path, src->flags & BTREE_ITER_INTENT);
	if (src->update_path)
		__btree_path_get(src->update_path, src->flags & BTREE_ITER_INTENT);
	dst->key_cache_path = NULL;
}

void *bch2_trans_kmalloc(struct btree_trans *trans, size_t size)
{
	size_t new_top = trans->mem_top + size;
	void *p;

	if (new_top > trans->mem_bytes) {
		size_t old_bytes = trans->mem_bytes;
		size_t new_bytes = roundup_pow_of_two(new_top);
		void *new_mem;

		WARN_ON_ONCE(new_bytes > BTREE_TRANS_MEM_MAX);

		new_mem = krealloc(trans->mem, new_bytes, GFP_NOFS);
		if (!new_mem && new_bytes <= BTREE_TRANS_MEM_MAX) {
			new_mem = mempool_alloc(&trans->c->btree_trans_mem_pool, GFP_KERNEL);
			new_bytes = BTREE_TRANS_MEM_MAX;
			kfree(trans->mem);
		}

		if (!new_mem)
			return ERR_PTR(-ENOMEM);

		trans->mem = new_mem;
		trans->mem_bytes = new_bytes;

		if (old_bytes) {
			trace_trans_restart_mem_realloced(trans->fn, _RET_IP_, new_bytes);
			btree_trans_restart(trans);
			return ERR_PTR(-EINTR);
		}
	}

	p = trans->mem + trans->mem_top;
	trans->mem_top += size;
	memset(p, 0, size);
	return p;
}

/**
 * bch2_trans_begin() - reset a transaction after a interrupted attempt
 * @trans: transaction to reset
 *
 * While iterating over nodes or updating nodes a attempt to lock a btree
 * node may return EINTR when the trylock fails. When this occurs
 * bch2_trans_begin() should be called and the transaction retried.
 */
void bch2_trans_begin(struct btree_trans *trans)
{
	struct btree_insert_entry *i;
	struct btree_path *path;

	trans_for_each_update(trans, i)
		__btree_path_put(i->path, true);

	memset(&trans->journal_res, 0, sizeof(trans->journal_res));
	trans->extra_journal_res	= 0;
	trans->nr_updates		= 0;
	trans->mem_top			= 0;

	trans->hooks			= NULL;
	trans->extra_journal_entries.nr	= 0;

	if (trans->fs_usage_deltas) {
		trans->fs_usage_deltas->used = 0;
		memset(&trans->fs_usage_deltas->memset_start, 0,
		       (void *) &trans->fs_usage_deltas->memset_end -
		       (void *) &trans->fs_usage_deltas->memset_start);
	}

	trans_for_each_path(trans, path) {
		path->should_be_locked = false;

		/*
		 * If the transaction wasn't restarted, we're presuming to be
		 * doing something new: dont keep iterators excpt the ones that
		 * are in use - except for the subvolumes btree:
		 */
		if (!trans->restarted && path->btree_id != BTREE_ID_subvolumes)
			path->preserve = false;

		/*
		 * XXX: we probably shouldn't be doing this if the transaction
		 * was restarted, but currently we still overflow transaction
		 * iterators if we do that
		 */
		if (!path->ref && !path->preserve)
			__bch2_path_free(trans, path);
		else
			path->preserve = false;
	}

	bch2_trans_cond_resched(trans);

	if (trans->restarted)
		bch2_btree_path_traverse_all(trans);

	trans->restarted = false;
}

static void bch2_trans_alloc_paths(struct btree_trans *trans, struct bch_fs *c)
{
	size_t paths_bytes	= sizeof(struct btree_path) * BTREE_ITER_MAX;
	size_t updates_bytes	= sizeof(struct btree_insert_entry) * BTREE_ITER_MAX;
	void *p = NULL;

	BUG_ON(trans->used_mempool);

#ifdef __KERNEL__
	p = this_cpu_xchg(c->btree_paths_bufs->path , NULL);
#endif
	if (!p)
		p = mempool_alloc(&trans->c->btree_paths_pool, GFP_NOFS);

	trans->paths		= p; p += paths_bytes;
	trans->updates		= p; p += updates_bytes;
}

void __bch2_trans_init(struct btree_trans *trans, struct bch_fs *c,
		       unsigned expected_nr_iters,
		       size_t expected_mem_bytes,
		       const char *fn)
	__acquires(&c->btree_trans_barrier)
{
	BUG_ON(lock_class_is_held(&bch2_btree_node_lock_key));

	memset(trans, 0, sizeof(*trans));
	trans->c		= c;
	trans->fn		= fn;

	bch2_trans_alloc_paths(trans, c);

	if (expected_mem_bytes) {
		trans->mem_bytes = roundup_pow_of_two(expected_mem_bytes);
		trans->mem = kmalloc(trans->mem_bytes, GFP_KERNEL|__GFP_NOFAIL);

		if (!unlikely(trans->mem)) {
			trans->mem = mempool_alloc(&c->btree_trans_mem_pool, GFP_KERNEL);
			trans->mem_bytes = BTREE_TRANS_MEM_MAX;
		}
	}

	trans->srcu_idx = srcu_read_lock(&c->btree_trans_barrier);

	trans->pid = current->pid;
	mutex_lock(&c->btree_trans_lock);
	list_add(&trans->list, &c->btree_trans_list);
	mutex_unlock(&c->btree_trans_lock);
}

static void check_btree_paths_leaked(struct btree_trans *trans)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	struct bch_fs *c = trans->c;
	struct btree_path *path;

	trans_for_each_path(trans, path)
		if (path->ref)
			goto leaked;
	return;
leaked:
	bch_err(c, "btree paths leaked from %s!", trans->fn);
	trans_for_each_path(trans, path)
		if (path->ref)
			printk(KERN_ERR "  btree %s %pS\n",
			       bch2_btree_ids[path->btree_id],
			       (void *) path->ip_allocated);
	/* Be noisy about this: */
	bch2_fatal_error(c);
#endif
}

void bch2_trans_exit(struct btree_trans *trans)
	__releases(&c->btree_trans_barrier)
{
	struct btree_insert_entry *i;
	struct bch_fs *c = trans->c;

	bch2_trans_unlock(trans);

	trans_for_each_update(trans, i)
		__btree_path_put(i->path, true);
	trans->nr_updates		= 0;

	check_btree_paths_leaked(trans);

	mutex_lock(&c->btree_trans_lock);
	list_del(&trans->list);
	mutex_unlock(&c->btree_trans_lock);

	srcu_read_unlock(&c->btree_trans_barrier, trans->srcu_idx);

	bch2_journal_preres_put(&c->journal, &trans->journal_preres);

	kfree(trans->extra_journal_entries.data);

	if (trans->fs_usage_deltas) {
		if (trans->fs_usage_deltas->size + sizeof(trans->fs_usage_deltas) ==
		    REPLICAS_DELTA_LIST_MAX)
			mempool_free(trans->fs_usage_deltas,
				     &c->replicas_delta_pool);
		else
			kfree(trans->fs_usage_deltas);
	}

	if (trans->mem_bytes == BTREE_TRANS_MEM_MAX)
		mempool_free(trans->mem, &c->btree_trans_mem_pool);
	else
		kfree(trans->mem);

#ifdef __KERNEL__
	/*
	 * Userspace doesn't have a real percpu implementation:
	 */
	trans->paths = this_cpu_xchg(c->btree_paths_bufs->path, trans->paths);
#endif

	if (trans->paths)
		mempool_free(trans->paths, &c->btree_paths_pool);

	trans->mem	= (void *) 0x1;
	trans->paths	= (void *) 0x1;
}

static void __maybe_unused
bch2_btree_path_node_to_text(struct printbuf *out,
			     struct btree_bkey_cached_common *_b,
			     bool cached)
{
	pr_buf(out, "    l=%u %s:",
	       _b->level, bch2_btree_ids[_b->btree_id]);
	bch2_bpos_to_text(out, btree_node_pos(_b, cached));
}

static bool trans_has_locks(struct btree_trans *trans)
{
	struct btree_path *path;

	trans_for_each_path(trans, path)
		if (path->nodes_locked)
			return true;
	return false;
}

void bch2_btree_trans_to_text(struct printbuf *out, struct bch_fs *c)
{
	struct btree_trans *trans;
	struct btree_path *path;
	struct btree *b;
	static char lock_types[] = { 'r', 'i', 'w' };
	unsigned l;

	mutex_lock(&c->btree_trans_lock);
	list_for_each_entry(trans, &c->btree_trans_list, list) {
		if (!trans_has_locks(trans))
			continue;

		pr_buf(out, "%i %s\n", trans->pid, trans->fn);

		trans_for_each_path(trans, path) {
			if (!path->nodes_locked)
				continue;

			pr_buf(out, "  path %u %c l=%u %s:",
			       path->idx,
			       path->cached ? 'c' : 'b',
			       path->level,
			       bch2_btree_ids[path->btree_id]);
			bch2_bpos_to_text(out, path->pos);
			pr_buf(out, "\n");

			for (l = 0; l < BTREE_MAX_DEPTH; l++) {
				if (btree_node_locked(path, l)) {
					pr_buf(out, "    %s l=%u ",
					       btree_node_intent_locked(path, l) ? "i" : "r", l);
					bch2_btree_path_node_to_text(out,
							(void *) path->l[l].b,
							path->cached);
					pr_buf(out, "\n");
				}
			}
		}

		b = READ_ONCE(trans->locking);
		if (b) {
			path = &trans->paths[trans->locking_path_idx];
			pr_buf(out, "  locking path %u %c l=%u %c %s:",
			       trans->locking_path_idx,
			       path->cached ? 'c' : 'b',
			       trans->locking_level,
			       lock_types[trans->locking_lock_type],
			       bch2_btree_ids[trans->locking_btree_id]);
			bch2_bpos_to_text(out, trans->locking_pos);

			pr_buf(out, " node ");
			bch2_btree_path_node_to_text(out,
					(void *) b, path->cached);
			pr_buf(out, "\n");
		}
	}
	mutex_unlock(&c->btree_trans_lock);
}

void bch2_fs_btree_iter_exit(struct bch_fs *c)
{
	if (c->btree_trans_barrier_initialized)
		cleanup_srcu_struct(&c->btree_trans_barrier);
	mempool_exit(&c->btree_trans_mem_pool);
	mempool_exit(&c->btree_paths_pool);
}

int bch2_fs_btree_iter_init(struct bch_fs *c)
{
	unsigned nr = BTREE_ITER_MAX;
	int ret;

	INIT_LIST_HEAD(&c->btree_trans_list);
	mutex_init(&c->btree_trans_lock);

	ret   = mempool_init_kmalloc_pool(&c->btree_paths_pool, 1,
			sizeof(struct btree_path) * nr +
			sizeof(struct btree_insert_entry) * nr) ?:
		mempool_init_kmalloc_pool(&c->btree_trans_mem_pool, 1,
					  BTREE_TRANS_MEM_MAX) ?:
		init_srcu_struct(&c->btree_trans_barrier);
	if (!ret)
		c->btree_trans_barrier_initialized = true;
	return ret;
}
