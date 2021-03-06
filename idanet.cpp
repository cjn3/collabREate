/*
    Asynchronous IDA communications handler
    Copyright (C) 2008 Chris Eagle <cseagle at gmail d0t com>
    Copyright (C) 2008 Tim Vidas <tvidas at gmail d0t com>


    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
    more details.

    You should have received a copy of the GNU General Public License along with
    this program; if not, write to the Free Software Foundation, Inc., 59 Temple
    Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifdef _WIN32
#ifndef _MSC_VER
#include <windows.h>
#endif
#include <winsock2.h>
#endif
#include <pro.h>

#include <ida.hpp>
#include <idp.hpp>
#include <kernwin.hpp>
#include <loader.hpp>
#include <nalt.hpp>
#include <md5.h>

#ifdef _MSC_VER
#if _MSC_VER >= 1600
#include <stdint.h>
#else
#include "ms_stdint.h"
#endif
#else
#include <stdint.h>
#endif

#include "collabreate.h"
#include "sdk_versions.h"
#include "idanet.hpp"

#if IDA_SDK_VERSION < 500
#include <fpro.h>
#endif

//array to track send and receive stats for all of the collabreate commands
extern int stats[2][MSG_IDA_MAX + 1];

#if IDA_SDK_VERSION < 550

#ifndef HWND_MESSAGE
#define HWND_MESSAGE ((HWND)(-3))
#endif

#define SOCKET_MSG WM_USER
#define PLUGIN_NAME "collabREate"

static SOCKET conn = INVALID_SOCKET;
static HWND msg_hwnd;
static WNDPROC old_proc;
static Dispatcher dispatch;

#else

#ifndef _WIN32
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#define closesocket close
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

struct BufferNode {
   Buffer *buf;
   BufferNode *next;

   BufferNode(Buffer *b) : buf(b), next(NULL) {}; 
};

struct BufferList {
   BufferNode *head;
   BufferNode *tail;
   qmutex_t mtx;

   BufferList();

   Buffer *dequeue();
   bool enqueue(Buffer *b);
};

struct disp_request_t : public exec_request_t {
   disp_request_t(Dispatcher disp) : d(disp) {};
   virtual int idaapi execute(void);

   //disp_requst_t takes ownership of the buffer
   //it will be deleted eventually in execute
   void queueBuffer(Buffer *b);

   void flush(void);

   BufferList buffers;
   Dispatcher d;
};

class AsyncSocket {
public:
   AsyncSocket(Dispatcher disp);
   bool isConnected();
   bool connect(const char *host, short port);
   bool close();
   void cleanup(bool warn = false);
   bool sendAll(Buffer &b);
   bool send(Buffer &b);
   int recv(unsigned char *buf, unsigned int len);
private:
#ifdef _WIN32
   HANDLE thread;
   static DWORD WINAPI recvHandler(void *sock);
#else
   pthread_t thread;
   static void *recvHandler(void *sock);
#endif
   Dispatcher d;
   disp_request_t *drt;
   _SOCKET conn;
   static bool initNetwork();
};

static AsyncSocket *comm;

#endif

//how large is the current data packet under construction
int requiredSize(Buffer &b) {
   if (b.size() >= (int)sizeof(int)) {
      return qntohl(*(int*)b.get_buf());
   }
   return -1;
}

//does the buffer contain a complete data packet?
bool isComplete(Buffer &b) {
   int rs = requiredSize(b);
   return rs > 0 && b.size() >= rs;
}

//shift the content of a buffer left by one data packet
void shift(Buffer &b) {
   if (isComplete(b)) {
      uint32_t rs = requiredSize(b);
      uint32_t extra = b.size() - rs;
      const unsigned char *buf = b.get_buf();
      b.reset();
      if (extra) {
         b.write(buf + rs, extra);
      }
   }
}

//shift the content of a buffer left by len bytes
void shift(Buffer &b, int len) {
   if (len <= b.size()) {
      int extra = b.size() - len;
      const unsigned char *buf = b.get_buf();
      b.reset();
      if (extra) {
         b.write(buf + len, extra);
      }
   }
}

bool init_network() {
   static bool isInit = false;
   if (!isInit) {
#ifdef _WIN32
   //initialize winsock.
      WSADATA wsock;
      if (WSAStartup(MAKEWORD(2, 2), &wsock) != 0) {
         msg(PLUGIN_NAME": initNetwork() failed.\n");
      }
      //check requested version
      else if (LOBYTE(wsock.wVersion) != 2 || HIBYTE(wsock.wVersion) != 2) {
//         WSACleanup();
         msg(PLUGIN_NAME": Winsock version 2.2 not found.\n");
      }
      else {
         isInit = true;
      }
#else
      isInit = true;
#endif

   }
   return isInit;
}

//connect to a remote host as specified by host and port
//host may be wither an ip address or a host name
_SOCKET connect_to(const char *host, short port) {
   _SOCKET sock;
   sockaddr_in server;
   memset(&server, 0, sizeof(server));
   server.sin_family = AF_INET;
   server.sin_addr.s_addr = inet_addr(host);
   server.sin_port = qhtons(port);

   //If a domain name was specified, we may not have an IP.
   if (server.sin_addr.s_addr == INADDR_NONE) {
      hostent *he = gethostbyname(host);
      if (he == NULL) {
         msg(PLUGIN_NAME": Unable to resolve name: %s\n", host);
         return INVALID_SOCKET;
      }
      server.sin_addr = *(in_addr*) he->h_addr;
   }

   //create a socket.
   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) != INVALID_SOCKET) {
      if (connect(sock, (sockaddr *) &server, sizeof(server)) == SOCKET_ERROR) {
         msg(PLUGIN_NAME": Failed to connect to server.\n");
         closesocket(sock);
         sock = INVALID_SOCKET;
      }
      else {
#ifdef _WIN32
         DWORD tv = 2000;
#else
         timeval tv;
         tv.tv_sec = 2;
         tv.tv_usec = 0;
#endif
         //we force a periodic timeout to force a recv error after
         //the socket has been closed. On windows, simply closing 
         //the socket causes a blocking recv to fail and the recvHandler
         //thread to terminate. On Linux, closing the socket was not
         //causing the blocking recv to terminate, hence the timeout
         //following a timeout, if the socket has been closed, the next
         //receive will fail. Not elegant but it works
         setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
      }
   }
   else {
      msg(PLUGIN_NAME": Failed to create socket.\n");
   }
      
   return sock;
}

void killWindow() {
#if IDA_SDK_VERSION < 550
   if (msg_hwnd) {
      DestroyWindow(msg_hwnd);
      msg_hwnd = NULL;
   }
#endif
}

bool term_network() {
#ifdef _WIN32
   killWindow();
//   return WSACleanup() == 0;
#endif
   return true;
}

#if IDA_SDK_VERSION < 550 

bool is_connected() {
   return conn != INVALID_SOCKET;
}

//buffer to cache data in the case WSAEWOULDBLOCK
static Buffer sendBuf;

/*
 * socket_callback()
 *
 * this is the proc handler we register with our invisible window for hooking the
 * socket notification messages.
 *
 * returns:   boolean value representing success or failure.
 */
