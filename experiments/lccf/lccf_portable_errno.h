/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LCCF_PORTABLE_ERRNO_H
#define LLAM_EXPERIMENTS_LCCF_PORTABLE_ERRNO_H

#include <errno.h>

/* Microsoft CRT does not expose every POSIX errno used by the model. */
#ifndef ECANCELED
#define ECANCELED 2001
#endif
#ifndef ENODATA
#define ENODATA 2002
#endif
#ifndef ENODEV
#define ENODEV 2003
#endif
#ifndef ENOTSUP
#define ENOTSUP 2004
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 2005
#endif
#ifndef EPROTO
#define EPROTO 2006
#endif
#ifndef ESTALE
#define ESTALE 2007
#endif

#endif
