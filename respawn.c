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

#include "clk.h"
#include "err.h"
#include "fd.h"
#include "int.h"
#include "proc.h"
#include "sig.h"
#include "macros.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/event.h>
#include <sys/wait.h>

/******************************************************************************/
static int optHelp;
static int optContinue;
static int optForever;
static int optParented;

static unsigned char optExit[256] = { 1 };

/******************************************************************************/
void
usage(void)
{
    static const char usageText[] =
        "[-dfZ] [-x N,...] -- cmd ...\n"
        "\n"
        "Options:\n"
        "  -d --debug      Emit debug information\n"
        "  -f --forever    Continually restart the monitored process\n"
        "  -P --parented   Terminate if no longer parented\n"
        "  -Z --continue   Continue monitored process if it suspends\n"
        "  -x --exit N,..  Additional success exit codes [default: 0]\n"
        "  -x --exit none  No success exit codes [default: 0]\n"
        "\n"
        "Arguments:\n"
        "  cmd ...         Program to monitor\n";

    help(usageText, optHelp);

    exit(EXIT_FAILURE);
}

/******************************************************************************/
char **
parse_options(int argc, char **argv)
{
    int rc = -1;

    static char shortOpts[] = "+hdfPZx:";

    static struct option longOpts[] = {
        { "help",      no_argument,       0, 'h' },
        { "debug",     no_argument,       0, 'd' },
        { "forever",   no_argument,       0, 'f' },
        { "parented",  no_argument,       0, 'P' },
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

        case 'P':
            optParented = 1; break;

        case 'Z':
            optContinue = 1; break;

        case 'd':
            debug("%s", DebugEnable); break;

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

                    unsigned long exitCode;
                    if (int_strtoul(&exitCode, word))
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
void
terminate(void)
{
    signal(SIGTERM, SIG_IGN);
    killpg(0, SIGTERM);
    sleep(3);
    killpg(0, SIGKILL);
}

/******************************************************************************/
int
spawn_command(char **aCmd, int aMonitorFd)
{
    int rc = -1;

    pid_t childPid = proc_execute(aCmd);
    if (-1 == childPid) {
        warn("Unable to spawn command %s", aCmd[0]);
        goto Finally;
    }

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
                DEBUG(
                    "Delivering signal %d to child process %d",
                    signal, childPid);

                kill(childPid, signal);
            }
            sigSet >>= 1;
        }

        int procEvent = proc_monitor_wait(aMonitorFd);
        if (-1 == procEvent) {
            warn("Unable to wait for process monitor");
            goto Finally;
        }

        if (procEvent) {

            /* If the process must be parented, and the parent has exited,
             * there is no parent waiting for exit status.
             */

            DEBUG("Parent process %d exited", procEvent);

            terminate();
            goto Finally;
        }

        int childStatus;

        pid_t pid = waitpid(childPid, &childStatus, WNOHANG|WUNTRACED);
        if (-1 == pid) {
            if (EINTR == errno)
                continue;
            warn("Unable to wait for child process %d", childPid);
            goto Finally;
        }

        if (pid) {

            if (WIFSTOPPED(childStatus)) {
                int stopSig = WSTOPSIG(childStatus);

                DEBUG("Child process %d stopped signal %d", childPid, stopSig);

                /* http://curiousthing.org/sigttin-sigttou-deep-dive-linux */

                if (SIGSTOP == stopSig || SIGTSTP == stopSig) {
                    if (optContinue) {
                        signalset_add(SIGCONT);
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
    }

Finally:

    return rc;
}

/******************************************************************************/
int
respawn_command(char **aCmd, int aMonitorFd)
{
    int rc = -1;

    int exitCode;

    unsigned spawnCount = 0;
    unsigned spawnAttempt = 0;

    unsigned backoffWindowSeconds = 0;

    uint64_t windowStartMillis = clk_monomillis();

    while (1) {

        ++spawnCount;
        ++spawnAttempt;

        DEBUG("Spawning count %u attempt %u", spawnCount, spawnAttempt);

        signal_catch();
        exitCode = spawn_command(aCmd, aMonitorFd);
        signal_release();

        uint64_t windowEndMillis = clk_monomillis();

        uint64_t runDurationMillis = windowEndMillis - windowStartMillis;

        if (-1 == exitCode)
            goto Finally;

        /* Normally only restart the process if it failed to exit
         * with EXIT_SUCCESS and did not terminate due to a signal. */

        if (!optForever) {
            if (0 <= exitCode && exitCode < NUMBEROF(optExit)) {
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

            clk_sleepmillis(1);

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
                clk_sleepmillis(backoffDelay * 1000);
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
    int monitorFd = -1;

    srand(getpid());

    char **cmd = parse_options(argc, argv);
    if (!cmd || !cmd[0])
        usage();

    pid_t parentPid = 0;
    if (optParented) {
        parentPid = getppid();
        if (1 >= parentPid)
            goto Finally;
    }

    monitorFd = proc_monitor_create(parentPid);
    if (-1 == monitorFd) {
        warn("Unable to create proc monitor");
        goto Finally;
    }

    int cmdExit = respawn_command(cmd, monitorFd);

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
