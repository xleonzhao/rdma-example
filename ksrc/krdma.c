// credit: https://github.com/xleonzhao/krdma/blob/nopost/krdma.c

/* 
 * Called after __krdma_bound_dev_{local, remote}.
 * Allocate pd, cq, qp, mr, freed by caller
 */

#include <linux/kernel.h>
#include <linux/inet.h>
#include <linux/proc_fs.h>
#include <linux/kthread.h>
// #include <linux/kvm_host.h>
#include <linux/dma-direct.h>
#include <linux/pci.h>
#include <linux/list.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "krdma.h"

static bool dbg = 1;

// static uint64_t __krdma_virt_to_dma(
// 	struct krdma_cb *cb, void *addr, size_t length, bool physical_allocation) {
// 	struct ib_device *ibd = cb->pd->device;

// 	return physical_allocation ?
// 		(uint64_t) phys_to_dma(ibd->dma_device, (phys_addr_t) virt_to_phys(addr))
// 	:	(uint64_t) ib_dma_map_single(ibd, addr, length, DMA_BIDIRECTIONAL);
// }

/* MR for RDMA read write */
int krdma_setup_mr(struct krdma_cb *cb) {
	if (!cb)
		return -1;

	krdma_rw_info_t *local_info = &cb->local_info;
	krdma_rw_info_t *remote_info = &cb->remote_info;
	struct ib_port_attr port_attr;
	int page_list_len;
	struct ib_mr *mr;
	void *local_buf=NULL, *remote_buf=NULL;
	dma_addr_t local_dma_addr=0L, remote_dma_addr=0L;
	int ret;
	struct scatterlist sg = {0};

	cb->send_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, cb->send_dma_addr)) {
		krdma_err("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			&cb->send_buf, sizeof(cb->send_buf));
		goto exit;
	}

	cb->recv_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, cb->recv_dma_addr)) {
		krdma_err("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			&cb->recv_buf, sizeof(cb->recv_buf));
		goto exit;
	}

	cb->size = RDMA_RDWR_BUF_LEN;
	cb->start_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->size,
							&cb->start_dma_addr,
							GFP_KERNEL);
	if (!cb->start_buf) {
		krdma_err("start_buf malloc failed\n");
		ret = -ENOMEM;
		goto exit;
	}	

	cb->rdma_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->size,
							&cb->rdma_dma_addr,
							GFP_KERNEL);
	if (!cb->rdma_buf) {
		krdma_err("rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto exit;
	}

	krdma_debug("send buffer allocated ok, addr %p, dma_addr %llx, size %d\n", 
					cb->start_buf, cb->start_dma_addr, cb->size);
	krdma_debug("recv buffer allocated ok, addr %p, dma_addr %llx, size %d\n", 
					cb->rdma_buf, cb->rdma_dma_addr, cb->size);

/*
	// allocate space for local use
	local_buf = kmalloc(RDMA_SEND_BUF_LEN , GFP_KERNEL);
	remote_buf = kmalloc(RDMA_RECV_BUF_LEN , GFP_KERNEL);
	if (!local_buf || !remote_buf) {
		krdma_err("buf alloc failed.\n");
		goto exit;
	}
	local_info->buf = local_buf;
	local_info->length = RDMA_SEND_BUF_LEN ;	
	remote_info->buf = remote_buf;
	remote_info->length = RDMA_RECV_BUF_LEN;

	local_dma_addr = ib_dma_map_single(cb->cm_id->device,
				   local_info->buf, local_info->length, DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, local_dma_addr)) {
		krdma_err("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			local_info->buf, local_info->length);
		goto exit;
	}
	local_info->dma_addr = local_dma_addr;

	remote_dma_addr = ib_dma_map_single(cb->cm_id->device,
				   remote_info->buf, remote_info->length, DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, remote_dma_addr)) {
		krdma_err("Failed to map buffer addr to dma addr, addr=%p, length=%ld\n", 
			remote_info->buf, remote_info->length);
		goto exit;
	}
	remote_info->dma_addr = remote_dma_addr;

	krdma_debug("send buffer allocated ok, addr %p, size %ld\n", 
					cb->local_info.buf, cb->local_info.length);
	krdma_debug("recv buffer allocated ok, addr %p, size %ld\n", 
					cb->remote_info.buf, cb->remote_info.length);
*/

	mr = cb->pd->device->ops.get_dma_mr(cb->pd, IB_ACCESS_REMOTE_READ | 
                                     IB_ACCESS_REMOTE_WRITE | 
                                     IB_ACCESS_LOCAL_WRITE);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		krdma_err("get_dma_mr failed %d\n", ret);
		goto exit;
	}
	cb->mr = mr;
	krdma_debug("mr registered: rkey 0x%x lkey 0x%x\n",
		mr->rkey, mr->lkey);

