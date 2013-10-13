/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheep_priv.h"

/* base structure for the recovery thread */
struct recovery_work {
	uint32_t epoch;
	uint32_t tgt_epoch;

	struct vnode_info *old_vinfo;
	struct vnode_info *cur_vinfo;

	struct work work;
};

/* for preparing lists */
struct recovery_list_work {
	struct recovery_work base;

	uint64_t count;
	uint64_t *oids;
};

/* for recoverying objects */
struct recovery_obj_work {
	struct recovery_work base;

	uint64_t oid; /* the object to be recovered */
	bool stop;

	/* local replica in the stale directory */
	uint32_t local_epoch;
	uint8_t local_sha1[SHA1_DIGEST_SIZE];
};

/*
 * recovery information
 *
 * We cannot access the members of this structure outside of the main thread.
 */
struct recovery_info {
	enum rw_state state;

	uint32_t epoch;
	uint32_t tgt_epoch;
	uint64_t done;

	/*
	 * true when automatic recovery is disabled
	 * and no recovery work is running
	 */
	bool suspended;
	bool notify_complete;

	uint64_t count;
	uint64_t *oids;
	uint64_t *prio_oids;
	uint64_t nr_prio_oids;
	uint64_t nr_scheduled_prio_oids;

	struct vnode_info *old_vinfo;
	struct vnode_info *cur_vinfo;
};

static struct recovery_info *next_rinfo;
static main_thread(struct recovery_info *) current_rinfo;

static void queue_recovery_work(struct recovery_info *rinfo);

/* Dynamically grown list buffer default as 4M (2T storage) */
#define DEFAULT_LIST_BUFFER_SIZE (UINT64_C(1) << 22)
static size_t list_buffer_size = DEFAULT_LIST_BUFFER_SIZE;

static int obj_cmp(const uint64_t *oid1, const uint64_t *oid2)
{
	const uint64_t hval1 = sd_hash_oid(*oid1);
	const uint64_t hval2 = sd_hash_oid(*oid2);

	return intcmp(hval1, hval2);
}

static inline bool node_is_gateway_only(void)
{
	return sys->this_node.nr_vnodes == 0;
}

static void *read_erasure_object(const struct sd_node *node, uint64_t oid,
				 uint32_t epoch, uint32_t tgt_epoch,
				 uint8_t idx)
{
	struct sd_req hdr;
	unsigned rlen = get_store_objsize(oid);
	void *buf = xvalloc(rlen);
	int ret;

	sd_init_req(&hdr, SD_OP_READ_PEER);
	hdr.epoch = epoch;
	hdr.flags = SD_FLAG_CMD_RECOVERY;
	hdr.data_length = rlen;
	hdr.obj.oid = oid;
	hdr.obj.tgt_epoch = tgt_epoch;
	hdr.obj.ec_index = idx;

	sd_debug("%s, epoch %"PRIu32" tgt %"PRIu32" idx %d", node_to_str(node),
		 epoch, tgt_epoch, idx);
	ret = sheep_exec_req(&node->nid, &hdr, buf);
	if (ret != SD_RES_SUCCESS) {
		free(buf);
		return NULL;
	}
	return buf;
}

/*
 * Read object from targeted node and store it in the local node.
 *
 * tgt_epoch: the specific epoch that the object has stayed
 * idx: erasure index. For non-erasure object, pass 0.
 */
