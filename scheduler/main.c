/*
 * "$Id: main.c,v 1.40 2005/03/10 00:24:06 jlovell Exp $"
 *
 *   Scheduler main loop for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2005 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Easy Software Products and are protected by Federal
 *   copyright law.  Distribution and use rights are outlined in the file
 *   "LICENSE" which should have been included with this file.  If this
 *   file is missing or damaged please contact Easy Software Products
 *   at:
 *
 *       Attn: CUPS Licensing Information
 *       Easy Software Products
 *       44141 Airport View Drive, Suite 204
 *       Hollywood, Maryland 20636 USA
 *
 *       Voice: (301) 373-9600
 *       EMail: cups-info@cups.org
 *         WWW: http://www.cups.org
 *
 * Contents:
 *
 *   main()               - Main entry for the CUPS scheduler.
 *   cupsdClosePipe()     - Close a pipe as necessary.
 *   cupsdOpenPipe()      - Create a pipe which is closed on exec.
 *   CatchChildSignals()  - Catch SIGCHLD signals...
 *   HoldSignals()        - Hold child and termination signals.
 *   IgnoreChildSignals() - Ignore SIGCHLD signals...
 *   ReleaseSignals()     - Release signals for delivery.
 *   SetString()          - Set a string value.
 *   SetStringf()         - Set a formatted string value.
 *   parent_handler()     - Catch USR1/CHLD signals...
 *   process_children()   - Process all dead children...
 *   sigchld_handler()    - Handle 'child' signals from old processes.
 *   sighup_handler()     - Handle 'hangup' signals to reconfigure the scheduler.
 *   sigterm_handler()    - Handle 'terminate' signals that stop the scheduler.
 *   select_timeout()     - Calculate the select timeout value.
 *   usage()              - Show scheduler usage.
 */

/*
 * Include necessary headers...
 */

#define _MAIN_C_
#include "cupsd.h"
#include <sys/resource.h>
#include <syslog.h>
#include <grp.h>

#if defined(HAVE_MALLOC_H) && defined(HAVE_MALLINFO)
#  include <malloc.h>
#endif /* HAVE_MALLOC_H && HAVE_MALLINFO */

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>

#ifdef HAVE_NOTIFY_H
#include <notify.h>

static time_t NotifyPostDelay = 0;
#endif

static kern_return_t registerBootstrapService();
static kern_return_t destroyBootstrapService();
static void emptyReceivePort();

static mach_port_t  server_priv_port = MACH_PORT_NULL;
static mach_port_t  service_rcv_port = MACH_PORT_NULL;

static char service_name[] = "/usr/sbin/cupsd";

#endif	/* __APPLE__ */

/*
 * Local functions...
 */

static void	parent_handler(int sig);
static void	process_children(void);
static void	sigchld_handler(int sig);
static void	sighup_handler(int sig);
static void	sigterm_handler(int sig);
static long	select_timeout(int fds);
static void	usage(void);


/*
 * Local globals...
 */

static int	parent_signal = 0;	/* Set to signal number from child */
static int	holdcount = 0;		/* Number of times "hold" was called */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
static sigset_t	holdmask;		/* Old POSIX signal mask */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */
static int	dead_children = 0;	/* Dead children? */
static int	stop_scheduler = 0;	/* Should the scheduler stop? */


/*
 * 'main()' - Main entry for the CUPS scheduler.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line arguments */
     char *argv[])			/* I - Command-line arguments */
{
  int			i;		/* Looping var */
  char			*opt;		/* Option character */
  int			fg;		/* Run in the foreground */
  int			fds;		/* Number of ready descriptors select returns */
  fd_set		*input,		/* Input set for select() */
			*output;	/* Output set for select() */
  client_t		*con,		/* Current client */
			*next_con;	/* Next client */
  job_t			*job,		/* Current job */
			*next;		/* Next job */
  listener_t		*lis;		/* Current listener */
  time_t		activity;	/* Activity timer */
  time_t		browse_time;	/* Next browse send time */
  time_t		senddoc_time;	/* Send-Document time */
#ifdef HAVE_DNSSD
  DNSServiceErrorType	sdErr;		/* Service discovery error */
  printer_t		*p;		/* Printer information */
  dnssd_resolve_t	*dnssdResolve,	/* Current outstanding dnssd request */
			*nextDNSSDResolve;/* Next outstanding dnssd request */
#endif /* HAVE_DNSSD */
#ifdef HAVE_MALLINFO
  time_t		mallinfo_time;	/* Malloc information time */
#endif /* HAVE_MALLINFO */
  struct timeval	timeout;	/* select() timeout */
  struct rlimit		limit;		/* Runtime limit */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */
#ifdef __sgi
  cups_file_t		*fp;		/* Fake lpsched lock file */
  struct stat		statbuf;	/* Needed for checking lpsched FIFO */
#endif /* __sgi */
#ifdef __APPLE__
  int			debug = 0;	/* Debugging mode, don't register as Mach service */
  int			lazy = 0;
#endif


 /*
  * Check for command-line arguments...
  */

  fg = 0;

  for (i = 1; i < argc; i ++)
    if (argv[i][0] == '-')
      for (opt = argv[i] + 1; *opt != '\0'; opt ++)
        switch (*opt)
	{
	  case 'c' : /* Configuration file */
	      i ++;
	      if (i >= argc)
	        usage();

              if (argv[i][0] == '/')
	      {
	       /*
	        * Absolute directory...
		*/

		SetString(&ConfigurationFile, argv[i]);
              }
	      else
	      {
	       /*
	        * Relative directory...
		*/

                char *current;			/* Current directory */
                if ((current = getcwd(NULL, 0)) == NULL)
                  exit(1);
		SetStringf(&ConfigurationFile, "%s/%s", current, argv[i]);
		free(current);
              }
	      break;

          case 'f' : /* Run in foreground... */
	      fg = 1;
	      break;

          case 'F' : /* Run in foreground, but still disconnect from terminal... */
	      fg = -1;
	      break;

#ifdef __APPLE__
          case 'd' : /* Debugging mode */
	      debug = 1;
	      break;
		  
	  case 'L' : /* Lazy mode */
	      lazy = 1;
	      break;
#endif

	  default : /* Unknown option */
              fprintf(stderr, "cupsd: Unknown option \'%c\' - aborting!\n", *opt);
	      usage();
	      break;
	}
    else
    {
      fprintf(stderr, "cupsd: Unknown argument \'%s\' - aborting!\n", argv[i]);
      usage();
    }

  if (!ConfigurationFile)
    SetString(&ConfigurationFile, CUPS_SERVERROOT "/cupsd.conf");

 /*
  * If the user hasn't specified "-f", run in the background...
  */

  if (!fg)
  {
   /*
    * Setup signal handlers for the parent...
    */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
    sigset(SIGUSR1, parent_handler);
    sigset(SIGCHLD, parent_handler);

    sigset(SIGHUP, SIG_IGN);
#elif defined(HAVE_SIGACTION)
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    action.sa_handler = parent_handler;
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGCHLD, &action, NULL);

    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &action, NULL);