BOOL CALLBACK socket_callback(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam) {
   if (message == SOCKET_MSG) {
      if (WSAGETSELECTERROR(lparam)) {
         msg(PLUGIN_NAME": connection to server severed at WSAGETSELECTERROR %d.\n", WSAGetLastError());
         cleanup(true);
         return FALSE;
      }
      switch(WSAGETSELECTEVENT(lparam)) {
         case FD_READ:   //receiving data.
            //msg(PLUGIN_NAME": receiving data.\n");
            if (dispatch) {
               static Buffer b;
               char buf[2048];  //read a large chunk, we'll be notified if there is more
               int len = recv(conn, buf, sizeof(buf), 0);
               //connection closed.
               if (len <= 0) {
                  cleanup();
                  msg(PLUGIN_NAME": Socket read failed. connection closed. %d\n", WSAGetLastError());
                  return false;
               }
               //msg(PLUGIN_NAME": received: %d bytes \n", len);
               b.write(buf, len);   //copy new data into static buffer
               //now dispatch any complete data packets to user dispatcher
               //it is important to understand that the recv above may receive 
               //partial data packets
               //if (isComplete(b)) {
               //      msg(PLUGIN_NAME": b is complete.\n");
               //}
               //else {
               //      msg(PLUGIN_NAME": b is not compelete.\n");
               //}
               while (isComplete(b)) {
                  Buffer data(b.get_buf() + sizeof(int), requiredSize(b) - sizeof(int));
                  //msg("dispatching a %d sized buffer (expected %d out of %d)\n", data.size(), requiredSize(b) - sizeof(int), b.size());
                  if (!(*dispatch)(data)) {  //not sure we really care what is returned here
                     msg(PLUGIN_NAME": connection to server severed at dispatch.\n");
                     cleanup(true);
                     break;
                  }
                  else {
                     //msg(PLUGIN_NAME": dispatch routine called successfully.\n");
                  }
                  shift(b);  //shift any remaining portions of the buffer to the front
               }
            }
            break;
         case FD_WRITE: {   //sending data.
            //msg(PLUGIN_NAME": writing data.\n");
            if (sendBuf.size() == 0) break;  //nothing to send
            int len = send(conn, (const char*)sendBuf.get_buf(), sendBuf.size(), 0);
            //remember, send is not guaranteed to send complete buffer
            if (len == SOCKET_ERROR) {
               int error = WSAGetLastError();
               if (error != WSAEWOULDBLOCK) {
                  cleanup(true);
               }
            }
            else if (len != sendBuf.size()) {
               //partial read, so shift remainder of buffer to front
               shift(sendBuf, (uint32_t)len);
               //msg(PLUGIN_NAME": wrote: %d bytes \n", len);
            }
            else {
               //entire buffer was sent, so clear the buffer
               sendBuf.reset();
               //msg(PLUGIN_NAME": wrote: %d bytes \n", len);
            }
            break;
         }
         case FD_CLOSE:  //server connection closed.
            cleanup(true);
            msg(PLUGIN_NAME": connection to server severed at FD_CLOSE.\n");
            break;
      }
   }
   return FALSE;
}