/*
    // create memory region
	page_list_len = (((remote_info->length - 1) & PAGE_MASK) + PAGE_SIZE)
				>> PAGE_SHIFT;
	mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, page_list_len);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		krdma_err("allocate mr failed %d\n", ret);
		goto exit;
	}
	cb->mr = mr;
	cb->page_list_len = page_list_len;
	krdma_debug("mr registered: rkey 0x%x page_list_len %u\n",
		mr->rkey, page_list_len);

	sg_dma_address(&sg) = remote_info->dma_addr;
	sg_dma_len(&sg) = remote_info->length;
	ret = ib_map_mr_sg(cb->mr, &sg, 1, NULL, PAGE_SIZE);
	if(ret <= 0 || ret > cb->page_list_len) {
		krdma_err("ib_map_mr_sg() error, ret %d\n", ret);
		goto exit;
	} else {
		krdma_debug("ib_map_mr_sg() ok, page size %u len %lu iova_start %llx\n",
			cb->mr->page_size,
			(unsigned long)cb->mr->length,
			(unsigned long long)cb->mr->iova);		
	}
*/

	ret = ib_query_port(cb->cm_id->device, cb->cm_id->port_num, &port_attr);
	if (ret) {
		krdma_err("ib_query_port failed.\n");
		goto exit;
	}
	local_info->lid = port_attr.lid;
	local_info->qp_num = cb->qp->qp_num;
	
	krdma_debug("krdma_setup_mr() is done\n");
	return 0;

exit:
	krdma_free_mr(cb);
	return -ENOMEM;
}

void krdma_free_mr(struct krdma_cb *cb) {
	// if (cb->mr) {
	// 	ib_dereg_mr(cb->mr);
	// 	cb->mr = NULL;
	// }

	if (cb->local_info.dma_addr) {
		ib_dma_unmap_single(cb->cm_id->device,
				   cb->local_info.dma_addr, cb->local_info.length, DMA_BIDIRECTIONAL);
		cb->local_info.dma_addr = 0L;
	}
	if (cb->remote_info.dma_addr) {
		ib_dma_unmap_single(cb->cm_id->device,
				   cb->remote_info.dma_addr, cb->remote_info.length, DMA_BIDIRECTIONAL);
		cb->remote_info.dma_addr = 0L;
	}
/*
	if (cb->local_info.buf) {
		kfree(cb->local_info.buf);
		cb->local_info.buf = NULL;
	}
	if (cb->remote_info.buf) {
		kfree(cb->remote_info.buf);
		cb->remote_info.buf = NULL;
	}
*/
	if (cb->start_buf) {
		dma_free_coherent(cb->pd->device->dma_device, cb->size, cb->start_buf,
					cb->start_dma_addr);
	}
	if (cb->rdma_buf) {
		dma_free_coherent(cb->pd->device->dma_device, cb->size, cb->rdma_buf,
					cb->rdma_dma_addr);
	}
}

int krdma_alloc_cb(struct krdma_cb **cbp, enum krdma_role role)
{
	struct krdma_cb *cb;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;
	init_completion(&cb->cm_done);

	cb->role = role;
	if (cb->role == KRDMA_LISTEN_CONN) {
		INIT_LIST_HEAD(&cb->ready_conn);
		INIT_LIST_HEAD(&cb->active_conn);
	}
	mutex_init(&cb->slock);
	mutex_init(&cb->rlock);

	if (cbp)
		*cbp = cb;
	return 0;
}