static int recover_object_from(struct recovery_obj_work *row,
			       const struct sd_node *node,
			       uint32_t tgt_epoch)
{
	uint64_t oid = row->oid;
	uint32_t local_epoch = row->local_epoch;
	uint8_t *sha1 = row->local_sha1;
	uint32_t epoch = row->base.epoch;
	int ret;
	unsigned rlen;
	void *buf = NULL;
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	struct siocb iocb = { 0 };

	if (node_is_local(node)) {
		if (tgt_epoch < sys_epoch())
			return sd_store->link(oid, tgt_epoch);

		return SD_RES_NO_OBJ;
	}

	/* compare sha1 hash value first */
	if (local_epoch > 0) {
		sd_init_req(&hdr, SD_OP_GET_HASH);
		hdr.obj.oid = oid;
		hdr.obj.tgt_epoch = tgt_epoch;
		ret = sheep_exec_req(&node->nid, &hdr, NULL);
		if (ret != SD_RES_SUCCESS)
			return ret;

		if (memcmp(rsp->hash.digest, sha1,
			   sizeof(SHA1_DIGEST_SIZE)) == 0) {
			sd_debug("use local replica at epoch %d", local_epoch);
			ret = sd_store->link(oid, local_epoch);
			if (ret == SD_RES_SUCCESS)
				return ret;
		}
	}

	rlen = get_store_objsize(oid);
	buf = xvalloc(rlen);

	/* recover from remote replica */
	sd_init_req(&hdr, SD_OP_READ_PEER);
	hdr.epoch = epoch;
	hdr.flags = SD_FLAG_CMD_RECOVERY;
	hdr.data_length = rlen;
	hdr.obj.oid = oid;
	hdr.obj.tgt_epoch = tgt_epoch;

	ret = sheep_exec_req(&node->nid, &hdr, buf);
	if (ret == SD_RES_SUCCESS) {
		iocb.epoch = epoch;
		iocb.length = rsp->data_length;
		iocb.offset = rsp->obj.offset;
		iocb.buf = buf;
		ret = sd_store->create_and_write(oid, &iocb);
	}

	free(buf);
	return ret;
}

/*
 * A node that does not match any node in current node list means the node has
 * left the cluster, then it's an invalid node.
 */
static bool invalid_node(const struct sd_node *n, struct vnode_info *info)
{

	if (rb_search(&info->nroot, n, rb, node_cmp))
		return false;
	return true;
}

static int recover_object_from_replica(struct recovery_obj_work *row,
				       struct vnode_info *old,
				       uint32_t tgt_epoch)
{
	uint64_t oid = row->oid;
	uint32_t epoch = row->base.epoch;
	int nr_copies, ret = SD_RES_SUCCESS, start = 0;
	bool fully_replicated = true;

	nr_copies = get_obj_copy_number(oid, old->nr_zones);

	/* find local node first to try to recover from local */
	for (int i = 0; i < nr_copies; i++) {
		const struct sd_vnode *vnode;

		vnode = oid_to_vnode(oid, &old->vroot, i);

		if (vnode_is_local(vnode)) {
			start = i;
			break;
		}
	}

	/* Let's do a breadth-first search */
	for (int i = 0; i < nr_copies; i++) {
		const struct sd_node *node;
		int idx = (i + start) % nr_copies;

		node = oid_to_node(oid, &old->vroot, idx);

		if (invalid_node(node, row->base.cur_vinfo))
			continue;

		ret = recover_object_from(row, node, tgt_epoch);
		switch (ret) {
		case SD_RES_SUCCESS:
			sd_debug("recovered oid %"PRIx64" from %d to epoch %d",
				 oid, tgt_epoch, epoch);
			return ret;
		case SD_RES_OLD_NODE_VER:
			/* move to the next epoch recovery */
			return ret;
		case SD_RES_NO_OBJ:
			fully_replicated = false;
			/* fall through */
		default:
			break;
		}
	}

	/*
	 * sheep would return a stale object when
	 *  - all the nodes hold the copies, and
	 *  - all the nodes are gone
	 * at the some epoch
	 */
	if (fully_replicated && ret != SD_RES_SUCCESS)
		ret = SD_RES_STALE_OBJ;

	return ret;
}

/*
 * Recover the object from its track in epoch history. That is,
 * the routine will try to recovery it from the nodes it has stayed,
 * at least, *theoretically* on consistent hash ring.
 */
