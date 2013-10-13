/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 * Copyright (C) 2012-2013 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheep_priv.h"

static inline void gateway_init_fwd_hdr(struct sd_req *fwd, struct sd_req *hdr)
{
	memcpy(fwd, hdr, sizeof(*fwd));
	fwd->opcode = gateway_to_peer_opcode(hdr->opcode);
	fwd->proto_ver = SD_SHEEP_PROTO_VER;
}

struct req_iter {
	uint8_t *buf;
	uint32_t wlen;
	uint32_t dlen;
	uint64_t off;
};

static struct req_iter *prepare_replication_requests(struct request *req,
						     int *nr)
{
	int nr_copies = get_req_copy_number(req);
	void *data = req->data;
	uint32_t len = req->rq.data_length;
	uint64_t off = req->rq.obj.offset;
	struct req_iter *reqs = xzalloc(sizeof(*reqs) * nr_copies);

	sd_debug("%"PRIx64, req->rq.obj.oid);

	*nr = nr_copies;
	for (int i = 0; i < nr_copies; i++) {
		reqs[i].buf = data;
		reqs[i].dlen = len;
		reqs[i].off = off;
		reqs[i].wlen = len;
	}
	return reqs;
}

/*
 * Make sure we don't overwrite the existing data for unaligned write
 *
 * If either offset or length of request isn't aligned to SD_EC_D_SIZE, we have
 * to read the unaligned blocks before write. This kind of write amplification
 * indeed slow down the write operation with extra read overhead.
 */
static void *init_erasure_buffer(struct request *req, int buf_len)
{
	char *buf = xzalloc(buf_len);
	uint32_t len = req->rq.data_length;
	uint64_t off = req->rq.obj.offset;
	uint64_t oid = req->rq.obj.oid;
	int opcode = req->rq.opcode;
	struct sd_req hdr;
	uint64_t head = round_down(off, SD_EC_D_SIZE);
	uint64_t tail = round_down(off + len, SD_EC_D_SIZE);
	int ret;

	if (opcode != SD_OP_WRITE_OBJ)
		goto out;

	if (off % SD_EC_D_SIZE) {
		/* Read head */
		sd_init_req(&hdr, SD_OP_READ_OBJ);
		hdr.obj.oid = oid;
		hdr.data_length = SD_EC_D_SIZE;
		hdr.obj.offset = head;
		ret = exec_local_req(&hdr, buf);
		if (ret != SD_RES_SUCCESS) {
			free(buf);
			return NULL;
		}
	}

	if ((len + off) % SD_EC_D_SIZE && tail - head > 0) {
		/* Read tail */
		sd_init_req(&hdr, SD_OP_READ_OBJ);
		hdr.obj.oid = oid;
		hdr.data_length = SD_EC_D_SIZE;
		hdr.obj.offset = tail;
		ret = exec_local_req(&hdr, buf + tail - head);
		if (ret != SD_RES_SUCCESS) {
			free(buf);
			return NULL;
		}
	}
out:
	memcpy(buf + off % SD_EC_D_SIZE, req->data, len);
	return buf;
}

/*
 * We spread data strips of req along with its parity strips onto replica for
 * write opertaion. For read we only need to prepare data strip buffers.
 */
static struct req_iter *prepare_erasure_requests(struct request *req, int *nr)
{
	uint32_t len = req->rq.data_length;
	uint64_t off = req->rq.obj.offset;
	int opcode = req->rq.opcode;
	int start = off / SD_EC_D_SIZE;
	int end = DIV_ROUND_UP(off + len, SD_EC_D_SIZE), i, j;
	int nr_stripe = end - start;
	struct fec *ctx = ec_init();
	int nr_to_send = (opcode == SD_OP_READ_OBJ) ? SD_EC_D : SD_EC_DP;
	struct req_iter *reqs = xzalloc(sizeof(*reqs) * nr_to_send);
	char *p, *buf = NULL;

	sd_debug("start %d, end %d, send %d, off %"PRIu64 ", len %"PRIu32,
		 start, end, nr_to_send, off, len);

	*nr = nr_to_send;
	for (i = 0; i < nr_to_send; i++) {
		int l = SD_EC_STRIP_SIZE * nr_stripe;

		reqs[i].buf = xmalloc(l);
		reqs[i].dlen = l;
		reqs[i].off = start * SD_EC_STRIP_SIZE;
		switch (opcode) {
		case SD_OP_CREATE_AND_WRITE_OBJ:
		case SD_OP_WRITE_OBJ:
			reqs[i].wlen = l;
			break;
		default:
			break;
		}
	}

