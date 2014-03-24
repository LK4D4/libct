/*
 * Daemon that gets requests from remote library backend
 * and forwards them to local session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include "uapi/libct.h"
#include "list.h"
#include "xmalloc.h"
#include "../protobuf/rpc.pb-c.h"

#define MAX_MSG_ONSTACK	2048

struct container_srv {
	struct list_head l;
	unsigned long rid;
	ct_handler_t hnd;
};

static LIST_HEAD(ct_srvs);
static unsigned long rids = 1;

static int serve_ct_create(int sk, libct_session_t ses, CreateReq *req)
{
	int len;
	unsigned char dbuf[MAX_MSG_ONSTACK];
	struct container_srv *cs;
	RpcResponce resp = RPC_RESPONCE__INIT;
	CreateResp cr = CREATE_RESP__INIT;

	cs = xmalloc(sizeof(*cs));
	if (!cs)
		return -1;

	cs->hnd = libct_container_create(ses);
	if (!cs->hnd) {
		xfree(cs);
		return -1;
	}

	cs->rid = rids++;

	resp.create = &cr;
	cr.rid = cs->rid;

	/* FIXME -- boundaries check */
	len = rpc_responce__pack(&resp, dbuf);
	if (len <= 0)
		goto err;
	if (send(sk, dbuf, len, 0) != len)
		goto err;

	list_add_tail(&cs->l, &ct_srvs);
	return 0;

err:
	libct_container_destroy(cs->hnd);
	xfree(cs);
	return -1;
}

static int serve_req(int sk, libct_session_t ses, RpcRequest *req)
{
	switch (req->req) {
	case REQ_TYPE__CT_CREATE:
		return serve_ct_create(sk, ses, req->create);
	default:
		break;
	}

	return -1;
}

static int serve(int sk)
{
	RpcRequest *req;
	int ret;
	unsigned char dbuf[MAX_MSG_ONSTACK];
	libct_session_t ses;

	ses = libct_session_open_local();
	if (!ses)
		return -1;

	while (1) {
		ret = recv(sk, dbuf, MAX_MSG_ONSTACK, 0);
		if (ret <= 0)
			break;

		req = rpc_request__unpack(NULL, ret, dbuf);
		if (!req) {
			ret = -1;
			break;
		}

		ret = serve_req(sk, ses, req);
		rpc_request__free_unpacked(req, NULL);

		if (ret < 0)
			break;
	}

	libct_session_close(ses);
	return ret;
}

int main(int argc, char **argv)
{
	int sk;
	struct sockaddr_un addr;
	socklen_t alen;

	sk = socket(PF_UNIX, SOCK_SEQPACKET, 0);
	if (sk == -1)
		goto err;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path));
	alen = strlen(addr.sun_path) + sizeof(addr.sun_family);

	unlink(addr.sun_path);
	if (bind(sk, (struct sockaddr *)&addr, alen))
		goto err;

	if (listen(sk, 16))
		goto err;

	signal(SIGCHLD, SIG_IGN); /* auto-kill zombies */

	while (1) {
		int ask;

		alen = sizeof(addr);
		ask = accept(sk, (struct sockaddr *)&addr, &alen);
		if (ask < 0)
			continue;

		if (fork() == 0) {
			int ret;

			ret = serve(ask);
			if (ret < 0)
				ret = -ret;

			close(ask);
			exit(ret);
		}

		close(ask);
	}

err:
	return 1;
}