static int recover_replication_object(struct recovery_obj_work *row)
{
	struct recovery_work *rw = &row->base;
	struct vnode_info *old;
	uint64_t oid = row->oid;
	uint32_t tgt_epoch = rw->tgt_epoch;
	int ret;
	struct vnode_info *new_old;

	old = grab_vnode_info(rw->old_vinfo);
again:
	sd_debug("try recover object %"PRIx64" from epoch %"PRIu32, oid,
		 tgt_epoch);

	ret = recover_object_from_replica(row, old, tgt_epoch);

	switch (ret) {
	case SD_RES_SUCCESS:
		/* Succeed */
		break;
	case SD_RES_OLD_NODE_VER:
		row->stop = true;
		break;
	case SD_RES_STALE_OBJ:
		sd_alert("cannot access any replicas of %"PRIx64" at epoch %d",
			 oid, tgt_epoch);
		sd_alert("clients may see old data");
		/* fall through */
	default:
		/* No luck, roll back to an older configuration and try again */
rollback:
		tgt_epoch--;
		if (tgt_epoch < 1) {
			sd_err("can not recover oid %"PRIx64, oid);
			ret = -1;
			break;
		}

		new_old = get_vnode_info_epoch(tgt_epoch, rw->cur_vinfo);
		if (!new_old) {
			/* We rollback in case we don't get a valid epoch */
			sd_alert("cannot get epoch %d", tgt_epoch);
			sd_alert("clients may see old data");
			goto rollback;
		}

		put_vnode_info(old);
		old = new_old;
		goto again;
	}

	put_vnode_info(old);
	return ret;
}

static void *rebuild_erasure_object(struct recovery_obj_work *row,
				    uint32_t tgt_epoch, const uint8_t idx)
{
	struct vnode_info *old = grab_vnode_info(row->base.old_vinfo);
	const struct sd_node *target_nodes[SD_MAX_NODES];
	uint8_t *bufs[SD_EC_D] = { 0 };
	uint64_t oid = row->oid;
	uint32_t epoch = row->base.epoch;
	int idxs[SD_EC_D], len = get_store_objsize(oid);
	struct fec *ctx = ec_init();
	char *lost = xvalloc(len);
	int i, j;

	/* Prepare replica */
	oid_to_nodes(oid, &old->vroot, SD_EC_DP, target_nodes);
	for (i = 0, j = 0; i < SD_EC_DP && j < SD_EC_D; i++) {
		if (i == idx)
			continue;
		bufs[j] = read_erasure_object(target_nodes[i], oid,
					      epoch, tgt_epoch, i);
		if (!bufs[j])
			continue;
		idxs[j++] = i;
	}
	if (j != SD_EC_D) {
		free(lost);
		lost = NULL;
		goto out;
	}

	/* Rebuild the lost replica */
	for (i = 0; i < SD_EC_NR_STRIPE_PER_OBJECT; i++) {
		const uint8_t *in[SD_EC_D];
		uint8_t out[SD_EC_STRIP_SIZE];

		for (j = 0; j < SD_EC_D; j++)
			in[j] = bufs[j] + SD_EC_STRIP_SIZE * i;
		ec_decode(ctx, in, idxs, out, idx);
		memcpy(lost + SD_EC_STRIP_SIZE * i, out, SD_EC_STRIP_SIZE);
	}
out:
	ec_destroy(ctx);
	put_vnode_info(old);
	for (i = 0; i < SD_EC_D; i++)
		free(bufs[i]);
	return lost;
}

static uint8_t local_node_copy_index(struct rb_root *vroot, uint64_t oid)
{
	const struct sd_node *target_nodes[SD_MAX_NODES];
	uint8_t idx;

	oid_to_nodes(oid, vroot, SD_EC_DP, target_nodes);
	for (idx = 0; idx < SD_EC_DP; idx++)
		if (node_is_local(target_nodes[idx]))
			return idx;
	panic("can't get valid index for %"PRIx64, oid);
}

