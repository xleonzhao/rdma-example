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

// static struct sockaddr_in server_sockaddr = {0};
/* Source and Destination buffers, where RDMA operations source and sink */
static char *src = NULL, *dst = NULL; 
const char test_string[] = "hello, world";

/* This is our testing function */
static int check_src_dst(void) 
{
	return memcmp((void*) src, (void*) dst, strlen(src));
}

/* This function prepares client side connection resources for an RDMA connection */
static int client_init_rdma(struct krdma_cb *cb)
{
	int ret = -1;

	/* Create cm_id */
	cb->cm_id = rdma_create_id(&init_net, krdma_cma_event_handler, cb,
					RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		rdma_error("rdma_create_id error %d\n", ret);
		goto exit;
	}
	debug("created cm_id %p\n", cb->cm_id);

	ret = krdma_resolve_remote(cb, server, port);
	if (ret)
		goto exit;
	/* 
	* Allocate pd, cq, qp, mr, freed by caller
	*/
	ret = krdma_init_cb(cb);
	if (ret < 0)
		goto exit;

	ret = krdma_setup_mr(cb);
	if (ret < 0)
		goto exit;

	return 0;

exit:
	return ret;
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

static int client_init(void) {
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

	return 0;
}

/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
static int client_xchange_metadata_with_server(struct krdma_cb *cb)
{
	int ret = -1;
	// struct ibv_wc wc[2];
	// struct scatterlist sg[2];

	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_recv_wr = NULL;
	// dma_addr_t dma_addr;

	// get ready for receiving server info
	memset(server_recv_sge, 0, sizeof(struct ib_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	server_recv_sge[0].addr = cb->remote_info.dma_addr;
	server_recv_sge[0].length = cb->remote_info.length;
	server_recv_sge[0].lkey = cb->pd->unsafe_global_rkey;
	server_recv_wr.sg_list = server_recv_sge;
	server_recv_wr.num_sge = 1;

	ret = ib_post_recv(cb->qp /* which QP */,
		      &server_recv_wr /* receive work request*/,
		      &bad_recv_wr /* error WRs */);
	if (ret) {
		rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debug("Receive buffer posting is successful \n");

	// now preparing data to send to server
	struct ib_sge client_send_sge[1];
	struct ib_send_wr client_send_wr;
	const struct ib_send_wr *bad_send_wr = NULL;

	memset(client_send_sge, 0, sizeof(struct ib_sge));
	memset(&client_send_wr, 0, sizeof(client_send_wr));

	struct krdma_buffer_info *info = (struct krdma_buffer_info *)&(cb->remote_info.buf);
	info->dma_addr = cb->remote_info.dma_addr;
	info->size = cb->remote_info.length;
	info->rkey = cb->mr->rkey;

	client_send_sge[0].addr = cb->remote_info.dma_addr;
	client_send_sge[0].length = sizeof(struct krdma_buffer_info);
	client_send_sge[0].lkey = cb->mr->lkey;
	client_send_wr.sg_list = client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IB_WR_SEND;
	client_send_wr.send_flags = IB_SEND_SIGNALED;

	/* Now we post it */
	ret = ib_post_send(cb->qp, 
		    	&client_send_wr,
	       		&bad_send_wr);
	if (ret) {
		rdma_error("Failed to send client metadata, ret: %d \n", ret);
		return ret;
	}
	debug("Send posting is successful \n");

	/* at this point we are expecting 2 work completion. One for our 
	 * send and one for recv that we will get from the server for 
	 * its buffer information */
	wait_for_completion(&cb->cm_done);

	return 0;
}

static int client_main(void * data) {
	int ret = -1;
	src = dst = NULL; 
	struct krdma_cb *cb;

	if (client_init() != 0) {
		goto exit;
	}

	ret = krdma_alloc_cb(&cb, KRDMA_CLIENT_CONN);
	if (ret) {
		rdma_error("alloc cb fail, ret %d\n", ret);
		goto exit;
	}

	ret = client_init_rdma(cb);
	if (ret) { 
		rdma_error("Failed to initialize RDMA resources\n");
		goto exit;
	}

	ret = client_connect_to_server(cb);
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
	}

	ret = client_xchange_metadata_with_server(cb);
	if (ret) {
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
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
	krdma_free_cb(cb);
	debug("quit");
	return 0;

exit:
	if (cb)
		krdma_free_cb(cb);

	debug("error occurred, abort");
	thread_running = false;
	return -EINVAL;

#ifdef TEMP_DISABLED
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

static int __init kclient_init(void) {
	int ret;

	thread = kthread_run(client_main, NULL, "krdma_client");
	if (IS_ERR(thread)) {
		rdma_error("start thead failed.\n");
		ret = PTR_ERR(thread);
		return ret;
	}
    return 0;
}

static void __exit kclient_exit(void) {
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