#else
    signal(SIGUSR1, parent_handler);
    signal(SIGCLD, parent_handler);

    signal(SIGHUP, SIG_IGN);
#endif /* HAVE_SIGSET */

    if (fork() > 0)
    {
     /*
      * OK, wait for the child to startup and send us SIGUSR1 or to crash
      * and the OS send us SIGCHLD...  We also need to ignore SIGHUP which
      * might be sent by the init script to restart the scheduler...
      */

      for (; parent_signal == 0;)
        sleep(1);

      if (parent_signal == SIGUSR1)
        return (0);

      if (wait(&i) < 0)
      {
        perror("cupsd");
	return (1);
      }
      else if (WIFEXITED(i))
      {
        fprintf(stderr, "cupsd: Child exited with status %d!\n", WEXITSTATUS(i));
	return (2);
      }
      else
      {
        fprintf(stderr, "cupsd: Child exited on signal %d!\n", WTERMSIG(i));
	return (3);
      }
    }
  }

  if (fg < 1)
  {
   /*
    * Make sure we aren't tying up any filesystems...
    */

    chdir("/");

#ifndef DEBUG
   /*
    * Disable core dumps...
    */

    getrlimit(RLIMIT_CORE, &limit);
    limit.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &limit);

   /*
    * Disconnect from the controlling terminal...
    */

    close(0);
    close(1);
    close(2);

    setsid();
#endif /* DEBUG */
  }

 /*
  * Set the timezone info...
  */

  if (getenv("TZ") != NULL)
    SetStringf(&TZ, "TZ=%s", getenv("TZ"));
  else
    SetString(&TZ, "");

  tzset();

#ifdef LC_TIME
  setlocale(LC_TIME, "");
#endif /* LC_TIME */

 /*
  * Set the maximum number of files...
  */

  getrlimit(RLIMIT_NOFILE, &limit);

  if (limit.rlim_max > CUPS_MAX_FDS)
    MaxFDs = CUPS_MAX_FDS;
  else
    MaxFDs = limit.rlim_max;

  limit.rlim_cur = MaxFDs;

  setrlimit(RLIMIT_NOFILE, &limit);

 /*
  * Allocate memory for the input and output sets...
  */

  SetSize = (MaxFDs + 31) / 8 + 4;
  if (SetSize < sizeof(fd_set))
    SetSize = sizeof(fd_set);

  InputSet  = (fd_set *)calloc(1, SetSize);
  OutputSet = (fd_set *)calloc(1, SetSize);
  input     = (fd_set *)calloc(1, SetSize);
  output    = (fd_set *)calloc(1, SetSize);

  if (InputSet == NULL || OutputSet == NULL || input == NULL || output == NULL)
  {
    syslog(LOG_LPR, "Unable to allocate memory for select() sets - exiting!");
    return (1);
  }

 /*
  * Read configuration...
  */

  if (!ReadConfiguration())
  {
    syslog(LOG_LPR, "Unable to read configuration file \'%s\' - exiting!",
           ConfigurationFile);
    return (1);
  }

 /*
  * Catch hangup and child signals and ignore broken pipes...
  */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  if (RunAsUser)
    sigset(SIGHUP, sigterm_handler);
  else
    sigset(SIGHUP, sighup_handler);

  sigset(SIGPIPE, SIG_IGN);
  sigset(SIGTERM, sigterm_handler);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGHUP);

  if (RunAsUser)
    action.sa_handler = sigterm_handler;
  else
    action.sa_handler = sighup_handler;

  sigaction(SIGHUP, &action, NULL);

  sigemptyset(&action.sa_mask);
  action.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &action, NULL);

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = sigterm_handler;
  sigaction(SIGTERM, &action, NULL);
#else
  if (RunAsUser)
    signal(SIGHUP, sigterm_handler);
  else
    signal(SIGHUP, sighup_handler);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, sigterm_handler);
#endif /* HAVE_SIGSET */

#ifdef __sgi
 /*
  * Try to create a fake lpsched lock file if one is not already there.
  * Some Adobe applications need it under IRIX in order to enable
  * printing...
  */

  if ((fp = cupsFileOpen("/var/spool/lp/SCHEDLOCK", "w")) == NULL)
  {
    syslog(LOG_LPR, "Unable to create fake lpsched lock file "
                    "\"/var/spool/lp/SCHEDLOCK\"\' - %s!",
           strerror(errno));
  }
  else
  {
    fchmod(cupsFileNumber(fp), 0644);
    fchown(cupsFileNumber(fp), User, Group);

    cupsFileClose(fp);
  }