static int recover_erasure_object(struct recovery_obj_work *row)
{
	struct recovery_work *rw = &row->base;
	struct vnode_info *old, *cur;
	uint64_t oid = row->oid;
	uint32_t tgt_epoch = rw->tgt_epoch, epoch = rw->epoch;
	const struct sd_node *node;
	struct siocb iocb = { 0 };
	void *buf;
	uint8_t idx;
	int ret = -1;

	cur = grab_vnode_info(rw->cur_vinfo);
	old = grab_vnode_info(rw->old_vinfo);

	idx = local_node_copy_index(&cur->vroot, oid);
	node = oid_to_node(oid, &old->vroot, idx);

	sd_debug("%"PRIx64" idx %d, from epoch %"PRIu32, oid, idx, tgt_epoch);

	if (invalid_node(node, cur))
		buf = rebuild_erasure_object(row, tgt_epoch, idx);
	else
		buf = read_erasure_object(node, oid, epoch, tgt_epoch, idx);
	if (!buf)
		goto out;

	iocb.epoch = epoch;
	iocb.length = get_store_objsize(oid);
	iocb.offset = 0;
	iocb.buf = buf;
	iocb.ec_index = idx;
	ret = sd_store->create_and_write(oid, &iocb);
	free(buf);
out:
	put_vnode_info(cur);
	put_vnode_info(old);
	return ret;
}

static int do_recover_object(struct recovery_obj_work *row)
{
	uint64_t oid = row->oid;

	if (is_erasure_oid(oid))
		return recover_erasure_object(row);
	else
		return recover_replication_object(row);
}

static void recover_object_work(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	struct recovery_obj_work *row = container_of(rw,
						     struct recovery_obj_work,
						     base);
	uint64_t oid = row->oid;
	int ret, epoch;

	if (sd_store->exist(oid)) {
		sd_debug("the object is already recovered");
		return;
	}

	/* find object in the stale directory */
	if (!is_erasure_oid(oid))
		for (epoch = sys_epoch() - 1; epoch > 0; epoch--) {
			ret = sd_store->get_hash(oid, epoch, row->local_sha1);
			if (ret == SD_RES_SUCCESS) {
				sd_debug("replica found in local at epoch %d",
					 epoch);
				row->local_epoch = epoch;
				break;
			}
		}

	ret = do_recover_object(row);
	if (ret != 0)
		sd_err("failed to recover object %"PRIx64, oid);
}

bool node_in_recovery(void)
{
	return main_thread_get(current_rinfo) != NULL;
}

static inline void prepare_schedule_oid(uint64_t oid)
{
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	if (xlfind(&oid, rinfo->prio_oids, rinfo->nr_prio_oids, oid_cmp)) {
		sd_debug("%" PRIx64 " has been already in prio_oids", oid);
		return;
	}

	rinfo->nr_prio_oids++;
	rinfo->prio_oids = xrealloc(rinfo->prio_oids,
				    rinfo->nr_prio_oids * sizeof(uint64_t));
	rinfo->prio_oids[rinfo->nr_prio_oids - 1] = oid;
	sd_debug("%"PRIx64" nr_prio_oids %"PRIu64, oid, rinfo->nr_prio_oids);

	resume_suspended_recovery();
}