	if (opcode != SD_OP_WRITE_OBJ && opcode != SD_OP_CREATE_AND_WRITE_OBJ)
		goto out; /* Read and remove operation */

	p = buf = init_erasure_buffer(req, SD_EC_D_SIZE * nr_stripe);
	if (!buf) {
		sd_err("failed to init erasure buffer %"PRIx64,
		       req->rq.obj.oid);
		free(reqs);
		reqs = NULL;
		goto out;
	}
	for (i = 0; i < nr_stripe; i++) {
		const uint8_t *ds[SD_EC_D];
		uint8_t *ps[SD_EC_P];

		for (j = 0; j < SD_EC_D; j++)
			ds[j] = reqs[j].buf + SD_EC_STRIP_SIZE * i;

		for (j = 0; j < SD_EC_P; j++)
			ps[j] = reqs[SD_EC_D + j].buf + SD_EC_STRIP_SIZE * i;

		for (j = 0; j < SD_EC_D; j++)
			memcpy((uint8_t *)ds[j], p + j * SD_EC_STRIP_SIZE,
			       SD_EC_STRIP_SIZE);
		ec_encode(ctx, ds, ps);
		p += SD_EC_D_SIZE;
	}
out:
	ec_destroy(ctx);
	free(buf);

	return reqs;
}

/* Requests from dog might not have vdi registered yet in the vdi state */
static bool is_erasure_req(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;

	if (is_vdi_obj(oid))
		return false;

	if (req->rq.obj.copy_policy > 0)
		return true;

	return get_vdi_copy_policy(oid_to_vid(oid)) > 0;
}

bool is_erasure_oid(uint64_t oid)
{
	return !is_vdi_obj(oid) && get_vdi_copy_policy(oid_to_vid(oid)) > 0;
}

/* Prepare request iterator and buffer for each replica */
static struct req_iter *prepare_requests(struct request *req, int *nr)
{
	if (is_erasure_req(req))
		return prepare_erasure_requests(req, nr);
	else
		return prepare_replication_requests(req, nr);
}

static void finish_requests(struct request *req, struct req_iter *reqs,
			    int nr_to_send)
{
	uint64_t oid = req->rq.obj.oid;
	uint32_t len = req->rq.data_length;
	uint64_t off = req->rq.obj.offset;
	int opcode = req->rq.opcode;
	int start = off / SD_EC_D_SIZE;
	int end = DIV_ROUND_UP(off + len, SD_EC_D_SIZE), i, j;
	int nr_stripe = end - start;

	if (!is_erasure_oid(oid))
		goto out;

	sd_debug("start %d, end %d, send %d, off %"PRIu64 ", len %"PRIu32,
		 start, end, nr_to_send, off, len);

	/* We need to assemble the data strips into the req buffer for read */
	if (opcode == SD_OP_READ_OBJ) {
		char *p, *buf = xmalloc(SD_EC_D_SIZE * nr_stripe);

		p = buf;
		for (i = 0; i < nr_stripe; i++) {
			for (j = 0; j < nr_to_send; j++) {
				memcpy(p, reqs[j].buf + SD_EC_STRIP_SIZE * i,
				       SD_EC_STRIP_SIZE);
				p += SD_EC_STRIP_SIZE;
			}
		}
		memcpy(req->data, buf + off % SD_EC_D_SIZE, len);
		req->rp.data_length = req->rq.data_length;
		free(buf);
	}
	for (i = 0; i < nr_to_send; i++)
		free(reqs[i].buf);
out:
	free(reqs);
}

/*
 * Try our best to read one copy and read local first.
 *
 * Return success if any read succeed. We don't call gateway_forward_request()
 * because we only read once.
 */
static int gateway_replication_read(struct request *req)
{
	int i, ret = SD_RES_SUCCESS;
	struct sd_req fwd_hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&fwd_hdr;
	const struct sd_vnode *v;
	const struct sd_vnode *obj_vnodes[SD_MAX_COPIES];
	uint64_t oid = req->rq.obj.oid;
	int nr_copies, j;

	nr_copies = get_req_copy_number(req);

	oid_to_vnodes(oid, &req->vinfo->vroot, nr_copies, obj_vnodes);
	for (i = 0; i < nr_copies; i++) {
		v = obj_vnodes[i];
		if (!vnode_is_local(v))
			continue;
		ret = peer_read_obj(req);
		if (ret == SD_RES_SUCCESS)
			goto out;

		sd_err("local read %"PRIx64" failed, %s", oid,
		       sd_strerror(ret));
		break;
	}

	/*
	 * Read random copy from cluster for better load balance, useful for
	 * reading base VM's COW objects
	 */
	j = random();
	for (i = 0; i < nr_copies; i++) {
		int idx = (i + j) % nr_copies;

		v = obj_vnodes[idx];
		if (vnode_is_local(v))
			continue;
		/*
		 * We need to re-init it because rsp and req share the same
		 * structure.
		 */
		gateway_init_fwd_hdr(&fwd_hdr, &req->rq);
		ret = sheep_exec_req(&v->node->nid, &fwd_hdr, req->data);
		if (ret != SD_RES_SUCCESS)
			continue;

		/* Read success */
		memcpy(&req->rp, rsp, sizeof(*rsp));
		break;
	}
out:
	return ret;
}