#endif /* __sgi */

 /*
  * Initialize authentication certificates...
  */

  InitCerts();

 /*
  * If we are running in the background, signal the parent process that
  * we are up and running...
  */

  if (!fg)
    kill(getppid(), SIGUSR1);

#ifdef __APPLE__
 /*
  * In an effort to make cupsd crash proof register ourselves as a 
  * Mach port server and service. If we should die unexpectedly Mach
  * will receive a port-destroyed notification and will re-launch us.
  */
  if (!debug)
    registerBootstrapService();
#endif

 /*
  * If the administrator has configured the server to run as an unpriviledged
  * user, change to that user now...
  */

  if (RunAsUser)
  {
    setgid(Group);
    setgroups(1, &Group);
    setuid(User);
  }

 /*
  * Start any pending print jobs...
  */

  CheckJobs();

#ifdef __APPLE__
  /* If printer sharing is not enabled and there are no
   * jobs waiting to be printerd then cupsd will be started
   * on demand.
   */
  if (lazy && NumBrowsers == 0 && NumJobs == 0)
  {
    LogMessage(L_INFO, "Printer sharing is off and there are no jobs pending, will restart on demand. Exiting.");
    return (0);
  }
#endif
  
#ifdef HAVE_NOTIFY_POST
   /*
    * Send initial notifications
    */

    NotifyPost = (CUPS_NOTIFY_PRINTER_LIST | CUPS_NOTIFY_JOB);
#endif        /* HAVE_NOTIFY_POST */

 /*
  * Loop forever...
  */

#ifdef HAVE_MALLINFO
  mallinfo_time = 0;