main_fn bool oid_in_recovery(uint64_t oid)
{
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	if (!node_in_recovery())
		return false;

	if (sd_store->exist(oid)) {
		sd_debug("the object %" PRIx64 " is already recoverd", oid);
		return false;
	}

	if (uatomic_read(&next_rinfo))
		/*
		 * The current recovery_info will be taken over by the next one
		 * soon, so no need to call prepare_schedule_oid() now.
		 */
		return true;

	switch (rinfo->state) {
	case RW_PREPARE_LIST:
		/* oid is not recovered yet */
		break;
	case RW_RECOVER_OBJ:
		if (xlfind(&oid, rinfo->oids, rinfo->done, oid_cmp)) {
			sd_debug("%" PRIx64 " has been already recovered", oid);
			return false;
		}

		if (rinfo->oids[rinfo->done] == oid) {
			if (rinfo->suspended)
				break;
			/*
			 * When recovery is not suspended,
			 * rinfo->oids[rinfo->done] is currently being recovered
			 * and no need to call prepare_schedule_oid().
			 */
			return true;
		}

		/*
		 * Check if oid is in the list that to be recovered later
		 *
		 * FIXME: do we need more efficient yet complex data structure?
		 */
		if (xlfind(&oid, rinfo->oids + rinfo->done + 1, rinfo->count,
			   oid_cmp))
			break;

		/*
		 * Newly created object after prepare_object_list() might not be
		 * in the list
		 */
		sd_debug("%"PRIx64" is not in the recovery list", oid);
		return false;
	case RW_NOTIFY_COMPLETION:
		sd_debug("the object %" PRIx64 " is already recoverd", oid);
		return false;
	}

	prepare_schedule_oid(oid);
	return true;
}

static void free_recovery_work(struct recovery_work *rw)
{
	put_vnode_info(rw->cur_vinfo);
	put_vnode_info(rw->old_vinfo);
	free(rw);
}

static void free_recovery_list_work(struct recovery_list_work *rlw)
{
	put_vnode_info(rlw->base.cur_vinfo);
	put_vnode_info(rlw->base.old_vinfo);
	free(rlw->oids);
	free(rlw);
}

static void free_recovery_obj_work(struct recovery_obj_work *row)
{
	put_vnode_info(row->base.cur_vinfo);
	put_vnode_info(row->base.old_vinfo);
	free(row);
}

static void free_recovery_info(struct recovery_info *rinfo)
{
	put_vnode_info(rinfo->cur_vinfo);
	put_vnode_info(rinfo->old_vinfo);
	free(rinfo->oids);
	free(rinfo->prio_oids);
	free(rinfo);
}

/* Return true if next recovery work is queued. */
static inline bool run_next_rw(void)
{
	struct recovery_info *nrinfo = uatomic_xchg_ptr(&next_rinfo, NULL);
	struct recovery_info *cur = main_thread_get(current_rinfo);

	if (nrinfo == NULL)
		return false;

	/*
	 * When md recovery supersed the reweight or node recovery, we need to
	 * notify completion.
	 */
	if (!nrinfo->notify_complete && cur->notify_complete)
		nrinfo->notify_complete = true;

	free_recovery_info(cur);

	if (!node_is_gateway_only())
		sd_store->update_epoch(nrinfo->tgt_epoch);

	main_thread_set(current_rinfo, nrinfo);
	wakeup_all_requests();
	queue_recovery_work(nrinfo);
	sd_debug("recovery work is superseded");
	return true;
}

static void notify_recovery_completion_work(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	struct sd_req hdr;
	int ret;

	sd_init_req(&hdr, SD_OP_COMPLETE_RECOVERY);
	hdr.obj.tgt_epoch = rw->epoch;
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = sizeof(sys->this_node);

	ret = exec_local_req(&hdr, &sys->this_node);
	if (ret != SD_RES_SUCCESS)
		sd_err("failed to notify recovery completion, %d", rw->epoch);
}

static void notify_recovery_completion_main(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	free_recovery_work(rw);
}

static inline void finish_recovery(struct recovery_info *rinfo)
{
	uint32_t recovered_epoch = rinfo->epoch;
	main_thread_set(current_rinfo, NULL);

	wakeup_all_requests();

	if (rinfo->notify_complete) {
		rinfo->state = RW_NOTIFY_COMPLETION;
		queue_recovery_work(rinfo);
	}

	free_recovery_info(rinfo);

	sd_debug("recovery complete: new epoch %"PRIu32, recovered_epoch);
}

static inline bool oid_in_prio_oids(struct recovery_info *rinfo, uint64_t oid)
{
	for (uint64_t i = 0; i < rinfo->nr_prio_oids; i++)
		if (rinfo->prio_oids[i] == oid)
			return true;
	return false;
}

