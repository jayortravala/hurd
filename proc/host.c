/* Proc server host management calls
   Copyright (C) 1992, 1993, 1994 Free Software Foundation

This file is part of the GNU Hurd.

The GNU Hurd is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

The GNU Hurd is distributed in the hope that it will be useful, 
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with the GNU Hurd; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* Written by Michael I. Bushnell.  */

#include <mach.h>
#include <hurd.h>
#include <sys/types.h>
#include <hurd/hurd_types.h>
#include <stdlib.h>
#include <errno.h>
#include <mach/notify.h>
#include <string.h>
#include <stdio.h>
#include <hurd/exec.h>
#include <unistd.h>
#include <assert.h>

#include "proc.h"
#include "process_S.h"

static long hostid;
static char *hostname;
static int hostnamelen;
static mach_port_t *std_port_array;
static int *std_int_array;
static int n_std_ports, n_std_ints;
static struct utsname uname_info;

struct server_version
{
  char *name;
  char *version;
  char *release;
} *server_versions;
int nserver_versions, server_versions_nalloc;

struct execdata_notify 
{
  mach_port_t notify_port;
  struct execdata_notify *next;
} *execdata_notifys;

/* Implement proc_sethostid as described in <hurd/proc.defs>. */
kern_return_t
S_proc_sethostid (struct proc *p,
		int newhostid)
{
  if (! check_uid (p, 0))
    return EPERM;
  
  hostid = newhostid;

  return 0;
}

/* Implement proc_gethostid as described in <hurd/proc.defs>. */
kern_return_t 
S_proc_gethostid (struct proc *p,
		int *outhostid)
{
  *outhostid = hostid;
  return 0;
}

/* Implement proc_sethostname as described in <hurd/proc.defs>. */
kern_return_t
S_proc_sethostname (struct proc *p,
		    char *newhostname,
		    u_int newhostnamelen)
{
  int len;
  if (! check_uid (p, 0))
    return EPERM;
  
  if (hostname)
    free (hostname);

  hostname = malloc (newhostnamelen + 1);
  hostnamelen = newhostnamelen;

  bcopy (newhostname, hostname, newhostnamelen);
  hostname[newhostnamelen] = '\0';

  len = newhostnamelen + 1;
  if (len > sizeof uname_info.nodename)
    len = sizeof uname_info.nodename;
  bcopy (hostname, uname_info.nodename, len);
  uname_info.nodename[sizeof uname_info.nodename - 1] = '\0';

  return 0;
}

/* Implement proc_gethostname as described in <hurd/proc.defs>. */
kern_return_t
S_proc_gethostname (struct proc *p,
		    char **outhostname,
		    u_int *outhostnamelen)
{
  if (*outhostnamelen < hostnamelen + 1)
    vm_allocate (mach_task_self (), (vm_address_t *)outhostname,
		 hostnamelen + 1, 1);
  *outhostnamelen = hostnamelen + 1;
  if (hostname)
    bcopy (hostname, *outhostname, hostnamelen + 1);
  else
    **outhostname = '\0';
  return 0;
}

/* Implement proc_getprivports as described in <hurd/proc.defs>. */
kern_return_t
S_proc_getprivports (struct proc *p,
		     mach_port_t *hostpriv,
		     mach_port_t *devpriv)
{
  if (! check_uid (p, 0))
    return EPERM;
  
  *hostpriv = master_host_port;
  *devpriv = master_device_port;
  return 0;
}


/* Implement proc_setexecdata as described in <hurd/proc.defs>. */
kern_return_t
S_proc_setexecdata (struct proc *p,
		    mach_port_t *ports,
		    u_int nports,
		    int *ints,
		    u_int nints)
{
  int i;
  struct execdata_notify *n;
  
  if (!check_uid (p, 0))
    return EPERM;
  
  if (std_port_array)
    {
      for (i = 0; i < n_std_ports; i++)
	mach_port_deallocate (mach_task_self (), std_port_array[i]);
      free (std_port_array);
    }
  if (std_int_array)
    free (std_int_array);
  
  std_port_array = malloc (sizeof (mach_port_t) * nports);
  n_std_ports = nports;
  bcopy (ports, std_port_array, sizeof (mach_port_t) * nports);
  
  std_int_array = malloc (sizeof (int) * nints);
  n_std_ints = nints;
  bcopy (ints, std_int_array, sizeof (int) * nints);
  
  for (n = execdata_notifys; n; n = n->next)
    exec_setexecdata (n->notify_port, std_port_array, MACH_MSG_TYPE_COPY_SEND,
		      n_std_ports, std_int_array, n_std_ints);
      
  return 0;
}