#endif /* HAVE_MALLINFO */
  browse_time   = time(NULL);
  senddoc_time  = time(NULL);
  fds           = 1;

  while (!stop_scheduler)
  {
#ifdef DEBUG
    LogMessage(L_DEBUG2, "main: Top of loop, dead_children=%d, NeedReload=%d",
               dead_children, NeedReload);
#endif /* DEBUG */

#ifdef __APPLE__
   /*
    * Don't let mach messages back up in our receive queue.
    */
	 
    emptyReceivePort();
#endif __APPLE__

   /*
    * Check if there are dead children to handle...
    */

    if (dead_children)
      process_children();

   /*
    * Check if we need to load the server configuration file...
    */

    if (NeedReload)
    {
     /*
      * Close any idle clients...
      */

      if (NumClients > 0)
      {
        for (con = Clients; con != NULL; con = next_con)
        {
          next_con = con->next;
	  if (con->http.state == HTTP_WAITING)
	    CloseClient(con);
	  else
	    con->http.keep_alive = HTTP_KEEPALIVE_OFF;
	}
        PauseListening();
      }

     /*
      * Check for any active jobs...
      */

      for (job = Jobs; job; job = job->next)
        if (job->state->values[0].integer == IPP_JOB_PROCESSING)
	  break;

     /*
      * Restart if all clients are closed and all jobs finished, or
      * if the reload timeout has elapsed...
      */

      if ((NumClients == 0 && (!job || NeedReload != RELOAD_ALL)) ||
          (time(NULL) - ReloadTime) >= ReloadTimeout)
      {
        if (!ReadConfiguration())
        {
          syslog(LOG_LPR, "Unable to read configuration file \'%s\' - exiting!",
		 ConfigurationFile);
          break;
	}
      }
    }

   /*
    * Check for available input or ready output.  If select() returns
    * 0 or -1, something bad happened and we should exit immediately.
    *
    * Note that we at least have one listening socket open at all
    * times.
    */

    memcpy(input, InputSet, SetSize);
    memcpy(output, OutputSet, SetSize);

    timeout.tv_sec  = select_timeout(fds);
    timeout.tv_usec = 0;

    if ((fds = select(MaxFDs, input, output, NULL, &timeout)) < 0)
    {
      char	s[16384],	/* String buffer */
		*sptr;		/* Pointer into buffer */
      int	slen;		/* Length of string buffer */


     /*
      * Got an error from select!
      */

      if (errno == EINTR)	/* Just interrupted by a signal */
        continue;

     /*
      * Log all sorts of debug info to help track down the problem.
      */

      LogMessage(L_EMERG, "select() failed - %s!", strerror(errno));

      strcpy(s, "InputSet =");
      slen = 10;
      sptr = s + 10;

      for (i = 0; i < MaxFDs; i ++)
        if (FD_ISSET(i, InputSet))
	{
          snprintf(sptr, sizeof(s) - slen, " %d", i);
	  slen += strlen(sptr);
	  sptr += strlen(sptr);
	}

      LogMessage(L_EMERG, s);

      strcpy(s, "OutputSet =");
      slen = 11;
      sptr = s + 11;

      for (i = 0; i < MaxFDs; i ++)
        if (FD_ISSET(i, OutputSet))
	{
          snprintf(sptr, sizeof(s) - slen, " %d", i);
	  slen += strlen(sptr);
	  sptr += strlen(sptr);
	}

      LogMessage(L_EMERG, s);

      for (con = Clients; con != NULL; con = con->next)
        LogMessage(L_EMERG, "Clients[%p] = %d, file = %d, state = %d",
	           con, con->http.fd, con->file, con->http.state);

      for (i = 0, lis = Listeners; i < NumListeners; i ++, lis ++)
        LogMessage(L_EMERG, "Listeners[%d] = %d", i, lis->fd);

      LogMessage(L_EMERG, "BrowseSocket = %d", BrowseSocket);

      LogMessage(L_EMERG, "CGIPipes[0] = %d", CGIPipes[0]);

      for (job = Jobs; job != NULL; job = job->next)
        LogMessage(L_EMERG, "Jobs[%d] = %d", job->id, job->pipe);

      LogMessage(L_EMERG, "SysEventPipes[0] = %d", SysEventPipes[0]);

#ifdef HAVE_DNSSD
      LogMessage(L_EMERG, "BrowseDNSSDfd = %d", BrowseDNSSDfd);

      for (dnssdResolve = DNSSDPendingResolves; dnssdResolve; dnssdResolve = dnssdResolve->next)
        LogMessage(L_EMERG, "dnssdResolve fd = %d", dnssdResolve->fd);

      for (p = Printers; p != NULL; p = p->next)
        LogMessage(L_EMERG, "printer[%s] %d, %d", p->name, p->dnssd_ipp_fd, p->dnssd_query_fd);
#endif /* HAVE_DNSSD */
      break;
    }

    for (i = NumListeners, lis = Listeners; i > 0; i --, lis ++)
      if (FD_ISSET(lis->fd, input))
        AcceptClient(lis);

    for (con = Clients; con != NULL; con = next_con)
    {
      next_con = con->next;

     /*
      * Process the input buffer...
      */

      if (FD_ISSET(con->http.fd, input) || con->http.used)
        if (!ReadClient(con))
	  continue;

     /*
      * Write data as needed...
      */

      if (con->pipe_pid && FD_ISSET(con->file, input))
      {
       /*
        * Keep track of pending input from the file/pipe separately
	* so that we don't needlessly spin on select() when the web
	* client is not ready to receive data...
	*/

        con->file_ready = 1;

#ifdef DEBUG
        LogMessage(L_DEBUG2, "main: Data ready file %d!", con->file);
#endif /* DEBUG */

	if (!FD_ISSET(con->http.fd, output))
	{
	  LogMessage(L_DEBUG2, "main: Removing fd %d from InputSet...", con->file);
	  FD_CLR(con->file, InputSet);
	}
      }

      if (FD_ISSET(con->http.fd, output) &&
          (!con->pipe_pid || con->file_ready))
        if (!WriteClient(con))
	  continue;

     /*
      * Check the activity and close old clients...
      */

      activity = time(NULL) - Timeout;
      if (con->http.activity < activity && !con->pipe_pid)
      {
        LogMessage(L_DEBUG, "Closing client %d after %d seconds of inactivity...",
	           con->http.fd, Timeout);

        CloseClient(con);
        continue;
      }
    }

   /*
    * Check for status info from job filters...
    */

    for (job = Jobs; job != NULL; job = next)
    {
      next = job->next;

      if (job->pipe >= 0 && FD_ISSET(job->pipe, input))
      {
       /*
        * Clear the input bit to avoid updating the next job
	* using the same status pipe file descriptor...
	*/

        FD_CLR(job->pipe, input);

       /*
        * Read any status messages from the filters...
	*/

        UpdateJob(job);
      }
    }

   /*
    * Update CGI messages as needed...
    */

    if (CGIPipes[0] >= 0 && FD_ISSET(CGIPipes[0], input))
      UpdateCGI();


#ifdef __APPLE__
   /*
    * Handle system events as needed...
    */

    if (SysEventPipes[0] >= 0 && FD_ISSET(SysEventPipes[0], input))
      UpdateSysEventMonitor();
#endif	/* __APPLE__ */


   /*
    * Update the browse list as needed...
    */

    if (Browsing && BrowseRemoteProtocols)
    {
      if (BrowseSocket >= 0 && FD_ISSET(BrowseSocket, input))
        UpdateCUPSBrowse();

      if (PollPipe >= 0 && FD_ISSET(PollPipe, input))
        UpdatePolling();

#ifdef HAVE_LIBSLP
      if ((BrowseRemoteProtocols & BROWSE_SLP) && BrowseSLPRefresh <= time(NULL))
        UpdateSLPBrowse();
#endif /* HAVE_LIBSLP */

#ifdef HAVE_DNSSD
      if (BrowseDNSSDRef && FD_ISSET(BrowseDNSSDfd, input))
      {
	if ((sdErr = DNSServiceProcessResult(BrowseDNSSDRef)) != kDNSServiceErr_NoError)
	{
	  LogMessage(L_ERROR, "DNS Service Discovery browsing error %d; removing fd %d from InputSet...",
			sdErr, BrowseDNSSDfd);
	  FD_CLR(BrowseDNSSDfd, InputSet);
	  DNSServiceRefDeallocate(BrowseDNSSDRef);
	  BrowseDNSSDRef = NULL;
	  BrowseDNSSDfd = -1;
	}
      }

      for (dnssdResolve = DNSSDPendingResolves; dnssdResolve;)
      {
        nextDNSSDResolve = dnssdResolve->next;

	if (dnssdResolve->sdRef && FD_ISSET(dnssdResolve->fd, input))
	{
	  if ((sdErr = DNSServiceProcessResult(dnssdResolve->sdRef)) != kDNSServiceErr_NoError)
	  {
	    LogMessage(L_ERROR, "DNS Service Discovery resolving error %d; removing fd %d from InputSet...",
			sdErr, dnssdResolve->fd);
	    FD_CLR(dnssdResolve->fd, InputSet);
	    DNSServiceRefDeallocate(dnssdResolve->sdRef);
	    dnssdResolve->sdRef = NULL;
	    dnssdResolve->fd = -1;
	  }
	}

	dnssdResolve = nextDNSSDResolve;
      }

      for (p = Printers; p != NULL; p = p->next)
      {
	if (p->dnssd_ipp_ref && FD_ISSET(p->dnssd_ipp_fd, input))
	{
	  if ((sdErr = DNSServiceProcessResult(p->dnssd_ipp_ref)) != kDNSServiceErr_NoError)
	  {
	    LogMessage(L_ERROR, "DNS Service Discovery IPP registration error %d; removing fd %d from InputSet...",
			sdErr, p->dnssd_ipp_fd);
	    FD_CLR(p->dnssd_ipp_fd, InputSet);
	    DNSServiceRefDeallocate(p->dnssd_ipp_ref);
	    p->dnssd_ipp_ref = NULL;
	    p->dnssd_ipp_fd = -1;
	  }
	}

	if (p->dnssd_query_ref && FD_ISSET(p->dnssd_query_fd, input))
	{
	  if ((sdErr = DNSServiceProcessResult(p->dnssd_query_ref)) != kDNSServiceErr_NoError)
	  {
	    LogMessage(L_ERROR, "DNS Service Discovery query error %d; removing fd %d from InputSet...",
			sdErr, p->dnssd_query_fd);
	    FD_CLR(p->dnssd_query_fd, InputSet);
	    DNSServiceRefDeallocate(p->dnssd_query_ref);
	    p->dnssd_query_ref = NULL;
	    p->dnssd_query_fd = -1;
	  }
	}
      }
#endif /* HAVE_DNSSD */

      if (time(NULL) > browse_time)
      {
        SendBrowseList();
	browse_time = time(NULL);
      }
    }

   /*
    * Update any pending multi-file documents...
    */

    if ((time(NULL) - senddoc_time) >= 10)
    {
      CheckJobs();
      senddoc_time = time(NULL);
    }

#ifdef HAVE_MALLINFO
   /*
    * Log memory usage every minute...
    */

    if ((time(NULL) - mallinfo_time) >= 60 && LogLevel >= L_DEBUG)
    {
      struct mallinfo mem;	/* Malloc information */


      mem = mallinfo();
      LogMessage(L_DEBUG, "mallinfo: arena = %d, used = %d, free = %d\n",
                 mem.arena, mem.usmblks + mem.uordblks,
		 mem.fsmblks + mem.fordblks);
      mallinfo_time = time(NULL);
    }
#endif /* HAVE_MALLINFO */

   /*
    * Update the root certificate once every 5 minutes...
    */

    if ((time(NULL) - RootCertTime) >= RootCertDuration && RootCertDuration)
    {
     /*
      * Update the root certificate...
      */

      DeleteCert(0);
      AddCert(0, "root");
    }

#ifdef HAVE_NOTIFY_POST
   /*
    * Handle any pending notifications. 
    * Send them no more frequently than once a second on average to work around 3691136.
    */

    if (NotifyPost && NotifyPostDelay <= time(NULL))
    {
#ifdef DEBUG
      static time_t first_notify_post = 0;
      if (!first_notify_post) first_notify_post = time(NULL);
      fprintf(stderr, "%d notify_post()\n", (int)(time(NULL) - first_notify_post));
#endif  /* DEBUG */

      if ((NotifyPost & CUPS_NOTIFY_PRINTER_LIST))
	notify_post("com.apple.printerListChange");

      if ((NotifyPost & CUPS_NOTIFY_PRINTER_HISTORY))
	notify_post("com.apple.printerHistoryChange");

      if ((NotifyPost & CUPS_NOTIFY_JOB))
	notify_post("com.apple.jobChange");

      NotifyPost = 0;
      NotifyPostDelay = time(NULL) + 1;
    }
#endif        /* HAVE_NOTIFY_POST */
  }

 /*
  * Log a message based on what happened...
  */

  if (stop_scheduler)
    LogMessage(L_INFO, "Scheduler shutting down normally.");
  else
    LogMessage(L_ERROR, "Scheduler shutting down due to program error.");

 /*
  * Close all network clients and stop all jobs...
  */

  StopServer();

  StopAllJobs();