/*
 * Schedule prio_oids to be recovered first in FIFO order
 *
 * rw->done is index of the original next object to be recovered and also the
 * number of objects already recovered.
 * we just move rw->prio_oids in between:
 *   new_oids = [0..rw->done - 1] + [rw->prio_oids] + [rw->done]
 */
static inline void finish_schedule_oids(struct recovery_info *rinfo)
{
	uint64_t i, nr_recovered = rinfo->done, new_idx;
	uint64_t *new_oids;

	/* If I am the last oid, done */
	if (nr_recovered == rinfo->count - 1)
		goto done;

	new_oids = xmalloc(list_buffer_size);
	memcpy(new_oids, rinfo->oids, nr_recovered * sizeof(uint64_t));
	memcpy(new_oids + nr_recovered, rinfo->prio_oids,
	       rinfo->nr_prio_oids * sizeof(uint64_t));
	new_idx = nr_recovered + rinfo->nr_prio_oids;

	for (i = rinfo->done; i < rinfo->count; i++) {
		if (oid_in_prio_oids(rinfo, rinfo->oids[i]))
			continue;
		new_oids[new_idx++] = rinfo->oids[i];
	}
	/* rw->count should eq new_idx, otherwise something is wrong */
	sd_debug("%snr_recovered %" PRIu64 ", nr_prio_oids %" PRIu64 ", count %"
		 PRIu64 " = new %" PRIu64,
		 rinfo->count == new_idx ? "" : "WARN: ", nr_recovered,
		 rinfo->nr_prio_oids, rinfo->count, new_idx);

	free(rinfo->oids);
	rinfo->oids = new_oids;
done:
	free(rinfo->prio_oids);
	rinfo->prio_oids = NULL;
	rinfo->nr_scheduled_prio_oids += rinfo->nr_prio_oids;
	rinfo->nr_prio_oids = 0;
}

/*
 * When automatic object recovery is disabled, the behavior of the
 * recovery process is like 'lazy recovery'.  This function returns
 * true if the recovery queue contains objects being accessed by
 * clients.  Sheep recovers such objects for availability even when
 * automatic object recovery is not enabled.
 */
static bool has_scheduled_objects(struct recovery_info *rinfo)
{
	return rinfo->done < rinfo->nr_scheduled_prio_oids;
}

static void recover_next_object(struct recovery_info *rinfo)
{
	if (run_next_rw())
		return;

	if (rinfo->nr_prio_oids)
		finish_schedule_oids(rinfo);

	if (sys->cinfo.disable_recovery && !has_scheduled_objects(rinfo)) {
		sd_debug("suspended");
		rinfo->suspended = true;
		/* suspend until resume_suspended_recovery() is called */
		return;
	}

	/* Try recover next object */
	queue_recovery_work(rinfo);
}

void resume_suspended_recovery(void)
{
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	if (rinfo && rinfo->suspended) {
		rinfo->suspended = false;
		recover_next_object(rinfo);
	}
}

static void recover_object_main(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	struct recovery_obj_work *row = container_of(rw,
						     struct recovery_obj_work,
						     base);
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	if (run_next_rw())
		goto out;

	if (row->stop) {
		/*
		 * Stop this recovery process and wait for epoch to be
		 * lifted and flush wait queue to requeue those
		 * requests
		 */
		rinfo->notify_complete = false;
		finish_recovery(rinfo);
		sd_debug("recovery is stopped");
		goto out;
	}

	wakeup_requests_on_oid(row->oid);
	rinfo->done++;

	sd_info("object %"PRIx64" is recovered (%"PRIu64"/%"PRIu64")", row->oid,
		rinfo->done, rinfo->count);

	if (rinfo->done < rinfo->count) {
		recover_next_object(rinfo);
		goto out;
	}

	finish_recovery(rinfo);
out:
	free_recovery_obj_work(row);
}

