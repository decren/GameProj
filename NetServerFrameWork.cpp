#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <string.h>
using namespace std;

//可放入配置文件中
/*连接请求队列（或者端口监听队列）的最大长度(一般由2 到4),用somaxconn则由系统确定
min(backlog value in listen(), somaxconn set by no option)
设置somaxconn：
	echo 2048>/proc/sys/net/core/somaxconn  但重启系统后会被还原
	在/etc/sysctl.conf中添加如下
	net.core.somaxconn=2048
	然后在终端执行 sysctl -p
*/
#define LISTENQ 20

/*监听端口*/
#define LISTEN_PORT 6011

/*CS消息缓冲区大小*/
#define MSG_BUFF_SIZE 65535

/*最大连接数*/
#define MAX_EVENTS 10

/*每次能处理的最大事件数量*/
#define MAX_EVENTS_PER_FRAME 100

/*epoll从内核态返回用户态之前，等待IO事件发生的超时时间，单位:毫秒*/
#define EPOLL_WAIT_TIMES 1000

int g_EpollFd;

typedef void (*LogicCallback)(int fd, int events, void* arg);

struct MsgBuff
{
	int iLen;
	int s_offset;
	char chBuff[MSG_BUFF_SIZE];
	MsgBuff()
	{
		iLen = 0;
		s_offset = 0;
	}
};

struct myEvents
{
	int iFd;
	int iEvents;
	LogicCallback pCBFunc;
	int iStatus;//1:in epoll wait list,0 not in
	MsgBuff stMsgBuff;
	long last_active_time;
};

/*
g_Events[0]----g_Events[MAX_EVENTS -1]:used by client connect fd

g_Events[MAX_EVENTS] is used by listen fd
*/
myEvents g_Events[MAX_EVENTS + 1]; 

void RecvData(int iFd, int iEvents, void *arg);
void SendData(int iFd, int iEvents, void *arg);

int SetNonBlock(int iFd)
{
	int iFileStatusFlags = fcntl(iFd, F_GETFL);
	if(iFileStatusFlags < 0)
	{
		perror("fcntl(iFd,F_GETFL)");	
		return -1;
	}
	if( fcntl( iFd, F_SETFL, iFileStatusFlags|O_NONBLOCK) < 0)
	{
		perror("fcntl(iFd, F_SETFL, FileStatusFlags|O_NONBLOCK)");	
		return -2;
	}
	return 0;
}

void EventSet(myEvents *ev, int fd, LogicCallback pFn)
{
	printf("%s: fd[%d]\n", __FUNCTION__, fd);
	ev->iFd = fd;
	ev->pCBFunc = pFn;
	ev->iStatus=0;
	ev->stMsgBuff.s_offset = 0;
	ev->stMsgBuff.iLen = 0;
	ev->last_active_time = time(NULL);
}

//add/mod an event to epoll
int EventAdd(int iEpollFd, int iEvents, myEvents *ev)
{	
	struct epoll_event epv = {0,{0}};
	epv.data.ptr = ev;
	//使用边沿触发
	epv.events = ev->iEvents = iEvents|EPOLLET;
	int iOP;
	if(ev->iStatus == 1)
	{
		iOP = EPOLL_CTL_MOD;	
		printf("%s: EPOLL_CTL_MOD fd[%d]\n", __FUNCTION__, ev->iFd);
	}
	else
	{
		iOP = EPOLL_CTL_ADD;
		ev->iStatus = 1;
		printf("%s: EPOLL_CTL_ADD fd[%d]\n", __FUNCTION__, ev->iFd);
	}
	if(epoll_ctl( iEpollFd,iOP, ev->iFd, &epv ) < 0)
	{
		perror("epoll_ctl");	
		return -1;
	}
	return 0;
}

//delete an event from epoll
int EventDel( int iEpollFd, myEvents *ev)
{
	printf("%s: EPOLL_CTL_DEL fd[%d]\n", __FUNCTION__, ev->iFd);
	
	struct epoll_event epv = {0,{0}};
	if(ev->iStatus != 1)
	{
		printf("%s: fd[%d] invalid status %d\n", __FUNCTION__, ev->iFd, ev->iStatus);
		return 0;
	}
	epv.data.ptr = ev;
	ev->iStatus = 0;
	if(epoll_ctl(iEpollFd , EPOLL_CTL_DEL, ev->iFd, &epv) < 0)
	{
		perror("epoll_ctl EPOLL_CTL_DEL");	
		return -1;
	}
	else
	{
		printf("%s: EPOLL_CTL_DEL fd[%d] sucess\n", __FUNCTION__, ev->iFd);
	}
	return 0;
}

