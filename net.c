/*
 *
 * net.c         Network related functions
 *
 * Copyright (c) 1996-2002  Hubert Mantel, SuSE Linux AG  (mantel@suse.de)
 *
 */

#define WITH_NFS

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>
#include <rpcsvc/mount.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <nfs/nfs.h>
#include "nfs_mount4.h"

/* this is probably the wrong solution... */
#ifndef NFS_FHSIZE
#define NFS_FHSIZE 32
#endif
#ifndef NFS_PORT
#define NFS_PORT 2049
#endif

#include <hd.h>

#include "global.h"
#include "text.h"
#include "dialog.h"
#include "window.h"
#include "util.h"
#include "net.h"
#include "bootpc.h"
#include "file.h"
#include "module.h"
#include "hotplug.h"

#define NFS_PROGRAM    100003
#define NFS_VERSION         2

static int  net_is_ptp_im = FALSE;

#if !defined(NETWORK_CONFIG)
#  define NETWORK_CONFIG 1
#endif

static int net_activate(void);
#if defined(__s390__) || defined(__s390x__)
int net_activate_s390_devs(void);
int net_activate_s390_devs_ex(hd_t* hd, char** device);
#endif
static void net_setup_nameserver(void);

#if NETWORK_CONFIG
static int net_choose_device(void);
static int net_input_data(void);
#endif
#ifdef WITH_NFS
static void net_show_error(enum nfs_stat status_rv);
#endif

static void if_down(char *dev);
static int wlan_auth_cb(dia_item_t di);

static dia_item_t di_wlan_auth_last = di_none;

/*
 * Ask for VNC & SSH password, unless they have already been set.
 *
 * Global vars changed:
 *  config.net.vncpassword
 *  config.net.sshpassword
 */
void net_ask_password()
{
  int win_old = config.win;

  if(config.vnc && (!config.net.vncpassword || strlen(config.net.vncpassword) < 8)) {
    if(!config.win) util_disp_init();
    for(;;) {
      if(dia_input2(txt_get(TXT_VNC_PASSWORD), &config.net.vncpassword, 20, 1)) break;
      if(config.net.vncpassword && strlen(config.net.vncpassword) >= 8) break;
      dia_message(txt_get(TXT_VNC_PASSWORD_TOO_SHORT), MSGTYPE_ERROR);
    }
  }

  if(config.usessh && !config.net.sshpassword) {
    if(!config.win) util_disp_init();
    dia_input2(txt_get(TXT_SSH_PASSWORD), &config.net.sshpassword, 20, 1);
  }

  if(config.win && !win_old) util_disp_done();
}

void net_ask_hostname()
{
  char* dot;
  int win_old = config.win;
  if(!config.net.realhostname || !config.net.domain)
  {
    if(!config.win) util_disp_init();
repeat:
    dia_input2(txt_get(TXT_HOSTNAME), &config.net.realhostname, 20, 0);
    dot=strstr(config.net.realhostname,".");	/* find first dot */
    if(!dot)
    {
      dia_message(txt_get(TXT_INVALID_FQHOSTNAME), MSGTYPE_ERROR);
      goto repeat;
    }
    str_copy(&config.net.domain,dot+1);	/* copy domain part */
    *dot=0;	/* cut off domain */
    if(config.win && !win_old) util_disp_done();
  }
}

/*
 * Configure network. Ask for network config data if necessary.
 * Does either DHCP, BOOTP or calls net_activate_ns() to setup the interface.
 *
 * Return:
 *      0: ok
 *   != 0: error or abort
 *
 * Global vars changed:
 *  config.net.is_configured
 *
 * Does nothing if DHCP is active.
 *
 * FIXME: needs window mode or not?
 */
int net_config()
{
  int rc = 0;
#if NETWORK_CONFIG
  char buf[256];

  if(config.net.keep) return 0;

  net_ask_password();

  if(
    config.win &&
    config.net.is_configured &&
    (!config.manual || dia_yesno(txt_get(TXT_NET_CONFIGURED), YES) == YES)
  ) {
    return 0;
  }

  if(net_choose_device()) return -1;

  net_stop();

  config.net.configured = nc_none;

  if(config.win && config.net.setup != NS_DHCP) {
    if((config.net.setup & NS_DHCP)) {
      sprintf(buf, txt_get(TXT_ASK_DHCP), config.net.use_dhcp ? "DHCP" : "BOOTP");
      rc = dia_yesno(buf, NO);
    }
    else {
      rc = NO;
    }
  }
  else {
    rc = YES;
  }

  if(rc == ESCAPE) return -1;

  if(rc == YES) {
    rc = config.net.use_dhcp ? net_dhcp() : net_bootp();
    if(!rc) config.net.configured = config.net.use_dhcp ? nc_dhcp : nc_bootp;
  }
  else {
    rc = net_input_data();
    if(!rc) config.net.configured = nc_static;
  }

  if(rc) return -1;

  if(net_activate_ns()) {
    dia_message(txt_get(TXT_ERROR_CONF_NET), MSGTYPE_ERROR);
    config.net.configured = nc_none;
    if(!config.test) return rc = -1;
  }

  net_check_address2(&config.net.server, 1);

#endif

  return rc;
}


/*
 * Shut down all network interfaces.
 *
 * Global vars changed:
 *  config.net.is_configured
 *
 * config.net.device:    interface
 * /proc/net/route: configured interfaces
 */
void net_stop()
{
  file_t *f0, *f;
  slist_t *sl0 = NULL, *sl;

  if(config.debug) fprintf(stderr, "shutting network down\n");

  if(config.net.keep) return;

  if(config.test) {
    config.net.is_configured = 0;
    return;
  }

  if(config.net.dhcp_active) {
    net_dhcp_stop();
    config.net.is_configured = 0;
  }

  if(!config.net.is_configured) return;

  /* build list of configured interfaces */
  slist_append_str(&sl0, config.net.device);

  f0 = file_read_file("/proc/net/route", kf_none);
  for((f = f0) && (f = f->next); f; f = f->next) {
    if(f->key_str && !slist_getentry(sl0, f->key_str)) slist_append_str(&sl0, f->key_str);
  }
  file_free_file(f0);

  for(sl = sl0; sl; sl = sl->next) if_down(sl->key);

  slist_free(sl0);

  config.net.is_configured = 0;
}


/*
 * Configure loopback interface.
 */
int net_setup_localhost()
{
    char                address_ti [20];
    struct in_addr      ipaddr_ri;
    int                 socket_ii;
    struct ifreq        interface_ri;
    struct sockaddr_in  sockaddr_ri;
    int                 error_ii = FALSE;

    if(config.test) return 0;

    fprintf (stderr, "Setting up localhost...");
    fflush (stdout);

    socket_ii = socket (AF_INET, SOCK_DGRAM, 0);
    if (socket_ii == -1)
        return (socket_ii);

    memset (&interface_ri, 0, sizeof (struct ifreq));
    strcpy (interface_ri.ifr_name, "lo");

    sockaddr_ri.sin_family = AF_INET;
    sockaddr_ri.sin_port = 0;
    strcpy (address_ti, "127.0.0.1");
    if (!inet_aton (address_ti, &ipaddr_ri))
        error_ii = TRUE;
    sockaddr_ri.sin_addr = ipaddr_ri;
    memcpy (&interface_ri.ifr_addr, &sockaddr_ri, sizeof (sockaddr_ri));
    if (ioctl (socket_ii, SIOCSIFADDR, &interface_ri) < 0)
        {
        HERE
        error_ii = TRUE;
        }

    strcpy (address_ti, "255.0.0.0");
    if (!inet_aton (address_ti, &ipaddr_ri))
        error_ii = TRUE;
    sockaddr_ri.sin_addr = ipaddr_ri;
    memcpy (&interface_ri.ifr_netmask, &sockaddr_ri, sizeof (sockaddr_ri));
    if (ioctl (socket_ii, SIOCSIFNETMASK, &interface_ri) < 0)
        if (config.net.netmask.ip.s_addr)
            {
            HERE
            error_ii = TRUE;
            }

    strcpy (address_ti, "127.255.255.255");
    if (!inet_aton (address_ti, &ipaddr_ri))
        error_ii = TRUE;
    sockaddr_ri.sin_addr = ipaddr_ri;
    memcpy (&interface_ri.ifr_broadaddr, &sockaddr_ri, sizeof (sockaddr_ri));
    if (ioctl (socket_ii, SIOCSIFBRDADDR, &interface_ri) < 0)
        if (config.net.broadcast.ip.s_addr != 0xffffffff)
            {
            HERE
            error_ii = TRUE;
            }

    if (ioctl (socket_ii, SIOCGIFFLAGS, &interface_ri) < 0)
        {
        HERE
        error_ii = TRUE;
        }

    interface_ri.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_LOOPBACK | IFF_BROADCAST;
    if (ioctl (socket_ii, SIOCSIFFLAGS, &interface_ri) < 0)
        {
        HERE
        error_ii = TRUE;
        }

    close (socket_ii);

  fprintf (stderr, "%s\n", error_ii ? "failure" : "done");

  return error_ii;
}


/*
 * Setup network interface and write name server config.
 *
 * Return:
 *      0: ok
 *   != 0: error code
 *
 * Global vars changed:
 *  config.net.is_configured
 *  config.net.nameserver
 *
 * Writes nameserver & domain to /etc/resolv.conf.
 *
 * config.net.device: interface
 */
int net_activate_ns()
{
  int rc;

  if(config.net.keep) return 0;

  rc = net_activate();

  if(!rc) net_setup_nameserver();

  return rc;
}


/*
 * Setup network interface.
 *
 * Return:
 *      0: ok
 *   != 0: error code
 *
 * Global vars changed:
 *  config.net.is_configured
 *
 * Does nothing if DHCP is active.
 *
 * config.net.device: interface
 */
