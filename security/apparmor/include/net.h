/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AppArmor security module
 *
 * This file contains AppArmor network mediation definitions.
 *
 * Copyright (C) 1998-2008 Novell/SUSE
 * Copyright 2009-2017 Canonical Ltd.
 */

#ifndef __AA_NET_H
#define __AA_NET_H

#include <net/sock.h>
#include <linux/path.h>

#include "apparmorfs.h"
#include "label.h"
#include "perms.h"
#include "policy.h"

#define AA_MAY_SEND		AA_MAY_WRITE
#define AA_MAY_RECEIVE		AA_MAY_READ

#define AA_MAY_SHUTDOWN		AA_MAY_DELETE

#define AA_MAY_CONNECT		AA_MAY_OPEN
#define AA_MAY_ACCEPT		0x00100000

#define AA_MAY_BIND		0x00200000
#define AA_MAY_LISTEN		0x00400000

#define AA_MAY_SETOPT		0x01000000
#define AA_MAY_GETOPT		0x02000000

#define NET_PERMS_MASK (AA_MAY_SEND | AA_MAY_RECEIVE | AA_MAY_CREATE |    \
			AA_MAY_SHUTDOWN | AA_MAY_BIND | AA_MAY_LISTEN |	  \
			AA_MAY_CONNECT | AA_MAY_ACCEPT | AA_MAY_SETATTR | \
			AA_MAY_GETATTR | AA_MAY_SETOPT | AA_MAY_GETOPT)

#define NET_FS_PERMS (AA_MAY_SEND | AA_MAY_RECEIVE | AA_MAY_CREATE |	\
		      AA_MAY_SHUTDOWN | AA_MAY_CONNECT | AA_MAY_RENAME |\
		      AA_MAY_SETATTR | AA_MAY_GETATTR | AA_MAY_CHMOD |	\
		      AA_MAY_CHOWN | AA_MAY_CHGRP | AA_MAY_LOCK |	\
		      AA_MAY_MPROT)

#define NET_PEER_MASK (AA_MAY_SEND | AA_MAY_RECEIVE | AA_MAY_CONNECT |	\
		       AA_MAY_ACCEPT)
struct aa_sk_ctx {
	struct aa_label *label;
	struct aa_label *peer;
	struct path path;
};

static inline bool aa_secmark(void)
{
	return apparmor_blob_sizes.lbs_secmark;
}

static inline struct aa_sk_ctx *aa_sock(const struct sock *sk)
{
	return sk->sk_security + apparmor_blob_sizes.lbs_sock;
}

#define DEFINE_AUDIT_NET(NAME, OP, SK, F, T, P)				  \
	struct lsm_network_audit NAME ## _net = { .sk = (SK),		  \
						  .family = (F)};	  \
	DEFINE_AUDIT_DATA(NAME,						  \
			  ((SK) && (F) != AF_UNIX) ? LSM_AUDIT_DATA_NET : \
						     LSM_AUDIT_DATA_NONE, \
						     AA_CLASS_NET,        \
			  OP);						  \
	NAME.common.u.net = &(NAME ## _net);				  \
	NAME.net.type = (T);						  \
	NAME.net.protocol = (P)

#define DEFINE_AUDIT_SK(NAME, OP, SK)					\
	DEFINE_AUDIT_NET(NAME, OP, SK, (SK)->sk_family, (SK)->sk_type,	\
			 (SK)->sk_protocol)



/* struct aa_net - network confinement data
 * @allow: basic network families permissions
 * @audit: which network permissions to force audit
 * @quiet: which network permissions to quiet rejects
 */
struct aa_net_compat {
	u16 allow[AF_MAX];
	u16 audit[AF_MAX];
	u16 quiet[AF_MAX];
};

#define af_select(FAMILY, FN, DEF_FN)		\
({						\
	int __e;				\
	switch ((FAMILY)) {			\
	case PF_UNIX:				\
		__e = aa_unix_ ## FN;		\
		break;				\
	case PF_INET:				\
	case PF_INET6:				\
		__e = aa_inet_ ## FN;		\
		break;				\
	default:				\
		__e = DEF_FN;			\
	}					\
	__e;					\
})

struct aa_secmark {
	u8 audit;
	u8 deny;
	u32 secid;
	char *label;
};

extern struct aa_sfs_entry aa_sfs_entry_network[];
extern struct aa_sfs_entry aa_sfs_entry_network_compat[];

void audit_net_cb(struct audit_buffer *ab, void *va);
int aa_profile_af_perm(struct aa_profile *profile,
		       struct apparmor_audit_data *ad,
		       u32 request, u16 family, int type);
int aa_af_perm(const struct cred *subj_cred, struct aa_label *label,
	       const char *op, u32 request, u16 family,
	       int type, int protocol);
static inline int aa_profile_af_sk_perm(struct aa_profile *profile,
					struct apparmor_audit_data *ad,
					u32 request,
					struct sock *sk)
{
	return aa_profile_af_perm(profile, ad, request, sk->sk_family,
				  sk->sk_type);
}
int aa_sk_perm(const char *op, u32 request, struct sock *sk);

int aa_sock_file_perm(const struct cred *subj_cred, struct aa_label *label,
		      const char *op, u32 request,
		      struct socket *sock);

int apparmor_secmark_check(struct aa_label *label, char *op, u32 request,
			   u32 secid, const struct sock *sk);

#endif /* __AA_NET_H */
