/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Copyright (C) 2001-2007 Vlad Seryakov
 * All rights reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * nsrtsp.c -- RTSP streaming server using liveMedia streaming library
 *
 * Authors
 *
 *     Vlad Seryakov vlad@crystalballinc.com
 */

#include "ns.h"
#include <sys/resource.h>
#include <sys/epoll.h>

#include <BasicUsageEnvironment.hh>
#include <liveMedia.hh>

// To use this option liveMedia should be compiled with READ_FROM_FILES_SYNCHRONOUSLY
// defined in ByteStreamFileSource.cpp, because epoll does not support disk files
//#define USE_EPOLL 1

class RTSPModule;

typedef struct {
   int fIdx;
   int fSock;
   void *fData;
   TaskScheduler::BackgroundHandlerProc* fProc;
} RTSPHandler;

class RTSPTaskScheduler: public BasicTaskScheduler0 {
public:
   static RTSPTaskScheduler* createNew();
   virtual ~RTSPTaskScheduler();

protected:
   RTSPTaskScheduler();

   virtual void SingleStep(unsigned maxDelayTime);
   virtual void turnOnBackgroundReadHandling(int sock, BackgroundHandlerProc *proc, void *data);
   virtual void turnOffBackgroundReadHandling(int sock);

   int fNumSockets;
   int fMaxSockets;
   RTSPHandler *fHandlers;
#ifdef USE_EPOLL
   int fEPollSock;
   struct epoll_event fEPoll;
#else
   struct pollfd *fPoll;
#endif
};

class RTSPThread: public RTSPServer {
public:

   RTSPThread(RTSPModule *envPtr, UsageEnvironment &env, int sock,  Port port);
   ~RTSPThread(void);

   static void run(void *arg);
   static void pipeHandler(void* arg, int mask);
   static RTSPThread* createNew(RTSPModule *env, int port);
   virtual ServerMediaSession* lookupServerMediaSession(char const* name);
   void triggerPipe(void);

   int fPort;
   int fPipe[2];
   char fSignal;
   RTSPModule *fEnv;
   Tcl_DString fDstr;
   RTSPThread *fNext;
};

class RTSPMediaSession: public ServerMediaSession {
public:
   RTSPMediaSession(UsageEnvironment& env, char const* name, char const *descr);
   virtual ~RTSPMediaSession(void);

   static RTSPMediaSession* createNew(UsageEnvironment& env, char const* name, char const *descr);
};

class RTSPModule {
public:
   RTSPModule(char *server);
   ~RTSPModule(void);

   int fReclaim;
   char *fServer;
   Ns_Set *fRoot;
   Ns_Mutex fMutex;
   RTSPThread *fThreads;
   UserAuthenticationDatabase *fAuth;
};

static int RTSPInterpInit(Tcl_Interp *interp, void *arg);
static int RTSPCmd(ClientData arg, Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);


extern "C" {

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int Ns_ModuleInit(char *server, char *module)
{
    Ns_Set *set;
    Ns_DString ds;
    char *path, *root;
    int n, port, threads;
    RTSPThread *thr;
    RTSPModule *envPtr;

    Ns_DStringInit(&ds);

    path = Ns_ConfigGetPath(server, module, NULL);
    threads = Ns_ConfigIntRange(path, "threads", 1, 1, INT_MAX);
    port = Ns_ConfigIntRange(path, "port", 554, 1, INT_MAX);

    envPtr = new RTSPModule(server);

    // Multiple root parameters can be specified
    set = Ns_ConfigGetSection(path);
    for (n = 0; set != NULL && n < Ns_SetSize(set); ++n) {
        if (strcasecmp(Ns_SetKey(set, n), "path")) {
            continue;
        }
        root = Ns_SetValue(set, n);
        if (!Ns_PathIsAbsolute(root)) {
            Ns_DStringSetLength(&ds, 0);
            root = Ns_PagePath(&ds, server, root, NULL);
        }
        Ns_SetPut(envPtr->fRoot, root, NULL);
        Ns_Log(Notice, "nsrtsp: adding path: %s", root);
    }

    // No root parameters are given, by default point to page root
    if (envPtr->fRoot->size == 0) {
        Ns_DStringSetLength(&ds, 0);
        Ns_PagePath(&ds, server, "", NULL);
        Ns_SetPut(envPtr->fRoot, ds.string, NULL);
        Ns_Log(Notice, "nsrtsp: adding path: %s", ds.string);
    }

    // Create streaming threads, each thread can handle multiple sessions,
    // each subsequent thread will increase port number for listening
    for (n = 0; n < threads; n++) {
         thr = RTSPThread::createNew(envPtr, port++);
         if (thr == NULL) {
             return NS_ERROR;
         }
         Ns_ThreadCreate(RTSPThread::run, (void *)thr, 0, 0);
    }

    Ns_DStringFree(&ds);
    Ns_TclRegisterTrace(server, RTSPInterpInit, envPtr, NS_TCL_TRACE_CREATE);
    return NS_OK;
}

}