#ifdef HAVE_NOTIFY_POST
  /*
   * Send one last notification as the server shuts down.
   */
  fprintf(stderr, "notify_post(com.apple.printerListChange) last\n");
  notify_post("com.apple.printerListChange");

#endif        /* HAVE_NOTIFY_POST */

#ifdef __sgi
 /*
  * Remove the fake IRIX lpsched lock file, but only if the existing
  * file is not a FIFO which indicates that the real IRIX lpsched is
  * running...
  */

  if (!stat("/var/spool/lp/FIFO", &statbuf))
    if (!S_ISFIFO(statbuf.st_mode))
      unlink("/var/spool/lp/SCHEDLOCK");
#endif /* __sgi */

#ifdef __APPLE__
  /*
   * Unregister our service so Mach won't launch us again.
   */
  destroyBootstrapService();
#endif

 /*
  * Free memory used by FD sets and return...
  */

  free(InputSet);
  free(OutputSet);
  free(input);
  free(output);

  return (!stop_scheduler);
}


/*
 * 'cupsdClosePipe()' - Close a pipe as necessary.
 */

void
cupsdClosePipe(int *fds)		/* I - Pipe file descriptors (2) */
{
 /*
  * Close file descriptors as needed...
  */

  if (fds[0] >= 0)
  {
    close(fds[0]);
    fds[0] = -1;
  }

  if (fds[1] >= 0)
  {
    close(fds[1]);
    fds[1] = -1;
  }
}


/*
 * 'cupsdOpenPipe()' - Create a pipe which is closed on exec.
 */

int					/* O - 0 on success, -1 on error */
cupsdOpenPipe(int *fds)			/* O - Pipe file descriptors (2) */
{
 /*
  * Create the pipe...
  */

  if (pipe(fds))
    return (-1);

 /*
  * Set the "close on exec" flag on each end of the pipe...
  */

  if (fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC))
  {
    close(fds[0]);
    close(fds[1]);
    return (-1);
  }

  if (fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC))
  {
    close(fds[0]);
    close(fds[1]);
    return (-1);
  }

 /*
  * Return 0 indicating success...
  */

  return (0);
}