int net_activate()
{
    int                 socket_ii;
    struct ifreq        interface_ri;
    struct rtentry      route_ri;
    struct sockaddr_in  sockaddr_ri;
    int                 error_ii = FALSE;

    if(config.test || !config.net.ifconfig || config.net.dhcp_active || config.net.keep) return 0;

    if(!config.net.device) {
      fprintf(stderr, "net_activate: no network interface!\n");
      return 1;
    }

    config.net.is_configured = 0;

    net_apply_ethtool(config.net.device, config.net.hwaddr);

    socket_ii = socket (AF_INET, SOCK_DGRAM, 0);
    if (socket_ii == -1)
    {
        fprintf(stderr, "net_activate: socket(AF_INET, SOCK_DGRAM, 0) failed at %d\n",__LINE__);
        return (socket_ii);
    }

    memset (&interface_ri, 0, sizeof (struct ifreq));
    strcpy (interface_ri.ifr_name, config.net.device);

    sockaddr_ri.sin_family = AF_INET;
    sockaddr_ri.sin_port = 0;
    sockaddr_ri.sin_addr = config.net.hostname.ip;
    memcpy (&interface_ri.ifr_addr, &sockaddr_ri, sizeof (sockaddr_ri));
    if (ioctl (socket_ii, SIOCSIFADDR, &interface_ri) < 0)
    {
        error_ii = TRUE;
        fprintf(stderr, "net_activate: SIOCSIFADDR failed at %d\n",__LINE__);
    }

    if (net_is_ptp_im)
        {
        sockaddr_ri.sin_addr = config.net.ptphost.ip;
        memcpy (&interface_ri.ifr_dstaddr, &sockaddr_ri, sizeof (sockaddr_ri));
        if (ioctl (socket_ii, SIOCSIFDSTADDR, &interface_ri) < 0)
        {
            error_ii = TRUE;
            fprintf(stderr, "net_activate: SIOCSIFDSTADDR failed at %d\n",__LINE__);
        }
        }
    else
        {
        sockaddr_ri.sin_addr = config.net.netmask.ip;
        memcpy (&interface_ri.ifr_netmask, &sockaddr_ri, sizeof (sockaddr_ri));
        if (ioctl (socket_ii, SIOCSIFNETMASK, &interface_ri) < 0)
            if (config.net.netmask.ip.s_addr)
            {
                error_ii = TRUE;
                fprintf(stderr, "net_activate: SIOCSIFNETMASK failed at %d\n",__LINE__);
            }

        sockaddr_ri.sin_addr = config.net.broadcast.ip;
        memcpy (&interface_ri.ifr_broadaddr, &sockaddr_ri, sizeof (sockaddr_ri));
        if (ioctl (socket_ii, SIOCSIFBRDADDR, &interface_ri) < 0)
            if (config.net.broadcast.ip.s_addr != 0xffffffff)
            {
                error_ii = TRUE;
                fprintf(stderr, "net_activate: SIOCSIFBRDADDR failed at %d\n",__LINE__);
            }
        }

    if (ioctl (socket_ii, SIOCGIFFLAGS, &interface_ri) < 0)
    {
        error_ii = TRUE;
        fprintf(stderr, "net_activate: SIOCGIFFLAGS failed at %d\n",__LINE__);
    }

    interface_ri.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (net_is_ptp_im)
        interface_ri.ifr_flags |= IFF_POINTOPOINT | IFF_NOARP;
    else
        interface_ri.ifr_flags |= IFF_BROADCAST;
    if (ioctl (socket_ii, SIOCSIFFLAGS, &interface_ri) < 0)
    {
        error_ii = TRUE;
        fprintf(stderr, "net_activate: SIOCSIFFLAGS failed at %d\n",__LINE__);
    }

    memset (&route_ri, 0, sizeof (struct rtentry));
    route_ri.rt_dev = config.net.device;

    if (net_is_ptp_im)
        {
        sockaddr_ri.sin_addr = config.net.ptphost.ip;
        memcpy (&route_ri.rt_dst, &sockaddr_ri, sizeof (sockaddr_ri));
        route_ri.rt_flags = RTF_UP | RTF_HOST;
        if (ioctl (socket_ii, SIOCADDRT, &route_ri) < 0)
        {
            error_ii = TRUE;
            fprintf(stderr, "net_activate: SIOCADDRT failed at %d\n",__LINE__);
        }

        memset (&route_ri.rt_dst, 0, sizeof (route_ri.rt_dst));
        route_ri.rt_dst.sa_family = AF_INET;
        memcpy (&route_ri.rt_gateway, &sockaddr_ri, sizeof (sockaddr_ri));
        route_ri.rt_flags = RTF_UP | RTF_GATEWAY;
        if (ioctl (socket_ii, SIOCADDRT, &route_ri) < 0)
        {
            error_ii = TRUE;
            fprintf(stderr, "net_activate: SIOCADDRT failed at %d\n",__LINE__);
        }
        }
    else
        {
        sockaddr_ri.sin_addr = config.net.network.ip;
        memcpy (&route_ri.rt_dst, &sockaddr_ri, sizeof (sockaddr_ri));
        route_ri.rt_flags = RTF_UP;
        ioctl (socket_ii, SIOCADDRT, &route_ri);

        if (
          config.net.gateway.ip.s_addr &&
          config.net.gateway.ip.s_addr != config.net.hostname.ip.s_addr
        )
            {
            sockaddr_ri.sin_addr = config.net.gateway.ip;
            memset (&route_ri.rt_dst, 0, sizeof (route_ri.rt_dst));
            route_ri.rt_dst.sa_family = AF_INET;
            memcpy (&route_ri.rt_gateway, &sockaddr_ri, sizeof (sockaddr_ri));

/*            route_ri.rt_flags = RTF_UP | RTF_HOST;
            if (ioctl (socket_ii, SIOCADDRT, &route_ri) < 0)
                {
                error_ii = TRUE;
                } */

            route_ri.rt_flags = RTF_UP | RTF_GATEWAY;
            if (ioctl (socket_ii, SIOCADDRT, &route_ri) < 0)
            {
                error_ii = TRUE;
                fprintf(stderr, "net_activate: SIOCADDRT failed at %d\n",__LINE__);
            }
            }
        }

  close(socket_ii);

  if(!error_ii) {
    config.net.is_configured = 1;
    if(config.net.ifup_wait) sleep(config.net.ifup_wait);
  }

  return error_ii;
}


int net_check_address (char *input_tv, struct in_addr *address_prr, int *net_bits)
    {
    unsigned char  tmp_ti [32];
    unsigned char *start_pci;
    unsigned char *end_pci;
    int            i_ii;
    unsigned char *address_pci;
    unsigned u;


    address_pci = (unsigned char *) address_prr;

    strncpy (tmp_ti, input_tv, sizeof (tmp_ti));
    tmp_ti [sizeof (tmp_ti) - 1] = 0;

    *net_bits = -1;
    if((start_pci = strrchr(tmp_ti, '/'))) {
      *start_pci++ = 0;
      i_ii = strtol(start_pci, (void *) &end_pci, 10);
      if(start_pci != end_pci && !*end_pci && i_ii > 0 && i_ii <= 32) {
        *net_bits = i_ii;
      }
      else {
        return -1;
      }
    }

    for (i_ii = 0; i_ii < (int) strlen (tmp_ti); i_ii++)
        if (!isdigit (tmp_ti [i_ii]) && tmp_ti [i_ii] != '.')
            return (-1);

    if (!isdigit (tmp_ti [strlen (tmp_ti) - 1]))
        return (-1);

    start_pci = tmp_ti;
    end_pci = strchr (tmp_ti, '.');
    if (!end_pci)
        return (-1);
    *end_pci = 0;
    u = atoi(start_pci);
    if(u > 255) return -1;
    address_pci[0] = u;

    start_pci = end_pci + 1;
    end_pci = strchr (start_pci, '.');
    if (!end_pci)
        return (-1);
    *end_pci = 0;
    u = atoi(start_pci);
    if(u > 255) return -1;
    address_pci[1] = u;

    start_pci = end_pci + 1;
    end_pci = strchr (start_pci, '.');
    if (!end_pci)
        return (-1);
    *end_pci = 0;
    u = atoi(start_pci);
    if(u > 255) return -1;
    address_pci[2] = u;
    u = atoi(end_pci + 1);
    if(u > 255) return -1;
    address_pci[3] = u;

    return (0);
    }


int net_check_address2(inet_t *inet, int do_dns)
{
  struct hostent *he = NULL;
  struct in_addr iaddr;
  slist_t *sl;
  int net_bits;
  uint32_t u32;

  if(!inet) return -1;

  if(inet->ok) return 0;

  if(!inet->name || !*inet->name) return -1;

  if(!net_check_address(inet->name, &iaddr, &net_bits)) {
    inet->ok = 1;
    inet->ip = iaddr;

    if(net_bits >= 0) {
      u32 = -(1 << (32 - net_bits));
      inet->net.s_addr = htonl(u32);
    }
  
    str_copy(&inet->name, inet_ntoa(inet->ip));

    return 0;
  }

  for(sl = config.net.dns_cache; sl; sl = sl->next) {
    if(sl->key && sl->value && !strcasecmp(sl->key, inet->name)) {
      if(!net_check_address(sl->value, &iaddr, &net_bits)) {
        inet->ok = 1;
        inet->ip = iaddr;
        return 0;
      }
    }
  }

  if(
    !do_dns ||
    (
      !config.net.dhcp_active &&
      !config.net.nameserver[0].ok &&
      !config.test &&
      config.run_as_linuxrc)
  ) {
    return -1;
  }

  he = gethostbyname(inet->name);

  if(!he) {
    if(config.run_as_linuxrc) {
      fprintf(stderr, "dns: what is \"%s\"?\n", inet->name);
    }
    return -1;
  }

  inet->ok = 1;
  inet->ip = *((struct in_addr *) *he->h_addr_list);

  sl = slist_add(&config.net.dns_cache, slist_new());
  str_copy(&sl->key, inet->name);
  str_copy(&sl->value, inet_ntoa(inet->ip));

  if(config.run_as_linuxrc) {
    fprintf(stderr, "dns: %s is %s\n", inet->name, inet_ntoa(inet->ip));
  }

  return 0;
}


