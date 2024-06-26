// credit: https://github.com/snake0/krdma

#include <linux/kernel.h>
#include <linux/inet.h>
#include <linux/proc_fs.h>
#include <linux/kthread.h>
#include <linux/dma-direct.h>
#include <linux/pci.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/types.h>

#include "krdma.h"

int krdma_set_addr(struct sockaddr_in *addr, const char *host, const int port) {
	if (!addr)
		return -1;

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = in_aton(host);
	addr->sin_port = htons(port);
	return 0;
}

static void show_rdma_buffer_info(struct krdma_server_info *info){
	if(!info){
		rdma_error("Passed info is NULL\n");
		return;
	}
	debug("---------------------------------------------------------\n");
	debug("buffer info, addr: %p , len: %u , rkey : 0x%x \n", 
			(void*) info->dma_addr, info->size, info->rkey);
	debug("---------------------------------------------------------\n");
}

uint64_t string_to_uint64(const char *str)
{
    uint64_t value = 0;
    size_t len = strlen(str);
    size_t i;

    // Ensure we do not read beyond 8 characters
    len = len > 8 ? 8 : len;

    for (i = 0; i < len; i++) {
        value |= (uint64_t)(unsigned char)str[i] << (8 * (7 - i));
    }

    return value;
}

static void krdma_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krdma_cb *cb = ctx;
	struct ib_wc wc;
	int ret;

	if (cb->state == KRDMA_ERROR) {
		rdma_error("cq completion in ERROR state\n");
		return;
	}

	// reiterate our interests to continue receiving notifications
	if (cq == cb->send_cq)
		ib_req_notify_cq(cb->send_cq, IB_CQ_NEXT_COMP);
	if (cq == cb->recv_cq)
		ib_req_notify_cq(cb->recv_cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				debug("cq flushed\n");
				continue;
			} else {
				rdma_error("cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			debug("send completion\n");
			// cb->stats.send_bytes += cb->send_sgl.length;
			// cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			debug("rdma write completion\n");
			// cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			// cb->stats.write_msgs++;
			cb->state = KRDMA_WRITE_COMPLETE;
			complete(&cb->cm_done);
			break;

		case IB_WC_RDMA_READ:
			debug("rdma read completion\n");
			// cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			// cb->stats.read_msgs++;
			cb->state = KRDMA_READ_COMPLETE;
			complete(&cb->cm_done);
			break;

		case IB_WC_RECV:
			debug("recv completion, %d bytes received\n", wc.byte_len);
			debug("Server sent us its buffer location and credentials, showing \n");
			show_rdma_buffer_info(&cb->recv_buf);

			complete(&cb->cm_done);
			break;

		default:
			rdma_error("%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		rdma_error("poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = KRDMA_ERROR;
	complete(&cb->cm_done);
}

int krdma_cma_event_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *event)
{
	struct krdma_cb *cb = cm_id->context;
	// struct krdma_cb *conn_cb = NULL;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		debug("%s: RDMA_CM_EVENT_ADDR_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_ADDR_RESOLVED;
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		debug("%s: RDMA_CM_EVENT_ROUTE_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_ROUTE_RESOLVED;
		break;

	case RDMA_CM_EVENT_ROUTE_ERROR:
		debug("%s: RDMA_CM_EVENT_ROUTE_ERROR, cm_id %p, error %d\n",
				__func__, cm_id, event->status);
		cb->state = KRDMA_ERROR;
		break;

/* this is for server
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		krdma_debug("%s: RDMA_CM_EVENT_CONNECT_REQUEST, cm_id %p\n",
				__func__, cm_id);
		ret = __krdma_create_cb(&conn_cb, KRDMA_ACCEPT_CONN);
		if (!ret) {
			conn_cb->cm_id = cm_id;
			cm_id->context = conn_cb;
			list_add_tail(&conn_cb->list, &cb->ready_conn);
		} else {
			krdma_err("__krdma_create_cb fail, ret %d\n", ret);
			cb->state = KRDMA_ERROR;
		}
		break;
*/

	case RDMA_CM_EVENT_ESTABLISHED:
		debug("%s: RDMA_CM_EVENT_ESTABLISHED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_CONNECTED;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		debug(KERN_ERR "%s: RDMA_CM_EVENT_DISCONNECTED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_DISCONNECTED;
		break;

	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		rdma_error("RDMA_CM_EVENT %d, cm_id %p\n", event->event, cm_id);
		cb->state = KRDMA_CONNECT_REJECTED;
		break;
	default:
		debug("%s: unknown event %d, cm_id %p\n",
				__func__, event->event, cm_id);
		cb->state = KRDMA_ERROR;
	}
	complete(&cb->cm_done);
	return 0;
}

/* for locally issued RDMA read write */
int krdma_setup_buf(struct krdma_cb *cb) {
	int ret;

	if (!cb)
		return -1;

	cb->send_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, cb->send_dma_addr)) {
		rdma_error("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			&cb->send_buf, sizeof(cb->send_buf));
		goto exit;
	}

	cb->recv_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, cb->recv_dma_addr)) {
		rdma_error("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			&cb->recv_buf, sizeof(cb->recv_buf));
		goto exit;
	}

	cb->rdma_buf_size = RDMA_RDWR_BUF_LEN;

	cb->rdma_write_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->rdma_buf_size,
							&cb->rdma_wbuf_dma_addr,
							GFP_KERNEL);
	if (!cb->rdma_write_buf) {
		rdma_error("start_buf malloc failed\n");
		ret = -ENOMEM;
		goto exit;
	}	

	cb->rdma_read_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->rdma_buf_size,
							&cb->rdma_rbuf_dma_addr,
							GFP_KERNEL);
	if (!cb->rdma_read_buf) {
		rdma_error("rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto exit;
	}

	debug("rdma write buffer allocated ok, addr %p, dma_addr %llx, size %d\n", 
					cb->rdma_write_buf, cb->rdma_wbuf_dma_addr, cb->rdma_buf_size);
	debug("rdma read buffer allocated ok, addr %p, dma_addr %llx, size %d\n", 
					cb->rdma_read_buf, cb->rdma_rbuf_dma_addr, cb->rdma_buf_size);

	debug("krdma_setup_mr() is done\n");
	return 0;

exit:
	krdma_free_buf(cb);
	return -ENOMEM;
}

