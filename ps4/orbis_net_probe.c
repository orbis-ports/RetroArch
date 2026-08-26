/* Which socket API is this process actually ALLOWED to use?
 *
 * ⚠ THE QUESTION IS PERMISSION, NOT PRESENCE, AND THE TWO LOOK IDENTICAL AT LINK TIME.
 * libkernel.so exports all 22 POSIX socket calls under their own names, so a survey by symbol
 * says the platform needs no wrapper at all - and the frontend links, and socket() even returns
 * a descriptor. What it does not do is connect: measured on hardware,
 *
 *   [net_http] socket_connect_failed: host=192.168.100.1 port=8000 fd=19 connected=0
 *              errno=13 (Permission denied)
 *
 * on a plain LAN address, no DNS and no TLS in the way. EACCES from connect() on a fake-signed
 * process is an authority decision, and no amount of correct code above it changes the answer.
 *
 * The log channel has been sending datagrams from this same console since the first session, and
 * it does NOT use these calls - orbis-compat/optional/orbis_netlog.cpp goes through sceNetSocket.
 * So the difference that matters might be sceNet-versus-libkernel rather than UDP-versus-TCP, and
 * that is worth one measurement before anyone writes a wrapper layer on the strength of a guess.
 *
 * This probe tries the same connection twice, once down each path, and prints both answers. It is
 * compiled only under ORBIS_NET_TRACE. */

#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>

#include <orbis/Net.h>

/* ⚠ ps4_log, NOT RARCH_LOG. This probe runs from frontend_orbis_get_env(), which happens
 * before RetroArch's own logger exists - the first attempt printed nothing at all for that
 * reason, on a build that was otherwise correct. ps4_log() writes klog AND the UDP channel
 * from the first instruction of main(); ps4_log_frame() is the UDP-only variant. */
#include <ps4_app.h>

#ifndef ORBIS_NET_PROBE_HOST
#define ORBIS_NET_PROBE_HOST "192.168.100.1"
#endif
#ifndef ORBIS_NET_PROBE_PORT
#define ORBIS_NET_PROBE_PORT 8000
#endif

/* The SDK ships the generic OrbisNetSockaddr (len, family, sa_data[14]) but no
 * OrbisNetSockaddrIn; the AF_INET layout fits inside sa_data. Same shape orbis_netlog.cpp
 * builds for the same reason. */
struct probe_sin
{
   uint8_t  sin_len;
   uint8_t  sin_family;
   uint16_t sin_port;
   uint32_t sin_addr;
   uint8_t  sin_zero[8];
};

static void probe_posix(void)
{
   struct sockaddr_in sa;
   int fd = socket(AF_INET, SOCK_STREAM, 0);

   if (fd < 0)
   {
      ps4_log("[net probe] posix  socket() failed: errno=%d (%s)",
            errno, strerror(errno));
      return;
   }

   memset(&sa, 0, sizeof(sa));
   sa.sin_family      = AF_INET;
   sa.sin_port        = htons(ORBIS_NET_PROBE_PORT);
   sa.sin_addr.s_addr = inet_addr(ORBIS_NET_PROBE_HOST);

   if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
      ps4_log("[net probe] posix  fd=%d connect(%s:%d) failed: errno=%d (%s)",
            fd, ORBIS_NET_PROBE_HOST, ORBIS_NET_PROBE_PORT, errno, strerror(errno));
   else
      ps4_log("[net probe] posix  fd=%d connect(%s:%d) OK",
            fd, ORBIS_NET_PROBE_HOST, ORBIS_NET_PROBE_PORT);

   close(fd);
}

