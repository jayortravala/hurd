/* 
   Copyright (C) 1995, 1996, 1997, 2000 Free Software Foundation, Inc.
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

#include "term.h"
#include <hurd.h>
#include <fcntl.h>
#include <hurd/trivfs.h>
#include <stdio.h>
#include <hurd/fsys.h>
#include <string.h>

int trivfs_fstype = FSTYPE_TERM;
int trivfs_fsid = 0;		/* pid?? */
int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ|O_WRITE;

int
demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  extern int term_server (mach_msg_header_t *, mach_msg_header_t *);
  extern int tioctl_server (mach_msg_header_t *, mach_msg_header_t *);
  extern int device_reply_server (mach_msg_header_t *, mach_msg_header_t *);

  return (trivfs_demuxer (inp, outp)
	  || term_server (inp, outp)
	  || tioctl_server (inp, outp)
	  || device_reply_server (inp, outp));
}

int
main (int argc, char **argv)
{
  struct port_class *ourclass, *ourcntlclass;
  struct port_class *peerclass, *peercntlclass;
  struct trivfs_control **ourcntl, **peercntl;
  mach_port_t bootstrap, right;
  enum {T_DEVICE, T_PTYMASTER, T_PTYSLAVE} type;
  struct stat st;

  term_bucket = ports_create_bucket ();
  
  trivfs_add_control_port_class (&tty_cntl_class);
  trivfs_add_control_port_class (&pty_cntl_class);
  trivfs_add_protid_port_class (&tty_class);
  trivfs_add_protid_port_class (&pty_class);

  cttyid_class = ports_create_class (0, 0);

  init_users ();

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  
  if (argc != 4)
    {
      fprintf (stderr, "Usage: term ttyname type arg\n");
      exit (1);
    }

  if (!strcmp (argv[2], "device"))
    {
      type = T_DEVICE;
      bottom = &devio_bottom;
      ourclass = tty_class;
      ourcntlclass = tty_cntl_class;
      ourcntl = &termctl;
      peerclass = 0;
      peercntlclass = 0;
      peercntl = 0;
      pterm_name = argv[3];
    }
  else if (!strcmp (argv[2], "pty-master"))
    {
      type = T_PTYMASTER;
      bottom = &ptyio_bottom;
      ourclass = pty_class;
      ourcntlclass = pty_cntl_class;
      ourcntl = &ptyctl;
      peerclass = tty_class;
      peercntlclass = tty_cntl_class;
      peercntl = &termctl;
    }
  else if (!strcmp (argv[2], "pty-slave"))
    {
      type = T_PTYSLAVE;
      bottom = &ptyio_bottom;
      ourclass = tty_class;
      ourcntlclass = tty_cntl_class;
      ourcntl = &termctl;
      peerclass = pty_class;
      peercntlclass = pty_cntl_class;
      peercntl = &ptyctl;
    }
  else
    {
      fprintf (stderr, 
	       "Allowable types are device, pty-master, and pty-slave.\n");
      exit (1);
    }
  
  if (bootstrap == MACH_PORT_NULL)
    {
      fprintf (stderr, "Must be started as a translator\n");
      exit (1);
    }

  /* Set our node */
  errno = trivfs_startup (bootstrap, 0,
			  ourcntlclass, term_bucket, ourclass, term_bucket,
			  ourcntl);
  if (errno)
    {
      perror ("Starting translator");
      exit (1);
    }

  /* For ptys, the nodename depends on which half is used.  For now just use
     the hook to store the nodename.  */
  (*ourcntl)->hook = argv[1];

  /* Set peer */
  if (peerclass)
    {
      char *peer_name = argv[3];
      file_t file = file_name_lookup (peer_name, O_CREAT|O_NOTRANS, 0666);

      if (file != MACH_PORT_NULL)
	errno = 0;

      if (! errno)
	errno = trivfs_create_control (file, peercntlclass, term_bucket,
				       peerclass, term_bucket, peercntl);
      if (! errno)
	{
	  right = ports_get_send_right (*peercntl);
	  errno = file_set_translator (file, 0, FS_TRANS_EXCL | FS_TRANS_SET,
				     0, 0, 0, right, MACH_MSG_TYPE_COPY_SEND);
	  mach_port_deallocate (mach_task_self (), right);
	}

      if (errno)
	{
	  perror (peer_name);
	  exit (1);
	}

      (*peercntl)->hook = peer_name;
      ports_port_deref (*peercntl);
    }

  bzero (&termstate, sizeof (termstate));
  termflags = NO_CARRIER | NO_OWNER;
  mutex_init (&global_lock);

  /* Initialize status from underlying node.  */
  errno = io_stat ((*ourcntl)->underlying, &st);
  if (errno)
    {
      /* We cannot stat the underlying node.  Fallback to the defaults.  */
      term_owner = term_group = 0;
      term_mode = (bottom == &ptyio_bottom ? DEFFILEMODE : S_IRUSR | S_IWUSR);
      errno = 0;
    }
  else
    {
      term_owner = st.st_uid;
      term_group = st.st_gid;
      term_mode = (st.st_mode & ACCESSPERMS);
    }
  term_mode |= S_IFCHR | S_IROOT;

  inputq = create_queue (256, QUEUE_LOWAT, QUEUE_HIWAT);
  
  rawq = create_queue (256, QUEUE_LOWAT, QUEUE_HIWAT);
  
  outputq = create_queue (256, QUEUE_LOWAT, QUEUE_HIWAT);
  
  if (bottom == &ptyio_bottom)
    ptyio_init ();

  condition_init (&carrier_alert);
  condition_init (&select_alert);
  condition_implies (inputq->wait, &select_alert);
  condition_implies (outputq->wait, &select_alert);

  /* Launch */
  ports_manage_port_operations_multithread (term_bucket, demuxer, 0, 0, 0);

  return 0;
}  