void krdma_free_buf(struct krdma_cb *cb) {
	if (cb->rdma_write_buf) {
		dma_free_coherent(cb->pd->device->dma_device, cb->rdma_buf_size, cb->rdma_write_buf,
					cb->rdma_wbuf_dma_addr);
	}
	if (cb->rdma_read_buf) {
		dma_free_coherent(cb->pd->device->dma_device, cb->rdma_buf_size, cb->rdma_read_buf,
					cb->rdma_rbuf_dma_addr);
	}
}

struct krdma_cb * krdma_alloc_cb(void)
{
	struct krdma_cb *cb;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb) {
		rdma_error("failed to allocate memory for cb\n");
		return NULL;
	}

	init_completion(&cb->cm_done);

	return cb;
}

int krdma_init_cb(struct krdma_cb *cb) {
	int ret;
	struct ib_cq_init_attr cq_attr;
	struct ib_qp_init_attr qp_init_attr;

	if (!cb)
		return -ENOMEM;

	/* Create Protection Domain. */
	cb->pd = ib_alloc_pd(cb->cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		ret = PTR_ERR(cb->pd);
		rdma_error("ib_alloc_pd failed\n");
		goto exit;
	}
	debug("ib_alloc_pd succeed, cm_id %p\n", cb->cm_id);

	/* Create send Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->send_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->send_cq)) {
		ret = PTR_ERR(cb->send_cq);
		rdma_error("ib_create_cq failed, ret%d\n", ret);
		goto free_pd;
	}

	/* Create recv Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->recv_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->recv_cq)) {
		ret = PTR_ERR(cb->recv_cq);
		rdma_error("ib_create_cq failed, ret%d\n", ret);
		goto free_send_cq;
	}

	ret = ib_req_notify_cq(cb->send_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdma_error("ib_req_notify_cq() for send cq failed\n");
		goto free_recv_cq;
	}

	ret = ib_req_notify_cq(cb->recv_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		rdma_error("ib_req_notify_cq() for recv cq failed\n");
		goto free_recv_cq;
	}

	debug("ib_create_cq succeed, cm_id %p\n", cb->cm_id);

	/* Create Queue Pair. */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.cap.max_send_wr = RDMA_SEND_QUEUE_DEPTH;
	qp_init_attr.cap.max_recv_wr = RDMA_RECV_QUEUE_DEPTH;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_send_sge = 1;
	/* Mlx doesn't support inline sends for kernel QPs (yet) */
	qp_init_attr.cap.max_inline_data = 0;
	qp_init_attr.qp_type = IB_QPT_RC;
	qp_init_attr.send_cq = cb->send_cq;
	qp_init_attr.recv_cq = cb->recv_cq;
	qp_init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	ret = rdma_create_qp(cb->cm_id, cb->pd, &qp_init_attr);
	if (ret) {
		rdma_error("rdma_create_qp failed, ret %d\n", ret);
		goto free_recv_cq;
	}
	cb->qp = cb->cm_id->qp;
	debug("ib_create_qp succeed, cm_id %p\n", cb->cm_id);

	return 0;

free_recv_cq:
	ib_destroy_cq(cb->recv_cq);
free_send_cq:
	ib_destroy_cq(cb->send_cq);
free_pd:
	ib_dealloc_pd(cb->pd);
exit:
	return ret;
}