static void probe_scenet(void)
{
   struct probe_sin sa;
   OrbisNetId s;
   int32_t rc;

   sceNetInit();

   /* sceNetSocket takes a name the system uses for accounting; POSIX socket() has no such
    * argument, so a wrapper would have to invent one. The log channel passes "<tag>-log". */
   s = sceNetSocket("retroarch-probe", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
   if (s < 0)
   {
      ps4_log("[net probe] sceNet sceNetSocket() failed: 0x%08X", (unsigned)s);
      return;
   }

   memset(&sa, 0, sizeof(sa));
   sa.sin_len    = sizeof(sa);
   sa.sin_family = ORBIS_NET_AF_INET;
   sa.sin_port   = sceNetHtons(ORBIS_NET_PROBE_PORT);
   if (sceNetInetPton(ORBIS_NET_AF_INET, ORBIS_NET_PROBE_HOST, &sa.sin_addr) != 1)
   {
      ps4_log("[net probe] sceNet sceNetInetPton failed");
      sceNetSocketClose(s);
      return;
   }

   rc = sceNetConnect(s, (const OrbisNetSockaddr*)&sa, sizeof(sa));
   if (rc < 0)
      ps4_log("[net probe] sceNet id=%d sceNetConnect(%s:%d) failed: 0x%08X",
            (int)s, ORBIS_NET_PROBE_HOST, ORBIS_NET_PROBE_PORT, (unsigned)rc);
   else
      ps4_log("[net probe] sceNet id=%d sceNetConnect(%s:%d) OK",
            (int)s, ORBIS_NET_PROBE_HOST, ORBIS_NET_PROBE_PORT);

   sceNetSocketClose(s);
}

/* ⚠ WHERE THE CORE DOWNLOADER ACTUALLY DIES, and it is not connect().
 *
 * socket_connect_with_timeout() (libretro-common/net/net_socket.c:711) calls socket_nonblock()
 * FIRST and returns false if it fails - before connect() is ever reached. ORBIS takes the
 * generic branch of socket_set_block(), which is fcntl(F_GETFL) followed by fcntl(F_SETFL).
 * net_http then logs "socket_connect_failed" with whatever errno is lying around, which is how
 * an fcntl refusal reads on hardware as a connect refusal:
 *
 *   [net_http] socket_connect_failed: host=192.168.100.1 port=8000 fd=19 errno=13
 *
 * while a plain blocking connect() to the same address, in this same probe, succeeds. So the
 * two steps have to be measured apart. FIONBIO is computed rather than included: this SDK's
 * bits/ioctl.h offers only LINUX_FIONBIO (0x5421), a Linux number on a FreeBSD target - the same
 * class of defect as its ETIMEDOUT. _IOW('f', 126, int) is what the kernel here expects. */
#define ORBIS_FIONBIO _IOW('f', 126, int)

static void probe_nonblock(void)
{
   struct sockaddr_in sa;
   int one = 1;
   int flags;
   int fd;

   memset(&sa, 0, sizeof(sa));
   sa.sin_family      = AF_INET;
   sa.sin_port        = htons(ORBIS_NET_PROBE_PORT);
   sa.sin_addr.s_addr = inet_addr(ORBIS_NET_PROBE_HOST);

   if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
   {
      ps4_log("[net probe] nonblock socket() failed: errno=%d (%s)", errno, strerror(errno));
      return;
   }

   errno = 0;
   flags = fcntl(fd, F_GETFL);
   ps4_log("[net probe] nonblock fcntl(F_GETFL)  = %d errno=%d (%s)",
         flags, errno, strerror(errno));

   if (flags >= 0)
   {
      errno = 0;
      ps4_log("[net probe] nonblock fcntl(F_SETFL) = %d errno=%d (%s)",
            fcntl(fd, F_SETFL, flags | O_NONBLOCK), errno, strerror(errno));
   }

   errno = 0;
   ps4_log("[net probe] nonblock ioctl(FIONBIO)  = %d errno=%d (%s)",
         ioctl(fd, ORBIS_FIONBIO, &one), errno, strerror(errno));

   errno = 0;
   ps4_log("[net probe] nonblock connect()       = %d errno=%d (%s)  [EINPROGRESS is success]",
         connect(fd, (struct sockaddr*)&sa, sizeof(sa)), errno, strerror(errno));

   close(fd);
}

/* ⚠ WHAT IS LEFT WHEN fcntl AND ioctl ARE BOTH REFUSED.
 *
 * Measured on hardware: fcntl(F_GETFL) reads flags fine (= 2), fcntl(F_SETFL) and
 * ioctl(FIONBIO) both answer EACCES, and a blocking connect() to the same address succeeds.
 * So the socket works; only the switch to non-blocking is denied. Three candidates remain,
 * and each implies a different fix upstream of here:
 *
 *   sceNetSetsockopt(SO_NBIO) on the SAME fd - if this works, musl's socket() and sceNetSocket()
 *     really are one descriptor namespace (musl's is a wrapper over __sys_socketex, the same
 *     syscall), and socket_set_block() just needs an ORBIS arm. Cleanest outcome by far.
 *   setsockopt(SO_SNDTIMEO/SO_RCVTIMEO) - if THOSE work, a blocking connect with a send timeout
 *     gives the HTTP task the deadline it actually needs, without non-blocking at all.
 *   poll() on a blocking socket - net_http waits before every read, so if poll works the read
 *     loop does not need non-blocking either.
 *
 * The SDK names no sceNet option constants at all, so SOL_SOCKET/SO_NBIO are the values this
 * API family uses elsewhere, tried rather than trusted. */
#define ORBIS_NET_SOL_SOCKET 0xFFFF
#define ORBIS_NET_SO_NBIO    0x1200

static void probe_alternatives(void)
{
   struct sockaddr_in sa;
   struct timeval tv;
   int one = 1;
   int fd;

   memset(&sa, 0, sizeof(sa));
   sa.sin_family      = AF_INET;
   sa.sin_port        = htons(ORBIS_NET_PROBE_PORT);
   sa.sin_addr.s_addr = inet_addr(ORBIS_NET_PROBE_HOST);

   if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
   {
      ps4_log("[net probe] alt socket() failed: errno=%d (%s)", errno, strerror(errno));
      return;
   }

   errno = 0;
   ps4_log("[net probe] alt sceNetSetsockopt(SO_NBIO) = 0x%08X errno=%d (%s)",
         (unsigned)sceNetSetsockopt(fd, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO,
            &one, sizeof(one)), errno, strerror(errno));

   tv.tv_sec  = 5;
   tv.tv_usec = 0;
   errno = 0;
   ps4_log("[net probe] alt setsockopt(SO_SNDTIMEO)   = %d errno=%d (%s)",
         setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)), errno, strerror(errno));
   errno = 0;
   ps4_log("[net probe] alt setsockopt(SO_RCVTIMEO)   = %d errno=%d (%s)",
         setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), errno, strerror(errno));

   errno = 0;
   if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0)
   {
      struct pollfd pfd;
      pfd.fd      = fd;
      pfd.events  = POLLOUT;
      pfd.revents = 0;
      errno = 0;
      ps4_log("[net probe] alt poll(POLLOUT)          = %d revents=0x%x errno=%d (%s)",
            poll(&pfd, 1, 1000), (unsigned)pfd.revents, errno, strerror(errno));
   }
   else
      ps4_log("[net probe] alt connect() failed: errno=%d (%s)", errno, strerror(errno));

   close(fd);
}

void orbis_net_probe(void)
{
   ps4_log("[net probe] target %s:%d - comparing libkernel POSIX against sceNet",
         ORBIS_NET_PROBE_HOST, ORBIS_NET_PROBE_PORT);
   probe_posix();
   probe_scenet();
   probe_nonblock();
   probe_alternatives();
}
