/*
 * Copyright (c) 1997, 1998 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. All advertising materials mentioning features or use of this software 
 *    must display the following acknowledgement: 
 *      This product includes software developed by Kungliga Tekniska 
 *      H�gskolan and its contributors. 
 *
 * 4. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "kpasswd_locl.h"
#include <admin.h>
RCSID("$Id$");

static krb5_context context;
static void *kadm5_handle;
static krb5_log_facility *log_facility;

static sig_atomic_t exit_flag = 0;

static void
send_reply (int s,
	    struct sockaddr *sa,
	    int sa_size,
	    krb5_data *ap_rep,
	    krb5_data *rest)
{
    struct msghdr msghdr;
    struct iovec iov[3];
    u_int16_t len, ap_rep_len;
    u_char header[6];
    u_char *p;

    if (ap_rep)
	ap_rep_len = ap_rep->length;
    else
	ap_rep_len = 0;

    len = 6 + ap_rep_len + rest->length;
    p = header;
    *p++ = (len >> 8) & 0xFF;
    *p++ = (len >> 0) & 0xFF;
    *p++ = 0;
    *p++ = 1;
    *p++ = (ap_rep_len >> 8) & 0xFF;
    *p++ = (ap_rep_len >> 0) & 0xFF;

    memset (&msghdr, 0, sizeof(msghdr));
    msghdr.msg_name       = (void *)sa;
    msghdr.msg_namelen    = sa_size;
    msghdr.msg_iov        = iov;
    msghdr.msg_iovlen     = sizeof(iov)/sizeof(*iov);
#if 0
    msghdr.msg_control    = NULL;
    msghdr.msg_controllen = 0;
#endif

    iov[0].iov_base       = (char *)header;
    iov[0].iov_len        = 6;
    if (ap_rep_len) {
	iov[1].iov_base   = ap_rep->data;
	iov[1].iov_len    = ap_rep->length;
    } else {
	iov[1].iov_base   = NULL;
	iov[1].iov_len    = 0;
    }
    iov[2].iov_base       = rest->data;
    iov[2].iov_len        = rest->length;

    if (sendmsg (s, &msghdr, 0) < 0)
	krb5_warn (context, errno, "sendmsg");
}

static int
make_result (krb5_data *data,
	     u_int16_t result_code,
	     const char *expl)
{
    krb5_data_zero (data);

    data->length = asprintf ((char **)&data->data,
			     "%c%c%s",
			     (result_code >> 8) & 0xFF,
			     result_code & 0xFF,
			     expl);

    if (data->data == NULL) {
	krb5_warnx (context, "Out of memory generating error reply");
	return 1;
    }
    return 0;
}

static void
reply_error (krb5_principal server,
	     int s,
	     struct sockaddr *sa,
	     int sa_size,
	     krb5_error_code error_code,
	     u_int16_t result_code,
	     const char *expl)
{
    krb5_error_code ret;
    krb5_data error_data;
    krb5_data e_data;

    if (make_result(&e_data, result_code, expl))
	return;

    ret = krb5_mk_error (context,
			 error_code,
			 NULL,
			 &e_data,
			 NULL,
			 server,
			 0,
			 &error_data);
    krb5_data_free (&e_data);
    if (ret) {
	krb5_warn (context, ret, "Could not even generate error reply");
	return;
    }
    send_reply (s, sa, sa_size, NULL, &error_data);
    krb5_data_free (&error_data);
}

static void
reply_priv (krb5_auth_context auth_context,
	    int s,
	    struct sockaddr *sa,
	    int sa_size,
	    u_int16_t result_code,
	    const char *expl)
{
    krb5_error_code ret;
    krb5_data krb_priv_data;
    krb5_data ap_rep_data;
    krb5_data e_data;

    ret = krb5_mk_rep (context,
		       &auth_context,
		       &ap_rep_data);
    if (ret) {
	krb5_warn (context, ret, "Could not even generate error reply");
	return;
    }

    if (make_result(&e_data, result_code, expl))
	return;

    ret = krb5_mk_priv (context,
			auth_context,
			&e_data,
			&krb_priv_data,
			NULL);
    krb5_data_free (&e_data);
    if (ret) {
	krb5_warn (context, ret, "Could not even generate error reply");
	return;
    }
    send_reply (s, sa, sa_size, &ap_rep_data, &krb_priv_data);
    krb5_data_free (&ap_rep_data);
    krb5_data_free (&krb_priv_data);
}

static char *
passwd_quality_check (krb5_data *pwd)
{
    if (pwd->length < 6)
	return "Password too short";
    else
	return NULL;
}

static void
change (krb5_auth_context auth_context,
	krb5_principal principal,
	int s,
	struct sockaddr *sa,
	int sa_size,
	krb5_data *pwd_data)
{
    krb5_error_code ret;
    char *c;
    kadm5_principal_ent_rec ent;
    krb5_key_data *kd;
    krb5_data salt;
    krb5_keyblock new_keyblock;
    char *pwd_reason;
    int unchanged;

    krb5_unparse_name (context, principal, &c);

    krb5_warnx (context, "Changing password for %s", c);
    free (c);

    pwd_reason = passwd_quality_check (pwd_data);
    if (pwd_reason != NULL ) {
	krb5_warnx (context, pwd_reason);
	reply_priv (auth_context, s, sa, sa_size, 4, pwd_reason);
	return;
    }

    ret = kadm5_get_principal (kadm5_handle,
			       principal,
			       &ent,
			       KADM5_KEY_DATA);
    if (ret) {
	krb5_warn (context, ret, "kadm5_get_principal");
	reply_priv (auth_context, s, sa, sa_size, 2,
		    "kadm5_get_principal failed");
	return;
    }

    /*
     * Compare with the first key to see if it already has been
     * changed.  If it hasn't, store the new key in the database and
     * string2key all the rest of them.
     */

    kd = &ent.key_data[0];

    salt.length = kd->key_data_length[1];
    salt.data   = kd->key_data_contents[1];

    memset (&new_keyblock, 0, sizeof(new_keyblock));
    krb5_string_to_key_data (pwd_data,
			     &salt,
			     kd->key_data_type[0],
			     &new_keyblock);

    unchanged = new_keyblock.keytype == kd->key_data_type[0]
	&& new_keyblock.keyvalue.length == kd->key_data_length[0]
	&& memcmp(new_keyblock.keyvalue.data,
		  kd->key_data_contents[0],
		  new_keyblock.keyvalue.length) == 0;

    krb5_free_keyblock_contents (context, &new_keyblock);

    if (unchanged) {
	ret = 0;
    } else {
	char *tmp;

	tmp = malloc (pwd_data->length + 1);
	if (tmp == NULL) {
	    krb5_warnx (context, "malloc: out of memory");
	    reply_priv (auth_context, s, sa, sa_size, 2,
			"malloc failed");
	    goto out;
	}
	memcpy (tmp, pwd_data->data, pwd_data->length);
	tmp[pwd_data->length] = '\0';

	ret = kadm5_chpass_principal (kadm5_handle,
				      principal,
				      tmp);
	memset (tmp, 0, pwd_data->length);
	free (tmp);
	if (ret) {
	    krb5_warn (context, ret, "kadm5_s_chpass_principal");
	    reply_priv (auth_context, s, sa, sa_size, 2,
			"change failed");
	    goto out;
	}
    }
    reply_priv (auth_context, s, sa, sa_size, 0, "password changed");
