/* fhandler_socket_unix.cc.

   See fhandler.h for a description of the fhandler classes.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#include "winsup.h"
#include <ntsecapi.h>
#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include <asm/byteorder.h>
#include "cygwin/version.h"
#include "perprocess.h"
#include "shared_info.h"
#include "sigproc.h"
#include "wininfo.h"
#include <unistd.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <cygwin/acl.h>
#include "cygtls.h"
#include <sys/un.h>
#include "ntdll.h"
#include "miscfuncs.h"
#include "tls_pbuf.h"

extern "C" {
  int sscanf (const char *, const char *, ...);
} /* End of "C" section */

#define ASYNC_MASK (FD_READ|FD_WRITE|FD_OOB|FD_ACCEPT|FD_CONNECT)
#define EVENT_MASK (FD_READ|FD_WRITE|FD_OOB|FD_ACCEPT|FD_CONNECT|FD_CLOSE)

#define LOCK_EVENTS	\
  if (wsock_mtx && \
      WaitForSingleObject (wsock_mtx, INFINITE) != WAIT_FAILED) \
    {

#define UNLOCK_EVENTS \
      ReleaseMutex (wsock_mtx); \
    }

static inline mode_t
adjust_socket_file_mode (mode_t mode)
{
  /* Kludge: Don't allow to remove read bit on socket files for
     user/group/other, if the accompanying write bit is set.  It would
     be nice to have exact permissions on a socket file, but it's
     necessary that somebody able to access the socket can always read
     the contents of the socket file to avoid spurious "permission
     denied" messages. */
  return mode | ((mode & (S_IWUSR | S_IWGRP | S_IWOTH)) << 1);
}

/* cygwin internal: map sockaddr into internet domain address */
static int __unused
get_inet_addr_unix (const struct sockaddr *in, int inlen,
		    struct sockaddr_storage *out, int *outlen,
		    int *type = NULL)
{
  /* Check for abstract socket. */
  if (inlen >= (int) sizeof (in->sa_family) + 7
      && in->sa_data[0] == '\0' && in->sa_data[1] == 'd'
      && in->sa_data[6] == '\0')
    {
      /* TODO */
      return 0;
    }

  path_conv pc (in->sa_data, PC_SYM_FOLLOW);
  if (pc.error)
    {
      set_errno (pc.error);
      return -1;
    }
  if (!pc.exists ())
    {
      set_errno (ENOENT);
      return -1;
    }
  /* Do NOT test for the file being a socket file here.  The socket file
     creation is not an atomic operation, so there is a chance that socket
     files which are just in the process of being created are recognized
     as non-socket files.  To work around this problem we now create the
     file with all sharing disabled.  If the below NtOpenFile fails
     with STATUS_SHARING_VIOLATION we know that the file already exists,
     but the creating process isn't finished yet.  So we yield and try
     again, until we can either open the file successfully, or some error
     other than STATUS_SHARING_VIOLATION occurs.
     Since we now don't know if the file is actually a socket file, we
     perform this check here explicitely. */
  NTSTATUS status;
  HANDLE fh;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;

  pc.get_object_attr (attr, sec_none_nih);
  do
    {
      status = NtOpenFile (&fh, GENERIC_READ | SYNCHRONIZE, &attr, &io,
			   FILE_SHARE_VALID_FLAGS,
			   FILE_SYNCHRONOUS_IO_NONALERT
			   | FILE_OPEN_FOR_BACKUP_INTENT
			   | FILE_NON_DIRECTORY_FILE);
      if (status == STATUS_SHARING_VIOLATION)
	{
	  /* While we hope that the sharing violation is only temporary, we
	     also could easily get stuck here, waiting for a file in use by
	     some greedy Win32 application.  Therefore we should never wait
	     endlessly without checking for signals and thread cancel event. */
	  pthread_testcancel ();
	  if (cygwait (NULL, cw_nowait, cw_sig_eintr) == WAIT_SIGNALED
	      && !_my_tls.call_signal_handler ())
	    {
	      set_errno (EINTR);
	      return -1;
	    }
	  yield ();
	}
      else if (!NT_SUCCESS (status))
	{
	  __seterrno_from_nt_status (status);
	  return -1;
	}
    }
  while (status == STATUS_SHARING_VIOLATION);
  /* Now test for the SYSTEM bit. */
  FILE_BASIC_INFORMATION fbi;
  status = NtQueryInformationFile (fh, &io, &fbi, sizeof fbi,
				   FileBasicInformation);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  if (!(fbi.FileAttributes & FILE_ATTRIBUTE_SYSTEM))
    {
      NtClose (fh);
      set_errno (EBADF);
      return -1;
    }
  /* Eventually check the content and fetch the required information. */
  char buf[128];
  memset (buf, 0, sizeof buf);
  status = NtReadFile (fh, NULL, NULL, NULL, &io, buf, 128, NULL, NULL);
  NtClose (fh);
  if (NT_SUCCESS (status))
    {
#if 0 /* TODO */
      struct sockaddr_in sin;
      char ctype;
      sin.sin_family = AF_INET;
      if (strncmp (buf, SOCKET_COOKIE, strlen (SOCKET_COOKIE)))
	{
	  set_errno (EBADF);
	  return -1;
	}
      sscanf (buf + strlen (SOCKET_COOKIE), "%hu %c", &sin.sin_port, &ctype);
      sin.sin_port = htons (sin.sin_port);
      sin.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
      memcpy (out, &sin, sizeof sin);
      *outlen = sizeof sin;
      if (type)
	*type = (ctype == 's' ? SOCK_STREAM :
		 ctype == 'd' ? SOCK_DGRAM
			      : 0);
#endif
      return 0;
    }
  __seterrno_from_nt_status (status);
  return -1;
}

