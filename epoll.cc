#include "epoll.h"

#include <string>
#include <cstring>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <utility>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#include "assert.h"
#include "socket_buffer.h"
#include "util.h"

namespace TCP_SERVER
{
const static int BUF_SIZE = 10240;
void Proc(int, boost::shared_ptr<SocketBuffer>, boost::shared_ptr<SocketBuffer>, boost::function<void()>);
void EpollCtl(int, int, int, struct epoll_event *);

void Epoll::Run()
{
	int res = -1;
	for(;;){
		res = epoll_wait(this->epfd_, this->events_, this->max_events_, -1);
		
		if (res < 0){
            throw SocketException("epoll_wait failed!");
        } else if (res == 0) {
            continue;
        }
        
        int i = 0;
        for (; i < res; ++i){
        	if ( (this->events_[i].events & EPOLLERR) || 
                 (this->events_[i].events & EPOLLHUP) ||
                 (!this->events_[i].events & EPOLLIN) ||
                 (!this->events_[i].events & EPOLLOUT)){
                //fprintf (stderr, "epoll error\n");
                close(this->events_[i].data.fd);
                continue;
            }
            
            if(this->events_[i].data.fd == this->serv_sock_){
            	/*Connection arrived*/
            	int conn_sock;
        		struct sockaddr_in client_addr;
				socklen_t client_addr_len;
				
				for(;;){
					client_addr_len = sizeof(client_addr);
					conn_sock = accept(this->serv_sock_, (struct sockaddr *) &client_addr, &client_addr_len);
				
					if (conn_sock == -1) {
						if ( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) {
						    /*non-blocking模式下无新connection请求，跳出for(;;)*/
						    break;
						} else {
						    ASSERT(false, __FILE__ , __LINE__, "");
						}
					}
					
					SetSockNonBlock(conn_sock);
					
					struct epoll_event event;
					event.events = EPOLLIN;
                    event.data.fd = conn_sock;
                    
                    EpollCtl(this->epfd_, EPOLL_CTL_ADD, conn_sock, &event);
                   
                    SocketBuffer *ptr_buf = new SocketBuffer();
                    //this->recv_buffer_.insert(std::pair<int, boost::shared_ptr<SocketBuffer> >(
                    //								conn_sock, 
                    //								boost::shared_ptr<SocketBuffer>(ptr_buf)));
                    this->recv_buffer_[conn_sock] = boost::shared_ptr<SocketBuffer>(ptr_buf);
                }
            }
            else if((this->events_[i].events & EPOLLIN)){
            	/*Recveive event.*/
            	int recv_sock;
            	int recv_size;
            	recv_sock = this->events_[i].data.fd;
            	char buffer[BUF_SIZE];
                memset(buffer, 0, sizeof(buffer));
                
                if ((recv_size = recv(recv_sock, buffer, BUF_SIZE, 0)) == -1  
                		&& ((errno != EAGAIN)&&(errno != EWOULDBLOCK))) {
                    /*recv在non-blocking模式下，返回-1且errno为EAGAIN或EWOULDBLOCK表示当前无可读数据，并不表示错误*/
                    //TODO
                    Socket sock(recv_sock);
                	if(0 == this->recv_buffer_.erase(sock)){
                		ASSERT(false, __FILE__ , __LINE__, "");
                	}
                	EpollCtl(this->epfd_, EPOLL_CTL_DEL, sock, NULL);
                }
                else if(recv_size == 0){
                	/*Connection closed.*/
                	Socket sock(recv_sock);
                	if(0 == this->recv_buffer_.erase(sock)){
                		ASSERT(false, __FILE__ , __LINE__, "");
                	}
                	EpollCtl(this->epfd_, EPOLL_CTL_DEL, sock, NULL);
                }
                else if(recv_size < BUF_SIZE){
                	std::unordered_map<int, boost::shared_ptr<SocketBuffer> >::const_iterator iter 
                			= this->recv_buffer_.find(recv_sock);
                	
                	if(iter != this->recv_buffer_.end()){
                		iter->second->Append(buffer);
                	}else{
                		ASSERT(false, __FILE__ , __LINE__, "");
                	}
                	               	
                	SocketBuffer *ptr_buf = new SocketBuffer();
                   	this->send_buffer_[recv_sock] = boost::shared_ptr<SocketBuffer>(ptr_buf);
                	
                	boost::function<void()> callback = boost::bind(
                							&EpollCtl, 
                							this->epfd_,
                							EPOLL_CTL_MOD,
                							recv_sock,
                							(epoll_event*)(EPOLLOUT | EPOLLET));

                	boost::function<void()> proc = boost::bind(
                							&Proc,
                							recv_sock,
                							this->recv_buffer_[recv_sock],
                							this->send_buffer_[recv_sock],
                							callback);
                							
                	this->thread_pool_ptr_->schedule(proc);
                	
                	if(0 == this->recv_buffer_.erase(recv_sock)){
                		ASSERT(false, __FILE__ , __LINE__, "");
                	}
                }
                else{
                	std::unordered_map<int, boost::shared_ptr<SocketBuffer> >::const_iterator \
                			iter = this->recv_buffer_.find(recv_sock);
                	
                	if(iter != this->recv_buffer_.end()){
                		iter->second->Append(buffer);
                	}else{
                		ASSERT(false, __FILE__ , __LINE__, "");
                	}
                }
            }
            else if((this->events_[i].events & EPOLLOUT)){
            	/*Send event.*/
            	int send_sock = this->events_[i].data.fd;
            	int send_size;
            	int buf_size = 0;
            	const char* buffer = 0;
                std::unordered_map<int, boost::shared_ptr<SocketBuffer> >::const_iterator iter 
            			= this->send_buffer_.find(send_sock);
            	if(iter != this->send_buffer_.end()){
            		buffer = iter->second->toString();
            		buf_size = strlen(buffer);
            	}else{
            		ASSERT(false, __FILE__ , __LINE__, "");
            	}
            	
            	if ((send_size = send(send_sock, buffer, buf_size, 0)) == -1 && 
            		(errno != EAGAIN) && 
            		(errno != EWOULDBLOCK)){
            		/*send在non-blocking模式下，返回-1且errno为EAGAIN或EWOULDBLOCK表示当前无可写数据，并不表示错误*/
                   	//TODO
                }
                else if(send_size != buf_size){
                	ASSERT(false, __FILE__ , __LINE__, "");
                }

                Socket sock(send_sock);
            	if(0 == this->send_buffer_.erase(sock)){
            		ASSERT(false, __FILE__ , __LINE__, "");
            	}
            	EpollCtl(this->epfd_, EPOLL_CTL_DEL, sock, NULL);
            }
            else{
            	ASSERT(false, __FILE__ , __LINE__, "");
            }
        }
	}
}

void Proc(	int fd, 
			boost::shared_ptr<SocketBuffer> recv_buf_ptr, 
			boost::shared_ptr<SocketBuffer> send_buf_ptr, 
			boost::function<void()> callback)
{
	send_buf_ptr = recv_buf_ptr;
	callback();
}

void EpollCtl(int epfd, int op, int fd, struct epoll_event *event)
{
	if(-1 == epoll_ctl(epfd, op, fd, event)){
		ASSERT(false, __FILE__ , __LINE__, "");
		//TODO:throw exception.
	}
}

}