//LogicCallback
void AcceptConnect(int iListenFd, int iEvents, void* arg)
{
	printf("%s: begin listenfd[%d]\n", __FUNCTION__, iListenFd);
	int iRetCode = 0;
	struct sockaddr_in stAddrIn;
	socklen_t iAddrLen = sizeof(struct sockaddr_in);
	int iConnectFd;
	if((iConnectFd = accept(iListenFd,(struct sockaddr*)&stAddrIn,&iAddrLen)) == -1)
	{
		perror("accept error");
		return;
	}
	printf("%s: server accept fd[%d]\n", __FUNCTION__, iConnectFd);
	//选择空闲epoll_events
	int i = 0;
	for( i = 0; i < MAX_EVENTS; ++i)
	{
		if(g_Events[i].iStatus == 0)
		{
			break;
		}
	}
	if( i == MAX_EVENTS )
	{
		printf("%s: max connection limit[%d].", __FUNCTION__, MAX_EVENTS);
		close(iConnectFd);
		return;
	}
	if((iRetCode = SetNonBlock(iConnectFd)) != 0)
	{
		printf("%s: SetNonBlock failed:%d!", __FUNCTION__, iRetCode);
		return;
	}
	//add a read event for receive data
	EventSet(&g_Events[i], iConnectFd, RecvData);
	EventAdd(g_EpollFd, EPOLLIN, &g_Events[i]);
	printf("%s: end listenfd[%d]\n", __FUNCTION__, iListenFd);
	return;
}

//receive data
void RecvData(int iFd, int iEvents, void *arg)
{
	printf("%s: begin fd[%d]\n", __FUNCTION__, iFd);
	struct myEvents *ev = (struct myEvents*)arg;
	MsgBuff stRecvMsgBuff;
	int iRecvLen = recv(iFd, stRecvMsgBuff.chBuff+stRecvMsgBuff.iLen, 
		sizeof(stRecvMsgBuff.chBuff)-1-stRecvMsgBuff.iLen, 0);
	EventDel(g_EpollFd, ev);
	if(iRecvLen > 0)
	{
		stRecvMsgBuff.iLen += iRecvLen;	
		stRecvMsgBuff.chBuff[stRecvMsgBuff.iLen] = '\0';
		printf("%s: fd[%d] pos[%d] recv data[%s] len[%d].\n", 
			__FUNCTION__, iFd,ev-g_Events, stRecvMsgBuff.chBuff, stRecvMsgBuff.iLen);
		//change to send event
		EventSet(ev, iFd, SendData);
		ev->stMsgBuff.iLen = 
			snprintf(ev->stMsgBuff.chBuff, sizeof(ev->stMsgBuff.chBuff),
			"Server Res:%s",stRecvMsgBuff.chBuff);
		//ev->stMsgBuff = stRecvMsgBuff;
		EventAdd(g_EpollFd, EPOLLOUT, ev);
	}
	else if(iRecvLen == 0)
	{
		close(ev->iFd);
		printf("%s: fd[%d] pos[%d] closed gracefully.\n", __FUNCTION__, iFd,ev-g_Events);
	}
	else
	{
		close(ev->iFd);
		printf("%s: recv fd[%d] error[%d]:%s.\n", __FUNCTION__, iFd,errno, strerror(errno));
	}
	printf("%s: end fd[%d]\n", __FUNCTION__, iFd);
	return;
}