static int RTSPInterpInit(Tcl_Interp *interp, void *arg)
{
    Tcl_CreateObjCommand(interp, "ns_rtsp", RTSPCmd, arg, NULL);
    return NS_OK;
}

static int RTSPCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    int cmd;
    enum {
        cmdVersion
    };
    static CONST char *subcmd[] = {
        "version",
        NULL
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], subcmd, "option", 0, &cmd) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (cmd) {
     case cmdVersion:
         break;
    }
    return TCL_OK;
}

RTSPModule::RTSPModule(char *server)
{
    fAuth = NULL;
    fServer = server;
    fReclaim = 45;
    fRoot = Ns_SetCreate(NULL);
    Ns_MutexInit(&fMutex);
}

RTSPModule::~RTSPModule(void)
{
    Ns_SetFree(fRoot);
}

RTSPTaskScheduler* RTSPTaskScheduler::createNew()
{
    return new RTSPTaskScheduler();
}

RTSPTaskScheduler::RTSPTaskScheduler(): fNumSockets(0)
{
    struct rlimit rlimit;

    getrlimit(RLIMIT_NOFILE, &rlimit);
    fMaxSockets = rlimit.rlim_max;

    fHandlers = (RTSPHandler*)ns_calloc(fMaxSockets, sizeof(RTSPHandler));

#ifdef USE_EPOLL
    fEPollSock = epoll_create(fMaxSockets);
    if (fEPollSock < 0) {
        Ns_Log(Error, "nsrtsp: epoll_create failed: %s", strerror(errno));
    }
#else
    fPoll = (struct pollfd*)ns_calloc(fMaxSockets, sizeof(struct pollfd));
#endif

    Ns_Log(Notice, "nsrtsp: max number of sockets %d", fMaxSockets);
}

RTSPTaskScheduler::~RTSPTaskScheduler()
{
#ifdef USE_EPOLL
    ::close(fEPollSock);
#else
    ns_free(fPoll);
#endif
    ns_free(fHandlers);
}

void RTSPTaskScheduler::SingleStep(unsigned maxDelayTime)
{
    int n, pollto, sock;
    DelayInterval const& timeToDelay = fDelayQueue.timeToNextAlarm();

    pollto = timeToDelay.seconds() * 1000 + timeToDelay.useconds() / 1000;

    // Also check our "maxDelayTime" parameter (if it's > 0):
    if (maxDelayTime > 0 && (unsigned)pollto > maxDelayTime/1000 ) {
        pollto = maxDelayTime/1000;
    }

    // Do the poll, ignore interruptions
    do {
#ifdef USE_EPOLL
        n = epoll_wait(fEPollSock, &fEPoll, 1, pollto);
#else
        n = poll(fPoll, fNumSockets, pollto);
#endif
    } while (n < 0  && errno == EINTR);

    if (n < 0) {
        Ns_Fatal("nsrtsp: poll() failed num=%d, time=%d: %s", fNumSockets, pollto, ns_sockstrerror(ns_sockerrno));
    }

    // Handle any delayed event that may have come due
    fDelayQueue.handleAlarm();

    // Nothing to read from
    if (n == 0) {
        return;
    }

#ifdef USE_EPOLL
    if (fEPoll.events & EPOLLIN) {
        sock = fEPoll.data.fd;
        if (fHandlers[sock].fProc != NULL) {
            (*fHandlers[sock].fProc)(fHandlers[sock].fData, SOCKET_READABLE);
        }
    }
#else
    int i, done = 0;

    // Continue from the last processed socket
    fLastHandledSocketNum++;

    // Array that describes start..end for each iteration
    int loop[5] = { fLastHandledSocketNum, fNumSockets, 0, fLastHandledSocketNum };

    // Reset to start if out of range
    if (fLastHandledSocketNum < 0 || fLastHandledSocketNum >= fNumSockets) {
        fLastHandledSocketNum = loop[0] = loop[3] = 0;
    }

    n = 0;
    // Scan sockets, execute one handler and clear all events in one loop
    while (n < 3) {
        for (i = loop[n]; i < loop[n + 1]; i++) {
            if (!done && fPoll[i].revents & POLLIN) {
                sock = fPoll[i].fd;
                if (fHandlers[sock].fProc != NULL) {
                    fLastHandledSocketNum = i;
                    (*fHandlers[sock].fProc)(fHandlers[sock].fData, SOCKET_READABLE);
                    done = 1;
                }
            }
            fPoll[i].revents = 0;
        }
        n += 2;
    }
#endif
}