out:
    kadm5_free_principal_ent (kadm5_handle, &ent);
}

static int
verify (krb5_auth_context *auth_context,
	krb5_principal server,
	krb5_ticket **ticket,
	krb5_data *out_data,
	int s,
	struct sockaddr *sa,
	int sa_size,
	u_char *msg,
	size_t len)
{
    krb5_error_code ret;
    u_int16_t pkt_len, pkt_ver, ap_req_len;
    krb5_data ap_req_data;
    krb5_data krb_priv_data;

    pkt_len = (msg[0] << 8) | (msg[1]);
    pkt_ver = (msg[2] << 8) | (msg[3]);
    ap_req_len = (msg[4] << 8) | (msg[5]);
    if (pkt_len != len) {
	krb5_warnx (context, "Strange len: %d != %d", pkt_len, len);
	reply_error (server, s, sa, sa_size, 0, 1, "bad length");
	return 1;
    }
    if (pkt_ver != 0x0001) {
	krb5_warnx (context, "Bad version (%d)", pkt_ver);
	reply_error (server, s, sa, sa_size, 0, 1, "bad version");
	return 1;
    }

    ap_req_data.data   = msg + 6;
    ap_req_data.length = ap_req_len;

    ret = krb5_rd_req (context,
		       auth_context,
		       &ap_req_data,
		       server,
		       NULL,
		       NULL,
		       ticket);
    if (ret) {
	krb5_warn (context, ret, "krb5_rd_req");
	reply_error (server, s, sa, sa_size, ret, 3, "rd_req failed");
	return 1;
    }

    if (!(*ticket)->ticket.flags.initial) {
	krb5_warnx (context, "initial flag not set");
	reply_error (server, s, sa, sa_size, ret, 1,
		     "initial flag not set");
	goto out;
    }
    krb_priv_data.data   = msg + 6 + ap_req_len;
    krb_priv_data.length = len - 6 - ap_req_len;

    ret = krb5_rd_priv (context,
			*auth_context,
			&krb_priv_data,
			out_data,
			NULL);
    
    if (ret) {
	krb5_warn (context, ret, "krb5_rd_priv");
	reply_error (server, s, sa, sa_size, ret, 3, "rd_priv failed");
	goto out;
    }
    return 0;
out:
    krb5_free_ticket (context, *ticket);
    return 1;
}