/*
 * Build mount option suitable for muont.cifs.
 */
void net_smb_get_mount_options(char *options, inet_t *server, char *user, char *password, char *workgroup)
{
  if(!options) return;
  *options = 0;

  if(!server) return;
  sprintf(options,"ip=%s", inet_ntoa(server->ip));

  if(user) {
    strcat(options, ",username=");
    strcat(options, user);
    strcat(options, ",password=");
    strcat(options, password ?: "");
    if(workgroup) {
      strcat(options, ",workgroup=");
      strcat(options, workgroup);
    }
  } else {
     /*
      * mount.cifs needs a username, otherwise it takes LOGNAME from
      * environment. see bugzilla #20152
      */
    strcat(options, ",username=root,guest");
  }
}


/*
 * Mount windows share.
 * (Run mount.cifs.)
 *
 * Return:
 *      0: ok
 *   != 0: error code
 *
 * mountpoint: mount point
 * server: SMB server
 * share: share
 * user: user (NULL: guest)
 * password: password (NULL: no password)
 * workgroup: workgroup (NULL: no workgroup)
 *
 */

/*
 * depending on guest login
 *   options += "guest"
 * resp.
 *   options += "username=" + USERNAME + ",password=" + PASSWORD
 *
 *   device = "//" + SERVER + "/" + SHARE
 *   options += ",workgroup=" + WORKGROUP   falls WORKGROUP gesetzt ist
 *   options += ",ip=" + SERVER_IP          falls SERVER_IP gesetzt ist
 * "  mount -t smbfs" + device + " " + mountpoint + " " + options
 */
int net_mount_smb(char *mountpoint, inet_t *server, char *share, char *user, char *password, char *workgroup)
{
  char tmp[1024];
  char mount_options[256];

  if(!config.net.cifs.binary) return -2;

  if(net_check_address2(server, 1)) return -3;

  if(!share) share = "";
  if(!mountpoint || !*mountpoint) mountpoint = "/";

  net_smb_get_mount_options(mount_options, server, user, password, workgroup);

  sprintf(tmp,
    "%s //%s/%s %s -o ro,%s >&2",
    config.net.cifs.binary, server->name, share, mountpoint, mount_options
  );

  mod_modprobe(config.net.cifs.module, NULL);

  fprintf(stderr, "%s\n", tmp);

  if(system(tmp)) return -1;

  return 0;
}


#ifdef WITH_NFS
int xdr_dirpath (XDR *xdrs, dirpath *objp)
    {
    if (!xdr_string(xdrs, objp, MNTPATHLEN))
        return (FALSE);
    else
        return (TRUE);
    }


int xdr_fhandle (XDR *xdrs, fhandle objp)
    {
    if (!xdr_opaque(xdrs, objp, FHSIZE))
        return (FALSE);
    else
        return (TRUE);
    }


int xdr_fhstatus (XDR *xdrs, fhstatus *objp)
    {
    if (!xdr_u_int(xdrs, &objp->fhs_status))
        return (FALSE);

    if (!objp->fhs_status)
        if (!xdr_fhandle(xdrs, objp->fhstatus_u.fhs_fhandle))
            return (FALSE);

    return (TRUE);
    }


/*
 * Mount NFS volume.
 *
 * Return:
 *      0: ok
 *   != 0: error code
 *
 * mountpoint: mount point
 * server: server address
 * hostdir: directory on server
 *
 * config.net.nfs_rsize: read size
 * config.net.nfs_wsize: write size
 * config.net.nfs_port: NFS port
 */
int net_mount_nfs(char *mountpoint, inet_t *server, char *hostdir)
{
    struct sockaddr_in     server_ri;
    struct sockaddr_in     mount_server_ri;
    struct nfs_mount_data  mount_data_ri;
    int                    socket_ii;
    int                    fsock_ii;
    int                    port_ii;
    CLIENT                *mount_client_pri;
    struct timeval         timeout_ri;
    int                    rc_ii;
    struct fhstatus        status_ri;
    char                   tmp_ti [1024];
    char                  *opts_pci;

  if(net_check_address2(server, 1)) return -2;

  if(!hostdir) hostdir = "/";
  if(!mountpoint || !*mountpoint) mountpoint = "/";

    memset (&server_ri, 0, sizeof (struct sockaddr_in));
    server_ri.sin_family = AF_INET;
    server_ri.sin_addr.s_addr = server->ip.s_addr;
    memcpy (&mount_server_ri, &server_ri, sizeof (struct sockaddr_in));

    memset (&mount_data_ri, 0, sizeof (struct nfs_mount_data));
//    mount_data_ri.flags = NFS_MOUNT_NONLM;
    if(config.net.nfs_tcp) {
      mount_data_ri.flags = NFS_MOUNT_TCP | NFS_MOUNT_VER3 | NFS_MOUNT_NONLM;
    }
    mount_data_ri.rsize = config.net.nfs_rsize;
    mount_data_ri.wsize = config.net.nfs_wsize;
    mount_data_ri.timeo = 70;
    mount_data_ri.retrans = 3;
    mount_data_ri.acregmin = 3;
    mount_data_ri.acregmax = 60;
    mount_data_ri.acdirmin = 30;
    mount_data_ri.acdirmax = 60;
    mount_data_ri.namlen = NAME_MAX;
    mount_data_ri.version = NFS_MOUNT_VERSION;

    mount_server_ri.sin_port = htons (0);
    socket_ii = RPC_ANYSOCK;
#if 0
    mount_client_pri = clnttcp_create (&mount_server_ri, MOUNTPROG, MOUNTVERS,
                                       &socket_ii, 0, 0);
#else
    mount_client_pri = NULL;
#endif

    if (!mount_client_pri)
        {
	mount_data_ri.timeo = 7;
        mount_server_ri.sin_port = htons (0);
        socket_ii = RPC_ANYSOCK;
        timeout_ri.tv_sec = 3;
        timeout_ri.tv_usec = 0;
        mount_client_pri = clntudp_create (&mount_server_ri, MOUNTPROG,
                                           MOUNTVERS, timeout_ri, &socket_ii);
        }

    if (!mount_client_pri)
        {
        sleep(2);

	mount_data_ri.timeo = 7;
        mount_server_ri.sin_port = htons (0);
        socket_ii = RPC_ANYSOCK;
        timeout_ri.tv_sec = 3;
        timeout_ri.tv_usec = 0;
        mount_client_pri = clntudp_create (&mount_server_ri, MOUNTPROG,
                                           MOUNTVERS, timeout_ri, &socket_ii);
        if (!mount_client_pri)
            {
            net_show_error ((enum nfs_stat) -1);
            return (-1);
            }
        }

    mount_client_pri->cl_auth = authunix_create_default ();
    timeout_ri.tv_sec = 20;
    timeout_ri.tv_usec = 0;

    rc_ii = clnt_call (mount_client_pri, MOUNTPROC_MNT,
                       (xdrproc_t) xdr_dirpath, (caddr_t) &hostdir,
                       (xdrproc_t) xdr_fhstatus, (caddr_t) &status_ri,
                       timeout_ri);
    if (rc_ii)
        {
        net_show_error ((enum nfs_stat) -1);
        return (-1);
        }

    if (status_ri.fhs_status)
        {
        net_show_error (status_ri.fhs_status);
        return (-1);
        }

    memcpy ((char *) &mount_data_ri.root.data,
            (char *) status_ri.fhstatus_u.fhs_fhandle,
            NFS_FHSIZE);
    mount_data_ri.root.size = NFS_FHSIZE;
    memcpy(&mount_data_ri.old_root.data,
	   (char *) status_ri.fhstatus_u.fhs_fhandle,
	   NFS_FHSIZE);

    fsock_ii = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fsock_ii < 0)
        {
        net_show_error ((enum nfs_stat) -1);
        return (-1);
        }

    if (bindresvport (fsock_ii, 0) < 0)
        {
        net_show_error ((enum nfs_stat) -1);
        return (-1);
        }

    if (config.net.nfs_port)
        {
        fprintf (stderr, "Using specified port %d\n", config.net.nfs_port);
        port_ii = config.net.nfs_port;
        }
    else
        {
        server_ri.sin_port = PMAPPORT;
        port_ii = pmap_getport (&server_ri, NFS_PROGRAM, NFS_VERSION, IPPROTO_UDP);
        if (!port_ii)
            port_ii = NFS_PORT;
        }

    server_ri.sin_port = htons (port_ii);

    mount_data_ri.fd = fsock_ii;
    memcpy ((char *) &mount_data_ri.addr, (char *) &server_ri,
            sizeof (mount_data_ri.addr));
    strncpy (mount_data_ri.hostname, inet_ntoa(server->ip),
             sizeof (mount_data_ri.hostname));

    auth_destroy (mount_client_pri->cl_auth);
    clnt_destroy (mount_client_pri);
    close (socket_ii);

    sprintf (tmp_ti, "%s:%s", inet_ntoa(server->ip), hostdir);
    opts_pci = (char *) &mount_data_ri;
    rc_ii = mount (tmp_ti, mountpoint, "nfs", MS_RDONLY | MS_MGC_VAL, opts_pci);

    if(rc_ii == -1) return errno;

  return rc_ii;
}

#else


/*
 * Dummy if we don't support NFS.
 */
int net_mount_nfs(char *mountpoint, inet_t *server, char *hostdir);
{
  return -1;
}

#endif	/* WITH_NFS */


#if NETWORK_CONFIG
/*
 * Let user select a network interface.
 *
 * Does nothing if network interface has been explicitly specified.
 * No user interaction if there is exactly one interface.
 * Shows error message if there is no interface.
 *
 * Note: expects window mode.
 *
 * Global vars changed:
 *  config.net.device
 *
 * config.net.device_given: do nothing if != 0
 */
