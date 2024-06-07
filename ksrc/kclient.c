/*
 * An example RDMA client side code. 
 * Author: Animesh Trivedi 
 *         atrivedi@apache.org
 */

/*
* RDMA client as a kernel module
* Author: Leon Zhao
*         xleonzhao@gmail.com 
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/inet.h>

// #include "krdma.h"
#include "kcommon.h"
#include "krdma.h"

static bool thread_running = false;

#define IPADDR_LEN 16
static char server[IPADDR_LEN]={0}; // server ip address
module_param_string(server, server, IPADDR_LEN, S_IRUGO);

static int port = 20886; // server port number
module_param(port, int, S_IRUGO);

static struct sockaddr_in server_sockaddr = {0};
/* Source and Destination buffers, where RDMA operations source and sink */
static char *src = NULL, *dst = NULL; 
const char test_string[] = "hello, world";

/* This is our testing function */
static int check_src_dst(void) 
{
	return memcmp((void*) src, (void*) dst, strlen(src));
}

/* This function prepares client side connection resources for an RDMA connection */
static int client_prepare_connection(struct krdma_cb *cb)
{
	// struct rdma_cm_event *cm_event = NULL;
	int ret = -1;

	/*  Open a channel used to report asynchronous communication event */
	/* LZ: not available in kernel space
	cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel) {
		rdma_error("Creating cm event channel failed, errno: %d \n", -errno);
		return -errno;
	}
	debug("RDMA CM event channel is created at : %p \n", cm_event_channel);
	*/

	/* rdma_cm_id is the connection identifier (like socket) which is used 
	 * to define an RDMA connection. 
	 */
	/* LZ: API changed in kernel 
	ret = rdma_create_id(cm_event_channel, &cm_client_id, 
			NULL,
			RDMA_PS_TCP);
	if (ret) {
		rdma_error("Creating cm id failed with errno: %d \n", -errno); 
		return -errno;
	}
	*/

#ifdef _OLD_CODE
	/* Resolve destination and optional source addresses from IP addresses  to
	 * an RDMA address.  If successful, the specified rdma_cm_id will be bound
	 * to a local device. */
	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr*) s_addr, 2000);
	if (ret) {
		rdma_error("Failed to resolve address, ret: %d \n", ret);
		return -errno;
	}
	debug("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
	ret  = process_rdma_cm_event(cm_event_channel, 
			RDMA_CM_EVENT_ADDR_RESOLVED,
			&cm_event);
	if (ret) {
		rdma_error("Failed to receive a valid event, ret = %d \n", ret);
		return ret;
	}
	/* we ack the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		rdma_error("Failed to acknowledge the CM event, errno: %d\n", -errno);
		return -errno;
	}
	debug("RDMA address is resolved \n");

	 /* Resolves an RDMA route to the destination address in order to 
	  * establish a connection */
	ret = rdma_resolve_route(cm_client_id, 2000);
	if (ret) {
		rdma_error("Failed to resolve route, erno: %d \n", -errno);
	       return -errno;
	}
	debug("waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");
	ret = process_rdma_cm_event(cm_event_channel, 
			RDMA_CM_EVENT_ROUTE_RESOLVED,
			&cm_event);
	if (ret) {
		rdma_error("Failed to receive a valid event, ret = %d \n", ret);
		return ret;
	}
	/* we ack the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		rdma_error("Failed to acknowledge the CM event, errno: %d \n", -errno);
		return -errno;
	}

	printf("Trying to connect to server at : %s port: %d \n", 
			inet_ntoa(s_addr->sin_addr),
			ntohs(s_addr->sin_port));
#endif //_OLD_CODE

	ret = __krdma_bound_dev_remote(cb, server, port);
	if (ret)
		return ret;

#ifdef _OLD_CODE
	/* Protection Domain (PD) is similar to a "process abstraction" 
	 * in the operating system. All resources are tied to a particular PD. 
	 * And accessing recourses across PD will result in a protection fault.
	 */
	pd = ibv_alloc_pd(cm_client_id->verbs);
	if (!pd) {
		rdma_error("Failed to alloc pd, errno: %d \n", -errno);
		return -errno;
	}
	debug("pd allocated at %p \n", pd);
	/* Now we need a completion channel, were the I/O completion 
	 * notifications are sent. Remember, this is different from connection 
	 * management (CM) event notifications. 
	 * A completion channel is also tied to an RDMA device, hence we will 
	 * use cm_client_id->verbs. 
	 */
	io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
	if (!io_completion_channel) {
		rdma_error("Failed to create IO completion event channel, errno: %d\n",
			       -errno);
	return -errno;
	}
	debug("completion event channel created at : %p \n", io_completion_channel);
	/* Now we create a completion queue (CQ) where actual I/O 
	 * completion metadata is placed. The metadata is packed into a structure 
	 * called struct ibv_wc (wc = work completion). ibv_wc has detailed 
	 * information about the work completion. An I/O request in RDMA world 
	 * is called "work" ;) 
	 */
	client_cq = ibv_create_cq(cm_client_id->verbs /* which device*/, 
			CQ_CAPACITY /* maximum capacity*/, 
			NULL /* user context, not used here */,
			io_completion_channel /* which IO completion channel */, 
			0 /* signaling vector, not used here*/);
	if (!client_cq) {
		rdma_error("Failed to create CQ, errno: %d \n", -errno);
		return -errno;
	}
	debug("CQ created at %p with %d elements \n", client_cq, client_cq->cqe);
	ret = ibv_req_notify_cq(client_cq, 0);
	if (ret) {
		rdma_error("Failed to request notifications, errno: %d\n", -errno);
		return -errno;
	}
       /* Now the last step, set up the queue pair (send, recv) queues and their capacity.
         * The capacity here is define statically but this can be probed from the 
	 * device. We just use a small number as defined in rdma_common.h */
       bzero(&qp_init_attr, sizeof qp_init_attr);
       qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
       qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
       qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
       qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
       qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
       /* We use same completion queue, but one can use different queues */
       qp_init_attr.recv_cq = client_cq; /* Where should I notify for receive completion operations */
       qp_init_attr.send_cq = client_cq; /* Where should I notify for send completion operations */
       /*Lets create a QP */
       ret = rdma_create_qp(cm_client_id /* which connection id */,
		       pd /* which protection domain*/,
		       &qp_init_attr /* Initial attributes */);
	if (ret) {
		rdma_error("Failed to create QP, errno: %d \n", -errno);
	       return -errno;
	}
	client_qp = cm_client_id->qp;
	debug("QP created at %p \n", client_qp);
	return 0;
#endif // _OLD_CODE

	return 0;

	// ret = __krdma_connect(cb);
	// if (ret < 0)
	// 	goto out_release_cb;
	// return 0;

}

