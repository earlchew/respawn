/**
 * Restart a monitored process
 *
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

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>

/******************************************************************************/
#define NUMBEROF(Array) ( sizeof((Array))/sizeof((Array)[0]) )

/******************************************************************************/
#define DEBUG(...) if (!optDebug) { } else debug(__VA_ARGS__)

/******************************************************************************/
#define FINALLY(...)                 \
    do {                             \
        int errno_ = errno;          \
        do { __VA_ARGS__ } while(0); \
        errno = errno_;              \
    } while (0)

/******************************************************************************/
#ifdef __GNUC__
#define PRINTF_FORMAT(Fmt, Args) \
    __attribute__((__format__ (__printf__, Fmt, Args)))
#else
#define PRINTF_FORMAT(Fmt, Args)
#endif

/******************************************************************************/
#ifdef __GNU_LIBRARY__
#define ARGV0 (program_invocation_short_name)
#else
#define ARGV0 (getprogname())
#endif

/******************************************************************************/
static int optDebug;
static int optHelp;
static int optContinue;
static int optForever;

static unsigned char optExit[256] = { 1 };

/******************************************************************************/
void
usage(void)
{
    static const char usageText[] =
        "usage: %s [-dfZ] [-x N,...] -- cmd ...\n"
        "Options:\n"
        "  -d --debug      Emit debug information\n"
        "  -f --forever    Continually restart the monitored process\n"
        "  -Z --continue   Continue monitored process if it suspends\n"
        "  -x --exit N,..  Additional success exit codes [default: 0]\n"
        "  -x --exit none  No success exit codes [default: 0]\n";

    if (optHelp)
        fprintf(stderr, usageText, ARGV0);
    else {
        const char *usageLine = strchr(usageText, '\n');

        if (!usageLine)
            usageLine = strchr(usageText, 0);

        size_t helpLen = usageLine - usageText;

        char helpText[helpLen+2];

        memcpy(helpText, usageText, helpLen);
        helpText[helpLen+0] = '\n';
        helpText[helpLen+1] = 0;

        fprintf(stderr, helpText, ARGV0);
    }

    exit(EXIT_FAILURE);
}

/******************************************************************************/
static void
alert_(const char *aFmt, const char *aLevel, int errCode, va_list argp)
{
    fprintf(stderr, "%s: ", ARGV0);
    if (aLevel)
        fprintf(stderr, "%s - ", aLevel);
    vfprintf(stderr, aFmt, argp);

    if (errCode) {
        const char *errText = strerror(errCode);

        fprintf(stderr, " [%d - %s]", errCode, errText);
    }

    fprintf(stderr, "\n");
}

/*----------------------------------------------------------------------------*/
PRINTF_FORMAT(1, 2)
void
warn(const char *aFmt, ...)
{
    int errCode = errno;

    va_list argp;

    va_start(argp, aFmt);
    alert_(aFmt, "WARN", errCode, argp);
    va_end(argp);

    errno = errCode;
}

/*----------------------------------------------------------------------------*/
void
errorv(const char *aFmt, va_list argp)
{
    int errCode = errno;

    alert_(aFmt, "ERROR", errCode, argp);

    errno = errCode;
}

/*----------------------------------------------------------------------------*/
PRINTF_FORMAT(1, 2)
void
error(const char *aFmt, ...)
{
    va_list argp;

    va_start(argp, aFmt);
    errorv(aFmt, argp);
    va_end(argp);
}

/*----------------------------------------------------------------------------*/
PRINTF_FORMAT(1, 2)
void
fatal(const char *aFmt, ...)
{
    va_list argp;

    va_start(argp, aFmt);
    errorv(aFmt, argp);
    va_end(argp);
    exit(EXIT_FAILURE);
}

/*----------------------------------------------------------------------------*/
PRINTF_FORMAT(1, 2)
void
debug(const char *aFmt, ...)
{
    va_list argp;

    va_start(argp, aFmt);
    alert_(aFmt, "DEBUG", 0, argp);
    va_end(argp);
}

/*----------------------------------------------------------------------------*/
PRINTF_FORMAT(1, 2)
void
die(const char *aFmt, ...)
{
    va_list argp;

    va_start(argp, aFmt);
    alert_(aFmt, 0, 0, argp);
    va_end(argp);

    exit(EXIT_FAILURE);
}

/******************************************************************************/
unsigned long
monomillis(void)
{
    struct timespec clockTime;

    if (clock_gettime(CLOCK_MONOTONIC, &clockTime))
        fatal("Unable to get CLOCK_MONOTONIC time");

    return 1000 * clockTime.tv_sec + clockTime.tv_nsec / 1000000;
}