int net_choose_device()
{
  char **items, **item_devs = NULL;
  int i, max_items = 0, item_cnt, choice, width;
  char *buf = NULL;
  file_t *f0, *f;
  slist_t *sl;
  static int last_item = 0;
  static struct {
    char *dev;
    int name;
  } net_dev[] = {
    { "eth",   TXT_NET_ETH0  },
    { "veth",  TXT_NET_ETH0  },
    { "plip",  TXT_NET_PLIP  },
    { "tr",    TXT_NET_TR0   },
    { "arc",   TXT_NET_ARC0  },
    { "fddi",  TXT_NET_FDDI  },
    { "hip",   TXT_NET_HIPPI },
    { "ctc",   TXT_NET_CTC   },
    { "escon", TXT_NET_ESCON },
    { "ci",    TXT_NET_CLAW  },
    { "iucv",  TXT_NET_IUCV  },
    { "hsi",   TXT_NET_HSI   }
  };
  hd_data_t *hd_data = calloc(1, sizeof *hd_data);
  hd_t *hd, *hd_cards;
  hd_t **item_hds = NULL;
    
  if(config.net.device_given) return 0;

  if(config.manual >= 2) {
#if defined(__s390__) || defined(__s390x__)
    /* bring up network devices, write hwcfg */
    while(net_activate_s390_devs()) {}
#endif

    /* re-read - just in case... */
    util_update_netdevice_list(NULL, 1);

    for(sl = config.net.devices; sl; sl = sl->next) {
      if(sl->key) max_items++;
    }

    items = calloc(max_items + 1, sizeof *items);
    item_devs = calloc(max_items + 1, sizeof *item_devs);

    f0 = file_read_file("/proc/net/dev", kf_none);
    if(!f0) return -1;

    for(item_cnt = 0, f = f0; f && item_cnt < max_items; f = f->next) {
      for(i = 0; i < sizeof net_dev / sizeof *net_dev; i++) {
        if(strstr(f->key_str, net_dev[i].dev) == f->key_str) {
          strprintf(&buf, "%-6s : %s", f->key_str, txt_get(net_dev[i].name));
          item_devs[item_cnt] = strdup(f->key_str);
          items[item_cnt++] = strdup(buf);
          break;
        }
      }
    }

    file_free_file(f0);
  }
  else {
    hd_data->flags.nowpa = 1;

    hd_cards = hd_list(hd_data, hw_network_ctrl, 1, NULL);
    for(hd = hd_cards; hd; hd = hd->next) max_items++;

    items = calloc(max_items + 1, sizeof *items);
    item_devs = calloc(max_items + 1, sizeof *item_devs);
    item_hds = calloc(max_items + 1, sizeof *item_hds);

    for(width = 0, hd = hd_cards; hd; hd = hd->next) {
      if(hd->unix_dev_name) {
        i = strlen(hd->unix_dev_name);
        if(i > width) width = i;
      }
    }

    for(item_cnt = 0, hd = hd_cards; hd; hd = hd->next) {
      item_hds[item_cnt] = hd;
      if(hd->unix_dev_name) {
        item_devs[item_cnt] = strdup(hd->unix_dev_name);
      }
#if defined(__s390__) || defined(__s390x__)
#define MAX_NET_DEVICES_SHOWN 20
      if(item_cnt > MAX_NET_DEVICES_SHOWN) {
        item_hds[item_cnt] = NULL;
        strprintf(items + item_cnt++, txt_get(TXT_MANUAL_NETDEV_PARAMS));
        break;
      } else {
        int lcss = -1;
        int ccw = -1;
        hd_res_t* r;
        char* annotation = 0;
        
        if(hd->detail && hd->detail->ccw.data)
          lcss = hd->detail->ccw.data->lcss;
        
        for(r = hd->res; r; r = r->next) {
          if(r->any.type == res_io) {
            ccw = (int) r->io.base;
          }
        }
        if ( ccw == -1 ) {
          /* IUCV device */
          if(hd->rom_id) strprintf(&annotation, "(%s)", hd->rom_id);
          else strprintf(&annotation, "");
        }
        else {
          strprintf(&annotation, "(%1x.%1x.%04x)", lcss >> 8, lcss & 0xf, ccw);
        }
        
        if(hd->unix_dev_name) {
          strprintf(items + item_cnt++, "%*s : %s %s", -width, hd->unix_dev_name, hd->model,
            annotation);
        }
        else {
          strprintf(items + item_cnt++, "%s %s", hd->model, annotation);
        }
        free(annotation);
      }
#else
      if(hd->unix_dev_name) {
        strprintf(items + item_cnt++, "%*s : %s", -width, hd->unix_dev_name, hd->model);
      }
      else {
        strprintf(items + item_cnt++, "%s", hd->model);
      }
#endif
    }
  }

  if(item_cnt == 0) {
    dia_message(txt_get(TXT_NO_NETDEVICE), MSGTYPE_ERROR);
    choice = -1;
  } else if(item_cnt == 1) {
    choice = 1;
  }
  else {
    choice = dia_list(txt_get(TXT_CHOOSE_NET), 50, NULL, items, last_item, align_left);
    if(choice) last_item = choice;
  }

  if(choice > 0 && !item_devs[choice - 1]) {
#if defined(__s390__) || defined(__s390x__)
    net_activate_s390_devs_ex(item_hds[choice - 1], &item_devs[choice - 1]);
    if(!item_devs[choice - 1]) {
#endif
      dia_message(txt_get(TXT_NO_NETDEVICE), MSGTYPE_ERROR);
      choice = -1;
#if defined(__s390__) || defined(__s390x__)
    }
#endif
  }

  if(choice > 0) {
    str_copy(&config.net.device, item_devs[choice - 1]);
    net_is_ptp_im = FALSE;
    if(strstr(config.net.device, "plip") == config.net.device) net_is_ptp_im = TRUE;
    if(strstr(config.net.device, "iucv") == config.net.device) net_is_ptp_im = TRUE;
    if(strstr(config.net.device, "ctc") == config.net.device) net_is_ptp_im = TRUE;

    if(item_hds && item_hds[choice - 1]) {
      hd = item_hds[choice - 1];
      if(hd->is.wlan) {
        if(wlan_setup()) choice = -1;
      }
    }
  }

  for(i = 0; i < item_cnt; i++) {
    free(items[i]);
    if(item_devs) free(item_devs[i]);
  }
  free(items);
  free(item_devs);
  free(item_hds);

  hd_free_hd_data(hd_data);
  free(hd_data);

  free(buf);

  return choice > 0 ? 0 : -1;
}
#endif


#ifdef WITH_NFS
/*
 * Show NFS error messages.
 *
 * Helper for net_mount_nfs().
 *
 * nfs_stat: NFS status
 */
static void net_show_error(enum nfs_stat status_rv)
{
  int i;
  char *s, tmp[1024], tmp2[64];

  struct {
    enum nfs_stat stat;
    int errnumber;
  } nfs_err[] = {
    { NFS_OK,                 0               },
    { NFSERR_PERM,            EPERM           },
    { NFSERR_NOENT,           ENOENT          },
    { NFSERR_IO,              EIO             },
    { NFSERR_NXIO,            ENXIO           },
    { NFSERR_ACCES,           EACCES          },
    { NFSERR_EXIST,           EEXIST          },
    { NFSERR_NODEV,           ENODEV          },
    { NFSERR_NOTDIR,          ENOTDIR         },
    { NFSERR_ISDIR,           EISDIR          },
    { NFSERR_INVAL,           EINVAL          },
    { NFSERR_FBIG,            EFBIG           },
    { NFSERR_NOSPC,           ENOSPC          },
    { NFSERR_ROFS,            EROFS           },
    { NFSERR_NAMETOOLONG,     ENAMETOOLONG    },
    { NFSERR_NOTEMPTY,        ENOTEMPTY       },
    { NFSERR_DQUOT,           EDQUOT          },
    { NFSERR_STALE,           ESTALE          }
  };

  s = NULL;

  for(i = 0; (unsigned) i < sizeof nfs_err / sizeof *nfs_err; i++) {
    if(nfs_err[i].stat == status_rv) {
      s = strerror(nfs_err[i].errnumber);
      break;
    }
  }

  if(!s) {
    sprintf(tmp2, "unknown error %d\n", status_rv);
    s = tmp2;
  }

  sprintf(tmp,
    config.win ? txt_get(TXT_ERROR_NFSMOUNT) : "mount: nfs mount failed, server says: %s\n",
    s
  );

  if(config.win) {
    dia_message(tmp, MSGTYPE_ERROR);
  }
  else {
    fprintf(stderr, "%s\n", tmp);
  }
}
#endif


/*
 * Let user enter nameservers.
 *
 * Asks for name servers if window mode is active.
 *
 * Writes nameserver & domain to /etc/resolv.conf.
 *
 * Global vars changed:
 *  config.net.nameserver
 */
void net_setup_nameserver()
{
  char *s, buf[256];
  FILE *f;
  unsigned u;

  if(config.win && !config.net.dhcp_active) {

    if(!config.net.nameserver[0].name && config.net.hostname.ok) {
      s = inet_ntoa(config.net.hostname.ip);
      config.net.nameserver[0].name = strdup(s ?: "");
    }

    if((config.net.setup & NS_NAMESERVER)) {
      for(u = 0; u < config.net.nameservers; u++) {
        if(config.net.nameservers == 1) {
          s = txt_get(TXT_INPUT_NAMED);
        }
        else {
           sprintf(buf, txt_get(TXT_INPUT_NAMED1), u + 1);
           s = buf;
        }
        if(net_get_address(s, &config.net.nameserver[u], 0)) break;
      }
      for(; u < config.net.nameservers; u++) {
        str_copy(&config.net.nameserver[u].name, NULL);
        config.net.nameserver[u].ok = 0;
      }
    }

  }

  if(config.test) return;

  if((f = fopen("/etc/resolv.conf", "w"))) {
    for(u = 0; u < config.net.nameservers; u++) {
      if(config.net.nameserver[u].ok) {
        fprintf(f, "nameserver %s\n", config.net.nameserver[u].name);
      }
    }
    if(config.net.domain) {
      fprintf(f, "search %s\n", config.net.domain);
    }
    fclose(f);
  }
}


