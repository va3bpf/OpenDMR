/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, hardware abstraction: the system calls newlib needs (snprintf() and the like). There is no heap
 * (docs/coding-style.md: no dynamic memory), no file: they all fail.
 */

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <reent.h>

void *_sbrk_r(struct _reent *r, ptrdiff_t increment)
{
    (void)increment;
    r->_errno = ENOMEM;
    return (void *)-1;
}

int _close_r(struct _reent *r, int fd)
{
    (void)fd;
    r->_errno = EBADF;
    return -1;
}

int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
    (void)fd;
    (void)st;
    r->_errno = EBADF;
    return -1;
}

int _getpid_r(struct _reent *r)
{
    (void)r;
    return 1;
}

int _isatty_r(struct _reent *r, int fd)
{
    (void)fd;
    r->_errno = EBADF;
    return 0;
}

int _kill_r(struct _reent *r, int pid, int sig)
{
    (void)pid;
    (void)sig;
    r->_errno = EINVAL;
    return -1;
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    r->_errno = EBADF;
    return -1;
}

_ssize_t _read_r(struct _reent *r, int fd, void *buffer, size_t length)
{
    (void)fd;
    (void)buffer;
    (void)length;
    r->_errno = EBADF;
    return -1;
}

_ssize_t _write_r(struct _reent *r, int fd, const void *buffer, size_t length)
{
    (void)fd;
    (void)buffer;
    (void)length;
    r->_errno = EBADF;
    return -1;
}