/////////////////////////////////////////////////////////////////////////////////////////
//cleanup(bool warn)
//
//cancel all notifications, close the socket and destroy the hook notification window.
//
//arguments: warn true displays a warning that cleanup is being called, false no warning
//returns:   none.
//
void cleanup(bool warn) {
   //cancel all notifications. if we don't do this ida will crash on exit.
   msg(PLUGIN_NAME": cleanup called.\n");
   if (conn != INVALID_SOCKET) {
      if (msg_hwnd) {
         WSAAsyncSelect(conn, msg_hwnd, 0, 0);
         dispatch = NULL;
      }
      closesocket(conn);
      conn = INVALID_SOCKET;
      if (warn) {
         warning("Connection to collabREate server has been closed.\n"
                 "You should reconnect to the server before sending\n"
                 "additional updates.");
      }
   }
}

//create a window for the async socket, this window
//receives the WM_xxx messages associated with the socket
bool createSocketWindow(_SOCKET s, Dispatcher d) {
   //create a message handling window for the async socket.
   msg_hwnd = CreateWindowEx(0, "STATIC", PLUGIN_NAME, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
   if (msg_hwnd == NULL) {
      msg(PLUGIN_NAME": CreateWindowEx() failed.\n");
      return false;
   }
   
   //register the callback function for our invisible window.
   old_proc = (WNDPROC)SetWindowLong(msg_hwnd, GWL_WNDPROC, (long) socket_callback);
   if (old_proc == 0) {
      killWindow();
      msg(PLUGIN_NAME": SetWindowLong() failed.\n");
      return false;
   }

   conn = s;
   dispatch = d;
   
   //make the socket a non-blocking asynchronous socket hooked with our socket_callback handler.
   if (WSAAsyncSelect(conn, msg_hwnd, SOCKET_MSG, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
      killWindow();
      dispatch = NULL;
      conn = INVALID_SOCKET;
      msg(PLUGIN_NAME": Failed to create asynchronous connection to server.\n");
      return false;
   }
   //asynchronous socket properly configured
#ifdef DEBUG 
   msg(PLUGIN_NAME": Successfully configured async socket\n");
#endif
   return true;
}

int send_all(Buffer &b) {
   int len = send(conn, (const char*)b.get_buf(), b.size(), 0);
   if (len == SOCKET_ERROR) {
      int error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK) {
         sendBuf << b;
         return 0;
      }
      else {
         cleanup();
         killWindow();
         msg(PLUGIN_NAME": Failed to send requested data. %d != %d. Error: %x, %d\n", len, b.size(), error, error);
         return -1;
      }
   }
   else if (len != b.size()) {
      //move the remainder into sendBuf
      shift(b, len);
      sendBuf << b;
      //msg(PLUGIN_NAME": Short send. %d != %d.", len, out.size());
   }
   return len;
}

//Send a buffer of data
int send_data(Buffer &b) {
//   if (!is_connected() || supress) return 0;   //silently fail
   if (!is_connected()) {
      if (changeCache != NULL) {
//         msg("writing to change cache\n");
         changeCache->writeInt(b.size() + sizeof(int));
         *changeCache << b;
      }
      return b.size();
   }
   Buffer out;
   int sz = b.size() + sizeof(int);
   out.writeInt(sz);
   int command = b.readInt();
   if (command >= 0 && command <= MSG_IDA_MAX) {
      stats[1][command]++;
   }
//   msg("send_data sending message: %d\n", command);
   out << b;
   return send_all(out);
/*
   int len = send(conn, (const char*)out.get_buf(), out.size(), 0);
   if (len == SOCKET_ERROR) {
      int error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK) {
         sendBuf << out;
         return 0;
      }
      else {
         cleanup();
         killWindow();
         msg(PLUGIN_NAME": Failed to send requested data. %d != %d. Error: %x, %d\n", len, out.size(), error, error);
         return -1;
      }
   }
   else if (len != out.size()) {
      //move the remainder into sendBuf
      shift(out, len);
      sendBuf << out;
      //msg(PLUGIN_NAME": Short send. %d != %d.", len, out.size());
   }
   return len;
*/
}

#else  //IDA_SDK_VERSION >= 550

BufferList::BufferList() : head(NULL), tail(NULL) {
   mtx = qmutex_create();
}

Buffer *BufferList::dequeue() {
   Buffer *b = NULL;
   if (head) {
      BufferNode *bn = head;
      b = head->buf;
      qmutex_lock(mtx);
      head = head->next;
      if (head == NULL) {
         tail = NULL;
      }
      qmutex_unlock(mtx);
      delete bn;
   }
   return b;
}

bool BufferList::enqueue(Buffer *b) {
   bool first = false;
   BufferNode *n = new BufferNode(b);
   qmutex_lock(mtx);
   if (tail) {
      tail->next = n;
   }
   else {
      head = n;
      first = true;
   }
   tail = n;
   qmutex_unlock(mtx);
   return first;
}

//this is the callback that gets called by execute_sync, in theory new datagrams
//can arrive and be processed during the loop since queue synchronization takes
//place within the BufferList
int idaapi disp_request_t::execute(void) {
//   msg("execute called\n");
   Buffer *b;
   while ((b = buffers.dequeue()) != NULL) {
//      (*d)(*b);
// /*
      if (!(*d)(*b)) {  //not sure we really care what is returned here
//         msg(PLUGIN_NAME": connection to server severed at dispatch.\n");
         comm->cleanup(true);
         break;
      }
      else {
         //msg(PLUGIN_NAME": dispatch routine called successfully.\n");
      }
// */
      delete b;
   }
   return 0;
}

//queue up a received datagram for eventual handlng via IDA's execute_sync mechanism
//call no sdk functions other than execute_sync
void disp_request_t::queueBuffer(Buffer *b) {
   if (buffers.enqueue(b)) {
      //only invoke execute_sync if the buffer just added was at the head of he queue
      //in theory this allows multiple datagrams to get queued for handling
      //in a single execute_sync callback
      execute_sync(*this, MFF_WRITE);
   }
}

void disp_request_t::flush() {
   Buffer *b;
   while ((b = buffers.dequeue()) != NULL) {
      delete b;
   }
}

bool connect_to(const char *host, short port, Dispatcher d) {
   comm = new AsyncSocket(d);
   if (!comm->connect(host, port)) {
      delete comm;
      comm = NULL;
   }
   return comm != NULL;
}

bool is_connected() {
   return comm != NULL;
}

bool AsyncSocket::isConnected() {
   return conn != INVALID_SOCKET;
}

/////////////////////////////////////////////////////////////////////////////////////////
//cleanup(bool warn)
//
//cancel all notifications, close the socket and destroy the hook notification window.
//
//arguments: warn true displays a warning that cleanup is being called, false no warning
//returns:   none.
//
void AsyncSocket::cleanup(bool warn) {
   //cancel all notifications. if we don't do this ida will crash on exit.
   msg(PLUGIN_NAME": cleanup called.\n");
   if (conn != INVALID_SOCKET) {
      ::closesocket(conn);
      conn = INVALID_SOCKET;
#ifdef _WIN32
      if (thread) {
         msg("attempting to sync on thread exit\n");
         WaitForSingleObject(thread, INFINITE);
         thread = NULL;
      }
#else
      if (thread) {
         msg("attempting to sync on thread exit\n");
         pthread_join(thread, NULL);
         thread = 0;
      }
#endif
      if (warn) {
         warning("Connection to collabREate server has been closed.\n"
                 "You should reconnect to the server before sending\n"
                 "additional updates.");
      }
   }
}

/////////////////////////////////////////////////////////////////////////////////////////
//cleanup(bool warn)
//
//cancel all notifications, close the socket and destroy the hook notification window.
//
//arguments: warn true displays a warning that cleanup is being called, false no warning
//returns:   none.
//
void cleanup(bool warn) {
   if (comm) {
      comm->cleanup(warn);
      delete comm;
      comm = NULL;
   }
}

//connect to a remote host as specified by host and port
//host may be wither an ip address or a host name
bool AsyncSocket::connect(const char *host, short port) {
   //create a socket.
   conn = connect_to(host, port);
   if (conn != INVALID_SOCKET) {
      //socket is connected create thread to handle receive data
#ifdef _WIN32
      if ((thread = CreateThread(NULL, 0, recvHandler, this, 0, NULL)) == NULL) {
#else
      if (pthread_create(&thread, NULL, recvHandler, this)) {
#endif
         //error failed to create thread
         msg(PLUGIN_NAME": Failed to create connection handler.\n");
         cleanup();
      }
   }
   else {
      msg(PLUGIN_NAME": Failed to create socket.\n");
   }
   return isConnected();
}

bool AsyncSocket::sendAll(Buffer &b) {
   while (true) {
//      msg("sending new buffer\n");
      int len = ::send(conn, (const char*)b.get_buf(), b.size(), 0);
      if (len == b.size()) {
         break;
      }
      if (len == SOCKET_ERROR) {
#ifdef _WIN32
         int sockerr = WSAGetLastError();
#else
         int sockerr = errno;
#endif
         cleanup();
         msg(PLUGIN_NAME": Failed to send requested data. %d != %d. Error: 0x%x(%d)\n", len, b.size(), sockerr, sockerr);
         return false;
      }
      else if (len != b.size()) {
         //shift the remainder and try again
         shift(b, len);
         //msg(PLUGIN_NAME": Short send. %d != %d.", len, out.size());
      }
   }
   return true;
}

//Send a buffer of data
bool AsyncSocket::send(Buffer &b) {
//   if (!isConnected() || supress) return 0;   //silently fail
   if (!isConnected()) {
      if (changeCache != NULL) {
         msg("writing to change cache\n");
         changeCache->writeInt(b.size() + sizeof(int));
         *changeCache << b;
      }
      return true;
   }
   Buffer out;
   int sz = b.size() + sizeof(int);
   out.writeInt(sz);
   int command = b.readInt();
   if (command >= 0 && command <= MSG_IDA_MAX) {
      stats[1][command]++;
   }
   out << b;
   return sendAll(out);
/*
   while (true) {
//      msg("sending new buffer\n");
      int len = ::send(conn, (const char*)out.get_buf(), out.size(), 0);
      if (len == out.size()) {
         break;
      }
      if (len == SOCKET_ERROR) {
#ifdef _WIN32
         int sockerr = WSAGetLastError();
#else
         int sockerr = errno;
#endif
         cleanup();
         msg(PLUGIN_NAME": Failed to send requested data. %d != %d. Error: 0x%x(%d)\n", len, out.size(), sockerr, sockerr);
         return false;
      }
      else if (len != out.size()) {
         //shift the remainder and try again
         shift(out, len);
         //msg(PLUGIN_NAME": Short send. %d != %d.", len, out.size());
      }
   }
   return true;
*/
}

AsyncSocket::AsyncSocket(Dispatcher disp) {
   d = disp;
   thread = 0;
   init_network();
   conn = INVALID_SOCKET;
   drt = new disp_request_t(d);
}

bool AsyncSocket::close() {
   cleanup();
   return true;
}

int AsyncSocket::recv(unsigned char *buf, unsigned int len) {
   return ::recv(conn, (char*)buf, len, 0);
}

//We don't call ANY sdk functions from here because this is a separate thread
//and we don't want to do anything other than execute_sync (which happens in
//queueBuffer
#ifdef _WIN32
DWORD WINAPI AsyncSocket::recvHandler(void *_sock) {
#else
void *AsyncSocket::recvHandler(void *_sock) {
#endif
   static Buffer b;
   unsigned char buf[2048];  //read a large chunk, we'll be notified if there is more
   AsyncSocket *sock = (AsyncSocket*)_sock;

   while (sock->conn != INVALID_SOCKET) {
      int len = sock->recv(buf, sizeof(buf));
      if (len <= 0) {
#ifdef _WIN32
         //timeouts are okay
         if (WSAGetLastError() == WSAETIMEDOUT) {
            continue;
         }
#else
         //timeouts are okay
         if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
         }
#endif
//       assumption is that socket is borked and next send will fail also
//       maybe should close socket here at a minimum.
//       in any case thread is exiting
         break;
      }
      if (sock->d) {
         b.write(buf, len);   //append new data into static buffer
         while (isComplete(b)) {
            Buffer *data = new Buffer(b.get_buf() + sizeof(int), requiredSize(b) - sizeof(int));
            sock->drt->queueBuffer(data);
            shift(b);  //shift any remaining portions of the buffer to the front
         }
      }
   }
   return 0;
}

int send_all(Buffer &b) {
   if (comm) {
      return comm->sendAll(b);
   }
   return 0;
}

int send_data(Buffer &b) {
   if (comm) {
      return comm->send(b);
   }
   else {
      if (changeCache != NULL) {
//         msg("writing to change cache\n");
         changeCache->writeInt(b.size() + sizeof(int));
         *changeCache << b;
         return b.size();
      }
   }
   return 0;
}

#endif