/* Implement proc_getexecdata as described in <hurd/proc.defs>. */
kern_return_t 
S_proc_getexecdata (struct proc *p,
		    mach_port_t **ports,
		    mach_msg_type_name_t *portspoly,
		    u_int *nports,
		    int **ints,
		    u_int *nints)
{
  /* XXX memory leak here */

  if (!std_port_array)
    return ENOENT;

  if (*nports < n_std_ports)
    *ports = malloc (n_std_ports * sizeof (mach_port_t));
  bcopy (std_port_array, *ports, n_std_ports * sizeof (mach_port_t));
  *nports = n_std_ports;
  
  if (*nints < n_std_ints)
    *ints = malloc (n_std_ints * sizeof (mach_port_t));
  bcopy (std_int_array, *ints, n_std_ints * sizeof (int));
  *nints = n_std_ints;

  return 0;
}

/* Implement proc_execdata_notify as described in <hurd/proc.defs>. */
kern_return_t
S_proc_execdata_notify (struct proc *p,
			mach_port_t notify)
{
  struct execdata_notify *n = malloc (sizeof (struct execdata_notify));
  mach_port_t foo;

  n->notify_port = notify;
  n->next = execdata_notifys;
  execdata_notifys = n;

  mach_port_request_notification (mach_task_self (), notify, 
				  MACH_NOTIFY_DEAD_NAME, 1,
				  generic_port, MACH_MSG_TYPE_MAKE_SEND_ONCE,
				  &foo);

  if (foo)
    mach_port_deallocate (mach_task_self (), foo);
  
  if (std_port_array)
    exec_setexecdata (n->notify_port, std_port_array, MACH_MSG_TYPE_COPY_SEND, 
		      n_std_ports, std_int_array, n_std_ints);
  return 0;
}

/* Check all the execdata notify ports and see if one of them is
   PORT; if it is, then free it. */
void
check_dead_execdata_notify (mach_port_t port)
{
  struct execdata_notify *en, **prevp;
  
  for (en = execdata_notifys, prevp = &execdata_notifys; en; en = *prevp)
    {
      if (en->notify_port == port)
	{
	  mach_port_deallocate (mach_task_self (), port);
	  *prevp = en->next;
	  free (en);
	}
      else
	prevp = &en->next;
    }
}

/* Version information handling.

   A server registers its name, release, and version with
   startup_register_version.  The uname version string is composed of all
   the server names and versions.  The uname release is composed of the
   differing server releases in order of decreasing popularity (just one if
   they all agree).

   The Hurd release comes from <hurd/hurd_types.h> and
   is compiled into proc as well as the other servers. */