#if NETWORK_CONFIG
/*
 * Let user enter some network config data.
 *
 * Note: expects window mode.
 *
 * Global vars changed:
 *  config.net.hostname
 *  config.net.netmask
 *  config.net.ptphost
 *  config.net.gateway
 *  config.net.server
 *  config.net.broadcast
 *  config.net.network
 *  config.net.gateway
 */
int net_input_data()
{

  if((config.net.setup & NS_HOSTIP)) {
    if(net_get_address(txt_get(TXT_INPUT_IPADDR), &config.net.hostname, 1)) return -1;
  }

  if(config.net.hostname.ok && config.net.hostname.net.s_addr) {
    s_addr2inet(&config.net.netmask, config.net.hostname.net.s_addr);
  }

  if(net_is_ptp_im) {
    if(!config.net.ptphost.name) {
      name2inet(&config.net.ptphost, config.net.hostname.name);
    }

    if(net_get_address(txt_get(TXT_INPUT_PLIP_IP), &config.net.ptphost, 1)) return -1;

    if(!config.net.gateway.name) {
      name2inet(&config.net.gateway, config.net.ptphost.name);
    }

    if(!config.net.server.name) {
      name2inet(&config.net.server, config.net.ptphost.name);
    }
  }
  else {
    name2inet(&config.net.ptphost, "");

    if(!config.net.gateway.name) {
      name2inet(&config.net.gateway, config.net.hostname.name);
    }

    if(!config.net.server.name) {
      name2inet(&config.net.server, config.net.hostname.name);
    }

    if(!config.net.netmask.ok) {
      char *s = inet_ntoa(config.net.hostname.ip);

      name2inet(
        &config.net.netmask,
        strstr(s, "10.10.") == s ? "255.255.0.0" : "255.255.255.0"
      );
    }
    if((config.net.setup & NS_NETMASK)) {
      if(net_get_address(txt_get(TXT_INPUT_NETMASK), &config.net.netmask, 0)) return -1;
    }

    s_addr2inet(
      &config.net.broadcast,
      config.net.hostname.ip.s_addr | ~config.net.netmask.ip.s_addr
    );

    s_addr2inet(
      &config.net.network,
      config.net.hostname.ip.s_addr & config.net.netmask.ip.s_addr
    );

    if((config.net.setup & NS_GATEWAY)) {
      net_get_address(txt_get(TXT_INPUT_GATEWAY), &config.net.gateway, 1);
    }
  }

  return 0;
}
#endif


/*
 * Use bootp to get network config data.
 *
 * Does nothing if we already got a hostip address (in config.net.hostname).
 *
 * Note: shuts all interfaces down.
 *
 * Global vars changed:
 *  config.net.hostname
 *  config.net.netmask
 *  config.net.network
 *  config.net.broadcast
 *  config.net.ptphost
 *  config.net.gateway
 *  config.net.nameserver
 *  config.net.domain
 *  config.net.server
 *  config.serverdir
 * 
 * config.net.bootp_wait: delay between interface setup & bootp request
 * config.net.device: interface
 */
int net_bootp()
{
  window_t  win;
  int rc, netconf_error;
  char *s;
  char tmp[256];

  if(config.net.hostname.ok || config.net.keep) return 0;

  if(config.test) return 0;

  name2inet(&config.net.netmask, "");
  name2inet(&config.net.network, "");
  s_addr2inet(&config.net.broadcast, 0xffffffff);
  name2inet(&config.net.ptphost, "");
  name2inet(&config.net.hostname, "");
  netconf_error	= 0;

  if(net_activate_ns()) {
    if(config.win) {
      dia_message(txt_get(TXT_ERROR_CONF_NET), MSGTYPE_ERROR);
    }
    else {
      fprintf(stderr, "network setup failed\n");
    }
            
    return -1;
  }

  if(config.win) {
    sprintf(tmp, txt_get(TXT_SEND_DHCP), "BOOTP");
    dia_info(&win, tmp);
  }

  if(config.net.bootp_wait) sleep(config.net.bootp_wait);

  rc = performBootp(
    config.net.device, "255.255.255.255", "",
    config.net.bootp_timeout, 0, NULL, 0, 1, BP_PUT_ENV, 1
  );

  win_close(&win);

  if(rc || !getenv("BOOTP_IPADDR")) {
    if(config.win) {
      sprintf(tmp, txt_get(TXT_ERROR_DHCP), "BOOTP");
      dia_message(tmp, MSGTYPE_ERROR);
    }
    return -1;
  }

  name2inet(&config.net.hostname, getenv("BOOTP_IPADDR"));
  net_check_address2(&config.net.hostname, 0);

  name2inet(&config.net.netmask, getenv("BOOTP_NETMASK"));
  net_check_address2(&config.net.netmask, 0);

  name2inet(&config.net.broadcast, getenv("BOOTP_BROADCAST"));
  net_check_address2(&config.net.broadcast, 0);

  name2inet(&config.net.network, getenv("BOOTP_NETWORK"));
  net_check_address2(&config.net.network, 0);

  name2inet(&config.net.gateway, getenv("BOOTP_GATEWAYS"));
  name2inet(&config.net.gateway, getenv("BOOTP_GATEWAYS_1"));
  net_check_address2(&config.net.gateway, 0);

  name2inet(&config.net.nameserver[0], getenv("BOOTP_DNSSRVS"));
  name2inet(&config.net.nameserver[0], getenv("BOOTP_DNSSRVS_1"));
  net_check_address2(&config.net.nameserver[0], 0);

  s = getenv("BOOTP_HOSTNAME");
  if(s && !config.net.hostname.name) config.net.hostname.name = strdup(s);

  if((s = getenv("BOOTP_DOMAIN"))) {
    if(config.net.domain) free(config.net.domain);
    config.net.domain = strdup(s);
  }

#if 0
  s = getenv("BOOTP_ROOT_PATH");
  if(!s) s = getenv("BOOTP_BOOTFILE");

  if(s && *s) {
    s = strdup(s);

    fprintf(stderr, "bootp root: \"%s\"\n", s);

    if((t = strchr(s, ':'))) {
      *t++ = 0;
    }
    else {
      t = s;
    }

    if(*t && !config.serverdir) config.serverdir = strdup(t);

    if(t != s && !config.net.server.name) {
      name2inet(&config.net.server, s);
      net_check_address2(&config.net.server, 0);
    }

    free(s);
  }
#endif

  if(!config.net.server.name) {
    name2inet(&config.net.server, getenv("BOOTP_SERVER"));
    net_check_address2(&config.net.server, 0);
  }

  net_stop();

  return 0;
}


/*
 * Ask user for some network address.
 * (Used for netmasks, too.)
 *
 * Either numeric or dns resolved.
 *
 * Return:
 *      0: ok
 *   != 0: error or abort
 *
 * Note: expects window mode.
 */
int net_get_address(char *text, inet_t *inet, int do_dns)
{
  int rc;
  char input[256];

  *input = 0;
  if(inet->name) strcpy(input, inet->name);

  do {
    if((rc = dia_input(text, input, sizeof input - 1, 16, 0))) return rc;
    name2inet(inet, input);
    rc = net_check_address2(inet, do_dns);
    if(rc) dia_message(txt_get(TXT_INVALID_INPUT), MSGTYPE_ERROR);
  } while(rc);

  return 0;
}


/*
 * Start dhcp client and reads dhcp info.
 *
 * Global vars changed:
 *  config.net.dhcp_active
 *  config.net.hostname
 *  config.net.netmask
 *  config.net.network
 *  config.net.broadcast
 *  config.net.gateway
 *  config.net.domain
 *  config.net.nameserver
 *  config.net.server
 *  config.serverdir
 */
int net_dhcp()
{
  char cmd[256], file[256], *s;
  file_t *f0, *f;
  window_t win;
  slist_t *sl0, *sl;

  if(config.net.dhcp_active || config.net.keep) return 0;

  if(config.test) {
    config.net.dhcp_active = 1;

    return 0;
  }

  if(config.win) {
    sprintf(cmd, txt_get(TXT_SEND_DHCP), "DHCP");
    dia_info(&win, cmd);
  }

  net_apply_ethtool(config.net.device, config.net.hwaddr);

  strcpy(cmd, "dhcpcd");

  if(config.net.dhcpcd) {
    sprintf(cmd + strlen(cmd), " %s", config.net.dhcpcd);
  }

  if(config.net.dhcp_timeout != 60) {
    sprintf(cmd + strlen(cmd), " -t %d", config.net.dhcp_timeout);
  }

  sprintf(cmd + strlen(cmd), " %s", config.net.device);

  sprintf(file, "/var/lib/dhcpcd/dhcpcd-%s.info", config.net.device);

  unlink(file);

  system(cmd);

  f0 = file_read_file(file, kf_dhcp);

  for(f = f0; f; f = f->next) {
    switch(f->key) {
      case key_ipaddr:
        name2inet(&config.net.hostname, f->value);
        net_check_address2(&config.net.hostname, 0);
        break;

      case key_netmask:
        name2inet(&config.net.netmask, f->value);
        net_check_address2(&config.net.netmask, 0);
        break;

      case key_network:
        name2inet(&config.net.network, f->value);
        net_check_address2(&config.net.network, 0);
        break;

      case key_broadcast:
        name2inet(&config.net.broadcast, f->value);
        net_check_address2(&config.net.broadcast, 0);
        break;

      case key_gateway:
        if((s = strchr(f->value, ','))) *s = 0;
        name2inet(&config.net.gateway, f->value);
        net_check_address2(&config.net.gateway, 0);
        break;

      case key_domain:
        if(*f->value) {
          if(config.net.domain) free(config.net.domain);
          config.net.domain = strdup(f->value);
        }
        break;

      case key_dhcpsiaddr:
        if(!config.net.server.name) {
          name2inet(&config.net.server, f->value);
          net_check_address2(&config.net.server, 0);
        }
        break;

#if 0
      case key_rootpath:
      case key_bootfile:
        if(*f->value && !config.serverdir) {
          str_copy(&config.serverdir, f->value);
        }
        break;
#endif

      case key_dns:
        for(config.net.nameservers = 0, sl = sl0 = slist_split(',', f->value); sl; sl = sl->next) {
          name2inet(&config.net.nameserver[config.net.nameservers], sl->key);
          net_check_address2(&config.net.nameserver[config.net.nameservers], 0);
          if(++config.net.nameservers >= sizeof config.net.nameserver / sizeof *config.net.nameserver) break;
        }
        slist_free(sl0);
        break;

      default:
        break;
    }
  }

  if(config.win) win_close(&win);

  if(f0) {
    config.net.dhcp_active = 1;
  }
  else {
    if(config.win) {
      sprintf(cmd, txt_get(TXT_ERROR_DHCP), "DHCP");
      dia_message(cmd, MSGTYPE_ERROR);
    }
  }

  file_free_file(f0);

  return config.net.dhcp_active ? 0 : 1;
}