void RTSPTaskScheduler::turnOnBackgroundReadHandling(int sock, BackgroundHandlerProc *proc, void *data)
{
    if (sock < 0 || sock >= fMaxSockets) {
        Ns_Log(Error, "nsrtsp: turnOn: invalid socket %d, num=%d, max=%d", sock, fNumSockets, fMaxSockets);
        return;
    }

    // Already enabled, not an error
    if (fHandlers[sock].fSock > 0) {
        return;
    }

    fHandlers[sock].fIdx = fNumSockets;
    fHandlers[sock].fSock = sock;
    fHandlers[sock].fProc = proc;
    fHandlers[sock].fData = data;

#ifdef USE_EPOLL
    fEPoll.data.fd = sock;
    fEPoll.events = EPOLLIN;

    if (epoll_ctl(fEPollSock, EPOLL_CTL_ADD, sock, &fEPoll) < 0) {
        Ns_Log(Error, "nsrtsp: turnOn: epoll failed: %d, %s", sock, ns_sockstrerror(ns_sockerrno));
    }
#else
    fPoll[fNumSockets].fd = sock;
    fPoll[fNumSockets].events = POLLIN;
#endif

    fNumSockets++;
}

void RTSPTaskScheduler::turnOffBackgroundReadHandling(int sock)
{
    if (sock < 0 || sock >= fMaxSockets) {
        Ns_Log(Error, "nsrtsp: turnOff: invalid socket: %d, num=%d, max=%d", sock, fNumSockets, fMaxSockets);
        return;
    }

    // Already disabled, not an error
    if (fHandlers[sock].fSock == 0) {
        return;
    }

#ifdef USE_EPOLL
    if (epoll_ctl(fEPollSock, EPOLL_CTL_DEL, sock, NULL) < 0) {
        Ns_Log(Error, "nsrtsp: turnOff: epoll failed: %d, %s", sock, ns_sockstrerror(ns_sockerrno));
    }
#else
    // Move sockets one slot left and reassign indexes in the handlers
    for (int i = fHandlers[sock].fIdx; i < fNumSockets - 1; i++) {
        fPoll[i] = fPoll[i + 1];
        fHandlers[fPoll[i].fd].fIdx = i;
    }
    fPoll[fNumSockets].fd = 0;
#endif

    fHandlers[sock].fSock = 0;
    fNumSockets--;
}

RTSPMediaSession::RTSPMediaSession(UsageEnvironment& env, char const* sname, char const *descr):
                  ServerMediaSession(env, sname, sname, descr, False, NULL)
{
    Ns_Log(Notice, "nsrtsp: %p: new session %s: %s/%s", this, name(), sname, descr);
}

RTSPMediaSession::~RTSPMediaSession(void)
{
    Ns_Log(Notice, "nsrtsp: %p: free session %s", this, name());
}

RTSPMediaSession* RTSPMediaSession::createNew(UsageEnvironment& env, char const* name, char const *descr)
{
    return new RTSPMediaSession(env, name, descr);
}

RTSPThread::RTSPThread(RTSPModule *envPtr, UsageEnvironment& env, int sock,  Port port):
            RTSPServer(env, sock, port, envPtr->fAuth, envPtr->fReclaim)
{
    fEnv = envPtr;
    Ns_DStringInit(&fDstr);
    fSignal = 0;
    fPort = ntohs(port.num());
    ns_sockpair(fPipe);
    this->fNext = envPtr->fThreads;
    envPtr->fThreads = this;
    envir().taskScheduler().turnOnBackgroundReadHandling(fPipe[0], &pipeHandler, this);
}

RTSPThread::~RTSPThread()
{
    Ns_DStringFree(&fDstr);
    envir().taskScheduler().turnOffBackgroundReadHandling(fPipe[0]);
    ::close(fPipe[0]);
    ::close(fPipe[1]);
}

void RTSPThread::pipeHandler(void* arg, int mask)
{
}

