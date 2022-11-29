/**
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "proc.h"

#include "err.h"
#include "fd.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/event.h>
#include <sys/wait.h>

/******************************************************************************/
pid_t
proc_execute(char **aCmd)
{
    int rc = -1;

    pid_t childPid = -1;

    int pipeRd = -1;
    int pipeWr = -1;

    {
        int pipeFds[2];
        if (pipe(pipeFds)) {
            error("Unable to create pipe");
            goto Finally;
        }

        pipeRd = pipeFds[0];
        pipeWr = pipeFds[1];

        if (fd_cloexec(pipeRd)) {
            error("Unable to set FD_CLOEXEC on fd %d", pipeRd);
            goto Finally;
        }
        if (fd_cloexec(pipeWr)) {
            error("Unable to set FD_CLOEXEC on fd %d", pipeWr);
            goto Finally;
        }
    }

    childPid = fork();
    if (-1 == childPid) {
        error("Unable to fork new process");
        goto Finally;
    }

    if (!childPid) {

        /* Signals that are being caught will revert to their default
         * action as described in execve(2). */

        execvp(aCmd[0], aCmd);

        int errCode = errno;

        error("Unable to execute %s", aCmd[0]);

        fd_write(pipeWr, (void *) &errCode, sizeof(errCode));

        _exit(EXIT_FAILURE);
    }

    DEBUG("Child process %d forked", childPid);

    pipeWr = fd_close(pipeWr);

    int errCode;

    ssize_t readLen = fd_read(pipeRd, (void *) &errCode, sizeof(errCode));

    int execCode = -1;

    if (-1 == readLen)
        errCode = errno;
    else if (!readLen)
        execCode = 0;
    else if (sizeof(errCode) == readLen)
        execCode = 1;

    DEBUG("Child process %d exec code %d", childPid, execCode);

    if (execCode) {
        errno = errCode;
        goto Finally;
    }

    rc = 0;

Finally:

    FINALLY({
        if (rc) {
            if (-1 != childPid) {
                kill(childPid, SIGKILL);

                while (childPid != waitpid(childPid, 0, 0))
                    continue;

                childPid = -1;
            }
        }

        pipeRd = fd_close(pipeRd);
        pipeWr = fd_close(pipeWr);
    });

    return rc ? -1 : childPid;
}

/******************************************************************************/
int proc_monitor_create(pid_t aParentPid)
{
    int rc = -1;

    int monitorFd = kqueue();
    if (-1 == monitorFd)
        goto Finally;

    struct kevent kevs[2];

    struct kevent *kevp = kevs;
    int nkevs = 0;

    EV_SET(
        kevp++, SIGCHLD,
        EVFILT_SIGNAL, EV_ADD | EV_ENABLE,
        0, 0, 0);
    ++nkevs;

    if (aParentPid) {
        EV_SET(
            kevp++, aParentPid,
            EVFILT_PROC,
            (aParentPid ? EV_ADD : EV_DISABLE),
            NOTE_EXIT | NOTE_EXITSTATUS, 0, 0);
        ++nkevs;
    }

    if (-1 == kevent(monitorFd, kevs, nkevs, 0, 0, 0))
        goto Finally;

    rc = 0;

Finally:
    FINALLY({
        if (rc) {
            if (-1 != monitorFd)
                close(monitorFd);
        }
    });

    return rc ? rc : monitorFd;
}

/*----------------------------------------------------------------------------*/
int proc_monitor_wait(int aMonitorFd)
{
    int rc = -1;

    int ident = 0;

    struct kevent kev;
    int kevents = kevent(aMonitorFd, 0, 0, &kev, 1, 0);

    if (-1 == kevents) {
        if (EINTR != errno)
            goto Finally;
    } else {
        if (kevents && EVFILT_PROC == kev.filter)
            ident = kev.ident;
    }

    rc = 0;

Finally:

    return rc ? rc : ident;
}

/*----------------------------------------------------------------------------*/
int proc_monitor_close(int aMonitorFd)
{
    close(aMonitorFd);

    return 0;
}

/******************************************************************************/