static void finish_object_list(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	struct recovery_list_work *rlw = container_of(rw,
						      struct recovery_list_work,
						      base);
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	rinfo->state = RW_RECOVER_OBJ;
	rinfo->count = rlw->count;
	rinfo->oids = rlw->oids;
	rlw->oids = NULL;
	free_recovery_list_work(rlw);

	if (run_next_rw())
		return;

	if (!rinfo->count) {
		finish_recovery(rinfo);
		return;
	}

	recover_next_object(rinfo);
	return;
}

/* Fetch the object list from all the nodes in the cluster */
static uint64_t *fetch_object_list(struct sd_node *e, uint32_t epoch,
				   size_t *nr_oids)
{
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	size_t buf_size = list_buffer_size;
	uint64_t *buf = xmalloc(buf_size);
	int ret;

	sd_debug("%s", addr_to_str(e->nid.addr, e->nid.port));

retry:
	sd_init_req(&hdr, SD_OP_GET_OBJ_LIST);
	hdr.data_length = buf_size;
	hdr.epoch = epoch;
	ret = sheep_exec_req(&e->nid, &hdr, buf);

	switch (ret) {
	case SD_RES_SUCCESS:
		break;
	case SD_RES_BUFFER_SMALL:
		buf_size *= 2;
		buf = xrealloc(buf, buf_size);
		goto retry;
	default:
		sd_alert("cannot get object list from %s",
			 addr_to_str(e->nid.addr, e->nid.port));
		sd_alert("some objects may be not recovered at epoch %d",
			 epoch);
		free(buf);
		return NULL;
	}

	*nr_oids = rsp->data_length / sizeof(uint64_t);
	sd_debug("%zu", *nr_oids);
	return buf;
}

/* Screen out objects that don't belong to this node */
static void screen_object_list(struct recovery_list_work *rlw,
			       uint64_t *oids, size_t nr_oids)
{
	struct recovery_work *rw = &rlw->base;
	const struct sd_vnode *vnodes[SD_MAX_COPIES];
	uint64_t old_count = rlw->count;
	uint64_t nr_objs;
	uint64_t i, j;

	for (i = 0; i < nr_oids; i++) {
		if (xbsearch(&oids[i], rlw->oids, old_count, obj_cmp))
			/* the object is already scheduled to be recovered */
			continue;

		nr_objs = get_obj_copy_number(oids[i], rw->cur_vinfo->nr_zones);

		oid_to_vnodes(oids[i], &rw->cur_vinfo->vroot, nr_objs, vnodes);
		for (j = 0; j < nr_objs; j++) {
			if (!vnode_is_local(vnodes[j]))
				continue;

			rlw->oids[rlw->count++] = oids[i];
			/* enlarge the list buffer if full */
			if (rlw->count == list_buffer_size / sizeof(uint64_t)) {
				list_buffer_size *= 2;
				rlw->oids = xrealloc(rlw->oids,
						     list_buffer_size);
			}
			break;
		}
	}

	xqsort(rlw->oids, rlw->count, obj_cmp);
}

/* Prepare the object list that belongs to this node */
static void prepare_object_list(struct work *work)
{
	struct recovery_work *rw = container_of(work, struct recovery_work,
						work);
	struct recovery_list_work *rlw = container_of(rw,
						      struct recovery_list_work,
						      base);
	int nr_nodes = rw->cur_vinfo->nr_nodes;
	int start = random() % nr_nodes, i, end = nr_nodes;
	uint64_t *oids;
	struct sd_node *nodes;

	if (node_is_gateway_only())
		return;

	sd_debug("%u", rw->epoch);
	wait_get_vdis_done();

	nodes = xmalloc(sizeof(struct sd_node) * nr_nodes);
	nodes_to_buffer(&rw->cur_vinfo->nroot, nodes);
again:
	/* We need to start at random node for better load balance */
	for (i = start; i < end; i++) {
		size_t nr_oids;
		struct sd_node *node = nodes + i;

		if (uatomic_read(&next_rinfo)) {
			sd_debug("go to the next recovery");
			goto out;
		}

		oids = fetch_object_list(node, rw->epoch, &nr_oids);
		if (!oids)
			continue;
		screen_object_list(rlw, oids, nr_oids);
		free(oids);
	}

	if (start != 0) {
		end = start;
		start = 0;
		goto again;
	}

	sd_debug("%"PRIu64, rlw->count);
out:
	free(nodes);
}

