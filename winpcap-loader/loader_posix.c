/*
 * libpcap dynamic loader (POSIX)
 *
 * The counterpart to loader.c, which does the same job for wpcap.dll on
 * Windows. Both exist so the binary does not carry a link-time dependency on
 * a library the host may not have under the name we built against.
 *
 * On Linux that name is the whole problem. Distributions disagree on
 * libpcap's soname: Debian and Ubuntu ship libpcap.so.0.8, while Fedora,
 * RHEL and openSUSE ship libpcap.so.1. They are the same library - Debian
 * simply kept an older soname - but a binary linked against one will not load
 * on the other at all.
 *
 * That is not hypothetical. The v0.2.2 Linux core was built on Ubuntu CI, so
 * it recorded NEEDED libpcap.so.0.8, and every Fedora-family user got a core
 * that failed to dlopen with no core options and no menu entry - a failure
 * indistinguishable, from the frontend, from the core not existing.
 *
 * Resolving at runtime fixes it for every distribution at once, and degrades
 * to "networking unavailable" rather than "core will not load" when libpcap
 * is genuinely absent - which is the right outcome, since the Xbox network
 * backend defaults to disabled.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <pcap/pcap.h>
#include <dlfcn.h>
#include <string.h>

static const char *const lib_not_loaded_err = "libpcap is not available";

/* Tried in order. The unversioned name is last because it only exists when
 * the -dev package is installed, so it is the least likely to be present on
 * an end user's machine. */
static const char *const pcap_sonames[] = {
    "libpcap.so.1",
    "libpcap.so.0.8",
    "libpcap.so",
};

static void *(*fptr_pcap_open_live)(const char *, int, int, int, char *);
static void  (*fptr_pcap_close)(pcap_t *);
static int   (*fptr_pcap_next_ex)(pcap_t *, struct pcap_pkthdr **,
                                  const u_char **);
static char *(*fptr_pcap_geterr)(pcap_t *);
static int   (*fptr_pcap_set_datalink)(pcap_t *, int);
static int   (*fptr_pcap_sendpacket)(pcap_t *, const u_char *, int);
static int   (*fptr_pcap_get_selectable_fd)(pcap_t *);

static int loaded;      /* 1 = usable, -1 = tried and failed */
static char geterr_buf[PCAP_ERRBUF_SIZE];

/* On Windows this is declared in winpcap-loader/include/pcap/Win32-Extensions.h,
 * which the POSIX build does not use - it takes the system pcap headers, and
 * those have no such function. Declare it here so the definition below has a
 * prototype. */
int pcap_load_library(void);

int pcap_load_library(void)
{
    if (loaded) {
        return loaded == 1 ? 0 : 1;
    }

    void *h = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(pcap_sonames); i++) {
        h = dlopen(pcap_sonames[i], RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            break;
        }
    }
    if (!h) {
        loaded = -1;
        return 1;
    }

    #define LOAD_FN(fname) do { \
        void *p = dlsym(h, #fname); \
        if (!p) { \
            dlclose(h); \
            loaded = -1; \
            return 1; \
        } \
        fptr_ ## fname = p; \
    } while (0)

    LOAD_FN(pcap_open_live);
    LOAD_FN(pcap_close);
    LOAD_FN(pcap_next_ex);
    LOAD_FN(pcap_geterr);
    LOAD_FN(pcap_set_datalink);
    LOAD_FN(pcap_sendpacket);
    LOAD_FN(pcap_get_selectable_fd);

    #undef LOAD_FN

    loaded = 1;
    return 0;
}

/* Every wrapper loads on demand. net/pcap.c only calls pcap_load_library()
 * under WIN32, so on POSIX the first real call has to do it. */
static bool pcap_ready(void)
{
    return pcap_load_library() == 0;
}

pcap_t *pcap_open_live(const char *device, int snaplen, int promisc,
                       int to_ms, char *errbuf)
{
    if (!pcap_ready()) {
        if (errbuf) {
            strncpy(errbuf, lib_not_loaded_err, PCAP_ERRBUF_SIZE - 1);
            errbuf[PCAP_ERRBUF_SIZE - 1] = '\0';
        }
        return NULL;
    }
    return fptr_pcap_open_live(device, snaplen, promisc, to_ms, errbuf);
}

void pcap_close(pcap_t *p)
{
    if (pcap_ready()) {
        fptr_pcap_close(p);
    }
}

int pcap_next_ex(pcap_t *p, struct pcap_pkthdr **pkt_header,
                 const u_char **pkt_data)
{
    return pcap_ready() ? fptr_pcap_next_ex(p, pkt_header, pkt_data) : -1;
}

char *pcap_geterr(pcap_t *p)
{
    if (!pcap_ready()) {
        strncpy(geterr_buf, lib_not_loaded_err, sizeof(geterr_buf) - 1);
        geterr_buf[sizeof(geterr_buf) - 1] = '\0';
        return geterr_buf;
    }
    return fptr_pcap_geterr(p);
}

int pcap_set_datalink(pcap_t *p, int dlt)
{
    return pcap_ready() ? fptr_pcap_set_datalink(p, dlt) : -1;
}

int pcap_sendpacket(pcap_t *p, const u_char *buf, int size)
{
    return pcap_ready() ? fptr_pcap_sendpacket(p, buf, size) : -1;
}

int pcap_get_selectable_fd(pcap_t *p)
{
    return pcap_ready() ? fptr_pcap_get_selectable_fd(p) : -1;
}