/******************************************************************************/
int
fd_cloexec(int aFd)
{
    int rc = -1;

    int arg;

    arg = fcntl(aFd, F_GETFD);
    if (-1 == arg)
        goto Finally;

    arg |= FD_CLOEXEC;

    if (-1 == fcntl(aFd, F_SETFD, arg))
        goto Finally;

    rc = 0;

Finally:

    return rc;
}

/*----------------------------------------------------------------------------*/
int
fd_close(int aFd)
{
    if (-1 != aFd)
        close(aFd);

    return -1;
}

/*----------------------------------------------------------------------------*/
ssize_t
fd_write(int aFd, const char *aBuf, ssize_t aLen)
{
    int rc = -1;

    const char *bufPtr = aBuf;
    ssize_t     bufLen = aLen;

    while (bufLen) {
        ssize_t writeLen = write(aFd, bufPtr, bufLen);
        if (-1 == writeLen) {
            if (EINTR == errno)
                continue;
            if (bufPtr != aBuf)
                break;
            goto Finally;
        }

        if (0 == writeLen)
            break;

        if (writeLen > bufLen)
            die("File descriptor %d write %lu overrunning buffer length %lu",
                aFd, (unsigned long) writeLen, (unsigned long) bufLen);

        bufPtr += writeLen;
        bufLen -= writeLen;
    }

    rc = 0;

Finally:

    return rc ? -1 : bufPtr - aBuf;
}

/*----------------------------------------------------------------------------*/
ssize_t
fd_read(int aFd, char *aBuf, ssize_t aLen)
{
    int rc = -1;

    char   *bufPtr = aBuf;
    ssize_t bufLen = aLen;

    while (bufLen) {
        ssize_t readLen = read(aFd, bufPtr, bufLen);
        if (-1 == readLen) {
            if (EINTR == errno)
                continue;
            if (bufPtr != aBuf)
                break;
            goto Finally;
        }

        if (0 == readLen)
            break;

        if (readLen > bufLen)
            die("File descriptor %d read %lu overrunning buffer length %lu",
                aFd, (unsigned long) readLen, (unsigned long) bufLen);

        bufPtr += readLen;
        bufLen -= readLen;
    }

    rc = 0;

Finally:

    return rc ? -1 : bufPtr - aBuf;
}

/******************************************************************************/
struct SignalBlock {
    sigset_t mSigSet;
};

static void
signal_block_acquire(struct SignalBlock *aSignalBlock)
{
    sigset_t blockMask;

    if (sigfillset(&blockMask))
        die("Unable to fill blocking signal set");

    if (sigprocmask(SIG_SETMASK, &blockMask, &aSignalBlock->mSigSet))
        die("Unable to set blocking signal mask");
}


/*----------------------------------------------------------------------------*/
static void
signal_block_release(const struct SignalBlock *aSignalBlock)
{
    if (sigprocmask(SIG_SETMASK, &aSignalBlock->mSigSet, 0))
        die("Unable to reset blocking signal mask");
}

/******************************************************************************/
static volatile sig_atomic_t SignalSet_;

/*----------------------------------------------------------------------------*/
sig_atomic_t
signal_set_sample(void)
{
    struct SignalBlock signalBlock;

    signal_block_acquire(&signalBlock);

    /* Sample the signal set with all signal delivery blocked to
     * prevent the set being updated while it is being sampled. */

    sig_atomic_t sigSet = SignalSet_;
    SignalSet_ = 0;

    signal_block_release(&signalBlock);

    return sigSet;
}

/*----------------------------------------------------------------------------*/
void
signal_set_add(int aSignal)
{
    struct SignalBlock signalBlock;

    signal_block_acquire(&signalBlock);

    /* Posix says that sig_atomic_t can be signed, so exclude the
     * sign bit from use in the bitmap. */

    if (aSignal >= sizeof(SignalSet_)*CHAR_BIT - 1)
        die("Signal %d exceeds set size", aSignal);

    /* Signal numbers are non-zero, but do not bother trying to
     * recover bit zero from the bitmap. */

    SignalSet_ |= (1 << aSignal);

    signal_block_release(&signalBlock);
}

/******************************************************************************/
static struct {
    const char *mName;
    int         mSignal;
    sig_t       mHandler;
} SigStrategy[] = {
    { "SIGHUP",  SIGHUP },
    { "SIGQUIT", SIGQUIT },
    { "SIGINT",  SIGINT },
    { "SIGABRT", SIGABRT },
    { "SIGTERM", SIGTERM },
    { "SIGCONT", SIGCONT },
};