int start_recovery(struct vnode_info *cur_vinfo, struct vnode_info *old_vinfo,
		   bool epoch_lifted)
{
	struct recovery_info *rinfo;

	rinfo = xzalloc(sizeof(struct recovery_info));
	rinfo->state = RW_PREPARE_LIST;
	rinfo->epoch = sys->cinfo.epoch;
	rinfo->tgt_epoch = epoch_lifted ? sys->cinfo.epoch - 1 :
		sys->cinfo.epoch;
	rinfo->count = 0;
	if (epoch_lifted)
		rinfo->notify_complete = true; /* Reweight or node recovery */
	else
		rinfo->notify_complete = false; /* MD recovery */

	rinfo->cur_vinfo = grab_vnode_info(cur_vinfo);
	rinfo->old_vinfo = grab_vnode_info(old_vinfo);

	if (!node_is_gateway_only())
		sd_store->update_epoch(rinfo->tgt_epoch);

	if (main_thread_get(current_rinfo) != NULL) {
		/* skip the previous epoch recovery */
		struct recovery_info *nrinfo;
		nrinfo = uatomic_xchg_ptr(&next_rinfo, rinfo);
		if (nrinfo)
			free_recovery_info(nrinfo);
		sd_debug("recovery skipped");

		/*
		 * This is necesary to invoke run_next_rw when
		 * recovery work is suspended.
		 */
		resume_suspended_recovery();
	} else {
		main_thread_set(current_rinfo, rinfo);
		queue_recovery_work(rinfo);
	}
	wakeup_requests_on_epoch();
	return 0;
}

static void queue_recovery_work(struct recovery_info *rinfo)
{
	struct recovery_work *rw;
	struct recovery_list_work *rlw;
	struct recovery_obj_work *row;

	switch (rinfo->state) {
	case RW_PREPARE_LIST:
		rlw = xzalloc(sizeof(*rlw));
		rlw->oids = xmalloc(list_buffer_size);

		rw = &rlw->base;
		rw->work.fn = prepare_object_list;
		rw->work.done = finish_object_list;
		break;
	case RW_RECOVER_OBJ:
		row = xzalloc(sizeof(*row));
		row->oid = rinfo->oids[rinfo->done];

		rw = &row->base;
		rw->work.fn = recover_object_work;
		rw->work.done = recover_object_main;
		break;
	case RW_NOTIFY_COMPLETION:
		rw = xzalloc(sizeof(*rw));
		rw->work.fn = notify_recovery_completion_work;
		rw->work.done = notify_recovery_completion_main;
		break;
	default:
		panic("unknow recovery state %d", rinfo->state);
		break;
	}

	rw->epoch = rinfo->epoch;
	rw->tgt_epoch = rinfo->tgt_epoch;
	rw->cur_vinfo = grab_vnode_info(rinfo->cur_vinfo);
	rw->old_vinfo = grab_vnode_info(rinfo->old_vinfo);

	queue_work(sys->recovery_wqueue, &rw->work);
}

void get_recovery_state(struct recovery_state *state)
{
	struct recovery_info *rinfo = main_thread_get(current_rinfo);

	memset(state, 0, sizeof(*state));

	if (!rinfo) {
		state->in_recovery = 0;
		return;
	}

	state->in_recovery = 1;
	state->state = rinfo->state;
	state->nr_finished = rinfo->done;
	state->nr_total = rinfo->count;
}
