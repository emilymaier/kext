/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * All rights reserved.
 */

#include "fuse_file.h"

#include "fuse_internal.h"

#if M_OSXFUSE_ENABLE_BIG_LOCK
#  include "fuse_biglock_vnops.h"
#endif

/*
 * Because of the vagaries of how a filehandle can be used, we try not to
 * be too smart in here (we try to be smart elsewhere). It is required that
 * you come in here only if you really do not have the said filehandle--else
 * we panic.
 */
int
fuse_filehandle_get(vnode_t       vp,
                    vfs_context_t context,
                    fufh_type_t   fufh_type,
                    int           mode)
{
    struct fuse_dispatcher  fdi;
    struct fuse_open_in     foi;
    struct fuse_open_out    foo;
    struct fuse_filehandle *fufh;
    struct fuse_vnode_data *fvdat = VTOFUD(vp);

    struct fuse_data        *data = fuse_get_mpdata(vnode_mount(vp));

    int err    = 0;
    int isdir  = 0;
    int oflags = 0;
    int op     = FUSE_OPEN;

    fuse_trace_printf("fuse_filehandle_get(vp=%p, fufh_type=%d, mode=%x)\n",
                      vp, fufh_type, mode);

    fufh = &(fvdat->fufh[fufh_type]);

    if (FUFH_IS_VALID(fufh)) {
        panic("OSXFUSE: filehandle_get called despite valid fufh (type=%d)",
              fufh_type);
        /* NOTREACHED */
    }

    /*
     * Note that this means we are effectively FILTERING OUT open() flags.
     */
    (void)mode;
    oflags = fuse_filehandle_xlate_to_oflags(fufh_type);

    if (vnode_isdir(vp)) {
        isdir = 1;
        op = FUSE_OPENDIR;
        if (fufh_type != FUFH_RDONLY) {
            IOLog("OSXFUSE: non-rdonly fufh requested for directory\n");
            fufh_type = FUFH_RDONLY;
        }
    }

    if (vnode_islnk(vp) && (mode & O_SYMLINK)) {
        oflags |= O_SYMLINK;
    }

    if ((mode & O_TRUNC) && (data->dataflags & FSESS_ATOMIC_O_TRUNC)) {
        oflags |= O_TRUNC;
    }

    foi.flags = oflags;

    fdisp_init_abi(&fdi, fuse_open_in, DTOABI(data));
    fdisp_make_vp(&fdi, op, vp, context);
    fuse_abi_in(fuse_open_in, DTOABI(data), &foi, fdi.indata);

    FUSE_OSAddAtomic(1, (SInt32 *)&fuse_fh_upcall_count);
    if ((err = fdisp_wait_answ(&fdi))) {
#if M_OSXFUSE_ENABLE_UNSUPPORTED
        const char *vname = vnode_getname(vp);
#endif /* M_OSXFUSE_ENABLE_UNSUPPORTED */
        if (err == ENOENT) {
            /*
             * See comment in fuse_vnop_reclaim().
             */
            cache_purge(vp);
        }
#if M_OSXFUSE_ENABLE_UNSUPPORTED
        IOLog("OSXFUSE: filehandle_get: failed for %s "
              "(type=%d, err=%d, caller=%p)\n",
              (vname) ? vname : "?", fufh_type, err,
               __builtin_return_address(0));
        if (vname) {
            vnode_putname(vname);
        }
#endif /* M_OSXFUSE_ENABLE_UNSUPPORTED */
        if (err == ENOENT) {
#if M_OSXFUSE_ENABLE_BIG_LOCK
            fuse_biglock_unlock(data->biglock);
#endif
            fuse_internal_vnode_disappear(vp, context, REVOKE_SOFT);
#if M_OSXFUSE_ENABLE_BIG_LOCK
            fuse_biglock_lock(data->biglock);
#endif
        }
        return err;
    }
    FUSE_OSAddAtomic(1, (SInt32 *)&fuse_fh_current);

    fuse_abi_out(fuse_open_out, DTOABI(data), fdi.answ, &foo);

    fufh->fh_id = foo.fh;
    fufh->open_count = 1;
    fufh->open_flags = oflags;
    fufh->fuse_open_flags = foo.open_flags;
    fufh->aux_count = 0;

    fuse_ticket_release(fdi.tick);

    return 0;
}

int
fuse_filehandle_put(vnode_t vp, vfs_context_t context, fufh_type_t fufh_type,
                    fuse_op_waitfor_t waitfor)
{
    struct fuse_data       *data;
    struct fuse_dispatcher  fdi;
    struct fuse_release_in  fri;
    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh  = NULL;

    int err   = 0;
    int isdir = 0;
    int op    = FUSE_RELEASE;

    fuse_trace_printf("fuse_filehandle_put(vp=%p, fufh_type=%d)\n",
                      vp, fufh_type);

    fufh = &(fvdat->fufh[fufh_type]);

    if (FUFH_IS_VALID(fufh)) {
        panic("OSXFUSE: filehandle_put called on a valid fufh (type=%d)",
              fufh_type);
        /* NOTREACHED */
    }

    if (fuse_isdeadfs(vp)) {
        goto out;
    }

    data = fuse_get_mpdata(vnode_mount(vp));

    if (vnode_isdir(vp)) {
        op = FUSE_RELEASEDIR;
        isdir = 1;
    }

    fri.fh = fufh->fh_id;
    fri.flags = fufh->open_flags;

    fdisp_init_abi(&fdi, fuse_release_in, DTOABI(data));
    fdisp_make_vp(&fdi, op, vp, context);
    fuse_abi_in(fuse_release_in, DTOABI(data), &fri, fdi.indata);

    if (waitfor == FUSE_OP_FOREGROUNDED) {
        if ((err = fdisp_wait_answ(&fdi))) {
            goto out;
        }
    } else {
        fuse_insert_message(fdi.tick);
    }
    fuse_ticket_release(fdi.tick);

out:
    FUSE_OSAddAtomic(-1, (SInt32 *)&fuse_fh_current);
    fuse_invalidate_attr(vp);

    return err;
}
