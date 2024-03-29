/*
 * DEBUG: section 93    ICAP (RFC 3507) Client
 */

#include "squid.h"
#include "base/AsyncCall.h"
#include "base/AsyncJob.h"
#include "base/AsyncJobCalls.h"
#include "base/TextException.h"
#include "cbdata.h"
#include "MemBuf.h"

#include <ostream>

InstanceIdDefinitions(AsyncJob, "job");
/*
/// schedule an async job call using a dialer; use CallJobHere macros instead
template <class Dialer>
bool
CallJob(int debugSection, int debugLevel, const char *fileName, int fileLine,
        const char *callName, const Dialer &dialer)
{
    AsyncCall::Pointer call = asyncCall(debugSection, debugLevel, callName, dialer);
    return ScheduleCall(fileName, fileLine, call);
}

#define CallJobHere(debugSection, debugLevel, job, Class, method) \
    CallJob((debugSection), (debugLevel), __FILE__, __LINE__, \
        (#Class "::" #method), \
        JobMemFun<Class>((job), &Class::method))

template <class C>
NullaryMemFunT<C>
JobMemFun(const CbcPointer<C> &job, typename NullaryMemFunT<C>::Method method)
{
    return NullaryMemFunT<C>(job, method);
}

*/
// AsyncJob::Start(new Comm::TcpAcceptor(s->listenConn, FdNote(portTypeNote), sub));
AsyncJob::Pointer AsyncJob::Start(AsyncJob *j)
{
    //AsyncJob::Pointer job(j);
	CbcPointer<AsyncJob> job(j);
    CallJobHere(93, 5, job, AsyncJob, start);
#if  xxx
	CallJob(93, 5, __FILE__, __LINE__, "AsyncJob::start", JobMemFun<AsyncJob>((job), &AsyncJob::start));
	CallJob(93, 5, __FILE__, __LINE__, "AsyncJob::start", NullaryMemFunT<AsyncJob>(job, &AsyncJob::start));
#endif
	/*
		CallJob(93, 5, __FILE__, __LINE__, AsyncJob::start, JobMemFun<AsyncJob>(job, &AsyncJob::start) )
		==> return NullaryMemFunT<AsyncJob>(job, method);
		
	*/
    return job;
}

AsyncJob::AsyncJob(const char *aTypeName) :
        stopReason(NULL), typeName(aTypeName), inCall(NULL)
{
    debugs(93,5, "AsyncJob constructed, this=" << this <<
           " type=" << typeName << " [" << id << ']');
}

AsyncJob::~AsyncJob()
{
    debugs(93,5, "AsyncJob destructed, this=" << this <<
           " type=" << typeName << " [" << id << ']');
}

void AsyncJob::start()
{
}

// XXX: temporary code to replace calls to "delete this" in jobs-in-transition.
// Will be replaced with calls to mustStop() when transition is complete.
void AsyncJob::deleteThis(const char *aReason)
{
    Must(aReason);
    stopReason = aReason;
    if (inCall != NULL) {
        // if we are in-call, then the call wrapper will delete us
        debugs(93, 4, typeName << " will NOT delete in-call job, reason: " << stopReason);
        return;
    }

    // there is no call wrapper waiting for our return, so we fake it
    debugs(93, 5, typeName << " will delete this, reason: " << stopReason);
    CbcPointer<AsyncJob> self(this);
    AsyncCall::Pointer fakeCall = asyncCall(93,4, "FAKE-deleteThis",
                                            JobMemFun(self, &AsyncJob::deleteThis, aReason));
    inCall = fakeCall;
    callEnd();
//    delete fakeCall;
}

void AsyncJob::mustStop(const char *aReason)
{
    // XXX: temporary code to catch cases where mustStop is called outside
    // of an async call context. Will be removed when that becomes impossible.
    // Until then, this will cause memory leaks and possibly other problems.
    if (!inCall) {
        stopReason = aReason;
        debugs(93, 5, typeName << " will STALL, reason: " << stopReason);
        return;
    }

    Must(inCall != NULL); // otherwise nobody will delete us if we are done()
    Must(aReason);
    if (!stopReason) {
        stopReason = aReason;
        debugs(93, 5, typeName << " will stop, reason: " << stopReason);
    } else {
        debugs(93, 5, typeName << " will stop, another reason: " << aReason);
    }
}

bool AsyncJob::done() const
{
    // stopReason, set in mustStop(), overwrites all other conditions
    return stopReason != NULL || doneAll();
}

bool AsyncJob::doneAll() const
{
    return true; // so that it is safe for kids to use
}

bool AsyncJob::canBeCalled(AsyncCall &call) const
{
    if (inCall != NULL) {
        // This may happen when we have bugs or some module is not calling
        // us asynchronously (comm used to do that).
        debugs(93, 5, HERE << inCall << " is in progress; " <<
               call << " canot reenter the job.");
        return call.cancel("reentrant job call");
    }

    return true;
}

void AsyncJob::callStart(AsyncCall &call)
{
    // we must be called asynchronously and hence, the caller must lock us
    Must(cbdataReferenceValid(toCbdata()));

    Must(!inCall); // see AsyncJob::canBeCalled

    inCall = &call; // XXX: ugly, but safe if callStart/callEnd,Ex are paired
    debugs(inCall->debugSection, inCall->debugLevel,
           typeName << " status in:" << status());
}

void AsyncJob::callException(const std::exception &e)
{
    // we must be called asynchronously and hence, the caller must lock us
    Must(cbdataReferenceValid(toCbdata()));

    mustStop("exception");
}

void AsyncJob::callEnd()
{
    if (done()) {
        debugs(93, 5, *inCall << " ends job" << status());

        AsyncCall::Pointer inCallSaved = inCall;
        void *thisSaved = this;

        swanSong();

        delete this; // this is the only place where the object is deleted

        // careful: this object does not exist any more
        debugs(93, 6, HERE << *inCallSaved << " ended " << thisSaved);
        return;
    }

    debugs(inCall->debugSection, inCall->debugLevel,
           typeName << " status out:" << status());
    inCall = NULL;
}

// returns a temporary string depicting transaction status, for debugging
const char *AsyncJob::status() const
{
    static MemBuf buf;
    buf.reset();

    buf.append(" [", 2);
    if (stopReason != NULL) {
        buf.Printf("Stopped, reason:");
        buf.Printf("%s",stopReason);
    }
    buf.Printf(" %s%u]", id.Prefix, id.value);
    buf.terminate();

    return buf.content();
}