fhandler_socket_unix::fhandler_socket_unix () :
  sun_path (NULL),
  peer_sun_path (NULL)
{
  set_cred ();
}

fhandler_socket_unix::~fhandler_socket_unix ()
{
  if (sun_path)
    cfree (sun_path);
  if (peer_sun_path)
    cfree (peer_sun_path);
}

void
fhandler_socket_unix::set_sun_path (const char *path)
{
  sun_path = path ? cstrdup (path) : NULL;
}

void
fhandler_socket_unix::set_peer_sun_path (const char *path)
{
  peer_sun_path = path ? cstrdup (path) : NULL;
}

void
fhandler_socket_unix::set_cred ()
{
  sec_pid = getpid ();
  sec_uid = geteuid32 ();
  sec_gid = getegid32 ();
  sec_peer_pid = (pid_t) 0;
  sec_peer_uid = (uid_t) -1;
  sec_peer_gid = (gid_t) -1;
}

int
fhandler_socket_unix::dup (fhandler_base *child, int flags)
{
  fhandler_socket_unix *fhs = (fhandler_socket_unix *) child;
  fhs->set_sun_path (get_sun_path ());
  fhs->set_peer_sun_path (get_peer_sun_path ());
  return fhandler_socket::dup (child, flags);
}

int
fhandler_socket_unix::socket (int af, int type, int protocol, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::socketpair (int af, int type, int protocol, int flags,
				  fhandler_socket_unix *fh_out)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::bind (const struct sockaddr *name, int namelen)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::listen (int backlog)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::accept4 (struct sockaddr *peer, int *len, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::connect (const struct sockaddr *name, int namelen)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::getsockname (struct sockaddr *name, int *namelen)
{
  struct sockaddr_un sun;

  sun.sun_family = AF_UNIX;
  sun.sun_path[0] = '\0';
  if (get_sun_path ())
    strncat (sun.sun_path, get_sun_path (), UNIX_PATH_MAX - 1);
  memcpy (name, &sun, MIN (*namelen, (int) SUN_LEN (&sun) + 1));
  *namelen = (int) SUN_LEN (&sun) + (get_sun_path () ? 1 : 0);
  return 0;
}

int
fhandler_socket_unix::getpeername (struct sockaddr *name, int *namelen)
{
  struct sockaddr_un sun;
  memset (&sun, 0, sizeof sun);
  sun.sun_family = AF_UNIX;
  sun.sun_path[0] = '\0';
  if (get_peer_sun_path ())
    strncat (sun.sun_path, get_peer_sun_path (), UNIX_PATH_MAX - 1);
  memcpy (name, &sun, MIN (*namelen, (int) SUN_LEN (&sun) + 1));
  *namelen = (int) SUN_LEN (&sun) + (get_peer_sun_path () ? 1 : 0);
  return 0;
}

int
fhandler_socket_unix::shutdown (int how)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::close ()
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::getpeereid (pid_t *pid, uid_t *euid, gid_t *egid)
{
  if (get_socket_type () != SOCK_STREAM)
    {
      set_errno (EINVAL);
      return -1;
    }
  if (connect_state () != connected)
    {
      set_errno (ENOTCONN);
      return -1;
    }

  __try
    {
      if (pid)
	*pid = sec_peer_pid;
      if (euid)
	*euid = sec_peer_uid;
      if (egid)
	*egid = sec_peer_gid;
      return 0;
    }
  __except (EFAULT) {}
  __endtry
  return -1;
}

ssize_t
fhandler_socket_unix::recvfrom (void *ptr, size_t len, int flags,
				struct sockaddr *from, int *fromlen)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t
fhandler_socket_unix::recvmsg (struct msghdr *msg, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

void __reg3
fhandler_socket_unix::read (void *ptr, size_t& len)
{
  set_errno (EAFNOSUPPORT);
  len = 0;
}

ssize_t __stdcall
fhandler_socket_unix::readv (const struct iovec *, int iovcnt, ssize_t tot)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t
fhandler_socket_unix::sendto (const void *in_ptr, size_t len, int flags,
			       const struct sockaddr *to, int tolen)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t
fhandler_socket_unix::sendmsg (const struct msghdr *msg, int flags)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t __stdcall
fhandler_socket_unix::write (const void *ptr, size_t len)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

ssize_t __stdcall
fhandler_socket_unix::writev (const struct iovec *, int iovcnt, ssize_t tot)
{
  set_errno (EAFNOSUPPORT);
  return -1;
}

int
fhandler_socket_unix::setsockopt (int level, int optname, const void *optval,
				   socklen_t optlen)
{
  /* Preprocessing setsockopt. */
  switch (level)
    {
    case SOL_SOCKET:
      switch (optname)
	{
	case SO_PASSCRED:
	  break;

	case SO_REUSEADDR:
	  saw_reuseaddr (*(int *) optval);
	  break;

	case SO_RCVBUF:
	  rmem (*(int *) optval);
	  break;

	case SO_SNDBUF:
	  wmem (*(int *) optval);
	  break;

	case SO_RCVTIMEO:
	case SO_SNDTIMEO:
	  if (optlen < (socklen_t) sizeof (struct timeval))
	    {
	      set_errno (EINVAL);
	      return -1;
	    }
	  if (!timeval_to_ms ((struct timeval *) optval,
			      (optname == SO_RCVTIMEO) ? rcvtimeo ()
						       : sndtimeo ()))
	  {
	    set_errno (EDOM);
	    return -1;
	  }
	  break;

	default:
	  /* AF_UNIX sockets simply ignore all other SOL_SOCKET options. */
	  break;
	}
      break;

    default:
      set_errno (ENOPROTOOPT);
      return -1;
    }

  return 0;
}

int
fhandler_socket_unix::getsockopt (int level, int optname, const void *optval,
				   socklen_t *optlen)
{
  /* Preprocessing getsockopt.*/
  switch (level)
    {
    case SOL_SOCKET:
      switch (optname)
	{
	case SO_ERROR:
	  {
	    int *e = (int *) optval;
	    *e = 0;
	    break;
	  }

	case SO_PASSCRED:
	  break;

	case SO_PEERCRED:
	  {
	    struct ucred *cred = (struct ucred *) optval;

	    if (*optlen < (socklen_t) sizeof *cred)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    int ret = getpeereid (&cred->pid, &cred->uid, &cred->gid);
	    if (!ret)
	      *optlen = (socklen_t) sizeof *cred;
	    return ret;
	  }

	case SO_REUSEADDR:
	  {
	    unsigned int *reuseaddr = (unsigned int *) optval;

	    if (*optlen < (socklen_t) sizeof *reuseaddr)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    *reuseaddr = saw_reuseaddr();
	    *optlen = (socklen_t) sizeof *reuseaddr;
	    break;
	  }

	case SO_RCVTIMEO:
	case SO_SNDTIMEO:
	  {
	    struct timeval *time_out = (struct timeval *) optval;

	    if (*optlen < (socklen_t) sizeof *time_out)
	      {
		set_errno (EINVAL);
		return -1;
	      }
	    DWORD ms = (optname == SO_RCVTIMEO) ? rcvtimeo () : sndtimeo ();
	    if (ms == 0 || ms == INFINITE)
	      {
		time_out->tv_sec = 0;
		time_out->tv_usec = 0;
	      }
	    else
	      {
		time_out->tv_sec = ms / MSPERSEC;
		time_out->tv_usec = ((ms % MSPERSEC) * USPERSEC) / MSPERSEC;
	      }
	    *optlen = (socklen_t) sizeof *time_out;
	    break;
	  }

	case SO_TYPE:
	  {
	    unsigned int *type = (unsigned int *) optval;
	    *type = get_socket_type ();
	    *optlen = (socklen_t) sizeof *type;
	    break;
	  }

	/* AF_UNIX sockets simply ignore all other SOL_SOCKET options. */

	case SO_LINGER:
	  {
	    struct linger *linger = (struct linger *) optval;
	    memset (linger, 0, sizeof *linger);
	    *optlen = (socklen_t) sizeof *linger;
	    break;
	  }

	default:
	  {
	    unsigned int *val = (unsigned int *) optval;
	    *val = 0;
	    *optlen = (socklen_t) sizeof *val;
	    break;
	  }
	}
      break;

    default:
      set_errno (ENOPROTOOPT);
      return -1;
    }

  return 0;
}

int
fhandler_socket_unix::ioctl (unsigned int cmd, void *p)
{
  int ret;

  switch (cmd)
    {
    case FIOASYNC:
#ifdef __x86_64__
    case _IOW('f', 125, int):
#endif
      break;
    case FIONREAD:
#ifdef __x86_64__
    case _IOR('f', 127, int):
#endif
    case FIONBIO:
    case SIOCATMARK:
      break;
    default:
      ret = fhandler_socket::ioctl (cmd, p);
      break;
    }
  return ret;
}

int
fhandler_socket_unix::fcntl (int cmd, intptr_t arg)
{
  int ret;

  switch (cmd)
    {
    case F_SETOWN:
      break;
    case F_GETOWN:
      break;
    default:
      ret = fhandler_socket::fcntl (cmd, arg);
      break;
    }
  return ret;
}

int __reg2
fhandler_socket_unix::fstat (struct stat *buf)
{
  int ret;

  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::fstat (buf);
  ret = fhandler_base::fstat_fs (buf);
  if (!ret)
    {
      buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
      buf->st_size = 0;
    }
  return ret;
}

int __reg2
fhandler_socket_unix::fstatvfs (struct statvfs *sfs)
{
  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::fstatvfs (sfs);
  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  return fh.fstatvfs (sfs);
}

int
fhandler_socket_unix::fchmod (mode_t newmode)
{
  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::fchmod (newmode);
  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  return fh.fchmod (S_IFSOCK | adjust_socket_file_mode (newmode));
}

int
fhandler_socket_unix::fchown (uid_t uid, gid_t gid)
{
  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::fchown (uid, gid);
  fhandler_disk_file fh (pc);
  return fh.fchown (uid, gid);
}

int
fhandler_socket_unix::facl (int cmd, int nentries, aclent_t *aclbufp)
{
  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::facl (cmd, nentries, aclbufp);
  fhandler_disk_file fh (pc);
  return fh.facl (cmd, nentries, aclbufp);
}

int
fhandler_socket_unix::link (const char *newpath)
{
  if (!get_sun_path () || get_sun_path ()[0] == '\0')
    return fhandler_socket::link (newpath);
  fhandler_disk_file fh (pc);
  return fh.link (newpath);
}
