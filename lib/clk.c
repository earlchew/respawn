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

#include "clk.h"

#include "err.h"

#include <time.h>

/******************************************************************************/
static volatile uint64_t ReferenceMillis;

uint64_t
clk_monomillis(void)
{
    struct timespec clockTime;

    if (clock_gettime(CLOCK_MONOTONIC, &clockTime))
        fatal("Unable to get CLOCK_MONOTONIC time");

    return
        1000 * (uint64_t) clockTime.tv_sec
            + clockTime.tv_nsec / 1000000
            - ReferenceMillis;
}

static void
clk_monomillis_init_(void)
__attribute__((constructor));

static void
clk_monomillis_init_(void)
{
    uint64_t monoMillis;

    /* Count the number of milliseconds elapsed since program initialisation
     * so that the observed clock will advance from 0 without concern
     * for wrapping. */

    ReferenceMillis = 0;

    do
        monoMillis = clk_monomillis();
    while (!monoMillis);

    ReferenceMillis = monoMillis;
}

/******************************************************************************/
void
clk_sleepmillis(uint32_t aDuration)
{
    uint64_t now = clk_monomillis();
    uint64_t deadline = now + aDuration;

    /* Do not use clock_nanosleep(2) because it is not available on MacOS,
     * and do not use sleep(3) because in principle it can interfere with
     * SIGALRM handling.
     *
     * Use a fixed deadline so that long durations will be immune to the
     * affect of small errors accumulating over multiple interrupted
     * nanosleep() calls. */

    while (now < deadline) {
        uint64_t duration = deadline - now;

        struct timespec sleepDuration;
        sleepDuration.tv_sec = duration / 1000;
        sleepDuration.tv_nsec = duration % 1000 * 1000000;

        nanosleep(&sleepDuration, 0);

        now = clk_monomillis();
    }
}

/******************************************************************************/
