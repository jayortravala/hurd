/* 
   Copyright (C) 1996 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "netfs.h"
#include "fsys_S.h"
#include "misc.h"
#include <fcntl.h>

error_t
netfs_S_fsys_getroot (mach_port_t cntl,
		      mach_port_t dotdot,
		      uid_t *uids, mach_msg_type_number_t nuids,
		      uid_t *gids, mach_msg_type_number_t ngids,
		      int flags,
		      retry_type *do_retry,
		      char *retry_name,
		      mach_port_t *retry_port,
		      mach_port_t *retry_port_type)
{
  struct port_info *pt = ports_lookup_port (netfs_port_bucket, cntl,
					    netfs_control_class);
  struct netcred *cred;
  error_t err;
  struct protid *newpi;
  mode_t type;

  if (!pt)
    return EOPNOTSUPP;
  ports_port_deref (pt);

  cred = netfs_make_credential (uids, nuids, gids, ngids);
  
  flags &= O_HURD;
  
  mutex_lock (&netfs_root_node->lock);
  err = netfs_validate_stat (netfs_root_node, cred);
  if (err)
    goto out;
  
  type = netfs_root_node->nn_stat.st_mode & S_IFMT;
  
  if (type == S_IFLNK && !(flags & (O_NOLINK | O_NOTRANS)))
    {
      char pathbuf[netfs_root_node->nn_stat.st_size + 1];
      
      err = netfs_attempt_readlink (cred, netfs_root_node, pathbuf);

      mutex_unlock (&netfs_root_node->lock);
      if (err)
	goto out;
      
      if (pathbuf[0] == '/')
	{
	  *do_retry = FS_RETRY_MAGICAL;
	  *retry_port = MACH_PORT_NULL;
	  *retry_port_type = MACH_MSG_TYPE_COPY_SEND;
	  strcpy (retry_name, pathbuf);
	  mach_port_deallocate (mach_task_self (), dotdot);
	  return 0;
	}
      else
	{
	  *do_retry = FS_RETRY_REAUTH;
	  *retry_port = dotdot;
	  *retry_port_type = MACH_MSG_TYPE_MOVE_SEND;
	  strcpy (retry_name, pathbuf);
	  return 0;
	}
    }
  
  if ((type == S_IFSOCK || type == S_IFBLK || type == S_IFCHR 
      || type == S_IFIFO) && (flags & (O_READ|O_WRITE|O_EXEC)))
    {
      mutex_unlock (&netfs_root_node->lock);
      return EOPNOTSUPP;
    }
  
  err = netfs_check_open_permissions (cred, netfs_root_node, flags, 0);
  if (err)
    goto out;
  
  flags &= ~OPENONLY_STATE_MODES;
  
  newpi = netfs_make_protid (netfs_make_peropen (netfs_root_node, flags,
						 dotdot),
			     cred);
  mach_port_deallocate (mach_task_self (), dotdot);
  *do_retry = FS_RETRY_NORMAL;
  *retry_port = ports_get_right (newpi);
  *retry_port_type = MACH_MSG_TYPE_MAKE_SEND;
  retry_name[0] = '\0';
  ports_port_deref (newpi);
  
 out:
  mutex_unlock (&netfs_root_node->lock);
  return err;
}

  
      
