/*
  Copyright (C) 2006, 2007, 2008, 2009  Anthony Catel <a.catel@weelya.com>

  This file is part of APE Server.
  APE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  APE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with APE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* dns.c */

#ifdef WINDOWS
#include <windows.h>
#include <winsock2.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <udns.h>
#include "dns.h"
#include "main.h"
#include "events.h"
#include "utils.h"
#include "ticks.h"


static enum dns_class qcls = DNS_C_IN;

static struct query *query_new(const char *name, const unsigned char *dn, enum dns_type qtyp) {
	struct query *q = xmalloc(sizeof(*q));
	
	unsigned l = dns_dnlen(dn);
	unsigned char *cdn = xmalloc(l);
	
	memcpy(cdn, dn, l);
	
	q->name = xstrdup(name);
	q->dn = cdn;
	q->qtyp = qtyp;
	
	return q;
}

static void ape_dns_io()
{
	dns_ioevent(NULL, 0);
}

static void ape_dns_read(ape_socket *client, ape_buffer *buf, size_t offset, acetables *g_ape)
{
	ape_dns_io();
}

static void ape_dns_write(ape_socket *client, acetables *g_ape)
{
	ape_dns_io();
}

static void query_free(struct query *q) {
	free(q->name);
	free(q->dn);
	free(q);
}


static void dnscb(struct dns_ctx *ctx, void *result, void *data) {
  int r = dns_status(ctx);
  struct query *q = data;
  struct dns_parse p;
  struct dns_rr rr;
  unsigned nrr;
  unsigned char dn[DNS_MAXDN];
  const unsigned char *pkt, *cur, *end;

  if (!result) {
    return;
  }

  pkt = result; end = pkt + r; cur = dns_payload(pkt);
  dns_getdn(pkt, &cur, end, dn, sizeof(dn));
  dns_initparse(&p, NULL, pkt, cur, end);
  p.dnsp_qcls = p.dnsp_qtyp = 0;
  nrr = 0;

  while((r = dns_nextrr(&p, &rr)) > 0) {
    if (!dns_dnequal(dn, rr.dnsrr_dn)) continue;
    if ((qcls == DNS_C_ANY || qcls == rr.dnsrr_cls) &&
        (q->qtyp == DNS_T_ANY || q->qtyp == rr.dnsrr_typ))
      ++nrr;
    else if (rr.dnsrr_typ == DNS_T_CNAME && !nrr) {
      if (dns_getdn(pkt, &rr.dnsrr_dptr, end,
                    p.dnsp_dnbuf, sizeof(p.dnsp_dnbuf)) <= 0 ||
          rr.dnsrr_dptr != rr.dnsrr_dend) {
        r = DNS_E_PROTOCOL;
        break;
      }
      else {
        dns_dntodn(p.dnsp_dnbuf, dn, sizeof(dn));
      }
    }
  }
  if (!r && !nrr)
    r = DNS_E_NODATA;
  if (r < 0) {
    free(result);
    return;
  }
  dns_rewind(&p, NULL);
  p.dnsp_qtyp = q->qtyp == DNS_T_ANY ? 0 : q->qtyp;
  p.dnsp_qcls = qcls == DNS_C_ANY ? 0 : qcls;
  while(dns_nextrr(&p, &rr)) {
	const unsigned char *dptr = rr.dnsrr_dptr;
	if (rr.dnsrr_dsz == 4)  {
		char *ip = xmalloc(sizeof(char) * 16);
		sprintf(ip, "%d.%d.%d.%d", dptr[0], dptr[1], dptr[2], dptr[3]);
		
		q->callback(ip, q->data, q->g_ape);
		break;
	}
  }

  free(result);
  query_free(q);
}


void ape_gethostbyname(char *name, void (*callback)(char *, void *, acetables *), void *data, acetables *g_ape)
{
   
    struct in_addr addr;
	struct query *q;
    unsigned char dn[DNS_MAXDN];
	int abs = 0;
	enum dns_type l_qtyp = 0;

    if (dns_pton(AF_INET, name, &addr) > 0) {
		/* We have an IP */
		callback(xstrdup(name), data, g_ape);
		return;
    } else if (!dns_ptodn(name, strlen(name), dn, sizeof(dn), &abs)) {
		/* We have an invalid domain name */
		return;
	} else {
		l_qtyp = DNS_T_A;
	}
	
	q = query_new(name, dn, l_qtyp);
	
	q->data = data;
	q->callback = callback;
	q->g_ape = g_ape;
	
	if (abs) {
		abs = DNS_NOSRCH;
	}
    if (!dns_submit_dn(NULL, dn, qcls, l_qtyp, abs, 0, dnscb, q)) {
		query_free(q);
		return;
	}
	
	dns_timeouts(NULL, -1, 0);
}

static void ape_dns_timeout(void *params, int last)
{
	dns_timeouts(NULL, -1, 0);
}


void ape_dns_init(acetables *g_ape)
{
	int dns_fd;
	
	ape_socket *co = g_ape->co;
	
	dns_fd = dns_init(NULL, 1);
	
	co[dns_fd].buffer_in.data = NULL;
	co[dns_fd].buffer_in.size = 0;
	co[dns_fd].buffer_in.length = 0;

	co[dns_fd].attach = NULL;
	co[dns_fd].idle = 0;
	co[dns_fd].burn_after_writing = 0;
	co[dns_fd].fd = dns_fd;

	co[dns_fd].stream_type = STREAM_DELEGATE;

	co[dns_fd].callbacks.on_accept = NULL;
	co[dns_fd].callbacks.on_connect = NULL;
	co[dns_fd].callbacks.on_disconnect = NULL;
	co[dns_fd].callbacks.on_read_lf = NULL;
	co[dns_fd].callbacks.on_data_completly_sent = NULL;
	
	co[dns_fd].callbacks.on_read = ape_dns_read;
	co[dns_fd].callbacks.on_write = ape_dns_write;

	events_add(g_ape->events, dns_fd, EVENT_READ|EVENT_WRITE);

	add_periodical(50, 0, ape_dns_timeout, NULL, g_ape);

}