/*
 * 'CatchChildSignals()' - Catch SIGCHLD signals...
 */

void
CatchChildSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGCHLD, sigchld_handler);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = sigchld_handler;
  sigaction(SIGCHLD, &action, NULL);
#else
  signal(SIGCLD, sigchld_handler);	/* No, SIGCLD isn't a typo... */
#endif /* HAVE_SIGSET */
}


/*
 * 'ClearString()' - Clear a string.
 */

void
ClearString(char **s)			/* O - String value */
{
  if (s && *s)
  {
    free(*s);
    *s = NULL;
  }
}


/*
 * 'HoldSignals()' - Hold child and termination signals.
 */

void
HoldSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  sigset_t		newmask;	/* New POSIX signal mask */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


  holdcount ++;
  if (holdcount > 1)
    return;

#ifdef HAVE_SIGSET
  sighold(SIGTERM);
  sighold(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigemptyset(&newmask);
  sigaddset(&newmask, SIGTERM);
  sigaddset(&newmask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &newmask, &holdmask);
#endif /* HAVE_SIGSET */
}


/*
 * 'IgnoreChildSignals()' - Ignore SIGCHLD signals...
 *
 * We don't really ignore them, we set the signal handler to SIG_DFL,
 * since some OS's rely on signals for the wait4() function to work.
 */

void
IgnoreChildSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGCHLD, SIG_DFL);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &action, NULL);
#else
  signal(SIGCLD, SIG_DFL);	/* No, SIGCLD isn't a typo... */
#endif /* HAVE_SIGSET */
}


/*
 * 'ReleaseSignals()' - Release signals for delivery.
 */