static int rdma_buffer_register(struct krdma_cb *cb, 
		struct metadata_mr * data_mr,
		enum ib_access_flags permission)
{
#ifdef _OLD_CODE
	struct ibv_mr *mr = NULL;
	if (!pd) {
		rdma_error("Protection domain is NULL, ignoring \n");
		return NULL;
	}
	mr = ibv_reg_mr(pd, addr, length, permission);
	if (!mr) {
		rdma_error("Failed to create mr on buffer, errno: %d \n", -errno);
		return NULL;
	}
	debug("Registered: %p , len: %u , stag: 0x%x \n", 
			mr->addr, 
			(unsigned int) mr->length, 
			mr->lkey);
	return mr;
#endif // _OLD_CODE

#ifdef _OLD_CODE2
	struct ib_mr *mr = NULL;
    u64 dma_addr;
	int page_list_len;
	struct scatterlist sg = {0};
	int ret = -1;

	void *addr = data_mr->buff;
	uint32_t length = sizeof data_mr->buff;

	if (!cb->pd) {
		rdma_error("Protection domain is NULL, ignoring \n");
		goto out;
	}

    // create memory region
	page_list_len = (((length - 1) & PAGE_MASK) + PAGE_SIZE)
				>> PAGE_SHIFT;
	mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, page_list_len);
	if (IS_ERR(mr)) {
		ret = PTR_ERR(mr);
		rdma_error("allocate mr failed %d\n", ret);
		goto out;
	}
	debug("mr registered: rkey 0x%x page_list_len %u\n",
		mr->rkey, page_list_len);

	// if (buf == (u64)cb->start_dma_addr)
	// 	cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
	// else
	// 	cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;

	dma_addr = ib_dma_map_single(cb->pd->device,
				   addr, length, DMA_BIDIRECTIONAL);
    if(ib_dma_mapping_error(cb->pd->device, dma_addr) == 0) {
		rdma_error("Failed to map buffer addr to dma addr, addr=%p, length=%d\n", addr, length);
		goto free_mr;
	}
	dma_unmap_addr_set(data_mr, dma_addr, dma_addr);
	dma_unmap_len_set(data_mr, len, length);

	sg_dma_address(&sg) = dma_addr;
	sg_dma_len(&sg) = length;

	ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
	if (ret != 1) {
		rdma_error("BUG: map mr to sg returns %d, should be the number of sg element\n", ret);
		goto unmap_dma_addr;
	}

	data_mr->mr = mr;

	debug("mr rkey 0x%x page size %u len %lu iova_start %llx\n",
		mr->rkey,
		mr->page_size,
		(unsigned long)mr->length,
		(unsigned long long)mr->iova);

    // mr = cb->pd->device->ops.get_dma_mr(cb->pd, IB_ACCESS_REMOTE_READ | 
    //                                  IB_ACCESS_REMOTE_WRITE | 
    //                                  IB_ACCESS_LOCAL_WRITE);
	// if (!mr) {
	// 	rdma_error("Error creating MR");
	// 	return NULL;
	// }

	// dma_addr = ib_dma_map_single(cb->pd->device,
	// 			   addr, length, DMA_BIDIRECTIONAL);
    // if(ib_dma_mapping_error(cb->pd->device, dma_addr) == 0) {
	// 	rdma_error("Failed to map buffer addr to dma addr\n");
	// 	return NULL;
	// }
	// dma_unmap_addr_set(cb, unmapping, dma_addr);

	// debug("Registered: %p , len: %u , stag: 0x%x \n", 
	// 		mr->addr, 
	// 		(unsigned int) mr->length, 
	// 		mr->lkey);

	return 0;