/*----------------------------------------------------------------------------*/
static void
signal_handler_(int aSignal)
{
    signal_set_add(aSignal);
}

/*----------------------------------------------------------------------------*/
void
signal_catch(void)
{
    /* Only catch signals that are still set at their default action,
     * and not being ignored. Processes inherit default and ignored
     * signal settings from their parent. */

    (void) signal_set_sample();

    for (unsigned ix = 0; ix < NUMBEROF(SigStrategy); ++ix) {

        struct sigaction action;

        if (sigaction(SigStrategy[ix].mSignal, 0, &action))
            die("Unable to query signal %s", SigStrategy[ix].mName);

        SigStrategy[ix].mHandler = action.sa_handler;
        action.sa_handler        = signal_handler_;

        if (SIG_DFL == SigStrategy[ix].mHandler) {
            DEBUG(
                "Installing catcher for signal %d", SigStrategy[ix].mSignal);
            if (sigaction(SigStrategy[ix].mSignal, &action, 0))
                die("Unable to intercept signal %s", SigStrategy[ix].mName);
        }
    }
}

/*----------------------------------------------------------------------------*/
void
signal_release(void)
{
    for (unsigned ix = 0; ix < NUMBEROF(SigStrategy); ++ix) {
        struct sigaction action;

        if (sigaction(SigStrategy[ix].mSignal, 0, &action))
            die("Unable to query signal %s", SigStrategy[ix].mName);

        action.sa_handler = SigStrategy[ix].mHandler;

        if (SIG_DFL == SigStrategy[ix].mHandler) {
            if (sigaction(SigStrategy[ix].mSignal, &action, 0))
                die("Unable to reset signal %s", SigStrategy[ix].mName);
        }
    }
}

/******************************************************************************/
char **
parse_options(int argc, char **argv)
{
    int rc = -1;

    static char shortOpts[] = "hdfZx:";

    static struct option longOpts[] = {
        { "help",      no_argument,       0, 'h' },
        { "debug",     no_argument,       0, 'd' },
        { "forever",   no_argument,       0, 'f' },
        { "continue",  no_argument,       0, 'Z' },
        { "exit",      required_argument, 0, 'x' },
        { 0 },
    };

    while (1) {
        int ch = getopt_long(argc, argv, shortOpts, longOpts, 0);

        if (-1 == ch)
            break;

        switch (ch) {
        default:
            break;

        case 'h':
            optHelp = 1;
            goto Finally;

        case ':':
        case '?':
            goto Finally;

        case 'f':
            optForever = 1; break;

        case 'Z':
            optContinue = 1; break;

        case 'd':
            optDebug = 1; break;

        case 'x':
            if (!strcmp(optarg, "none")) {

                memset(optExit, 0, sizeof(optExit));

            } else {

                char *lastSep;

                char *argList = optarg;

                while (1) {
                    char *word = strtok_r(argList, ",", &lastSep);

                    if (!word) {
                        if (argList)
                            die("No exit codes specified");
                        break;
                    }

                    argList = 0;

                    if (!isdigit((unsigned char) word[0]))
                        die("Exit code %s must start with a digit", word);

                    char *endPtr;
                    unsigned long exitCode = strtoul(word, &endPtr, 10);

                    if (*endPtr)
                        die("Unable to parse exit code %s", word);

                    if (exitCode > 255)
                        die("Exit code %s exceeds 255", word);

                    optExit[exitCode] = 1;
                }
            }
        }
    }

    if (argc >= optind && !strcmp("--", argv[optind-1]))
        rc = 0;

Finally:

    FINALLY({
        if (!rc) {
            argv += optind;
            argc -= optind;
        }
    });

    return rc ? 0 : argv;
}