/* Rebuild the uname version string.  */
static void
rebuild_uname (void)
{
  unsigned int i, j;
  char *p, *end;

  /* Set up for addstr to write into STRING.  */
  inline void initstr (char *string)
    {
      p = string;
      end = p + _UTSNAME_LENGTH;
    }
  /* If NAME is not null, write "name-version/", else "version/".  */
  inline void addstr (const char *name, const char *version)
    {
      size_t len;
      if (name)
	{
	  len = strlen (name);
	  if (p + len + 1 < end)
	    memcpy (p, name, len);
	  p += len;
	  if (p < end)
	    *p++ = '-';
	}
      len = strlen (version);
      if (p + len + 1 < end)
	memcpy (p, version, len);
      p += len;
      if (p < end)
	*p++ = '/';
    }

  /* Collect all the differing release strings and count how many
     servers use each.  */
  struct release
    {
      const char *release;
      unsigned int count;
    } releases[nserver_versions];
  int compare_releases (const void *a, const void *b)
    {
      return (((const struct release *) b)->count -
	      ((const struct release *) a)->count);
    }
  unsigned int nreleases = 0;

  for (i = 0; i < nserver_versions; ++i)
    {
      for (j = 0; j < nreleases; ++j)
	if (! strcmp (releases[j].release, server_versions[i].release))
	  {
	    ++releases[j].count;
	    break;
	  }
      if (j == nreleases)
	{
	  releases[nreleases].release = server_versions[i].release;
	  releases[nreleases].count = 1;
	  ++nreleases;
	}
    }

  /* Sort the releases in order of decreasing popularity.  */
  qsort (releases, nreleases, sizeof (struct release), compare_releases);

  /* Now build the uname strings.  */

  initstr (uname_info.release);
  for (i = 0; i < nreleases; ++i)
    addstr (NULL, releases[i].release);

  if (p > end)
#ifdef notyet
    syslog (LOG_EMERG,
	    "_UTSNAME_LENGTH %u too short; inform bug-glibc@prep.ai.mit.edu\n",
	    p - end)
#endif
      ;
  else
    p[-1] = '\0';
  end[-1] = '\0';

  for (i = 2; i < nserver_versions; i++)
    if (strcmp (server_versions[i].version, server_versions[1].version))
      break;

  initstr (uname_info.version);

  if (i == nserver_versions)
    {
      /* All the servers after [0] (the microkernel version)
	 are the same, so just write one "hurd" version.  */
      addstr (server_versions[0].name, server_versions[0].version);
      addstr ("Hurd", server_versions[1].version);
    }
  else
    for (i = 0; i < nserver_versions; i++)
      addstr (server_versions[i].name, server_versions[i].version);

  if (p > end)
#ifdef notyet
    syslog (LOG_EMERG,
	    "_UTSNAME_LENGTH %u too short; inform bug-glibc@prep.ai.mit.edu\n",
	    p - end)
#endif
      ;
  else
    p[-1] = '\0';
  end[-1] = '\0';
}

      
void
initialize_version_info (void)
{
  extern const char *const mach_cpu_types[];
  extern const char *const mach_cpu_subtypes[][32];
  kernel_version_t kernel_version;
  char *p;
  struct host_basic_info info;
  unsigned int n = sizeof info;
  error_t err;

  /* Fill in fixed slots sysname and machine.  */
  strcpy (uname_info.sysname, "GNU");

  err = host_info (mach_host_self (), HOST_BASIC_INFO, (int *) &info, &n);
  assert (! err);
  snprintf (uname_info.machine, sizeof uname_info.machine, "%s/%s",
	    mach_cpu_types[info.cpu_type],
	    mach_cpu_subtypes[info.cpu_type][info.cpu_subtype]);

  /* Notice Mach's and our own version and initialize server version
     varables. */
  server_versions = malloc (sizeof (struct server_version) * 10);
  server_versions_nalloc = 10;

  err = host_kernel_version (mach_host_self (), kernel_version);
  assert (! err);
  p = index (kernel_version, ':');
  if (p)
    *p = '\0';
  p = index (kernel_version, ' ');
  if (p)
    *p = '\0';
  server_versions[0].name = strdup (p ? kernel_version : "mach");
  server_versions[0].release = strdup (HURD_RELEASE);
  server_versions[0].version = strdup (p ? p + 1 : kernel_version);

  server_versions[1].name = strdup (OUR_SERVER_NAME);
  server_versions[1].release = strdup (HURD_RELEASE);
  server_versions[1].version = strdup (OUR_VERSION);

  nserver_versions = 2;

  rebuild_uname ();
  
  uname_info.nodename[0] = '\0';
}

kern_return_t
S_proc_uname (pstruct_t process,
	      struct utsname *uname)
{
  *uname = uname_info;
  return 0;
}

kern_return_t
S_proc_register_version (pstruct_t server,
			 mach_port_t credential,
			 char *name,
			 char *release, 
			 char *version)
{
  int i;

  if (credential != master_host_port)
    /* Must be privileged to register for uname. */
    return EPERM;
  
  for (i = 0; i < nserver_versions; i++)
    if (!strcmp (name, server_versions[i].name))
      {
	/* Change this entry.  */
	free (server_versions[i].version);
	free (server_versions[i].release);
	server_versions[i].version = malloc (strlen (version) + 1);
	server_versions[i].release = malloc (strlen (version) + 1);
	strcpy (server_versions[i].version, version);
	strcpy (server_versions[i].release, release);
	break;
      }
  if (i == nserver_versions)
    {
      /* Didn't find it; extend.  */
      if (nserver_versions == server_versions_nalloc)
	{
	  server_versions_nalloc *= 2;
	  server_versions = realloc (server_versions,
				     sizeof (struct server_version) *
				     server_versions_nalloc);
	}
      server_versions[nserver_versions].name = malloc (strlen (name) + 1);
      server_versions[nserver_versions].version = malloc (strlen (version) 
							  + 1);
      server_versions[nserver_versions].release = malloc (strlen (release)
							  + 1);
      strcpy (server_versions[nserver_versions].name, name);
      strcpy (server_versions[nserver_versions].version, version);
      strcpy (server_versions[nserver_versions].release, release);
      nserver_versions++;
    }
  
  rebuild_uname ();
  mach_port_deallocate (mach_task_self (), credential);
  return 0;
}
