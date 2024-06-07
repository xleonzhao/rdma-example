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
	int ret = -1;

	ret = krdma_resolve_remote(cb, server, port);
	if (ret)
		return ret;

	return 0;
}

/* Pre-posts a receive buffer before calling rdma_connect () */
static int client_pre_post_recv_buffer(struct krdma_cb *cb)
{
	int ret = -1;
	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_wr = NULL;

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
