/* Copyright  (C) 2010-2022 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (net_compat.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>

#include <compat/strl.h>
#include <retro_miscellaneous.h>
#include <retro_timers.h>

#include <net/net_compat.h>

#if defined(_WIN32) && !defined(_XBOX)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
   struct sockaddr_storage addr;

   switch (af)
   {
      case AF_INET:
         memcpy(&((struct sockaddr_in*)&addr)->sin_addr, src,
            sizeof(struct in_addr));
         break;
#ifdef HAVE_INET6
      case AF_INET6:
         memcpy(&((struct sockaddr_in6*)&addr)->sin6_addr, src,
            sizeof(struct in6_addr));
         break;
#endif
      default:
         return NULL;
   }

   addr.ss_family = af;
   if (getnameinfo((struct sockaddr*)&addr, sizeof(addr), dst, size, NULL, 0,
         NI_NUMERICHOST))
      return NULL;

   return dst;
}

int inet_pton(int af, const char *src, void *dst)
{
   struct addrinfo *addr = NULL;
   struct addrinfo hints = {0};

   switch (af)
   {
      case AF_INET:
#ifdef HAVE_INET6
      case AF_INET6:
#endif
         break;
      default:
         return -1;
   }

   hints.ai_family = af;
   hints.ai_flags  = AI_NUMERICHOST;
   switch (getaddrinfo(src, NULL, &hints, &addr))
   {
      case 0:
         break;
      case EAI_NONAME:
         return 0;
      default:
         return -1;
   }

   if (!addr)
      return -1;

   switch (af)
   {
      case AF_INET:
         memcpy(dst, &((struct sockaddr_in*)addr->ai_addr)->sin_addr,
            sizeof(struct in_addr));
         break;
#ifdef HAVE_INET6
      case AF_INET6:
         memcpy(dst, &((struct sockaddr_in6*)addr->ai_addr)->sin6_addr,
            sizeof(struct in6_addr));
         break;
#endif
      default:
         break;
   }

   freeaddrinfo(addr);

   return 1;
}
#endif

#elif defined(_XBOX)
struct hostent *gethostbyname(const char *name)
{
   static struct in_addr addr = {0};
   static struct hostent he   = {0};
   WSAEVENT event;
   XNDNS          *dns = NULL;
   struct hostent *ret = NULL;

   if (!name)
      return NULL;

   event = WSACreateEvent();

   XNetDnsLookup(name, event, &dns);
   if (!dns)
   {
      WSACloseEvent(event);
      return NULL;
   }

   WaitForSingleObject((HANDLE)event, INFINITE);

   if (!dns->iStatus)
   {
      memcpy(&addr, dns->aina, sizeof(addr));

      he.h_name      = NULL;
      he.h_aliases   = NULL;
      he.h_addrtype  = AF_INET;
      he.h_length    = sizeof(addr);
      he.h_addr_list = &he.h_addr;
      he.h_addr      = (char*)&addr;

      ret = &he;
   }

   WSACloseEvent(event);
   XNetDnsRelease(dns);

   return ret;
}

#elif defined(VITA)
#define COMPAT_NET_INIT_SIZE 0x80000

char *inet_ntoa(struct in_addr in)
{
   static char ip_addr[16];

   sceNetInetNtop(AF_INET, &in, ip_addr, sizeof(ip_addr));

   return ip_addr;
}

int inet_aton(const char *cp, struct in_addr *inp)
{
   return sceNetInetPton(AF_INET, cp, inp);
}

uint32_t inet_addr(const char *cp)
{
   struct in_addr in;

   return (sceNetInetPton(AF_INET, cp, &in) == 1) ? in.s_addr : INADDR_NONE;
}

struct hostent *gethostbyname(const char *name)
{
   static struct SceNetInAddr addr = {0};
   static struct hostent      he   = {0};
   int rid;
   struct hostent *ret = NULL;

   if (!name)
      return NULL;

   rid = sceNetResolverCreate("resolver", NULL, 0);
   if (rid < 0)
      return NULL;

   if (sceNetResolverStartNtoa(rid, name, &addr, 0, 0, 0) < 0)
      goto done;

   he.h_name      = NULL;
   he.h_aliases   = NULL;
   he.h_addrtype  = AF_INET;
   he.h_length    = sizeof(addr);
   he.h_addr_list = &he.h_addr;
   he.h_addr      = (char*)&addr;

   ret = &he;

done:
   sceNetResolverDestroy(rid);

   return ret;
}

#elif defined(GEKKO)
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
   const char *addr_str = inet_ntoa(*(struct in_addr*)src);

   if (addr_str)
   {
      strlcpy(dst, addr_str, size);

      return dst;
   }

   return NULL;
}

int inet_pton(int af, const char *src, void *dst)
{
   if (inet_aton(src, (struct in_addr*)dst))
      return 1;

   return 0;
}

#elif defined(WIIU)
#include <malloc.h>

static int _net_compat_thread_entry(int argc, const char **argv)
{
   void *buf = memalign(128, WIIU_RCVBUF + WIIU_SNDBUF);

   if (!buf)
      return -1;

   somemopt(1, buf, WIIU_RCVBUF + WIIU_SNDBUF, 0);

   free(buf);

   return 0;
}

static void _net_compat_thread_cleanup(OSThread *thread, void *stack)
{
   free(stack);
}

#elif defined(_3DS)
#include <malloc.h>
#include <3ds/types.h>
#include <3ds/services/soc.h>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#endif

int getaddrinfo_retro(const char *node, const char *service,
      struct addrinfo *hints, struct addrinfo **res)
{
#if defined(HAVE_SOCKET_LEGACY) || defined(WIIU)
   struct addrinfo default_hints = {0};

   if (!hints)
      hints            = &default_hints;
   if (!hints->ai_family)
      hints->ai_family = AF_INET;

   if (!node)
      node = (hints->ai_flags & AI_PASSIVE) ? "0.0.0.0" : "127.0.0.1";
#endif

#if defined(ORBIS)
   /* ⚠ musl's RESOLVER CANNOT RUN ON THIS CONSOLE, AND IT FAILS BEFORE IT EVER ASKS ANYTHING.
    *
    * Measured on hardware. sceNetGetDnsInfo() reports a perfectly good nameserver, so there is
    * nothing wrong with the network configuration - and getaddrinfo() still answers -11
    * (EAI_SYSTEM) with errno EACCES. The reason is in how musl opens its query socket:
    * __res_msend asks for SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, and this SDK defines those two
    * flags as 02000000 and 04000 - LINUX's values. The kernel here is FreeBSD-derived, where
    * they are 0x10000000/0x20000000 and where socket() did not accept them at all until
    * FreeBSD 10. The same family of defect as bits/ioctl.h offering only LINUX_FIONBIO, and as
    * the shipped libc++ comparing against Linux's ETIMEDOUT.
    *
    * ⚠ AND OVERRIDING socket() WOULD NOT BE ENOUGH. Probed: socket() with those flags returns a
    * valid descriptor (16) AND leaves errno at 13. musl tries the flagged form, is refused,
    * retries plain, then attempts fcntl(F_SETFL, O_NONBLOCK) - which this platform also refuses
    * - and hands back the descriptor anyway. So its resolver proceeds on a BLOCKING socket it
    * believes is non-blocking. Repairing the flags would leave that mismatch untouched.
    *
    * Sony's resolver has no such problem: sceNetPoolCreate() then sceNetResolverStartNtoa()
    * answered 188.114.96.11 for buildbot.libretro.com in 73 ms, first try. ⚠ The pool is what
    * the first attempt was missing - sceNetResolverCreate(memid=0) fails with 0x80410109.
    *
    * Pool and resolver are created and destroyed PER CALL rather than kept: net_http resolves
    * from a worker thread and caches the result itself, so this runs rarely, and per-call state
    * needs no lock to be safe against that. */
   {
      struct addrinfo    *info = (struct addrinfo*)calloc(1, sizeof(*info));
      struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(*addr));

      if (!info || !addr)
         goto orbis_failure;

      info->ai_family   = AF_INET;
      info->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
      info->ai_protocol = hints ? hints->ai_protocol : 0;
      info->ai_addrlen  = sizeof(*addr);
      info->ai_addr     = (struct sockaddr*)addr;

      memset(addr, 0, sizeof(*addr));
      addr->sin_family = AF_INET;

      if (service)
      {
         char *service_end = NULL;
         uint16_t port     = (uint16_t)strtoul(service, &service_end, 10);

         /* Numeric ports only. net_http sets AI_NUMERICSERV and passes a number; a name
          * here would need /etc/services, which this platform does not have either. */
         if (service_end == service || *service_end)
            goto orbis_failure;

         addr->sin_port = htons(port);
      }

      if (!node)
         goto orbis_failure;

      /* A literal address needs no resolver at all, and this is the common case for the
       * bring-up server. inet_aton is real here - it comes from libc.a, not from the
       * resolver. */
      if (inet_aton(node, &addr->sin_addr))
      {
         *res = info;
         return 0;
      }

      if (hints && (hints->ai_flags & AI_NUMERICHOST))
         goto orbis_failure;

      {
         OrbisNetInAddr resolved;
         OrbisNetId     rid;
         int32_t        pool = sceNetPoolCreate("retroarch-dns", 4 * 1024, 0);

         if (pool < 0)
            goto orbis_failure;

         if ((rid = sceNetResolverCreate("retroarch-dns", pool, 0)) < 0)
         {
            sceNetPoolDestroy(pool);
            goto orbis_failure;
         }

         memset(&resolved, 0, sizeof(resolved));

         /* 5 s, 3 retries: net_http gives its own task a 5 s connect budget, and a name that
          * takes longer than that to resolve would not have connected either. */
         if (sceNetResolverStartNtoa(rid, node, &resolved,
                  5 * 1000 * 1000, 3, 0) < 0)
         {
            sceNetResolverDestroy(rid);
            sceNetPoolDestroy(pool);
            goto orbis_failure;
         }

         memcpy(&addr->sin_addr, &resolved, sizeof(resolved));

         sceNetResolverDestroy(rid);
         sceNetPoolDestroy(pool);
      }

      *res = info;

      return 0;

