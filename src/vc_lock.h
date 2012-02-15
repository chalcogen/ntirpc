
#ifndef TIRPC_VC_LOCK_H
#define TIRPC_VC_LOCK_H

struct vc_fd_rec; /* in clnt_internal.h (avoids circular dependency) */

struct vc_fd_rec_set
{
    mutex_t clnt_fd_lock; /* global mtx we'll try to spam less than formerly */
    struct rbtree_x xt;
};

static inline int fd_cmpf(const struct opr_rbtree_node *lhs,
                          const struct opr_rbtree_node *rhs)
{
    struct vc_fd_rec *lk, *rk;

    lk = opr_containerof(lhs, struct vc_fd_rec, node_k);
    rk = opr_containerof(rhs, struct vc_fd_rec, node_k);

    if (lk->fd_k < rk->fd_k)
        return (-1);

    if (lk->fd_k == rk->fd_k)
        return (0);

    return (1);
}

/* XXX perhaps better off as a flag bit */
#define rpc_flag_clear 0
#define rpc_lock_value 1

void vc_fd_lock(int fd, sigset_t *mask);
void vc_fd_unlock(int fd, sigset_t *mask);

struct vc_fd_rec *vc_lookup_fd_rec(int fd);

static inline void vc_lock_init_cl(CLIENT *cl)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    if (! ct->ct_crec) {
        /* many clients shall point to crec */
        ct->ct_crec = vc_lookup_fd_rec(ct->ct_fd);
    }
}

static inline void vc_fd_lock_c(CLIENT *cl, sigset_t *mask)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    struct vc_fd_rec *crec;
    sigset_t newmask;

    vc_lock_init_cl(cl);

    crec = ct->ct_crec;

    sigfillset(&newmask);
    sigdelset(&newmask, SIGINT); /* XXXX debugger */
    thr_sigsetmask(SIG_SETMASK, &newmask, mask);

    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value)
        cond_wait(&crec->cv, &crec->mtx);
    crec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&crec->mtx);
}

static inline void vc_fd_unlock_c(CLIENT *cl, sigset_t *mask)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    struct vc_fd_rec *crec = ct->ct_crec;

    /* XXX I -think- this need not be clnt_fd_lock, however this is a
     * significant unserialization */
    mutex_lock(&crec->mtx);
    crec->lock_flag_value = rpc_flag_clear;
    mutex_unlock(&crec->mtx);
    thr_sigsetmask(SIG_SETMASK, mask, (sigset_t *) NULL);
    cond_signal(&crec->cv);
}

static inline void vc_fd_wait_c(CLIENT *cl, uint32_t wait_for)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    struct vc_fd_rec *crec;

    vc_lock_init_cl(cl);

    crec = ct->ct_crec;

    /* XXX hopefully this need NOT be clnt_fd_lock, however this is a
     * significant unserialization */
    mutex_lock(&crec->mtx);
    while (crec->lock_flag_value != rpc_flag_clear)
        cond_wait(&crec->cv, &crec->mtx);
    mutex_unlock(&crec->mtx);
}

static inline void vc_fd_signal_c(CLIENT *cl)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    struct vc_fd_rec *crec = ct->ct_crec;
    cond_signal(&crec->cv);
}

static inline void vc_fd_lock_x(SVCXPRT *xprt, sigset_t *mask)
{
    /* use the duplex client, if present */
    if (xprt->xp_p4)
        vc_fd_lock_c((CLIENT *) xprt->xp_p4, mask);
    else
        vc_fd_lock(xprt->xp_fd, mask);
}

static inline void vc_fd_unlock_x(SVCXPRT *xprt, sigset_t *mask)
{
    /* use the duplex client, if present */
    if (xprt->xp_p4)
        vc_fd_unlock_c((CLIENT *) xprt->xp_p4, mask);
    else
        vc_fd_unlock(xprt->xp_fd, mask);
}

#endif /* TIRPC_VC_LOCK_H */