static void show_rdma_buffer_info(struct krdma_buffer_info *info){
	if(!info){
		krdma_err("Passed info is NULL\n");
		return;
	}
	krdma_debug("---------------------------------------------------------\n");
	krdma_debug("buffer info, addr: %p , len: %u , rkey : 0x%u \n", 
			(void*) info->dma_addr, info->size, info->rkey);
	krdma_debug("---------------------------------------------------------\n");
}

static void krdma_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krdma_cb *cb = ctx;
	struct ib_wc wc;
	int ret;

	if (cb->state == KRDMA_ERROR) {
		krdma_err("cq completion in ERROR state\n");
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
				krdma_debug("cq flushed\n");
				continue;
			} else {
				krdma_err("cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			krdma_debug("send completion\n");
			// cb->stats.send_bytes += cb->send_sgl.length;
			// cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			krdma_debug("rdma write completion\n");
			// cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			// cb->stats.write_msgs++;
			cb->state = KRDMA_WRITE_COMPLETE;
			complete(&cb->cm_done);
			break;

		case IB_WC_RDMA_READ:
			krdma_debug("rdma read completion\n");
			// cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			// cb->stats.read_msgs++;
			cb->state = KRDMA_READ_COMPLETE;
			complete(&cb->cm_done);
			break;

		case IB_WC_RECV:
			krdma_debug("recv completion, %d bytes received\n", wc.byte_len);
			krdma_debug("Server sent us its buffer location and credentials, showing \n");
			show_rdma_buffer_info(&cb->recv_buf);
			// cb->stats.recv_bytes += sizeof(cb->recv_buf);
			// cb->stats.recv_msgs++;

			// ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			// if (ret) {
			// 	printk(KERN_ERR PFX "post recv error: %d\n", 
			// 	       ret);
			// 	goto error;
			// }
			complete(&cb->cm_done);
			break;

		default:
			krdma_err("%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		krdma_err("poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = KRDMA_ERROR;
	complete(&cb->cm_done);
}

int krdma_init_cb(struct krdma_cb *cb) {
	int ret;
	struct ib_cq_init_attr cq_attr;
	struct ib_qp_init_attr qp_init_attr;

	/* Create Protection Domain. */
	cb->pd = ib_alloc_pd(cb->cm_id->device, IB_PD_UNSAFE_GLOBAL_RKEY);
	// cb->pd = ib_alloc_pd(cb->cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		ret = PTR_ERR(cb->pd);
		krdma_err("ib_alloc_pd failed\n");
		goto exit;
	}
	krdma_debug("ib_alloc_pd succeed, cm_id %p\n", cb->cm_id);

	/* Create send Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->send_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->send_cq)) {
		ret = PTR_ERR(cb->send_cq);
		krdma_err("ib_create_cq failed, ret%d\n", ret);
		goto free_pd;
	}

	/* Create recv Completion Queue. */
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.cqe = RDMA_CQ_QUEUE_DEPTH;
	cq_attr.comp_vector = 0;
	cb->recv_cq = ib_create_cq(cb->cm_id->device, krdma_cq_event_handler, NULL, cb, &cq_attr);
	if (IS_ERR(cb->recv_cq)) {
		ret = PTR_ERR(cb->recv_cq);
		krdma_err("ib_create_cq failed, ret%d\n", ret);
		goto free_send_cq;
	}

	ret = ib_req_notify_cq(cb->send_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		krdma_err("ib_req_notify_cq() for send cq failed\n");
		goto free_recv_cq;
	}

	ret = ib_req_notify_cq(cb->recv_cq, IB_CQ_NEXT_COMP);
	if (ret) {
		krdma_err("ib_req_notify_cq() for recv cq failed\n");
		goto free_recv_cq;
	}

	krdma_debug("ib_create_cq succeed, cm_id %p\n", cb->cm_id);

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
		krdma_err("rdma_create_qp failed, ret %d\n", ret);
		goto free_recv_cq;
	}
	cb->qp = cb->cm_id->qp;
	krdma_debug("ib_create_qp succeed, cm_id %p\n", cb->cm_id);

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
	struct krdma_cb *entry = NULL;
	struct krdma_cb *this = NULL;

	if (cb == NULL)
		return -EINVAL;

	if (!cb->cm_id)
		return -EINVAL;

	rdma_disconnect(cb->cm_id);
	if (cb->cm_id->qp)
		rdma_destroy_qp(cb->cm_id);

	krdma_free_mr(cb);
	if (cb->send_cq)
		ib_destroy_cq(cb->send_cq);
	if (cb->recv_cq)
		ib_destroy_cq(cb->recv_cq);

	if (cb->pd)
		ib_dealloc_pd(cb->pd);

	rdma_destroy_id(cb->cm_id);
	cb->cm_id = NULL;

	if (cb->role == KRDMA_LISTEN_CONN) {
		list_for_each_entry_safe(entry, this, &cb->ready_conn, list) {
			krdma_free_cb(entry);
			list_del(&entry->list);
		}
		list_for_each_entry_safe(entry, this, &cb->active_conn, list) {
			krdma_free_cb(entry);
			list_del(&entry->list);
		}
	}

	kfree(cb);
	return 0;
}

