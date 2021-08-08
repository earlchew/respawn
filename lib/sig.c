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

#include "sig.h"

#include "err.h"

/******************************************************************************/
static volatile sig_atomic_t SignalSet_;

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
sig_atomic_t
signalset_sample(void)
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
signalset_add(int aSignal)
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
    int         mFlags;
} SigStrategy[] = {
    { "SIGHUP",  SIGHUP },
    { "SIGQUIT", SIGQUIT },
    { "SIGINT",  SIGINT },
    { "SIGABRT", SIGABRT },
    { "SIGTERM", SIGTERM },
    { "SIGCONT", SIGCONT },
    { "SIGALRM", SIGALRM },
};

/*----------------------------------------------------------------------------*/
static void
signal_handler_(int aSignal)
{
    signalset_add(aSignal);
}

/*----------------------------------------------------------------------------*/
void
signal_catch(void)
{
    /* Only catch signals that are still set at their default action,
     * and not being ignored. Processes inherit default and ignored
     * signal settings from their parent. */

    (void) signalset_sample();

    for (unsigned ix = 0; ix < NUMBEROF(SigStrategy); ++ix) {

        struct sigaction action;

        if (sigaction(SigStrategy[ix].mSignal, 0, &action))
            die("Unable to query signal %s", SigStrategy[ix].mName);

        SigStrategy[ix].mHandler = action.sa_handler;
        SigStrategy[ix].mFlags   = action.sa_flags;

        action.sa_handler = signal_handler_;
        action.sa_flags   = action.sa_flags & ~ SA_RESTART;

        if (SIG_DFL == SigStrategy[ix].mHandler) {
            DEBUG("Installing catcher for signal %d", SigStrategy[ix].mSignal);
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
        action.sa_flags   = SigStrategy[ix].mFlags;

        if (SIG_DFL == SigStrategy[ix].mHandler) {
            if (sigaction(SigStrategy[ix].mSignal, &action, 0))
                die("Unable to reset signal %s", SigStrategy[ix].mName);
        }
    }
}

/******************************************************************************/