struct forward_info_entry {
	struct pollfd pfd;
	const struct node_id *nid;
	struct sockfd *sfd;
	void *buf;
};

struct forward_info {
	struct forward_info_entry ent[SD_MAX_NODES];
	int nr_sent;
};

static inline void forward_info_update(struct forward_info *fi, int pos)
{
	sd_debug("%d, %d", fi->nr_sent, pos);
	fi->nr_sent--;
	memmove(fi->ent + pos, fi->ent + pos + 1,
		sizeof(struct forward_info_entry) * (fi->nr_sent - pos));
}

static inline void finish_one_entry(struct forward_info *fi, int i)
{
	sockfd_cache_put(fi->ent[i].nid, fi->ent[i].sfd);
	forward_info_update(fi, i);
}

static inline void finish_one_entry_err(struct forward_info *fi, int i)
{
	sockfd_cache_del(fi->ent[i].nid, fi->ent[i].sfd);
	forward_info_update(fi, i);
}

static inline struct forward_info_entry *
forward_info_find(struct forward_info *fi, int fd)
{
	for (int i = 0; i < fi->nr_sent; i++)
		if (fi->ent[i].pfd.fd == fd)
			return &fi->ent[i];

	panic("can't find entry for %d", fd);
	return NULL;
}

struct pfd_info {
	struct pollfd pfds[SD_MAX_NODES];
	int nr;
};

static inline void pfd_info_init(struct forward_info *fi, struct pfd_info *pi)
{
	int i;
	for (i = 0; i < fi->nr_sent; i++)
		pi->pfds[i] = fi->ent[i].pfd;
	pi->nr = fi->nr_sent;
}

/*
 * Wait for all forward requests completion.
 *
 * Even if something goes wrong, we have to wait forward requests completion to
 * avoid interleaved requests.
 *
 * Return error code if any one request fails.
 */
static int wait_forward_request(struct forward_info *fi, struct request *req)
{
	int nr_sent, err_ret = SD_RES_SUCCESS, ret, pollret, i,
	    repeat = MAX_RETRY_COUNT;
	struct pfd_info pi;
	struct sd_rsp *rsp = &req->rp;
again:
	pfd_info_init(fi, &pi);
	pollret = poll(pi.pfds, pi.nr, 1000 * POLL_TIMEOUT);
	if (pollret < 0) {
		if (errno == EINTR)
			goto again;

		panic("%m");
	} else if (pollret == 0) {
		/*
		 * If IO NIC is down, epoch isn't incremented, so we can't retry
		 * for ever.
		 */
		if (sheep_need_retry(req->rq.epoch) && repeat) {
			repeat--;
			sd_warn("poll timeout %d, disks of some nodes or "
				"network is busy. Going to poll-wait again",
				fi->nr_sent);
			goto again;
		}

		nr_sent = fi->nr_sent;
		/* XXX Blinedly close all the connections */
		for (i = 0; i < nr_sent; i++)
			sockfd_cache_del(fi->ent[i].nid, fi->ent[i].sfd);

		return SD_RES_NETWORK_ERROR;
	}

	nr_sent = fi->nr_sent;
	for (i = 0; i < nr_sent; i++)
		if (pi.pfds[i].revents & POLLIN)
			break;
	if (i < nr_sent) {
		int re = pi.pfds[i].revents;
		sd_debug("%d, revents %x", i, re);
		if (re & (POLLERR | POLLHUP | POLLNVAL)) {
			err_ret = SD_RES_NETWORK_ERROR;
			finish_one_entry_err(fi, i);
			goto out;
		}
		if (do_read(pi.pfds[i].fd, rsp, sizeof(*rsp), sheep_need_retry,
			    req->rq.epoch, MAX_RETRY_COUNT)) {
			sd_err("remote node might have gone away");
			err_ret = SD_RES_NETWORK_ERROR;
			finish_one_entry_err(fi, i);
			goto out;
		}

		if (rsp->data_length) {
			struct forward_info_entry *ent;

			ent = forward_info_find(fi, pi.pfds[i].fd);
			if (do_read(pi.pfds[i].fd, ent->buf, rsp->data_length,
				    sheep_need_retry, req->rq.epoch,
				    MAX_RETRY_COUNT)) {
				sd_err("remote node might have gone away");
				err_ret = SD_RES_NETWORK_ERROR;
				finish_one_entry_err(fi, i);
				goto out;
			}
		}
		ret = rsp->result;
		if (ret != SD_RES_SUCCESS) {
			sd_err("fail %"PRIx64", %s", req->rq.obj.oid,
			       sd_strerror(ret));
			err_ret = ret;
		}
		finish_one_entry(fi, i);
	}
out:
	if (fi->nr_sent > 0)
		goto again;

	return err_ret;
}