/*
 * Stop dhcp client.
 *
 * Global vars changed:
 *  config.net.dhcp_active
 */
void net_dhcp_stop()
{
  if(!config.net.dhcp_active) return;

//  system("dhcpcd -k");
  /* kill them all */
  util_killall("dhcpcd", SIGHUP);
  /* give them some time */
  sleep(2);

  config.net.dhcp_active = 0;
}


/*
 * Return current network config state as bitmask.
 */
unsigned net_config_mask()
{
  unsigned u = 0;

  if(config.net.hostname.name) u |= 1;
  if(config.net.netmask.ok) u |= 2;
  if(config.net.gateway.ok) u |= 4;
  if(config.net.server.name) u |= 8;
  if(config.net.nameserver[0].ok) u |= 0x10;
  if(config.serverdir) u |= 0x20;

  return u;
}


/*
 * Return module that handles a network interface.
 *
 * Returns NULL if unknown.
 *
 * net_if: interface
 */
char *net_if2module(char *net_if)
{
  slist_t *sl;

  for(sl = config.net.devices; sl; sl = sl->next ) {
    if(sl->key && !strcmp(sl->key, net_if)) return sl->value;
  }

  return NULL;
}


/*
 * Shut down single network interface.
 *
 * dev: interface
 */
void if_down(char *dev)
{
  int sock;
  struct ifreq iface = {};

  if(!dev || !*dev) return;

  fprintf(stderr, "if %s down\n", dev);

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if(sock == -1) return;

  strcpy(iface.ifr_name, dev);
  iface.ifr_addr.sa_family = AF_INET;

  ioctl(sock, SIOCGIFFLAGS, &iface);
  iface.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
  ioctl(sock, SIOCSIFFLAGS, &iface);
  close(sock);
}

#if defined(__s390__) || defined(__s390x__)

/* switch to manual mode if expr is false, execute following statement if in manual mode */
#define IFNOTAUTO(expr) if(!config.manual && !(expr)) { \
                          config.manual=1; \
                          if(!config.win) util_disp_init(); \
                        } \
                        if(config.manual)

#include <dirent.h>

void net_list_s390_devs(char* driver, int model)
{
#if 0	/* unnecessary with new interface */
  char buf[4<<10];	/* good enough for ca. 240 devices */
  char tmp[256];
  DIR* driv;
  struct dirent* devs;
  char* bp=&buf[0];
  int count = 0;

  strcpy(buf,"no devices found\n");

  if(!config.manual) return;
  
  driv=opendir("/sys/bus/ccw/devices");
  while((devs = readdir(driv)))
  {
    if(devs->d_type == DT_DIR) continue;
    sprintf(tmp,"/sys/bus/ccw/devices/%s/cutype", devs->d_name);
    util_read_and_chop(tmp, tmp);
    if(model && strtol(tmp+5,NULL,16)!=model) continue;
    bp+=sprintf(bp,"%s ",devs->d_name);
    bp+=sprintf(bp,"%s\n",tmp);	/* attribute contains a LF */
    count++;
    if(count % 100 == 0)	/* avoid buffer overruns with many devices */
    {
      dia_message(buf, MSGTYPE_INFO);
      bp = &buf[0];
      buf[0] = 0;
    }
  }
  closedir(driv);
  if(buf[0]) dia_message(buf,MSGTYPE_INFO);
#endif
}

int net_check_ccw_address(char* addr)
{
  int i;
  fprintf(stderr, "checking CCW address %s\n",addr);
  /* format: x.x.xxxx, each x is a hex digit */
  if(strlen(addr)!=8) goto error;
  for(i=0;i<8;i++)
  {
    if(i==1 || i==3)
    {
     if(addr[i] != '.') goto error;
    }
    else
    {
     addr[i] = tolower(addr[i]);
     if((addr[i] < 'a' || addr[i] > 'f') && (addr[i] < '0' || addr[i] > '9')) goto error;
    }
  }

  return 0;

error:
  if(!config.win) util_disp_init();
  dia_message(txt_get(TXT_INVALID_CCW_ADDRESS), MSGTYPE_ERROR);
  return -1;
}

/* set config variables common to CCW devices */
static void net_s390_set_config_ccwdev()
{
    config.hwp.scriptup="hwup-ccw";
    config.hwp.scriptup_ccw="hwup-ccw";
    config.hwp.scriptdown="hwdown-ccw";
}

/* ask user for read and write channels */
static int net_s390_getrwchans_ex(hd_t* hd)
{
  int rc;

  if(hd && config.hwp.readchan == 0) {
    int lcss = hd->detail->ccw.data->lcss;
    int ccw = -1;
    hd_res_t* r;
    for(r = hd->res; r; r = r->next) {
      if(r->any.type == res_io) {
        ccw = (int) r->io.base;
      }
    }
    if(ccw != -1) strprintf(&config.hwp.readchan, "%1x.%1x.%04x", lcss >> 8, lcss & 0xf, ccw);
  }

  IFNOTAUTO(config.hwp.readchan) if((rc=dia_input2_chopspace(txt_get(TXT_CTC_CHANNEL_READ), &config.hwp.readchan, 9, 0))) return rc;
  if((rc=net_check_ccw_address(config.hwp.readchan))) return rc;
  IFNOTAUTO(config.hwp.writechan) if((rc=dia_input2_chopspace(txt_get(TXT_CTC_CHANNEL_WRITE), &config.hwp.writechan, 9, 0))) return rc;
  if((rc=net_check_ccw_address(config.hwp.writechan))) return rc;
  return 0;
}

#if 0 /* currently unused */
static int net_s390_getrwchans()
{
  return net_s390_getrwchans_ex(NULL);
}
#endif

/* group CCW channels */
static int net_s390_group_chans(int num, char* driver)
{
  int rc;
  char devs[8*3+1*3+1];	/* three channel addresses, commas/LF, zero */
  char path[100];

  fprintf(stderr,"grouping %d channels for driver %s\n",num,driver);
  
  if(num==2)
    sprintf(devs,"%s,%s\n",config.hwp.readchan,config.hwp.writechan);
  else if(num==3)
    sprintf(devs,"%s,%s,%s\n",config.hwp.readchan,config.hwp.writechan,config.hwp.datachan);
  else
    return -1;
  
  sprintf(path,"/sys/bus/ccwgroup/drivers/%s/group",driver);
  if((rc=util_set_sysfs_attr(path,devs))) return rc;
  sprintf(path,"/sys/bus/ccwgroup/devices/%s", config.hwp.readchan);
  if((rc=hotplug_wait_for_path(path))) return rc;
  return 0;
}

/* set protocol */
static int net_s390_ctc_protocol(int protocol)
{
    char devpath[256];
    char proto_value[3];
    int rc;
    
    fprintf(stderr,"setting CTC protocol to %d\n",protocol);

    sprintf(devpath,"/sys/bus/ccwgroup/devices/%s/protocol",
	    config.hwp.readchan);
    sprintf(proto_value,"%d", protocol);
    rc=util_set_sysfs_attr(devpath,proto_value);

    return rc;
}

/* set port number */
static int net_s390_lcs_portno(char *portno)
{
    char devpath[256];
    int rc = 0;
    
    fprintf(stderr, "setting LCS port number to %s\n",portno);

    if (portno && strlen(portno) > 0) {
	sprintf(devpath,"/sys/bus/ccwgroup/devices/%s/portno",
		config.hwp.readchan);
        rc=util_set_sysfs_attr(devpath,portno);
    }

    return rc;
}

/* set portname */
static int net_s390_qdio_portname(char *portname)
{
    char devpath[256];
    int rc = 0;
    
    fprintf(stderr, "setting QDIO portname to %s\n",portname);

    if(portname && strlen(portname) > 0)
    {
	sprintf(devpath,"/sys/bus/ccwgroup/devices/%s/portname",
		config.hwp.readchan);
        rc=util_set_sysfs_attr(devpath,portname);
    }

    return rc;
}

/* enable layer2 support */
static int net_s390_enable_layer2(int enable)
{
    char devpath[256];
    char value[10];
    
    fprintf(stderr, "setting layer2 support to %d\n",enable);

    sprintf(devpath,"/sys/bus/ccwgroup/devices/%s/layer2",
            config.hwp.readchan);
    sprintf(value,"%d",enable);
    return util_set_sysfs_attr(devpath,value);
}