int krdma_free_cb(struct krdma_cb *cb)
{
	if (cb == NULL)
		return -EINVAL;

	if (!cb->cm_id)
		return -EINVAL;

	rdma_disconnect(cb->cm_id);
	if (cb->cm_id->qp)
		rdma_destroy_qp(cb->cm_id);

	krdma_free_buf(cb);

	if (cb->send_cq)
		ib_destroy_cq(cb->send_cq);
	if (cb->recv_cq)
		ib_destroy_cq(cb->recv_cq);

	if (cb->pd)
		ib_dealloc_pd(cb->pd);

	rdma_destroy_id(cb->cm_id);
	cb->cm_id = NULL;

	kfree(cb);
	return 0;
}

/*
 * Call rdma_resolve_route for dev detection
 */
int krdma_resolve_remote(struct krdma_cb *cb, const char *host, const int port) {
	int ret;
	struct sockaddr_in addr;

	/* Resolve address */
	ret = krdma_set_addr(&addr, host, port);
	if (ret < 0)
		goto exit;
	ret = rdma_resolve_addr(cb->cm_id, NULL,
			(struct sockaddr *)&addr, RDMA_RESOLVE_TIMEOUT);
	if (ret) {
		rdma_error("rdma_resolve_addr failed, ret %d\n", ret);
		goto exit;
	}
	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_ADDR_RESOLVED) {
		ret = -1;
		rdma_error("rdma_resolve_route state error, ret %d\n", ret);
		goto exit;
	}
	debug("rdma_resolve_addr succeed, device[%s] port_num[%u]\n",
		cb->cm_id->device->name, cb->cm_id->port_num);

	/* Resolve route. */
	ret = rdma_resolve_route(cb->cm_id, RDMA_RESOLVE_TIMEOUT);
	if (ret) {
		rdma_error("rdma_resolve_route failed, ret %d\n", ret);
		goto exit;
	}
	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_ROUTE_RESOLVED) {
		ret = -1;
		rdma_error("rdma_resolve_route state error, ret %d\n", ret);
		goto exit;
	}
	debug("rdma_resolve_route succeed, cm_id %p, "
			"remote host[%pI4], remote port[%d]\n",
			cb->cm_id,
			&((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_addr.s_addr,
			ntohs(((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_port));

	return 0;

exit:
	return ret;
}
