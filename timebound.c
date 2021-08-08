/**
 * Bound runtime of a monitored process
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

#include "clk.h"
#include "err.h"
#include "int.h"
#include "macros.h"
#include "proc.h"
#include "sig.h"

#include <ctype.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/wait.h>

/******************************************************************************/
static int optHelp;
static uint32_t optMin;
static uint32_t optMax;

/******************************************************************************/
void
usage(void)
{
    static const char usageText[] =
        "[-d] [ min [max] ] -- cmd ...\n"
        "\n"
        "Options:\n"
        "  -d --debug   Emit debug information\n"
        "\n"
        "Arguments:\n"
        "  min          Minimum runtime in seconds\n"
        "  max          Maximum runtime in seconds [default: unbounded]\n"
        "  cmd ...      Program to monitor\n";

    help(usageText, optHelp);
    exit(EXIT_FAILURE);
}

/******************************************************************************/
char **
parse_options(int argc, char **argv)
{
    int rc = -1;

    static char shortOpts[] = "+hd";

    static struct option longOpts[] = {
        { "help",      no_argument,       0, 'h' },
        { "debug",     no_argument,       0, 'd' },
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

        case 'd':
            debug("%s", DebugEnable); break;
        }
    }

    if (argc > optind && isdigit((unsigned char) argv[optind][0])) {

        unsigned long minDuration;
        if (int_strtoul(&minDuration, argv[optind]))
            die("Unable to parse minimum time bound %s", argv[optind]);

        optMin = minDuration;
        if (optMin != minDuration)
            die("Minimum time bound too large %lu", minDuration);

        ++optind;
    }

    if (argc > optind && isdigit((unsigned char) argv[optind][0])) {

        unsigned long maxDuration;
        if (int_strtoul(&maxDuration, argv[optind]))
            die("Unable to parse maximum time bound %s", argv[optind]);

        optMax = maxDuration;
        if (optMax != maxDuration)
            die("Maximum time bound too large %lu", maxDuration);

        if (optMax < optMin)
            die("Maximum time bound %" PRIu32 "s is smaller than "
                "minimum time bound %" PRIu32 "s", optMax, optMin);

        ++optind;
    }

    if (argc > optind && !strcmp("--", argv[optind])) {
        if (argc > ++optind)
            rc = 0;
    }

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
int
spawn_command(char **aCmd)
{
    int rc = -1;

    pid_t childPid = proc_execute(aCmd);
    if (-1 == childPid)
        goto Finally;

    int alarmDelivered = 0;

    while (1) {

        /* Propagate all caught signals to the child process. The child
         * might choose to ignore or catch the signals, and might not
         * terminate.
         *
         * https://people.freebsd.org/~cracauer/homepage-mirror/sigint.html
         */

        sig_atomic_t sigSet = signalset_sample();

        for (int signal = 0; sigSet; ++signal) {
            if (sigSet & 1) {
                if (SIGALRM == signal) {

                    /* Allow the child to react to the first alarm using
                     * SIGTERM, but use SIGKILL on subsequent alarms. */

                    if (alarmDelivered)
                        signal = SIGKILL;
                    else {
                        signal = SIGTERM;
                        alarmDelivered = 1;
                    }

                    DEBUG("Using signal %d for expired alarm", signal);
                }

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

            if (kill(getpid(), stopSig)) {
                warn("Unable to stop process after signal %d", stopSig);
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
run_command(uint32_t aMinDuration, uint32_t aMaxDuration, char **aCmd)
{
    int rc = -1;

    uint64_t beginMillis = clk_monomillis();

    signal_catch();

    if (aMaxDuration) {

        /* Configure the upper bound, and also set the timer to
         * recur after the initial timeout in order to forcibly
         * terminate the child. */

        struct itimerval maxDuration;

        maxDuration.it_interval.tv_sec = 5;
        maxDuration.it_interval.tv_usec = 0;

        maxDuration.it_value.tv_sec = aMaxDuration;
        maxDuration.it_value.tv_usec = 0;

        DEBUG("Configured timer for %" PRIu32 "s", aMaxDuration);

        if (maxDuration.it_value.tv_sec != aMaxDuration)
            die("Unable to set maximum duration %" PRIu32 "s", aMaxDuration);

        if (setitimer(ITIMER_REAL, &maxDuration, 0))
            die("Unable to set timer for "
                "maximum duration %" PRIu32 "s", aMaxDuration);
    }

    int exitCode = spawn_command(aCmd);
    if (-1 == exitCode)
        goto Finally;

    rc = exitCode;

Finally:

    FINALLY({
        struct itimerval disableTimer;

        disableTimer.it_interval.tv_sec = 0;
        disableTimer.it_interval.tv_usec = 0;

        disableTimer.it_value.tv_sec = 0;
        disableTimer.it_value.tv_usec = 0;

        if (setitimer(ITIMER_REAL, &disableTimer, 0))
            fatal("Unable to disable timer");

        signal_release();

        while (-1 != rc && exitCode < 0x100) {
            uint64_t endMillis = clk_monomillis();

            uint64_t durationMillis = endMillis - beginMillis;

            DEBUG("Elapsed runtime %" PRIu64 "ms", durationMillis);

            if (durationMillis / 1000 >= aMinDuration)
                break;

            uint32_t sleepMillis =
                (uint64_t) aMinDuration * 1000 - durationMillis;

            DEBUG("Waiting %" PRIu32 "ms", sleepMillis);
            clk_sleepmillis(sleepMillis);
        }
    });

    return rc;
}

/******************************************************************************/
int
main(int argc, char **argv)
{
    int exitCode = 255;

    char **cmd = parse_options(argc, argv);
    if (!cmd || !cmd[0])
        usage();

    int cmdExit = run_command(optMin, optMax, cmd);

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
