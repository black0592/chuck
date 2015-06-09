#include <assert.h>
#include "socket/socket.h"
#include "engine/engine.h"
#include "socket/socket_helper.h"

extern int32_t is_read_enable(handle*h);
extern int32_t is_write_enable(handle*h);
extern void    release_socket(socket_ *s);

typedef void(*stream_callback)(stream_socket_*,void*,int32_t,int32_t);

static int32_t 
imp_engine_add(engine *e,handle *h,
			   generic_callback callback)
{
	assert(e && h && callback);
	if(h->e) return -EASSENG;
	int32_t ret;
#ifdef _LINUX
	ret = event_add(e,h,EVENT_READ|EVENT_WRITE);
#elif   _BSD
	ret = event_add(e,h,EVENT_READ) || event_add(e,h,EVENT_WRITE);
#else
	return -EUNSPPLAT;
#endif
	if(ret == 0){
		easy_noblock(h->fd,1);
		((stream_socket_*)h)->callback = (stream_callback)callback;
	}
	return ret;
}

 
static void 
process_read(stream_socket_ *s)
{
	iorequest *req = NULL;
	int32_t bytes = 0;
	s->status |= SOCKET_READABLE;
	while((req = (iorequest*)list_pop(&s->pending_recv))!=NULL){
		errno = 0;
		bytes = TEMP_FAILURE_RETRY(readv(((handle*)s)->fd,req->iovec,req->iovec_count));	
		if(bytes < 0 && errno == EAGAIN){
				s->status ^= SOCKET_READABLE;
				//将请求重新放回到队列
				list_pushback(&s->pending_recv,(listnode*)req);
				break;
		}else{
			s->callback(s,req,bytes,errno);
			if(s->status & SOCKET_CLOSE)
				return;
			if(!s->e || !(s->status & SOCKET_READABLE))
				break;			
		}
	}	
	if(s->e && !list_size(&s->pending_recv)){
		//没有接收请求了,取消EPOLLIN
		disable_read((handle*)s);
	}	
}

static void 
process_write(stream_socket_ *s)
{
	iorequest *req = 0;
	int32_t bytes = 0;
	s->status |= SOCKET_WRITEABLE;
	while((req = (iorequest*)list_pop(&s->pending_send))!=NULL){
		errno = 0;	
		bytes = TEMP_FAILURE_RETRY(writev(s->fd,req->iovec,req->iovec_count));
		if(bytes < 0 && errno == EAGAIN){
				s->status ^= SOCKET_WRITEABLE;
				//将请求重新放回到队列
				list_pushback(&s->pending_send,(listnode*)req);
				break;
		}else{
			s->callback(s,req,bytes,errno);
			if(s->status & SOCKET_CLOSE)
				return;
			if(!s->e || !(s->status & SOCKET_WRITEABLE))
				break;				
		}
	}
	if(s->e && !list_size(&s->pending_send)){
		//没有接收请求了,取消EPOLLOUT
		disable_write((handle*)s);
	}		
}

static void 
on_events(handle *h,int32_t events)
{
	stream_socket_ *s = (stream_socket_*)h;
	if(!s->e || s->status & SOCKET_CLOSE)
		return;
	if(events == EENGCLOSE){
		s->callback(s,NULL,-1,EENGCLOSE);
		return;
	}
	do{
		s->status |= SOCKET_INLOOP;
		if(events & EVENT_READ){
			process_read(s);	
			if(s->status & SOCKET_CLOSE) 
				break;								
		}		
		if(s->e && (events & EVENT_WRITE))
			process_write(s);			
		s->status ^= SOCKET_INLOOP;
	}while(0);
	if(s->status & SOCKET_CLOSE){
		release_socket((socket_*)s);		
	}
}


void    
stream_socket_init(stream_socket_ *s,int32_t fd)
{
	s->on_events = on_events;
	s->imp_engine_add = imp_engine_add;
	s->type = STREAM;
	s->fd = fd;
	s->on_events = on_events;
	s->imp_engine_add = imp_engine_add;
	s->status = SOCKET_READABLE | SOCKET_WRITEABLE;
	easy_close_on_exec(fd);		
}	

stream_socket_*
new_stream_socket(int32_t fd)
{
	stream_socket_ *s = calloc(1,sizeof(*s));
	stream_socket_init(s,fd);
	return s;
}

int32_t 
stream_socket_recv(stream_socket_ *s,
				   iorequest *req,int32_t flag)
{
	handle *h = (handle*)s;
	if(s->status & SOCKET_CLOSE)
		return -ESOCKCLOSE;
	else if(!h->e)
		return -ENOASSENG;
	errno = 0;
	if(s->status & SOCKET_READABLE && 
	   flag == IO_NOW && 
	   !list_size(&s->pending_recv))
	{
		int32_t bytes = TEMP_FAILURE_RETRY(readv(h->fd,req->iovec,req->iovec_count));
		if(bytes >= 0)
			return bytes;
		else if(errno != EAGAIN)
			return -errno;
	}
	s->status ^= SOCKET_READABLE;
	list_pushback(&s->pending_recv,(listnode*)req);
	if(!is_read_enable(h)) enable_read(h);
	return -EAGAIN;	
}

int32_t 
stream_socket_send(stream_socket_ *s,
				   iorequest *req,int32_t flag)
{
	handle *h = (handle*)s;
	if(s->status & SOCKET_CLOSE)
		return -ESOCKCLOSE;
	else if(!h->e)
		return -ENOASSENG;

	errno = 0;
	if(s->status & SOCKET_WRITEABLE && 
	   flag == IO_NOW && 
	   !list_size(&s->pending_send))
	{
		int32_t bytes = TEMP_FAILURE_RETRY(writev(h->fd,req->iovec,req->iovec_count));
		if(bytes >= 0)
			return bytes;
		else if(errno != EAGAIN)
			return -errno;
	}
	s->status ^= SOCKET_WRITEABLE;
	list_pushback(&s->pending_send,(listnode*)req);
	if(!is_write_enable(h)) enable_write(h);
	return -EAGAIN;	
}