orbis_failure:
      free(addr);
      free(info);

      return -1;
   }
#elif defined(HAVE_SOCKET_LEGACY)
   {
      struct addrinfo    *info = (struct addrinfo*)calloc(1, sizeof(*info));
      struct sockaddr_in *addr = (struct sockaddr_in*)malloc(sizeof(*addr));

      if (!info || !addr)
         goto failure;

      info->ai_family   = AF_INET;
      info->ai_socktype = hints->ai_socktype;
      info->ai_protocol = hints->ai_protocol;
      info->ai_addrlen  = sizeof(*addr);
      info->ai_addr     = (struct sockaddr*)addr;
      /* We ignore AI_CANONNAME; ai_canonname is always NULL. */

      addr->sin_family = AF_INET;

      if (service)
      {
         /* We can only handle numeric ports; ignore AI_NUMERICSERV. */
         char *service_end = NULL;
         uint16_t port     = (uint16_t)strtoul(service, &service_end, 10);

         if (service_end == service || *service_end)
            goto failure;

         addr->sin_port = htons(port);
      }

      if (hints->ai_flags & AI_NUMERICHOST)
      {
         if (!inet_aton(node, &addr->sin_addr))
            goto failure;
      }
      else
      {
         struct hostent *host = gethostbyname(node);

         if (!host || !host->h_addr)
            goto failure;

         memcpy(&addr->sin_addr, host->h_addr, sizeof(addr->sin_addr));
      }

      *res = info;

      return 0;

failure:
      free(addr);
      free(info);

      return -1;
   }