static inline void forward_info_init(struct forward_info *fi, size_t nr_to_send)
{
	int i;
	for (i = 0; i < nr_to_send; i++)
		fi->ent[i].pfd.fd = -1;
	fi->nr_sent = 0;
}

static inline void
forward_info_advance(struct forward_info *fi, const struct node_id *nid,
		     struct sockfd *sfd, void *buf)
{
	fi->ent[fi->nr_sent].nid = nid;
	fi->ent[fi->nr_sent].pfd.fd = sfd->fd;
	fi->ent[fi->nr_sent].pfd.events = POLLIN;
	fi->ent[fi->nr_sent].sfd = sfd;
	fi->ent[fi->nr_sent].buf = buf;
	fi->nr_sent++;
}

static int gateway_forward_request(struct request *req)
{
	int i, err_ret = SD_RES_SUCCESS, ret;
	unsigned wlen;
	uint64_t oid = req->rq.obj.oid;
	struct forward_info fi;
	struct sd_req hdr;
	const struct sd_node *target_nodes[SD_MAX_NODES];
	int nr_copies = get_req_copy_number(req), nr_to_send = 0;
	struct req_iter *reqs = NULL;

	sd_debug("%"PRIx64, oid);

	gateway_init_fwd_hdr(&hdr, &req->rq);
	oid_to_nodes(oid, &req->vinfo->vroot, nr_copies, target_nodes);
	forward_info_init(&fi, nr_copies);
	reqs = prepare_requests(req, &nr_to_send);
	if (!reqs)
		return SD_RES_NETWORK_ERROR;

	for (i = 0; i < nr_to_send; i++) {
		struct sockfd *sfd;
		const struct node_id *nid;

		nid = &target_nodes[i]->nid;
		sfd = sockfd_cache_get(nid);
		if (!sfd) {
			err_ret = SD_RES_NETWORK_ERROR;
			break;
		}

		hdr.data_length = reqs[i].dlen;
		wlen = reqs[i].wlen;
		hdr.obj.offset = reqs[i].off;
		hdr.obj.ec_index = i;
		ret = send_req(sfd->fd, &hdr, reqs[i].buf, wlen,
			       sheep_need_retry, req->rq.epoch,
			       MAX_RETRY_COUNT);
		if (ret) {
			sockfd_cache_del_node(nid);
			err_ret = SD_RES_NETWORK_ERROR;
			sd_debug("fail %d", ret);
			break;
		}
		forward_info_advance(&fi, nid, sfd, reqs[i].buf);
	}

	sd_debug("nr_sent %d, err %x", fi.nr_sent, err_ret);
	if (fi.nr_sent > 0) {
		ret = wait_forward_request(&fi, req);
		if (ret != SD_RES_SUCCESS)
			err_ret = ret;
	}

	finish_requests(req, reqs, nr_to_send);
	return err_ret;
}

int gateway_read_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;

	if (!bypass_object_cache(req))
		return object_cache_handle_request(req);

	if (is_erasure_oid(oid))
		return gateway_forward_request(req);
	else
		return gateway_replication_read(req);
}

int gateway_write_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;

	if (oid_is_readonly(oid))
		return SD_RES_READONLY;

	if (!bypass_object_cache(req))
		return object_cache_handle_request(req);

	return gateway_forward_request(req);
}

int gateway_create_and_write_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;

	if (oid_is_readonly(oid))
		return SD_RES_READONLY;

	if (!bypass_object_cache(req))
		return object_cache_handle_request(req);

	return gateway_forward_request(req);
}

int gateway_remove_obj(struct request *req)
{
	return gateway_forward_request(req);
}
