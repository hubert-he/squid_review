/*
 * DEBUG: section 54    Interprocess Communication
 *
 */

#include "squid.h"
#include "base/TextException.h"
#include "comm.h"
#include "comm/Connection.h"
#include "ipc/SharedListen.h"
#include "ipc/StartListening.h"
#include "tools.h"

#if HAVE_ERRNO_H
#include <errno.h>
#endif

Ipc::StartListeningCb::StartListeningCb(): conn(NULL), errNo(0)
{
}

Ipc::StartListeningCb::~StartListeningCb()
{
}

std::ostream &Ipc::StartListeningCb::startPrint(std::ostream &os) const
{
    return os << "(" << conn << ", err=" << errNo;
}
// (SOCK_STREAM, IPPROTO_TCP, s->listenConn, Ipc::fdnHttpSocket, listenCall
// AsyncCall::Pointer listenCall = asyncCall(33,2, "clientListenerConnectionOpened",
//                                       ListeningStartedDialer(&clientListenerConnectionOpened, s, Ipc::fdnHttpSocket, sub));
void Ipc::StartListening(int sock_type, int proto, 
					const Comm::ConnectionPointer &listenConn,
                    FdNoteId fdNote, // Ipc::fdnHttpSocket
                    AsyncCall::Pointer &callback) // listenCall
{
    StartListeningCb *cbd = dynamic_cast<StartListeningCb*>(callback->getDialer());
    Must(cbd);
    cbd->conn = listenConn;

    if (UsingSmp()) { // if SMP is on, share
        OpenListenerParams p;
        p.sock_type = sock_type; // SOCK_STREAM
        p.proto = proto;
        p.addr = listenConn->local;
        p.flags = listenConn->flags;
        p.fdNote = fdNote;
        Ipc::JoinSharedListen(p, callback);
        return; // wait for the call back
    }

    enter_suid();
    comm_open_listener(sock_type, proto, cbd->conn, FdNote(fdNote));
    cbd->errNo = Comm::IsConnOpen(cbd->conn) ? 0 : errno;
    leave_suid();

    debugs(54, 3, HERE << "opened listen " << cbd->conn);
    ScheduleCallHere(callback); // AsyncCallQueue::Instance().schedule(callback);
}