unmap_dma_addr:
	dma_unmap_single(cb->pd->device->dma_device,
			 dma_unmap_addr(data_mr, dma_addr),
			 dma_unmap_len(data_mr, len), 
			 DMA_BIDIRECTIONAL);

free_mr:
	if (mr && !IS_ERR(mr))
		ib_dereg_mr(mr);

out:
	return ret;
#endif // _OLD_CODE2
	return 0;
}

// static struct metadata_mr server_metadata_mr = {0};
// static struct rdma_buffer_attr server_metadata_attr = {0};

/* Pre-posts a receive buffer before calling rdma_connect () */
static int client_pre_post_recv_buffer(struct krdma_cb *cb)
{
	int ret = -1;
	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_wr = NULL;

#ifdef _OLD_CODE2
	server_metadata_mr.buff = &server_metadata_attr;
	ret = rdma_buffer_register(cb, &server_metadata_mr,
			(IB_ACCESS_LOCAL_WRITE));
	if(ret < 0){
		rdma_error("Failed to setup the server metadata mr\n");
		return -ENOMEM;
	}

	memset(server_recv_sge, 0, sizeof(struct ib_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	server_recv_sge[0].addr = (uint64_t) server_metadata_mr.dma_addr;
	server_recv_sge[0].length = (uint32_t) server_metadata_mr.len;
	server_recv_sge[0].lkey = (uint32_t) server_metadata_mr.mr->lkey;
	server_recv_wr.sg_list = server_recv_sge;
	server_recv_wr.num_sge = 1;
#endif // _OLD_CODE2

	memset(server_recv_sge, 0, sizeof(struct ib_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	server_recv_sge[0].addr = cb->mr.rw_mr.local_info->dma_addr;
	server_recv_sge[0].length = cb->mr.rw_mr.local_info->length;
	server_recv_sge[0].lkey = cb->mr.mr->lkey;
	server_recv_wr.sg_list = server_recv_sge;
	server_recv_wr.num_sge = 1;

	ret = ib_post_recv(cb->qp /* which QP */,
		      &server_recv_wr /* receive work request*/,
		      &bad_wr /* error WRs */);
	if (ret) {
		rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debug("Receive buffer pre-posting is successful \n");

	return 0;
}

static int client_connect_to_server(struct krdma_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret = -1;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 3;
	conn_param.initiator_depth = 3;
	conn_param.retry_count = 3;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		krdma_err("rdma_connect error %d\n", ret);
		return ret;
	}

	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_CONNECTED) {
		krdma_err("wait for KRDMA_CONNECTED state, but get %d\n", cb->state);
		goto exit;
	}

	debug("rdma_connect successful\n");
	return 0;

exit:
	return ret;
}

static int client_main(void * data) {
	int ret = 0;
	src = dst = NULL; 
	struct krdma_cb *cb;

	// TODO: server ip and port should read from *data

	src = kzalloc(strlen(test_string), GFP_KERNEL);
	if (!src) {
		rdma_error("Failed to allocate memory : -ENOMEM\n");
		return -ENOMEM;
	}
	/* Copy the passes arguments */
	strncpy(src, test_string, strlen(test_string));
	dst = kzalloc(strlen(test_string), GFP_KERNEL);
	if (!dst) {
		rdma_error("Failed to allocate destination memory, -ENOMEM\n");
		kfree(src);
		return -ENOMEM;
	}

	ret = __krdma_create_cb(&cb, KRDMA_CLIENT_CONN);
	if (ret) {
		rdma_error("__krdma_create_cb fail, ret %d\n", ret);
		return -ENOMEM;
	}

	/* Create cm_id */
	cb->cm_id = rdma_create_id(&init_net, krdma_cma_event_handler, cb,
					RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		rdma_error("rdma_create_id error %d\n", ret);
		return -EINVAL;
	}
	debug("created cm_id %p\n", cb->cm_id);

	ret = client_prepare_connection(cb);
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
	 }

	/* 
	* Allocate pd, cq, qp, mr, freed by caller
	*/
	ret = krdma_init_cb(cb);
	if (ret < 0)
		goto exit;

	ret = krdma_setup_mr(cb);
	if (ret < 0)
		goto free_cb;

	ret = client_pre_post_recv_buffer(cb); 
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto free_cb;
	}

	ret = client_connect_to_server(cb);
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}

	debug("hello");
	debug("server is %s\n", server);
	debug("src is %s\n", src);
	thread_running = true;
	while (!kthread_should_stop()) {
		msleep(1000);
		debug("sleeping");
	}

	if (check_src_dst()) {
		rdma_error("src and dst buffers do not match \n");
	} else {
		debug("...\nSUCCESS, source and destination buffers match \n");
	}

	// free up pd, cq, qp, and mr
	krdma_release_cb(cb);
	debug("quit");
	return 0;

free_cb:
	krdma_release_cb(cb);

exit:
	debug("error occurred, abort");
	thread_running = false;
	return -EINVAL;

#ifdef TEMP_DISABLED
	ret = client_connect_to_server();
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	ret = client_xchange_metadata_with_server();
	if (ret) {
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		return ret;
	}
	ret = client_remote_memory_ops();
	if (ret) {
		rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
		return ret;
	}
	if (check_src_dst()) {
		rdma_error("src and dst buffers do not match \n");
	} else {
		printf("...\nSUCCESS, source and destination buffers match \n");
	}
	ret = client_disconnect_and_clean();
	if (ret) {
		rdma_error("Failed to cleanly disconnect and clean up resources \n");
	}
	return ret;
#endif
	return 0;
}

/////////////////////////////////////////////////////////////

static struct task_struct *thread = NULL;

int __init kclient_init(void) {
	int ret;

	thread = kthread_run(client_main, NULL, "krdma_client");
	if (IS_ERR(thread)) {
		rdma_error("start thead failed.\n");
		ret = PTR_ERR(thread);
		return ret;
	}
    return 0;
}

void __exit kclient_exit(void) {
	int ret;
	if (thread_running) {
		send_sig(SIGKILL, thread, 1);
		ret = kthread_stop(thread);
		if (ret < 0) {
			rdma_error("kill thread failed.\n");
		}
	}
}

module_init(kclient_init);
module_exit(kclient_exit);
MODULE_AUTHOR("Leon Zhao <xleonzhao@gmail.com>");
MODULE_DESCRIPTION("RDMA client kernel module");
MODULE_LICENSE("GPLv2");