void
ReleaseSignals(void)
{
  holdcount --;
  if (holdcount > 0)
    return;

#ifdef HAVE_SIGSET
  sigrelse(SIGTERM);
  sigrelse(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigprocmask(SIG_SETMASK, &holdmask, NULL);
#endif /* HAVE_SIGSET */
}


/*
 * 'SetString()' - Set a string value.
 */

void
SetString(char       **s,		/* O - New string */
          const char *v)		/* I - String value */
{
  if (!s || *s == v)
    return;

  if (*s)
    free(*s);

  if (v)
    *s = strdup(v);
  else
    *s = NULL;
}


/*
 * 'SetStringf()' - Set a formatted string value.
 */

void
SetStringf(char       **s,		/* O - New string */
           const char *f,		/* I - Printf-style format string */
	   ...)				/* I - Additional args as needed */
{
  char		v[1024];		/* Formatting string value */
  va_list	ap;			/* Argument pointer */
  char		*olds;			/* Old string */


  if (!s)
    return;

  olds = *s;

  if (f)
  {
    va_start(ap, f);
    vsnprintf(v, sizeof(v), f, ap);
    va_end(ap);

    *s = strdup(v);
  }
  else
    *s = NULL;

  if (olds)
    free(olds);
}


/*
 * 'parent_handler()' - Catch USR1/CHLD signals...
 */

static void
parent_handler(int sig)		/* I - Signal */
{
 /*
  * Store the signal we got from the OS and return...
  */

  parent_signal = sig;
}


/*
 * 'process_children()' - Process all dead children...
 */

static void
process_children(void)
{
  int		status;		/* Exit status of child */
  int		pid;		/* Process ID of child */
  job_t		*job;		/* Current job */
  int		i;		/* Looping var */
  char		*filter;	/* Filter name */


 /*
  * Reset the dead_children flag...
  */

  dead_children = 0;

 /*
  * Collect the exit status of some children...
  */

#ifdef HAVE_WAITPID
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
#elif defined(HAVE_WAIT3)
  while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
#else
  if ((pid = wait(&status)) > 0)
#endif /* HAVE_WAITPID */
  {
    DEBUG_printf(("process_children: pid = %d, status = %d\n", pid, status));

   /*
    * Ignore SIGTERM errors - that comes when a job is cancelled...
    */

    if (status == SIGTERM)
      status = 0;

    if (status)
    {
      if (WIFSIGNALED(status))
	LogMessage(L_ERROR, "PID %d crashed on signal %d!", pid,
	           WTERMSIG(status));
      else
	LogMessage(L_ERROR, "PID %d stopped with status %d!", pid,
	           WEXITSTATUS(status));

      if (LogLevel < L_DEBUG)
        LogMessage(L_INFO, "Hint: Try setting the LogLevel to \"debug\" to find out more.");
    }
    else
      LogMessage(L_DEBUG, "PID %d exited with no errors.", pid);

   /*
    * Delete certificates for CGI processes...
    */

    if (pid)
      DeleteCert(pid);

   /*
    * Lookup the PID in the jobs list...
    */

    for (job = Jobs; job != NULL; job = job->next)
      if (job->state != NULL &&
          job->state->values[0].integer == IPP_JOB_PROCESSING)
      {
	for (i = 0; job->procs[i]; i ++)
          if (job->procs[i] == pid)
	    break;

	if (job->procs[i])
	{
	 /*
          * OK, this process has gone away; what's left?
	  */

          job->procs[i] = -pid;

          if (status && job->status >= 0)
	  {
	   /*
	    * An error occurred; save the exit status so we know to stop
	    * the printer or cancel the job when all of the filters finish...
	    *
	    * A negative status indicates that the backend failed and the
	    * printer needs to be stopped.
	    */

            if (!job->procs[i + 1])
 	      job->status = -status;	/* Backend failed */
	    else
 	      job->status = status;	/* Filter failed */

	   /*
	    * Set the printer's state_message so user's have a clue what happened...
	    */

	    if ((filter = basename(job->filters[i])) == NULL)
	      filter = "";

	    if (WIFSIGNALED(status))
	      snprintf(job->printer->state_message, sizeof(job->printer->state_message), 
			"The process \"%s\" terminated unexpectedly on signal %d", 
			filter, WTERMSIG(status));
	    else
	      snprintf(job->printer->state_message, sizeof(job->printer->state_message),
			"The process \"%s\" stopped unexpectedly with status %d", 
			filter, WEXITSTATUS(status));

	    AddPrinterHistory(job->printer);
	  }
	  break;
	}
      }
  }
}


/*
 * 'sigchld_handler()' - Handle 'child' signals from old processes.
 */

static void
sigchld_handler(int sig)	/* I - Signal number */
{
  (void)sig;

 /*
  * Flag that we have dead children...
  */

  dead_children = 1;

 /*
  * Reset the signal handler as needed...
  */

#if !defined(HAVE_SIGSET) && !defined(HAVE_SIGACTION)
  signal(SIGCLD, sigchld_handler);
#endif /* !HAVE_SIGSET && !HAVE_SIGACTION */
}


/*
 * 'sighup_handler()' - Handle 'hangup' signals to reconfigure the scheduler.
 */

static void
sighup_handler(int sig)	/* I - Signal number */
{
  (void)sig;

  NeedReload = RELOAD_ALL;
  ReloadTime = time(NULL);

#if !defined(HAVE_SIGSET) && !defined(HAVE_SIGACTION)
  signal(SIGHUP, sighup_handler);
#endif /* !HAVE_SIGSET && !HAVE_SIGACTION */
}


/*
 * 'sigterm_handler()' - Handle 'terminate' signals that stop the scheduler.
 */

static void
sigterm_handler(int sig)		/* I - Signal */
{
  (void)sig;	/* remove compiler warnings... */

 /*
  * Flag that we should stop and return...
  */

  stop_scheduler = 1;
}


/*
 * 'select_timeout()' - Calculate the select timeout value.
 *
 */

static long				/* O - Number of seconds */
select_timeout(int fds)			/* I - Number of ready descriptors select returned */
{
  long			timeout;	/* Timeout for select */
  time_t		now;		/* Current time */
  client_t		*con;		/* Client information */
  printer_t		*p;		/* Printer information */
  job_t			*job;		/* Job information */
  const char		*why;		/* Debugging aid */


 /*
  * Check to see if any of the clients have pending data to be
  * processed; if so, the timeout should be 0...
  */

  for (con = Clients; con != NULL; con = con->next)
    if (con->http.used > 0)
      return (0);

 /*
  * If select has been active in the last second (fds != 0) or we have
  * many resources in use then don't bother trying to optimize the
  * timeout, just make it 1 second.
  */

  if (fds || NumClients > 50)
    return (1);

#ifdef HAVE_NOTIFY_POST
 /*
  * Send notifications no more frequently than once a second to work around 3691136.
  */

  if (NotifyPost)
    return (1);
#endif        /* HAVE_NOTIFY_POST */

 /*
  * Otherwise, check all of the possible events that we need to wake for...
  */

  now     = time(NULL);
  timeout = now + 86400;		/* 86400 == 1 day */
  why     = "do nothing";

 /*
  * Check the activity and close old clients...
  */

  for (con = Clients; con != NULL; con = con->next)
    if ((con->http.activity + Timeout) < timeout)
    {
      timeout = con->http.activity + Timeout;
      why     = "timeout a client connection";
    }

 /*
  * Update the browse list as needed...
  */

  if (Browsing && (BrowseLocalProtocols || BrowseRemoteProtocols))
  {
#ifdef HAVE_LIBSLP
    if ((BrowseRemoteProtocols & BROWSE_SLP) && (BrowseSLPRefresh < timeout))
    {
      timeout = BrowseSLPRefresh;
      why     = "update SLP browsing";
    }
#endif /* HAVE_LIBSLP */

    if ((BrowseLocalProtocols | BrowseRemoteProtocols) & BROWSE_CUPS)
    {
      for (p = Printers; p != NULL; p = p->next)
      {
	if (p->type & CUPS_PRINTER_REMOTE)
	{
	  if (p->browse_protocol == BROWSE_CUPS && (p->browse_time + BrowseTimeout) < timeout)
	  {
	    timeout = p->browse_time + BrowseTimeout;
	    why     = "browse timeout a printer";
	  }
	}
	else if (!(p->type & CUPS_PRINTER_IMPLICIT))
	{
	  if (BrowseInterval && (p->browse_time + BrowseInterval) < timeout)
	  {
	    timeout = p->browse_time + BrowseInterval;
	    why     = "send browse update";
	  }
	}
      }
    }
  }

 /*
  * Check for any active jobs...
  */

  if (timeout > (now + 10))
  {
    for (job = Jobs; job != NULL; job = job->next)
      if (job->state->values[0].integer <= IPP_JOB_PROCESSING)
      {
	timeout = now + 10;
	why     = "process active jobs";
	break;
      }
  }

#ifdef HAVE_MALLINFO
 /*
  * Log memory usage every minute...
  */

  if (LogLevel >= L_DEBUG && (mallinfo_time + 60) < timeout)
  {
    timeout = mallinfo_time + 60;
    why     = "display memory usage";
  }
#endif /* HAVE_MALLINFO */

 /*
  * Update the root certificate when needed...
  */

  if (RootCertDuration && (RootCertTime + RootCertDuration) < timeout)
  {
    timeout = RootCertTime + RootCertDuration;
    why     = "update root certificate";
  }

 /*
  * Adjust from absolute to relative time.  If p->browse_time above
  * was 0 then we can end up with a negative value here, so check.
  * We add 1 second to the timeout since events occur after the
  * timeout expires, and limit the timeout to 86400 seconds (1 day)
  * to avoid select() timeout limits present on some operating
  * systems...
  */

  timeout = timeout - now + 1;

  if (timeout < 1)
    timeout = 1;
  else if (timeout > 86400)
    timeout = 86400;

 /*
  * Log and return the timeout value...
  */

  LogMessage(L_DEBUG2, "select_timeout: %ld seconds to %s", timeout, why);

  return (timeout);
}


/*
 * 'usage()' - Show scheduler usage.
 */

static void
usage(void)
{
#ifdef __APPLE__
  static const char usage_msg[] = \
		"Usage: cupsd [-c config-file] [-f] [-F] [-d] [-L]\n" \
		"       -c   Use specified configuration file.\n" \
		"       -d   Debugging mode, don't auto-relaunch on process termination.\n" \
		"       -f   Run in foreground.\n" \
		"       -F   Run in foreground but still disconnect from terminal.\n" \
		"       -L   Lazy mode.\n";

  fputs(usage_msg, stderr);
#else
  fputs("Usage: cupsd [-c config-file] [-f] [-F]\n", stderr);
#endif
  exit(1);
}


#ifdef __APPLE__

/*
 * 'registerBootstrapService()' - Register ourselves as a Mach port server and service.
 *
 * If we should die unexpectedly Mach will receive a port-destroyed notification and 
 * will re-launch us.
 */

static kern_return_t registerBootstrapService()
{
  kern_return_t status;
  mach_port_t service_send_port;

  /* syslog(LOG_ERR, "Registering Bootstrap Service"); */

  /*
   * See if our service name is already registered and if we have privilege to check in.
   */
  status = bootstrap_check_in(bootstrap_port, (char*)service_name, &service_rcv_port);
  if (status == KERN_SUCCESS)
  {
    /*
     * If so, we must be a followup instance of an already defined server.  In that case,
     * the bootstrap port we inherited from our parent is the server's privilege port, so set
     * that in case we have to unregister later (which requires the privilege port).
     */
    server_priv_port = bootstrap_port;
  }
  else if (status == BOOTSTRAP_UNKNOWN_SERVICE)
  {
    /* relaunch on demand */
    status = bootstrap_create_server(bootstrap_port, "/usr/sbin/cupsd -f", getuid(),
          true , &server_priv_port);
    if (status != KERN_SUCCESS)
      return status;

    status = bootstrap_create_service(server_priv_port, (char*)service_name, &service_send_port);
    if (status != KERN_SUCCESS)
    {
      mach_port_deallocate(mach_task_self(), server_priv_port);
      return status;
    }

    status = bootstrap_check_in(server_priv_port, (char*)service_name, &service_rcv_port);
    if (status != KERN_SUCCESS)
    {
      mach_port_deallocate(mach_task_self(), server_priv_port);
      mach_port_deallocate(mach_task_self(), service_send_port);
      return status;
    }
  }

  return status;
}

/*
 * 'emptyReceivePort()' - Loop through any waiting mach messages and try to send
 *                        a reply.
 *
 */
 
static void emptyReceivePort()
{
  mach_msg_empty_rcv_t	aMsg;			/* Mach message */
  kern_return_t		msg_rcv_result,		/* Message receive result */
			msg_snd_result;		/* Message send result */

  if (service_rcv_port != MACH_PORT_NULL)
  {

   /* Empty the message queue on our receive port. We do not
    * want to wait for a message so go with a 0 timeout.
    * We no not care about the contents of the message so
    * we ignore the message too large error.
    */

    do
    {
      aMsg.header.msgh_size = sizeof(aMsg);
      msg_rcv_result = mach_msg(&aMsg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 
				aMsg.header.msgh_size, service_rcv_port, 0, MACH_PORT_NULL);
		
     /*
      * If we received a message then send a reply letting the caller
      * know we are alive.
      */

      if (msg_rcv_result == MACH_MSG_SUCCESS)
      {
	aMsg.header.msgh_bits = MACH_MSGH_BITS( MACH_MSG_TYPE_MOVE_SEND, MACH_MSG_TYPE_MAKE_SEND );
	aMsg.header.msgh_size = sizeof(aMsg.header);

	msg_snd_result = mach_msg(&aMsg.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, aMsg.header.msgh_size,
				  0, MACH_PORT_NULL, 500, MACH_PORT_NULL);

	if (msg_snd_result != KERN_SUCCESS)
	  LogMessage(L_WARN, "emptyReceivePort: mach_msg send returns: %s", mach_error_string(msg_snd_result));

       /*
	* if the reply can't be delivered destroy the message...
	*/

	if (msg_snd_result == MACH_SEND_INVALID_DEST || msg_snd_result == MACH_SEND_TIMED_OUT)
	  mach_msg_destroy(&aMsg.header);
      }
      else if (msg_rcv_result != MACH_RCV_TIMED_OUT)
	LogMessage(L_WARN, "emptyReceivePort: mach_msg receive returns: %s", mach_error_string(msg_rcv_result));
    }
    while(msg_rcv_result == MACH_MSG_SUCCESS);
  }
}


/*
 * 'destroyBootstrapService()' - Unregister ourselves as a Mach port service.
 */

static kern_return_t destroyBootstrapService()
{
  /* syslog(LOG_ERR, "Destroying Bootstrap Service"); */
  
  if (service_rcv_port != MACH_PORT_NULL)
  {
    mach_port_destroy(mach_task_self(), service_rcv_port);
    service_rcv_port = MACH_PORT_NULL;
  }
  
  return bootstrap_register(server_priv_port, (char*)service_name, MACH_PORT_NULL);
}

#endif	/* __APPLE__ */

/*
 * End of "$Id: main.c,v 1.40 2005/03/10 00:24:06 jlovell Exp $".
 */