//send data
void SendData(int iFd, int iEvents, void *arg)
{
	printf("%s: begin fd[%d]\n", __FUNCTION__, iFd);
	struct myEvents *ev = (struct myEvents*)arg;
	int iSendLen = send(iFd, ev->stMsgBuff.chBuff + ev->stMsgBuff.s_offset, 
		ev->stMsgBuff.iLen - ev->stMsgBuff.s_offset,0);
	if( iSendLen > 0)
	{
		printf("send fd[%d], [%d<--->%d]%s \n",
			iFd, iSendLen, ev->stMsgBuff.iLen, ev->stMsgBuff.chBuff);	
		ev->stMsgBuff.s_offset += ev->stMsgBuff.iLen;
		if(ev->stMsgBuff.s_offset == ev->stMsgBuff.iLen)
		{
			//change to receive event
			EventDel(g_EpollFd, ev);
			EventSet(ev, iFd, RecvData);
			EventAdd(g_EpollFd, EPOLLIN, ev);
		}
	}
	else
	{		
		EventDel(g_EpollFd, ev);
		close(ev->iFd);
		printf("send fd[%d] error [%d] \n", iFd, errno);
	}
	printf("%s: end fd[%d]\n", __FUNCTION__, iFd);
	return;
}

int InitListenSocket(int iPort)
{
	int iListenFd = socket( AF_INET, SOCK_STREAM, 0 );
	//set non-blocking
	SetNonBlock(iListenFd);
	
	EventSet(&g_Events[MAX_EVENTS], iListenFd, AcceptConnect);
	//add listen socket
	EventAdd(g_EpollFd, EPOLLIN, &g_Events[MAX_EVENTS]);
	//bind & listen
	struct sockaddr_in stServerAddr;
	memset(&stServerAddr, 0 , sizeof(stServerAddr));
	stServerAddr.sin_family = AF_INET;
	stServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	stServerAddr.sin_port = htons(iPort);
	//bind and listen
	bind( iListenFd, (sockaddr *)&stServerAddr, sizeof(stServerAddr) );
	listen(iListenFd, LISTENQ);
	printf("%s: server listen fd[%d] port[%d]\n", __FUNCTION__, iListenFd, iPort);
	return iListenFd;
}
int main()
{
	g_EpollFd = epoll_create1(0);
	if( g_EpollFd <= 0)
	{
		perror("epoll_create1(0)");
		return -1;
	}
	InitListenSocket(LISTEN_PORT);

	//ev用于注册事件，events用于回传要处理的事件
	struct epoll_event stEvents[MAX_EVENTS];
	printf("server running:port[%d].\n", LISTEN_PORT);

	int iCheckPos = 0;
	while(1)
	{
		//timeout check every time 100
		long now = time(NULL);
		for(int i = 0; i < 100; ++i, ++iCheckPos)
		{
			if(iCheckPos == MAX_EVENTS)
			{
				iCheckPos = 0;
			}
			if(g_Events[iCheckPos].iStatus != 1)
			{
				//not in epoll wait list
				continue;
			}
			long duration = now - g_Events[iCheckPos].last_active_time;
			if(duration >= 60)//60s timeout
			{					
				printf("fd[%d] timeout[%d--%d].\n", 
					g_Events[iCheckPos].iFd, g_Events[iCheckPos].last_active_time, now);
				EventDel(g_EpollFd, &g_Events[iCheckPos]);
				close(g_Events[iCheckPos].iFd);
			}
		}

		//wait for events to happen
		int iReadyFdNum = epoll_wait(g_EpollFd, stEvents, MAX_EVENTS_PER_FRAME, EPOLL_WAIT_TIMES);
		if(iReadyFdNum < 0)
		{
			perror("epoll_wait error,exit\n");	
			return -1;
		}
		//disp events
		for(int i = 0; i < iReadyFdNum; ++i)
		{
			myEvents *ev = (struct myEvents*)stEvents[i].data.ptr;	
			if((stEvents[i].events&EPOLLIN)&&(ev->iEvents&EPOLLIN))//read event
			{
				printf("%s: EPOLLIN fd[%d].\n", __FUNCTION__, ev->iFd);
				if(ev->pCBFunc == NULL)
				{
					//默认读取数据
					ev->pCBFunc = RecvData;
				}				
			}
			if((stEvents[i].events&EPOLLOUT)&&(ev->iEvents&EPOLLOUT))//write event
			{
				printf("%s: EPOLLOUT fd[%d].\n", __FUNCTION__, ev->iFd);
				if(ev->pCBFunc == NULL)
				{
					//默认写数据
					ev->pCBFunc = SendData;
				}
			}
			ev->pCBFunc(ev->iFd, stEvents[i].events, ev);
		}
	}
	return 0;
}