/******************************************************************************/
pid_t
exec_command(char **aCmd)
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
int
spawn_command(char **aCmd)
{
    int rc = -1;

    pid_t childPid = exec_command(aCmd);
    if (-1 == childPid)
        goto Finally;

    while (1) {

        /* Propagate all caught signals to the child process. The child
         * might choose to ignore or catch the signals, and might not
         * terminate.
         *
         * https://people.freebsd.org/~cracauer/homepage-mirror/sigint.html
         */

        sig_atomic_t sigSet = signal_set_sample();

        for (int signal = 0; sigSet; ++signal) {
            if (sigSet & 1) {
                DEBUG(
                    "Delivering signal %d to child process %d",
                    signal, childPid);

                kill(childPid, signal);
            }
            sigSet >>= 1;
        }

        int childStatus;

        pid_t pid = waitpid(childPid, &childStatus, WUNTRACED);
        if (-1 == pid) {
            if (EINTR == errno)
                continue;
            fatal("Unable to wait for child process %d", childPid);
        }

        if (WIFSTOPPED(childStatus)) {
            int stopSig = WSTOPSIG(childStatus);

            DEBUG("Child process %d stopped signal %d", childPid, stopSig);

            /* http://curiousthing.org/sigttin-sigttou-deep-dive-linux */

            if (SIGSTOP == stopSig || SIGTSTP == stopSig) {
                if (optContinue) {
                    signal_set_add(SIGCONT);
                    stopSig = 0;
                }
            }

            if (stopSig) {
                if (kill(getpid(), stopSig)) {
                    warn("Unable to stop process after signal %d", stopSig);
                }
            }

            continue;
        }

        if (WIFEXITED(childStatus)) {
            int exitStatus = WEXITSTATUS(childStatus);

            DEBUG("Child process %d exit status %d", childPid, exitStatus);
            rc = 0x000 + exitStatus;
        }
        else if (WIFSIGNALED(childStatus)) {
            int termSig = WTERMSIG(childStatus);

            DEBUG("Child process %d termination signal %d", childPid, termSig);
            rc = 0x100 + termSig;
        }

        break;
    }

Finally:

    return rc;
}

/******************************************************************************/
int
respawn_command(char **aCmd)
{
    int rc = -1;

    int exitCode;

    unsigned spawnCount = 0;
    unsigned spawnAttempt = 0;

    unsigned backoffWindowSeconds = 0;

    unsigned long windowStartMillis = monomillis();

    while (1) {

        ++spawnCount;
        ++spawnAttempt;

        DEBUG("Spawning count %u attempt %u", spawnCount, spawnAttempt);

        signal_catch();
        exitCode = spawn_command(aCmd);
        signal_release();

        unsigned long windowEndMillis = monomillis();

        unsigned long runDurationMillis = windowEndMillis - windowStartMillis;

        if (-1 == exitCode)
            goto Finally;

        /* Normally only restart the process if it failed to exit
         * with EXIT_SUCCESS and did not terminate due to a signal. */

        if (!optForever) {
            if (0 < exitCode && exitCode < NUMBEROF(optExit)) {
                if (optExit[exitCode])
                    break;
            }

            if (exitCode >= 0x100)
                break;
        }

        /* Classify the duration that the program runs. Durations less
         * than 1s are considered short, and imply that there is an
         * issue starting the program. Durations between 1s and 60s
         * imply that there is a problem initialising the program
         * (eg issue connecting to remote). Durations longer than 60s
         * are considered long, and imply that the program initialised
         * successfully but terminated unexpectedly. */

        static const unsigned ShortDurationMillis = 1000;
        static const unsigned LongDurationMillis  = 60000;

        if (runDurationMillis <= ShortDurationMillis) {

            /* Reset the backoff window because the previous attempt
             * terminated so quickly. */

            backoffWindowSeconds = 0;

            /* Limit the number of attempts within a 1s window to
             * limit the number of attempts to start a broken program. */

            if (spawnAttempt >= 10) {
                errno = 0;
                error("Failed to start %s", aCmd[0]);
                goto Finally;
            }

            /* Limit the retries so that a broken program cannot
             * overwhelm the host. */

            usleep(1000);

        } else {

            /* Reset the backoff window if the previous attempt ran
             * for a significant period of time. Otherwise, use the
             * backoff window to try to restart the program. */

            if (runDurationMillis > LongDurationMillis)
                backoffWindowSeconds = 0;
            else {
                if (backoffWindowSeconds < 60)
                    backoffWindowSeconds = (backoffWindowSeconds + 1) * 2;

                unsigned backoffDelay = rand() % backoffWindowSeconds;

                DEBUG("Waiting %us before respawning", backoffDelay);
                sleep(backoffDelay);
            }

            windowStartMillis = windowEndMillis;
            spawnAttempt = 0;
        }
    }

    rc = 0;

Finally:

    return rc ? rc : exitCode;
}

/******************************************************************************/
int
main(int argc, char **argv)
{
    int exitCode = 255;

    srand(getpid());

    char **cmd = parse_options(argc, argv);
    if (!cmd || !cmd[0])
        usage();

    int cmdExit = respawn_command(cmd);

    if (-1 == cmdExit)
        goto Finally;

    /* If the child process terminated due to a signal, reproduce
     * that signal here so that the outcome is visible to the
     * grandparent. */

    if (0x100 <= cmdExit) {
        kill(getpid(), cmdExit - 0x100);
        goto Finally;
    }

    exitCode = cmdExit;

Finally:

    return exitCode;
}

/******************************************************************************/
