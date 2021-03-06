#include "nsf_master.h"

//守护进程服务器
static int nsf_srv_epoll;		//epoll套接字集合 
static int nsf_master_server;
static struct worker_mgr *nsf_worker_map;
static int nsf_worker_amount;
static struct nsf_master_module
{
	NsfftMsgproc cblk;
	int state;
}nsf_mod;

int nsf_fork_amount()
{
	return nsf_worker_amount;
}

int nsf_register_msgproc(NsfftMsgproc cblk)
{
	if(nsf_mod.state == 1)
		return 1;
	nsf_mod.cblk = cblk;
	nsf_mod.state = 1;
	return 0;
}

int nsf_create_mastersrv(int core)
{
	struct sockaddr_in addr_s;
	int sfd;
	nsf_mod.state = 0;
	nsf_worker_amount = core;
	nsf_worker_map = (struct worker_mgr *)malloc(core*sizeof(struct worker_mgr));
	memset(nsf_worker_map, 0, core*sizeof(struct worker_mgr));
	nsf_srv_epoll=epoll_create(core+2);
	
	sfd=socket(AF_INET,SOCK_STREAM,0);
	if(sfd==-1)
		return -1;
	bzero(&addr_s,sizeof(addr_s));
	addr_s.sin_family=AF_INET;
	addr_s.sin_addr.s_addr=htonl(INADDR_ANY);
	addr_s.sin_port=htons(12111);
	if(-1==bind(sfd,(struct sockaddr *)(&addr_s),sizeof(struct sockaddr)))
		return -2;  //bind fail;
	if(-1==listen(sfd,core+2))
		return -3;   //listen fail
	nsf_master_server = sfd;
	nsf_add_srvepoll(sfd);
	return sfd;	
}

void nsf_close_mastersrv()
{
	close(nsf_master_server);
}

//消息通知(master通知消息)，向某个工作进程通知一个消息
//如果pid=0，则向所有工作进程通知
void nsf_notification(int pid, struct nsf_notification_message msg)
{
	int i;
	for(i = 0; i < nsf_worker_amount; i++){
		if((nsf_worker_map[i].state != 0)
		&&(nsf_worker_map[i].nsf_worker_pid == pid
		||pid == 0)){
			if(msg.srcpid != nsf_worker_map[i].nsf_worker_pid)
				write(nsf_worker_map[i].nsf_worker_client, (char *)&msg, sizeof(msg));
		}
	}
}

/*把套接字加入到监听队列*/
void nsf_add_srvepoll(int fd)
{
	struct epoll_event ev;
	ev.data.fd = fd;
    ev.events = EPOLLIN;
    epoll_ctl(nsf_srv_epoll, EPOLL_CTL_ADD, fd, &ev);
}

/*从监听队列去掉套接字*/
int nsf_delete_srvepoll(int client)
{
	struct epoll_event event_del;  
	event_del.data.fd = client;
	event_del.events = 0;  
	epoll_ctl(nsf_srv_epoll, EPOLL_CTL_DEL, event_del.data.fd, &event_del);  
	return event_del.data.fd;
}

//epoll循环
void nsf_srvepoll()
{
	int i;
	int loop;
	socklen_t sin_size;	
	struct sockaddr_in addr_c;					//客户端地址
	struct epoll_event events[5];				//epoll事件集合
	struct nsf_notification_message msg;		//消息包
	int nfds = epoll_wait(nsf_srv_epoll, events, MAXEVENTS, 200);
	for(i = 0; i < nfds; i++){
		if(nsf_master_server==events[i].data.fd){
			//工作进程上线
			int cfd=accept(nsf_master_server, (struct sockaddr *)&addr_c, &sin_size);
			if(cfd == -1)
				continue;
			nsf_add_srvepoll(cfd);
			continue;
		}
		if(events[i].events&EPOLLIN){
			//工作进程消息
			memset(&msg, 0, sizeof(msg));
			int len=read(events[i].data.fd,&msg,sizeof(msg));
			if(len <= 0)
			{
				for(loop = 0; loop < nsf_worker_amount; loop++){
					if(events[loop].data.fd == nsf_worker_map[loop].nsf_worker_client){
						memset(&nsf_worker_map[loop], 0, sizeof(struct worker_mgr));
						break;
					}
				}
				close(nsf_delete_srvepoll(events[i].data.fd));
				continue;
			}
			msg.srcfd = events[i].data.fd;
			
			if(nsf_mod.state == 1){
				(nsf_mod.cblk)(msg);
			}
			nsf_default_msgproc(msg);
			continue;
		}
		
	}
}

void nsf_default_msgproc(struct nsf_notification_message msg)
{
	int i;
	char cmd[20];
	memset(cmd,0,20);
	int state[100];
	switch(msg.message)
	{
	case NM_CONNECT:
		for(i = 0; i < nsf_worker_amount; i++){
			if(nsf_worker_map[i].state == 0){
				nsf_worker_map[i].nsf_worker_pid = msg.srcpid;
				nsf_worker_map[i].nsf_worker_client = msg.srcfd;
				nsf_worker_map[i].state = 1;
				break;
			}
		}
		break;
	case NM_ADDUSER:
		for(i = 0; i < nsf_worker_amount; i++){
			if(nsf_worker_map[i].state == 1 
			&&nsf_worker_map[i].nsf_worker_pid == msg.srcpid){
				nsf_worker_map[i].nsf_client_amount++;
				break;
			}
		}
		break;
	case NM_DELUSER:
		for(i = 0; i < nsf_worker_amount; i++){
			if(nsf_worker_map[i].state == 1 
			&&nsf_worker_map[i].nsf_worker_pid == msg.srcpid){
				nsf_worker_map[i].nsf_client_amount--;
				break;
			}
		}
		break;
	case NM_NOTICE:
		nsf_notification(msg.destpid, msg);
		break;
	case NM_CMD:
		memcpy(cmd, msg.pkg.data, msg.pkg.datalen);
		if(strcmp(cmd, "state") == 0)
		{
			for(i = 0; i < nsf_worker_amount; i++){
				state[i*2] = nsf_worker_map[i].nsf_worker_pid;
				state[i*2+1] = nsf_worker_map[i].nsf_client_amount;
			}
			memcpy(msg.pkg.data, state, 2*nsf_worker_amount*sizeof(int));
			msg.pkg.datalen=2*nsf_worker_amount*sizeof(int);
			write(msg.srcfd, &msg,sizeof(msg));
		}

		if(strcmp(cmd, "remod") == 0)
		{
			msg.message = NM_REMOD;
			nsf_notification(0, msg);
		}
		break;
	case NM_CLOSE:
		nsf_stop_sys();			//停止重启服务
		kill(getpid(), SIGINT);
		break;
	default:
		break;
	}
}