/* put device online */
static int net_s390_put_online(char* channel)
{
  int rc;
  char path[256];
  int online;
  
  fprintf(stderr, "putting device %s online\n",channel);

  if (!channel || !strlen(channel))
      return 1;
  sprintf(path,"/sys/bus/ccwgroup/devices/%s/online",channel);
  if((rc=util_set_sysfs_attr(path,"1"))) return rc;
  rc=util_get_sysfs_int_attr(path,&online);
  fprintf(stderr,"grmtob: %d/%d\n",rc,online);
  if(rc) return rc;
  if (online == 0)
      return 1;
  return 0;
}

int net_activate_s390_devs(void)
{
  return net_activate_s390_devs_ex(NULL, NULL);
}

/* find the network device name for a CCW group */
int net_s390_get_ifname(char* channel, char** device)
{
  char path[100];
  DIR* d;
  struct dirent* e;
  sprintf(path, "/sys/bus/ccwgroup/devices/%s", channel);
  d = opendir(path);
  if(!d) return -1;
  while((e = readdir(d))) {
    if(!strncmp(e->d_name, "net:", 4)) {
      fprintf(stderr, "ccwgroup %s has network IF %s\n", channel, e->d_name + 4);
      strprintf(device, e->d_name + 4);
      closedir(d);
      return 0;
    }
  }
  closedir(d);
  fprintf(stderr, "no network IF found for channel group %s\n", channel);
  return -1;
}

int net_activate_s390_devs_ex(hd_t* hd, char** device)
{
  int rc;
  char buf[100];
  char hwcfg_name[40];

  if(hd) switch(hd->sub_class.id) {
  case 0x89:	/* OSA2 */
    config.hwp.type = di_390net_osa;
    config.hwp.interface = di_osa_lcs;
    break;
  case 0x86:	/* OSA Express */
    config.hwp.type = di_390net_osa;
    config.hwp.interface = di_osa_qdio;
    break;
  case 0x90:	/* IUCV */
    config.hwp.type = di_390net_iucv;
    break;
  case 0x87:	/* HSI */
    config.hwp.type = di_390net_hsi;
    config.hwp.interface = di_osa_qdio;
    config.hwp.medium = di_osa_eth;
    break;
  case 0x88:	/* CTC */
    config.hwp.type = di_390net_ctc;
    break;
  case 0x8f:	/* ESCON */
    config.hwp.type = di_390net_escon;
    break;
  default:
    return -1;
  } else {	/* no hd_t entry -> ask */
    dia_item_t di;
    dia_item_t items[] = {
      di_390net_osa,
      di_390net_hsi,
      di_390net_sep,
      di_390net_ctc,
      di_390net_escon,
      di_390net_iucv,
      di_none
    };
  
    IFNOTAUTO(config.hwp.type) {
      di = dia_menu2(txt_get(TXT_CHOOSE_390NET), 60, 0, items, config.hwp.type?:di_390net_iucv);
      config.hwp.type = di;
    } else di = config.hwp.type;
  }
       
  
  /* hwcfg parms common to all devices */
  config.hwp.startmode="auto";
  config.hwp.module_options="";
  config.hwp.module_unload="yes";
  
  switch(config.hwp.type)
  {
  case di_390net_iucv:
    IFNOTAUTO(config.hwp.userid)
      if((rc=dia_input2_chopspace(txt_get(TXT_IUCV_PEER), &config.hwp.userid,20,0))) return rc;

    mod_modprobe("netiucv",NULL);	// FIXME: error handling

    if((rc=util_set_sysfs_attr("/sys/bus/iucv/drivers/netiucv/connection",config.hwp.userid))) return rc;

    sprintf(hwcfg_name,"static-iucv-id-%s",config.hwp.userid);
    config.hwp.module="netiucv";
    config.hwp.scriptup="hwup-iucv";
    
    /* bold assumption */
    if(device) strprintf(device, "iucv0");
    
    break;

  case di_390net_ctc:
  case di_390net_escon:
    mod_modprobe("ctc",NULL);	// FIXME: error handling

    if(!hd) net_list_s390_devs("cu3088",0);	// FIXME: filter CTC/ESCON devices

    if((rc=net_s390_getrwchans_ex(hd))) return rc;
    
    /* ask for CTC protocol */
    dia_item_t protocols[] = {
      di_ctc_compat,
      di_ctc_ext,
      di_ctc_zos390,
      di_none
    };
    if(config.hwp.protocol)
      switch(config.hwp.protocol)
      {
      case 0+1:	rc=di_ctc_compat; break;
      case 1+1: rc=di_ctc_ext; break;
      case 3+1: rc=di_ctc_zos390; break;
      default: return -1;
      }
    else rc=0;    
    IFNOTAUTO(config.hwp.protocol)
    {
      rc=dia_menu2(txt_get(TXT_CHOOSE_CTC_PROTOCOL), 50, 0, protocols, rc);
      switch(rc)
      {
      case di_ctc_compat: config.hwp.protocol=0+1; break;
      case di_ctc_ext: config.hwp.protocol=1+1; break;
      case di_ctc_zos390: config.hwp.protocol=3+1; break;
      default: return -1;
      }
    }

    if((rc=net_s390_group_chans(2,"ctc"))) return rc;

    /* set protocol */
    if ((rc=net_s390_ctc_protocol(config.hwp.protocol-1))) return rc;
    
    if((rc=net_s390_put_online(config.hwp.readchan))) {
	fprintf(stderr,"Could not activate device\n");
	return rc;
    }
    net_s390_set_config_ccwdev();
    sprintf(hwcfg_name, "ctc-bus-ccw-%s",config.hwp.readchan);
    config.hwp.module="ctc";
    config.hwp.scriptup_ccwgroup="hwup-ctc";
    config.hwp.ccw_chan_num=2;
    strprintf(&config.hwp.ccw_chan_ids,"%s %s",config.hwp.readchan,config.hwp.writechan);
    
    if(device) net_s390_get_ifname(config.hwp.readchan, device);
    
    break;
    
  case di_390net_osa:
  case di_390net_hsi:
    if(config.hwp.type == di_390net_hsi)
    {
      config.hwp.interface=di_osa_qdio;
      config.hwp.medium=di_osa_eth;
    }
    else
    {
      if(!hd) {	/* if we have an hd_t entry, we know already */
        /* ask for LCS/QDIO */
        dia_item_t interfaces[] = {
          di_osa_qdio,
          di_osa_lcs,
          di_none
        };

        IFNOTAUTO(config.hwp.interface)
        {
          rc = dia_menu2(txt_get(TXT_CHOOSE_OSA_INTERFACE), 33, 0, interfaces, config.hwp.interface?:di_osa_qdio);
          if(rc == -1) return rc;
          config.hwp.interface=rc;
        }
        else
          rc=config.hwp.interface;
      }

      /* ask for TR/ETH */

      dia_item_t media[] = {
        di_osa_eth,
        di_osa_tr,
        di_none
      };

      IFNOTAUTO(config.hwp.medium)
      {
        rc = dia_menu2(txt_get(TXT_CHOOSE_OSA_MEDIUM), 33, 0, media, config.hwp.medium?:di_osa_eth);
        if(rc == -1) return rc;
        config.hwp.medium=rc;
      }
      else
        rc=config.hwp.medium;
    }

    if(config.hwp.interface == di_osa_qdio)
    {
      mod_modprobe("qeth",NULL);	// FIXME: error handling

      if(!hd) net_list_s390_devs("qeth", config.hwp.type == di_390net_hsi ? 5 : 1);

      if((rc=net_s390_getrwchans_ex(hd))) return rc;
      IFNOTAUTO(config.hwp.datachan)
        if((rc=dia_input2_chopspace(txt_get(TXT_CTC_CHANNEL_DATA), &config.hwp.datachan, 9, 0))) return rc;
      if((rc=net_check_ccw_address(config.hwp.datachan))) return rc;

      if (config.hwp.type != di_390net_hsi) {
	  IFNOTAUTO(config.hwp.portname)
	  {
	      if((rc=dia_input2_chopspace(txt_get(TXT_QETH_PORTNAME), &config.hwp.portname,9,0))) return rc;
	      // FIXME: warn about problems related to empty portnames
	  }
      }
      
      if(config.hwp.medium == di_osa_eth && config.hwp.type != di_390net_hsi)
      {
        IFNOTAUTO(config.hwp.layer2)
        {
          config.hwp.layer2 = dia_yesno(txt_get(TXT_ENABLE_LAYER2), YES) == YES ? 2 : 1;
        }
      }
      
      if((rc=net_s390_group_chans(3,"qeth"))) return rc;

      if((rc=net_s390_qdio_portname(config.hwp.portname))) return rc;
      
      if(config.hwp.layer2 == 2) if((rc=net_s390_enable_layer2(1))) return rc;

      if((rc=net_s390_put_online(config.hwp.readchan))) return rc;
      
      sprintf(hwcfg_name, "qeth-bus-ccw-%s",config.hwp.readchan);
      config.hwp.module="qeth";
      net_s390_set_config_ccwdev();
      config.hwp.scriptup_ccwgroup="hwup-qeth";
      strprintf(&config.hwp.ccw_chan_ids,"%s %s %s",config.hwp.readchan,config.hwp.writechan,config.hwp.datachan);
      config.hwp.ccw_chan_num=3;
    }
    else	/* LCS */
    {
      mod_modprobe("lcs",NULL);	// FIXME: error handling
      
      if(!hd) net_list_s390_devs("cu3088",0);	// FIXME: filter LCS devices
      
      if((rc=net_s390_getrwchans_ex(hd))) return rc;
      
      IFNOTAUTO(config.hwp.portname)
        if((rc=dia_input2_chopspace(txt_get(TXT_OSA_PORTNO), &config.hwp.portname,9,0))) return rc;

      if((rc=net_s390_group_chans(2,"lcs"))) return rc;
      
      /* set port number */
      if((rc=net_s390_lcs_portno(config.hwp.portname))) return rc;
      
      if((rc=net_s390_put_online(config.hwp.readchan))) return rc;
      
      net_s390_set_config_ccwdev();
      sprintf(hwcfg_name, "lcs-bus-ccw-%s",config.hwp.readchan);
      config.hwp.module="lcs";
      config.hwp.scriptup_ccwgroup="hwup-lcs";
      config.hwp.ccw_chan_num=2;
      strprintf(&config.hwp.ccw_chan_ids,"%s %s",config.hwp.readchan,config.hwp.writechan);
    }
    
    if(device) net_s390_get_ifname(config.hwp.readchan, device);
    
    break;
    
  default:
    return -1;
  }
  
  /* write hwcfg file */
  if (mkdir("/etc/sysconfig/hardware", (mode_t)0755) && errno != EEXIST)
    return -1;
  sprintf(buf,"/etc/sysconfig/hardware/hwcfg-%s",hwcfg_name);
  FILE* fp=fopen(buf,"w");
  if(!fp) return -1;
# define HWE(var,string) if(config.hwp.var) fprintf(fp, #string "=\"%s\"\n",config.hwp.var);
  HWE(startmode,STARTMODE)
  HWE(module,MODULE)
  HWE(module_options,MODULE_OPTIONS)
  HWE(module_unload,MODULE_UNLOAD)
  HWE(scriptup,SCRIPTUP)
  HWE(scriptup_ccw,SCRIPTUP_ccw)
  HWE(scriptup_ccwgroup,SCRIPTUP_ccwgroup)
  HWE(scriptdown,SCRIPTDOWN)
  HWE(ccw_chan_ids,CCW_CHAN_IDS)
  if(config.hwp.ccw_chan_num) fprintf(fp,"CCW_CHAN_NUM=\"%d\"\n",config.hwp.ccw_chan_num);
  if(config.hwp.protocol) fprintf(fp,"CCW_CHAN_MODE=\"%d\"\n",config.hwp.protocol-1);
  HWE(portname,CCW_CHAN_MODE)
  if(config.hwp.layer2) fprintf(fp,"QETH_LAYER2_SUPPORT=\"%d\"\n",config.hwp.layer2 - 1);
# undef HWE
  fclose(fp);
  
  //net_ask_hostname();	/* not sure if this is the best place; ssh login does not work if the hostname is not correct */

  return 0;
}
#endif