#else
   return getaddrinfo(node, service, hints, res);
#endif
}

void freeaddrinfo_retro(struct addrinfo *res)
{
   /* ORBIS builds the same single-node shape the legacy branch does - one addrinfo owning one
    * sockaddr_in, both from malloc - so it frees the same way. Handing one of those to the
    * SDK's freeaddrinfo() would be a free() of memory its allocator never issued. */
#if defined(ORBIS) || defined(HAVE_SOCKET_LEGACY)
   if (res)
   {
      free(res->ai_addr);
      free(res);
   }
#else
   freeaddrinfo(res);
#endif
}

int getnameinfo_retro(const struct sockaddr *addr, socklen_t addrlen,
      char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
#ifdef HAVE_SOCKET_LEGACY
   const struct sockaddr_in *addr4 = (const struct sockaddr_in*)addr;

   /* We cannot perform reverse DNS lookups here; ignore the following flags:
      NI_NAMEREQD
      NI_NOFQDN
      NI_NUMERICHOST (always enforced)
    */
   if (host && hostlen)
   {
      const char *_host = inet_ntoa(addr4->sin_addr);

      if (!_host)
         return -1;

      strlcpy(host, _host, hostlen);
   }

   /* We cannot get service names here; ignore the following flags:
      NI_DGRAM
      NI_NUMERICSERV (always enforced)
    */
   if (serv && servlen)
      snprintf(serv, servlen, "%hu", (unsigned short)ntohs(addr4->sin_port));

   return 0;
#else
   return getnameinfo(addr, addrlen, host, hostlen, serv, servlen, flags);
#endif
}