int krdma_cma_event_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *event)
{
	struct krdma_cb *cb = cm_id->context;
	// struct krdma_cb *conn_cb = NULL;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		krdma_debug("%s: RDMA_CM_EVENT_ADDR_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_ADDR_RESOLVED;
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		krdma_debug("%s: RDMA_CM_EVENT_ROUTE_RESOLVED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_ROUTE_RESOLVED;
		break;

	case RDMA_CM_EVENT_ROUTE_ERROR:
		krdma_debug("%s: RDMA_CM_EVENT_ROUTE_ERROR, cm_id %p, error %d\n",
				__func__, cm_id, event->status);
		cb->state = KRDMA_ERROR;
		break;

/* LZ: this is for server code
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
		krdma_debug("%s: RDMA_CM_EVENT_ESTABLISHED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_CONNECTED;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		krdma_debug(KERN_ERR "%s: RDMA_CM_EVENT_DISCONNECTED, cm_id %p\n",
				__func__, cm_id);
		cb->state = KRDMA_DISCONNECTED;
		break;

	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		krdma_err("RDMA_CM_EVENT %d, cm_id %p\n", event->event, cm_id);
		cb->state = KRDMA_CONNECT_REJECTED;
		break;
	default:
		krdma_debug("%s: unknown event %d, cm_id %p\n",
				__func__, event->event, cm_id);
		cb->state = KRDMA_ERROR;
	}
	complete(&cb->cm_done);
	return 0;
}

int krdma_set_addr(struct sockaddr_in *addr, const char *host, const int port) {
	if (!addr)
		return -1;

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = in_aton(host);
	addr->sin_port = htons(port);
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
		krdma_err("rdma_resolve_addr failed, ret %d\n", ret);
		goto exit;
	}
	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_ADDR_RESOLVED) {
		ret = -STATE_ERROR;
		krdma_err("rdma_resolve_route state error, ret %d\n", ret);
		goto exit;
	}
	krdma_debug("rdma_resolve_addr succeed, device[%s] port_num[%u]\n",
		cb->cm_id->device->name, cb->cm_id->port_num);

	/* Resolve route. */
	ret = rdma_resolve_route(cb->cm_id, RDMA_RESOLVE_TIMEOUT);
	if (ret) {
		krdma_err("rdma_resolve_route failed, ret %d\n", ret);
		goto exit;
	}
	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_ROUTE_RESOLVED) {
		ret = -STATE_ERROR;
		krdma_err("rdma_resolve_route state error, ret %d\n", ret);
		goto exit;
	}
	krdma_debug("rdma_resolve_route succeed, cm_id %p, "
			"remote host[%pI4], remote port[%d]\n",
			cb->cm_id,
			&((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_addr.s_addr,
			ntohs(((struct sockaddr_in *)&cb->cm_id->route.addr.dst_addr)->sin_port));

	return 0;

exit:
	return ret;
}
