#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef __IO
#define __IO volatile
#endif
#ifndef __PACKED
#define __PACKED __attribute__((packed, aligned(1)))
#endif
#ifndef __ALIGN_BEGIN
#define __ALIGN_BEGIN
#endif
#ifndef __ALIGN_END
#define __ALIGN_END __attribute__((aligned(4)))
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef UNUSED
#define UNUSED(X) ((void)(X))
#endif

#define USBD_MAX_NUM_INTERFACES       1U
#define USBD_MAX_NUM_CONFIGURATION    1U
#define USBD_MAX_STR_DESC_SIZ         0x100U
#define USBD_SUPPORT_USER_STRING      0U
#define USBD_SELF_POWERED             0U
#define USBD_DEBUG_LEVEL              0U

#define USBD_malloc                   malloc
#define USBD_free                     free
#define USBD_memset                   memset
#define USBD_memcpy                   memcpy

#define USBD_UsrLog(...)              do { } while (0)
#define USBD_ErrLog(...)              do { } while (0)
#define USBD_DbgLog(...)              do { } while (0)

#endif