bool addr_6to4(struct sockaddr_storage *addr)
{
#ifdef HAVE_INET6
   /* ::ffff:a.b.c.d */
   static const uint16_t prefix[] = {0,0,0,0,0,0xffff};
   uint32_t address;
   uint16_t port;
   struct sockaddr_in6 *addr6 = (struct sockaddr_in6*)addr;
   struct sockaddr_in  *addr4 = (struct sockaddr_in*)addr;

   switch (addr->ss_family)
   {
      case AF_INET:
         /* No need to convert. */
         return true;
      case AF_INET6:
         /* Is the address provided an IPv4? */
         if (!memcmp(&addr6->sin6_addr, prefix, sizeof(prefix)))
            break;
      default:
         /* We don't know how to handle this. */
         return false;
   }

   memcpy(&address, ((uint8_t*)&addr6->sin6_addr) + sizeof(prefix),
      sizeof(address));
   port = addr6->sin6_port;

   memset(addr, 0, sizeof(*addr));

   addr4->sin_family = AF_INET;
   addr4->sin_port   = port;
   memcpy(&addr4->sin_addr, &address, sizeof(addr4->sin_addr));
#endif

   return true;
}

bool ipv4_is_lan_address(const struct sockaddr_in *addr)
{
   static const uint32_t subnets[] = {0x0A000000, 0xAC100000, 0xC0A80000};
   static const uint32_t masks[]   = {0xFF000000, 0xFFF00000, 0xFFFF0000};
   size_t i;
   uint32_t uaddr;

   memcpy(&uaddr, &addr->sin_addr, sizeof(uaddr));
   uaddr = ntohl(uaddr);

   for (i = 0; i < ARRAY_SIZE(subnets); i++)
      if ((uaddr & masks[i]) == subnets[i])
         return true;

   return false;
}

bool ipv4_is_cgnat_address(const struct sockaddr_in *addr)
{
   static const uint32_t subnet = 0x64400000;
   static const uint32_t mask   = 0xFFC00000;
   uint32_t uaddr;

   memcpy(&uaddr, &addr->sin_addr, sizeof(uaddr));
   uaddr = ntohl(uaddr);

   return (uaddr & mask) == subnet;
}

/**
 * network_init:
 *
 * Platform specific socket library initialization.
 *
 * @return true if successful, otherwise false.
 **/