static void
process (krb5_principal server,
	 int s,
	 krb5_address *this_addr,
	 struct sockaddr *sa,
	 int sa_size,
	 u_char *msg,
	 int len)
{
    krb5_error_code ret;
    krb5_auth_context auth_context = NULL;
    krb5_data out_data;
    krb5_ticket *ticket;
    krb5_address other_addr;

    krb5_data_zero (&out_data);

    ret = krb5_auth_con_init (context, &auth_context);
    if (ret) {
	krb5_warn (context, ret, "krb5_auth_con_init");
	return;
    }

    krb5_auth_con_setflags (context, auth_context,
			    KRB5_AUTH_CONTEXT_DO_SEQUENCE);

    ret = krb5_sockaddr2address (sa, &other_addr);
    if (ret) {
	krb5_warn (context, ret, "krb5_sockaddr2address");
	goto out;
    }

    ret = krb5_auth_con_setaddrs (context,
				  auth_context,
				  this_addr,
				  &other_addr);
    krb5_free_address (context, &other_addr);
    if (ret) {
	krb5_warn (context, ret, "krb5_auth_con_setaddr");
	goto out;
    }

    if (verify (&auth_context, server, &ticket, &out_data,
		s, sa, sa_size, msg, len) == 0) {
	change (auth_context,
		ticket->client,
		s,
		sa, sa_size,
		&out_data);
	krb5_free_ticket (context, ticket);
	free (ticket);
    }

out:
    krb5_data_free (&out_data);
    krb5_auth_con_free (context, auth_context);
}

static int
doit (int port)
{
    krb5_error_code ret;
    krb5_principal server;
    int *sockets;
    int maxfd;
    char *realm;
    krb5_addresses addrs;
    unsigned n, i;
    fd_set real_fdset;
    void *sa_buf;
    int sa_max_size;
    struct sockaddr *sa;

    sa_max_size = krb5_max_sockaddr_size ();
    sa_buf = malloc (sa_max_size);
    if (sa_buf == NULL)
	krb5_errx (context, 1, "out of memory");
    sa = (struct sockaddr *)sa_buf;

    ret = krb5_get_default_realm (context, &realm);
    if (ret)
	krb5_err (context, 1, ret, "krb5_get_default_realm");

    ret = krb5_build_principal (context,
				&server,
				strlen(realm),
				realm,
				"kadmin",
				"changepw",
				NULL);
    if (ret)
	krb5_err (context, 1, ret, "krb5_build_principal");

    free (realm);

    ret = krb5_get_all_client_addrs (&addrs);
    if (ret)
	krb5_err (context, 1, ret, "krb5_get_all_client_addrs");

    n = addrs.len;

    sockets = malloc (n * sizeof(*sockets));
    if (sockets == NULL)
	krb5_errx (context, 1, "out of memory");
    maxfd = 0;
    FD_ZERO(&real_fdset);
    for (i = 0; i < n; ++i) {
	int sa_size;

	krb5_addr2sockaddr (&addrs.val[i], sa, &sa_size, port);

	sockets[i] = socket (sa->sa_family, SOCK_DGRAM, 0);
	if (sockets[i] < 0)
	    krb5_err (context, 1, errno, "socket");
	if (bind (sockets[i], sa, sa_size) < 0)
	    krb5_err (context, 1, errno, "bind");
	maxfd = max (maxfd, sockets[i]);
	FD_SET(sockets[i], &real_fdset);
    }

    while(exit_flag == 0) {
	int ret;
	fd_set fdset = real_fdset;

	ret = select (maxfd + 1, &fdset, NULL, NULL, NULL);
	if (ret < 0)
	    if (errno == EINTR)
		continue;
	    else
		krb5_err (context, 1, errno, "select");
	for (i = 0; i < n; ++i)
	    if (FD_ISSET(sockets[i], &fdset)) {
		u_char buf[BUFSIZ];
		int addrlen = sa_max_size;

		ret = recvfrom (sockets[i], buf, sizeof(buf), 0,
				sa, &addrlen);
		if (ret < 0)
		    if(errno == EINTR)
			break;
		    else
			krb5_err (context, 1, errno, "recvfrom");

		process (server, sockets[i],
			 &addrs.val[i],
			 sa, addrlen,
			 buf, ret);
	    }
    }
    krb5_free_addresses (context, &addrs);
    krb5_free_principal (context, server);
    krb5_free_context (context);
    free (sa_buf);
    return 0;
}

static RETSIGTYPE
sigterm(int sig)
{
    exit_flag = 1;
}

int
main (int argc, char **argv)
{
    krb5_error_code ret;
    kadm5_config_params conf;

    krb5_init_context (&context);

    set_progname (argv[0]);
    krb5_openlog (context, "kpasswdd", &log_facility);
    krb5_set_warn_dest(context, log_facility);

    memset (&conf, 0, sizeof(conf));
    
    ret = kadm5_s_init_with_password_ctx(context, 
					 KADM5_ADMIN_SERVICE,
					 "password",
					 KADM5_ADMIN_SERVICE,
					 &conf, 0, 0, 
					 &kadm5_handle);
    if (ret)
	krb5_err (context, 1, ret, "kadm5_s_init_with_password_ctx");

#ifdef HAVE_SIGACTION
    {
	struct sigaction sa;

	sa.sa_flags = 0;
	sa.sa_handler = sigterm;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGINT, &sa, NULL);
    }
#else
    signal(SIGINT, sigterm);
#endif

    return doit (krb5_getportbyname (context, "kpasswd", "udp", KPASSWD_PORT));
}
