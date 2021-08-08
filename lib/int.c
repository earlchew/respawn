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

#include "int.h"

#include "err.h"

#include "macros.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/******************************************************************************/
int
int_strtoul(unsigned long *aInteger, const char *aString)
{
    int rc = -1;

    if (!isdigit((unsigned char) *aString))
        goto Finally;

    if ('0' == aString[0] && !aString[1])
        goto Finally;

    char *endPtr;

    errno = 0;
    unsigned long value = strtoul(aString, &endPtr, 10);

    if (ERANGE == errno)
        goto Finally;

    if (*endPtr)
        goto Finally;

    *aInteger = value;

    rc = 0;

Finally:

    return rc;
}

/******************************************************************************/