bool network_init(void)
{
#if defined(_WIN32)
   static bool initialized = false;

   if (!initialized)
   {
      WSADATA wsaData;

      if (WSAStartup(MAKEWORD(2, 2), &wsaData))
      {
         WSACleanup();

         return false;
      }

      initialized = true;
   }

   return true;
#elif defined(__PSL1GHT__) || defined(__PS3__)
   static bool initialized = false;

   if (!initialized)
   {
      int tries;

      sysModuleLoad(SYSMODULE_NET);

      netInitialize();
      if (netCtlInit() < 0)
         goto failure;

      for (tries = 10;;)
      {
         int state;

         if (netCtlGetState(&state) < 0)
            goto failure;
         if (state == NET_CTL_STATE_IPObtained)
            break;

         if (!(--tries))
            goto failure;

         retro_sleep(500);
      }

      initialized = true;
   }

   return true;

failure:
   netCtlTerm();
   netFinalizeNetwork();

   sysModuleUnload(SYSMODULE_NET);

   return false;
#elif defined(VITA)
   if (sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT)
   {
      SceNetInitParam param;
      void *net_compat_memory = malloc(COMPAT_NET_INIT_SIZE);

      if (!net_compat_memory)
         return false;

      param.memory = net_compat_memory;
      param.size   = COMPAT_NET_INIT_SIZE;
      param.flags  = 0;

      if (sceNetInit(&param) < 0)
         goto failure;
      if (sceNetCtlInit() < 0)
         goto failure;

      return true;

failure:
      sceNetCtlTerm();
      sceNetTerm();

      free(net_compat_memory);

      return false;
   }

   return true;
#elif defined(GEKKO)
   static bool initialized = false;

   if (!initialized)
   {
      char localip[16] = {0};
      char netmask[16] = {0};
      char gateway[16] = {0};

      if (if_config(localip, netmask, gateway, true, 10) < 0)
      {
         net_deinit();

         return false;
      }

      initialized = true;
   }

   return true;
#elif defined(WIIU)
   static OSThread net_compat_thread;
   static bool initialized = false;

   if (!initialized)
   {
      void *stack = malloc(0x1000);

      if (!stack)
         return false;

      socket_lib_init();

      if (!OSCreateThread(&net_compat_thread, _net_compat_thread_entry,
            0, NULL, (void*)((size_t)stack + 0x1000), 0x1000, 3,
            OS_THREAD_ATTRIB_AFFINITY_ANY))
      {
         free(stack);

         return false;
      }

      OSSetThreadName(&net_compat_thread, "Network compat thread");
      OSSetThreadDeallocator(&net_compat_thread, _net_compat_thread_cleanup);
      OSResumeThread(&net_compat_thread);

      initialized = true;
   }

   return true;
#elif defined(_3DS)
   static bool initialized = false;

   if (!initialized)
   {
      u32 *net_compat_memory = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);

      if (!net_compat_memory)
         return false;

      /* WIFI init */
      if (socInit(net_compat_memory, SOC_BUFFERSIZE))
      {
         socExit();

         free(net_compat_memory);

         return false;
      }

      initialized = true;
   }

   return true;
#elif defined(ORBIS)
   /* ⚠ THE ONLY THING THIS PLATFORM NEEDS, and it is not the socket API.
    *
    * connect/bind/listen/accept/select/poll/close and the rest come from libkernel under
    * their POSIX names - see the long note in net_compat.h - so nothing below has to be
    * redirected. What does not come for free is the network stack itself: musl's resolver
    * asks libSceNet for the console's DNS servers (sceNetGetDnsInfo, in resolvconf.lo), and
    * that call answers with an error until sceNetInit() has run. Without this arm, name
    * resolution fails on a console that is plainly online, which reads as a dead HTTP task.
    *
    * ⚠ THE RETURN VALUE IS DELIBERATELY NOT FATAL. sceNetInit() answers with a negative SCE
    * code when the stack is ALREADY up, and this SDK's orbis/Net.h names no constant for
    * that case - it declares `int32_t sceNetInit();` and nothing else. The log channel
    * (orbis-compat/optional/orbis_netlog.cpp) calls sceNetInit() during early boot and then
    * sends datagrams successfully on hardware, so by the time RetroArch reaches here the
    * second call is the expected one. Treating its error as failure would turn the proven
    * path into the broken one.
    *
    * ⚠ NO sceNetCtl WAIT. The PS3 and Vita arms above poll for an IP because their stacks
    * come up with the title. Here the system is already associated before the application
    * starts; there is nothing to wait for, and a wait loop would only add a stall to every
    * cold boot. If a console is genuinely offline the connect timeout in
    * socket_connect_with_timeout() is what reports it, which is where it belongs.
    */
   static bool initialized = false;

   if (!initialized)
   {
      sceNetInit();

      /* Same reason as the generic branch: a peer that hangs up mid-transfer must not take
       * the process with it. SIGPIPE is 13 here (bits/signal.h) and signal() is real. */
      signal(SIGPIPE, SIG_IGN);

      initialized = true;
   }

   return true;
#else
   static bool initialized = false;

   if (!initialized)
   {
      /* Do not like SIGPIPE killing our app. */
      signal(SIGPIPE, SIG_IGN);

      initialized = true;
   }

   return true;
#endif
}