void RTSPThread::triggerPipe(void)
{
    if (send(fPipe[1], "", 1, 0) != 1) {
        Ns_Fatal("nsrtsp: trigger send() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
}

void RTSPThread::run(void *arg)
{
    RTSPThread *thr = (RTSPThread*)arg;

    Ns_ThreadSetName("rtsp%d", thr->fPort);
    Ns_Log(Notice, "nsrtsp: started: %s", thr->rtspURLPrefix());
    thr->envir().taskScheduler().doEventLoop(&thr->fSignal);
    delete thr;
}

RTSPThread* RTSPThread::createNew(RTSPModule *envPtr, int port)
{
    Port mPort(port);
    TaskScheduler *mSched = RTSPTaskScheduler::createNew();
    UsageEnvironment *mEnv = BasicUsageEnvironment::createNew(*mSched);
    int mSock = setUpOurSocket(*mEnv, mPort);

    if (port > 0 && mSock == -1) {
        Ns_Log(Error, "nsrtsp: unable to bind to port %d", port);
        return NULL;
    }
    return new RTSPThread(envPtr, *mEnv, mSock, mPort);
}

ServerMediaSession* RTSPThread::lookupServerMediaSession(char const* name)
{
    // Use the file name extension to determine the type of "ServerMediaSession"
    char* ext = strrchr(name, '.');
    if (ext == NULL) {
        return NULL;
    }

    // Check whether the specified "name" exists as a local file
    Boolean fileNotFound = -1;

    // Scan all directories for file name
    for (int i = 0; fileNotFound && i < fEnv->fRoot->size; i++) {
       Ns_DStringSetLength(&fDstr, 0);
       Ns_DStringPrintf(&fDstr, "%s/%s", Ns_SetKey(fEnv->fRoot, i), name);
       fileNotFound = access(fDstr.string, R_OK);
    }

    // Check whether we already have a "ServerMediaSession" for this file
    ServerMediaSession* sms = RTSPServer::lookupServerMediaSession(name);

    if (fileNotFound) {
        // "sms" was created for a file that no longer exists
        if (sms != NULL) {
            removeServerMediaSession(sms);
        }
        Ns_Log(Error, "nsrtsp: file not found %s", fDstr.string);
        return NULL;
    }

    if (sms != NULL) {
        Ns_Log(Notice, "nsrtsp: reusing session %s for %s", sms->name(), fDstr.string);
        return sms;
    }

    // Assumed to be an AAC Audio (ADTS format) file:
    if (strcmp(ext, ".aac") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "AAC Audio");
        sms->addSubsession(ADTSAudioFileServerMediaSubsession::createNew(envir(), fDstr.string, False));
    } else
    // Assumed to be an AMR Audio file:
    if (strcmp(ext, ".amr") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "AMR Audio");
        sms->addSubsession(AMRAudioFileServerMediaSubsession::createNew(envir(), fDstr.string, False));
    } else
    // Assumed to be a MPEG-4 Video Elementary Stream file:
    if (strcmp(ext, ".m4e") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "MPEG-4 Video");
        sms->addSubsession(MPEG4VideoFileServerMediaSubsession::createNew(envir(), fDstr.string, False));
    } else
    // Assumed to be a MPEG-1 or 2 Audio file:
    if (strcmp(ext, ".mp3") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "MPEG-1/2 Audio");
        Boolean useADUs = False;
        Interleaving* interleaving = NULL;
        sms->addSubsession(MP3AudioFileServerMediaSubsession::createNew(envir(), fDstr.string, False, useADUs, interleaving));
    } else
    // Assumed to be a MPEG-1 or 2 Program Stream (audio+video) file:
    if (strcmp(ext, ".mpg") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "MPEG-1/2 PS");
        MPEG1or2FileServerDemux* demux = MPEG1or2FileServerDemux::createNew(envir(), fDstr.string, False);
        sms->addSubsession(demux->newVideoServerMediaSubsession());
        sms->addSubsession(demux->newAudioServerMediaSubsession());
    } else
    // Assumed to be a 'VOB' file (e.g., from an unencrypted DVD):
    if (strcmp(ext, ".vob") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "VOB");
        MPEG1or2FileServerDemux* demux = MPEG1or2FileServerDemux::createNew(envir(), fDstr.string, False);
        sms->addSubsession(demux->newVideoServerMediaSubsession());
        sms->addSubsession(demux->newAC3AudioServerMediaSubsession());
    } else
    // Assumed to be a MPEG Transport Stream file:
    // Use an index file name that's the same as the TS file name, except with ".tsx":
    if (strcmp(ext, ".ts") == 0) {
        char* indexFileName = new char[strlen(fDstr.string) + 2];
        sprintf(indexFileName, "%sx", fDstr.string);
        sms = RTSPMediaSession::createNew(envir(), name, "MPEG TS");
        sms->addSubsession(MPEG2TransportFileServerMediaSubsession::createNew(envir(), fDstr.string, indexFileName, False));
    } else
    // Assumed to be a WAV Audio file:
    if (strcmp(ext, ".wav") == 0) {
        sms = RTSPMediaSession::createNew(envir(), name, "WAV Audio");
        sms->addSubsession(WAVAudioFileServerMediaSubsession::createNew(envir(), fDstr.string, False, False));
    }
    // Deallocate session on stream close
    //sms->deleteWhenUnreferenced() = True;
    addServerMediaSession(sms);
    return sms;
}