/*
 * Run ethtool for matching devices.
 */
void net_apply_ethtool(char *device, char *hwaddr)
{
  slist_t *sl;
  char *s = NULL;

  for(sl = config.ethtool; sl; sl = sl->next) {
    if(
      (device && !fnmatch(sl->key, device, 0)) ||
      (hwaddr && !fnmatch(sl->key, hwaddr, 0))
    ) {
      if(s) {
        strprintf(&s, "%s %s", s, sl->value);
      }
      else {
        str_copy(&s, sl->value);
      }
    }
  }

  if(s) {
    str_copy(&config.net.ethtool_used, s);
    strprintf(&s, "ethtool -s %s %s", device, s);
    fprintf(stderr, "%s\n", s);
    system(s);
    free(s);
  }
}


/*
 * 0: ok, 1: failed
 */
int wlan_setup()
{
  int win_old = config.win;
  dia_item_t di;
  dia_item_t items[] = {
    di_wlan_open,
    di_wlan_wep_o,
    di_wlan_wep_r,
    di_wlan_wpa,
    di_none
  };

  switch(config.net.wlan.auth) {
    case wa_open:
      di = di_wlan_open;
      break;

    case wa_wep_open:
      di = di_wlan_wep_o;
      break;

    case wa_wep_resticted:
      di = di_wlan_wep_r;
      break;

    case wa_wpa:
      di = di_wlan_wpa;
      break;

    default:
      di = di_none;
  }

  if(config.manual || di == di_none) {
    di = dia_menu2("WLAN Authentication", 40, wlan_auth_cb, items, di_wlan_auth_last);
  }
  else {
    if(wlan_auth_cb(di)) di = di_none;
  }

  if(config.win && !win_old) util_disp_done();

  return di == di_none ? 1 : 0;
}


/*
 * return values:
 * -1    : abort (aka ESC)
 *  0    : ok
 *  other: stay in menu
 */
int wlan_auth_cb(dia_item_t di)
{
  int rc = 1, i, j;
  char *buf = NULL, *key = NULL, *s;
  int wep_mode = 0;
  static char *wep_key_items[] = {
    "ASCII", "HEX", "Passphrase - 40 bit", "Passphrase - 104 bit",
    NULL
  };
  static char *wpa_key_items[] = {
    "HEX", "Passphrase",
    NULL
  };
  FILE *f;

  di_wlan_auth_last = di;

  util_killall("wpa_supplicant", 15);
  usleep(100000);
  util_killall("wpa_supplicant", 9);

  switch(di) {
    case di_wlan_open:
      config.net.wlan.auth = wa_open;

      if(config.manual || !config.net.wlan.essid) {
        if(dia_input2("ESSID", &config.net.wlan.essid, 30, 0)) {
         rc = -1;
          break;
        }
      }

      strprintf(&buf, "iwconfig %s essid %s'%s'",
        config.net.device,
        config.net.wlan.essid ? "-- " : "",
        config.net.wlan.essid ?: "any"
      );
      fprintf(stderr, "%s\n", buf);
      system_log(buf);

      strprintf(&buf, "iwconfig %s key off", config.net.device);
      fprintf(stderr, "%s\n", buf);
      system_log(buf);

      rc = 0;
      break;

    case di_wlan_wep_o:
      wep_mode = 1;

    case di_wlan_wep_r:
      config.net.wlan.auth = wep_mode ? wa_wep_open : wa_wep_resticted;

      if(config.manual || !config.net.wlan.essid) {
        if(dia_input2("ESSID", &config.net.wlan.essid, 30, 0)) {
          rc = -1;
         break;
        }
      }

      if(config.manual || !config.net.wlan.key) {
        i = dia_list("WEP Key Type", 30, NULL, wep_key_items, config.net.wlan.key_type + 1, align_left) - 1;

        if(i < 0) {
          rc = -1;
          break;
        }

        switch(i) {
          case 0:
            config.net.wlan.key_type = kt_ascii;
            config.net.wlan.key_len = 0;
            break;

          case 1:
            config.net.wlan.key_type = kt_hex;
            config.net.wlan.key_len = 0;
            break;

          case 2:
            config.net.wlan.key_type = kt_pass;
            config.net.wlan.key_len = 40;
            break;

          case 3:
            config.net.wlan.key_type = kt_pass;
            config.net.wlan.key_len = 104;
            break;
        }

        if(dia_input2("WEP Key", &config.net.wlan.key, 30, 0) || !config.net.wlan.key) {
          rc = -1;
          break;
        }
      }

      switch(config.net.wlan.key_type) {
        case kt_ascii:
          strprintf(&key, "s:%s", config.net.wlan.key);
          break;

        case kt_hex:
          str_copy(&key, config.net.wlan.key);
          break;

        case kt_pass:
          strprintf(&buf, "lwepgen%s '%s'",
            config.net.wlan.key_len == 104 ? " -s" : "",
            config.net.wlan.key
          );
          f = popen(buf, "r");
          if(f) {
            fgets(key = calloc(1, 256), 256, f);
            if((s = strchr(key, '\n'))) *s = 0;
            pclose(f);
            if(!*key) rc = -1;
          }
          else {
            rc = -1;
          }
          break;

        default:
          rc = -1;
          break;
      }

      if(rc == -1) break;

      strprintf(&buf, "iwconfig %s essid %s'%s'",
        config.net.device,
        config.net.wlan.essid ? "-- " : "",
        config.net.wlan.essid ?: "any"
      );
      fprintf(stderr, "%s\n", buf);
      system_log(buf);

      strprintf(&buf, "iwconfig %s key %s '%s'",
        config.net.device,
        config.net.wlan.auth == wa_wep_open ? "open" : "restricted",
        key
      );
      fprintf(stderr, "%s\n", buf);
      system_log(buf);

      rc = 0;
      break;

    case di_wlan_wpa:
      if(config.manual || !config.net.wlan.essid) {
        if(dia_input2("ESSID", &config.net.wlan.essid, 30, 0)) {
          rc = -1;
          break;
        }
      }

      if(config.manual || !config.net.wlan.key) {
        j = config.net.wlan.key_type == kt_pass ? 2 : 1;

        i = dia_list("WPA Key Type", 30, NULL, wpa_key_items, j, align_left);

        if(i < 1) {
          rc = -1;
          break;
        }

        config.net.wlan.key_type = i == 1 ? kt_hex : kt_pass;
        config.net.wlan.key_len = 0;

        if(dia_input2("WPA Key", &config.net.wlan.key, 30, 0) || !config.net.wlan.key) {
          rc = -1;
          break;
        }

        if(config.net.wlan.key_type == kt_pass && strlen(config.net.wlan.key) < 8) {
          dia_message(txt_get(TXT_VNC_PASSWORD_TOO_SHORT), MSGTYPE_ERROR);
          rc = -1;
          break;
        }
      }

      if(config.net.wlan.key_type == kt_pass) {
        strprintf(&key, "\"%s\"", config.net.wlan.key);
      }
      else {
        str_copy(&key, config.net.wlan.key);
      }

      f = fopen("/tmp/wpa_supplicant.conf", "w");
      if(!f) {
        rc = -1;
        break;
      }
      fprintf(f,
        "ap_scan=1\n"
        "network={\n"
        "  key_mgmt=WPA-PSK\n"
        "  scan_ssid=1\n"
        "  ssid=\"%s\"\n"
        "  psk=%s\n"
        "}\n",
        config.net.wlan.essid,
        key
      );
      fclose(f);

      strprintf(&buf, "wpa_supplicant -B -Dwext -i%s -c/tmp/wpa_supplicant.conf",
        config.net.device
      );
      fprintf(stderr, "%s\n", buf);
      system_log(buf);

      rc = 0;
      break;

    default:
      break;
  }

  free(buf);
  free(key);

  return rc;
}


