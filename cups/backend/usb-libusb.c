/*
 * "$Id: usb-libusb.c 3648 2012-02-13 17:37:38Z msweet $"
 *
 *   LIBUSB interface code for CUPS.
 *
 *   Copyright 2007-2012 by Apple Inc.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 *   which should have been included with this file.  If this file is
 *   file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * Contents:
 *
﻿ *   list_devices()	  - List the available printers.
 *   print_device()	  - Print a file to a USB device.
 *   close_device()	  - Close the connection to the USB printer.
 *   find_device()	  - Find or enumerate USB printers.
 *   get_device_id()	  - Get the IEEE-1284 device ID for the printer.
 *   list_cb()		  - List USB printers for discovery.
 *   make_device_uri()	  - Create a device URI for a USB printer.
 *   open_device()	  - Open a connection to the USB printer.
 *   print_cb() 	  - Find a USB printer for printing.
 *   read_thread()	  - Thread to read the backchannel data on.
 *   sidechannel_thread() - Handle side-channel requests.
 *   soft_reset()	  - Send a soft reset to the device.
 */

/*
 * Include necessary headers...
 */

#include <libusb.h>
#include <cups/cups-private.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>


/*
 * WAIT_EOF_DELAY is number of seconds we'll wait for responses from
 * the printer after we've finished sending all the data
 */

#define WAIT_EOF			0
#define WAIT_EOF_DELAY			7
#define WAIT_SIDE_DELAY			3
#define DEFAULT_TIMEOUT			5000L


/*
 * Local types...
 */

typedef struct usb_printer_s		/**** USB Printer Data ****/
{
  struct libusb_device	*device;	/* Device info */
  int			conf,		/* Configuration */
			iface,		/* Interface */
			altset,		/* Alternate setting */
			write_endp,	/* Write endpoint */
                        read_endp,	/* Read endpoint */
			protocol,	/* Protocol: 1 = Uni-di, 2 = Bi-di. */
                        usblp_attached; /* Is the "usblp" kernel module
					   attached? */
  struct libusb_device_handle *handle;	/* Open handle to device */
} usb_printer_t;

typedef int (*usb_cb_t)(usb_printer_t *, const char *, const char *,
                        const void *);

typedef struct usb_globals_s
{
  usb_printer_t		*printer;	/* Printer */

  pthread_mutex_t	read_thread_mutex;
  pthread_cond_t	read_thread_cond;
  int			read_thread_stop;
  int			read_thread_done;

  pthread_mutex_t	readwrite_lock_mutex;
  pthread_cond_t	readwrite_lock_cond;
  int			readwrite_lock;

  int			print_fd;	/* File descriptor to print */
  ssize_t		print_bytes;	/* Print bytes read */

  int			wait_eof;
  int			drain_output;	/* Drain all pending output */
  int			bidi_flag;	/* 0=unidirectional, 1=bidirectional */

  pthread_mutex_t	sidechannel_thread_mutex;
  pthread_cond_t	sidechannel_thread_cond;
  int			sidechannel_thread_stop;
  int			sidechannel_thread_done;
} usb_globals_t;


/*
 * Globals...
 */

usb_globals_t		g = { 0 };	/* Globals */
libusb_device           **list;         /* List of connected USB devices */


/*
 * Local functions...
 */

static int		close_device(usb_printer_t *printer);
static usb_printer_t	*find_device(usb_cb_t cb, const void *data);
static int		get_device_id(usb_printer_t *printer, char *buffer,
			              size_t bufsize);
static int		list_cb(usb_printer_t *printer, const char *device_uri,
			        const char *device_id, const void *data);
static char		*make_device_uri(usb_printer_t *printer,
			                 const char *device_id,
					 char *uri, size_t uri_size);
static int		open_device(usb_printer_t *printer, int verbose);
static int		print_cb(usb_printer_t *printer, const char *device_uri,
			         const char *device_id, const void *data);
static void		*read_thread(void *reference);
static void		*sidechannel_thread(void *reference);
static void		soft_reset(void);


/*
 * 'list_devices()' - List the available printers.
 */

void
list_devices(void)
{
  fputs("DEBUG: list_devices\n", stderr);
  find_device(list_cb, NULL);
}


/*
 * 'print_device()' - Print a file to a USB device.
 */

int					/* O - Exit status */
print_device(const char *uri,		/* I - Device URI */
             const char *hostname,	/* I - Hostname/manufacturer */
             const char *resource,	/* I - Resource/modelname */
	     char       *options,	/* I - Device options/serial number */
	     int        print_fd,	/* I - File descriptor to print */
	     int        copies,		/* I - Copies to print */
	     int	argc,		/* I - Number of command-line arguments (6 or 7) */
	     char	*argv[])	/* I - Command-line arguments */
{
  int	        bytes;			/* Bytes written */
  ssize_t	total_bytes;		/* Total bytes written */
  struct sigaction action;		/* Actions for POSIX signals */
  int		status = CUPS_BACKEND_OK,
					/* Function results */
		iostatus;		/* Current IO status */
  pthread_t	read_thread_id,		/* Read thread */
		sidechannel_thread_id;	/* Side-channel thread */
  int		have_sidechannel = 0;	/* Was the side-channel thread started? */
  struct stat   sidechannel_info;	/* Side-channel file descriptor info */
  unsigned char	print_buffer[8192],	/* Print data buffer */
		*print_ptr;		/* Pointer into print data buffer */
  fd_set	input_set;		/* Input set for select() */
  int		nfds;			/* Number of file descriptors */
  struct timeval *timeout,		/* Timeout pointer */
		tv;			/* Time value */
  struct timespec cond_timeout;		/* pthread condition timeout */


 /*
  * See if the side-channel descriptor is valid...
  */

  have_sidechannel = !fstat(CUPS_SC_FD, &sidechannel_info) &&
                     S_ISSOCK(sidechannel_info.st_mode);

  g.wait_eof = WAIT_EOF;

 /*
  * Connect to the printer...
  */

  while ((g.printer = find_device(print_cb, uri)) == NULL)
  {
    _cupsLangPrintFilter(stderr, "INFO",
			 _("Waiting for printer to become available."));
    sleep(5);
  }

  g.print_fd = print_fd;

 /*
  * If we are printing data from a print driver on stdin, ignore SIGTERM
  * so that the driver can finish out any page data, e.g. to eject the
  * current page.  We only do this for stdin printing as otherwise there
  * is no way to cancel a raw print job...
  */

  if (!print_fd)
  {
    memset(&action, 0, sizeof(action));

    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    sigaction(SIGTERM, &action, NULL);
  }

 /*
  * Start the side channel thread if the descriptor is valid...
  */

  pthread_mutex_init(&g.readwrite_lock_mutex, NULL);
  pthread_cond_init(&g.readwrite_lock_cond, NULL);
  g.readwrite_lock = 1;

  if (have_sidechannel)
  {
    g.sidechannel_thread_stop = 0;
    g.sidechannel_thread_done = 0;

    pthread_cond_init(&g.sidechannel_thread_cond, NULL);
    pthread_mutex_init(&g.sidechannel_thread_mutex, NULL);

    if (pthread_create(&sidechannel_thread_id, NULL, sidechannel_thread, NULL))
    {
      fprintf(stderr, "DEBUG: Fatal USB error.\n");
      _cupsLangPrintFilter(stderr, "ERROR",
			   _("There was an unrecoverable USB error."));
      fputs("DEBUG: Couldn't create side-channel thread.\n", stderr);
      close_device(g.printer);
      return (CUPS_BACKEND_STOP);
    }
  }

 /*
  * Get the read thread going...
  */

  g.read_thread_stop = 0;
  g.read_thread_done = 0;

  pthread_cond_init(&g.read_thread_cond, NULL);
  pthread_mutex_init(&g.read_thread_mutex, NULL);

  if (pthread_create(&read_thread_id, NULL, read_thread, NULL))
  {
    fprintf(stderr, "DEBUG: Fatal USB error.\n");
    _cupsLangPrintFilter(stderr, "ERROR",
                         _("There was an unrecoverable USB error."));
    fputs("DEBUG: Couldn't create read thread.\n", stderr);
    close_device(g.printer);
    return (CUPS_BACKEND_STOP);
  }

 /*
  * The main thread sends the print file...
  */

  g.drain_output = 0;
  g.print_bytes	 = 0;
  total_bytes	 = 0;
  print_ptr	 = print_buffer;

  while (status == CUPS_BACKEND_OK && copies-- > 0)
  {
    _cupsLangPrintFilter(stderr, "INFO", _("Sending data to printer."));

    if (print_fd != STDIN_FILENO)
    {
      fputs("PAGE: 1 1\n", stderr);
      lseek(print_fd, 0, SEEK_SET);
    }

    while (status == CUPS_BACKEND_OK)
    {
      FD_ZERO(&input_set);

      if (!g.print_bytes)
	FD_SET(print_fd, &input_set);

     /*
      * Calculate select timeout...
      *   If we have data waiting to send timeout is 100ms.
      *   else if we're draining print_fd timeout is 0.
      *   else we're waiting forever...
      */

      if (g.print_bytes)
      {
	tv.tv_sec  = 0;
	tv.tv_usec = 100000;		/* 100ms */
	timeout    = &tv;
      }
      else if (g.drain_output)
      {
	tv.tv_sec  = 0;
	tv.tv_usec = 0;
	timeout    = &tv;
      }
      else
	timeout = NULL;

     /*
      * I/O is unlocked around select...
      */

      pthread_mutex_lock(&g.readwrite_lock_mutex);
      g.readwrite_lock = 0;
      pthread_cond_signal(&g.readwrite_lock_cond);
      pthread_mutex_unlock(&g.readwrite_lock_mutex);

      nfds = select(print_fd + 1, &input_set, NULL, NULL, timeout);

     /*
      * Reacquire the lock...
      */

      pthread_mutex_lock(&g.readwrite_lock_mutex);
      while (g.readwrite_lock)
	pthread_cond_wait(&g.readwrite_lock_cond, &g.readwrite_lock_mutex);
      g.readwrite_lock = 1;
      pthread_mutex_unlock(&g.readwrite_lock_mutex);

      if (nfds < 0)
      {
	if (errno == EINTR && total_bytes == 0)
	{
	  fputs("DEBUG: Received an interrupt before any bytes were "
	        "written, aborting.\n", stderr);
	  close_device(g.printer);
          return (CUPS_BACKEND_OK);
	}
	else if (errno != EAGAIN && errno != EINTR)
	{
	  _cupsLangPrintFilter(stderr, "ERROR",
	                       _("Unable to read print data."));
	  perror("DEBUG: select");
	  close_device(g.printer);
          return (CUPS_BACKEND_FAILED);
	}
      }

     /*
      * If drain output has finished send a response...
      */

      if (g.drain_output && !nfds && !g.print_bytes)
      {
	/* Send a response... */
	cupsSideChannelWrite(CUPS_SC_CMD_DRAIN_OUTPUT, CUPS_SC_STATUS_OK, NULL, 0, 1.0);
	g.drain_output = 0;
      }

     /*
      * Check if we have print data ready...
      */

      if (FD_ISSET(print_fd, &input_set))
      {
	g.print_bytes = read(print_fd, print_buffer, sizeof(print_buffer));

	if (g.print_bytes < 0)
	{
	 /*
	  * Read error - bail if we don't see EAGAIN or EINTR...
	  */

	  if (errno != EAGAIN && errno != EINTR)
	  {
	    _cupsLangPrintFilter(stderr, "ERROR",
				 _("Unable to read print data."));
	    perror("DEBUG: read");
	    close_device(g.printer);
	    return (CUPS_BACKEND_FAILED);
	  }

	  g.print_bytes = 0;
	}
	else if (g.print_bytes == 0)
	{
	 /*
	  * End of file, break out of the loop...
	  */

	  break;
	}

	print_ptr = print_buffer;

	fprintf(stderr, "DEBUG: Read %d bytes of print data...\n",
		(int)g.print_bytes);
      }

      if (g.print_bytes)
      {
	iostatus = libusb_bulk_transfer(g.printer->handle,
					g.printer->write_endp,
					print_buffer, g.print_bytes,
					&bytes, 60000);
       /*
	* Ignore timeout errors, but retain the number of bytes written to
	* avoid sending duplicate data...
	*/

	if (iostatus == LIBUSB_ERROR_TIMEOUT)
	{
	  fputs("DEBUG: Got USB transaction timeout during write.\n", stderr);
	  iostatus = 0;
	}

       /*
        * If we've stalled, retry the write...
	*/

	else if (iostatus == LIBUSB_ERROR_PIPE)
	{
	  fputs("DEBUG: Got USB pipe stalled during write.\n", stderr);

	  iostatus = libusb_bulk_transfer(g.printer->handle,
					  g.printer->write_endp,
					  print_buffer, g.print_bytes,
					  &bytes, 60000);
	}

       /*
	* Retry a write after an aborted write since we probably just got
	* SIGTERM...
	*/

	else if (iostatus == LIBUSB_ERROR_INTERRUPTED)
	{
	  fputs("DEBUG: Got USB return aborted during write.\n", stderr);

	  iostatus = libusb_bulk_transfer(g.printer->handle,
					  g.printer->write_endp,
					  print_buffer, g.print_bytes,
					  &bytes, 60000);
        }

	if (iostatus)
	{
	 /*
	  * Write error - bail if we don't see an error we can retry...
	  */

	  _cupsLangPrintFilter(stderr, "ERROR",
	                       _("Unable to send data to printer."));
	  fprintf(stderr, "DEBUG: libusb write operation returned %x.\n",
	          iostatus);

	  status = CUPS_BACKEND_FAILED;
	  break;
	}
	else if (bytes > 0)
	{
	  fprintf(stderr, "DEBUG: Wrote %d bytes of print data...\n",
	          (int)bytes);

	  g.print_bytes -= bytes;
	  print_ptr   += bytes;
	  total_bytes += bytes;
	}
      }

      if (print_fd != 0 && status == CUPS_BACKEND_OK)
	fprintf(stderr, "DEBUG: Sending print file, " CUPS_LLFMT " bytes...\n",
		CUPS_LLCAST total_bytes);
    }
  }

  fprintf(stderr, "DEBUG: Sent " CUPS_LLFMT " bytes...\n",
          CUPS_LLCAST total_bytes);

 /*
  * Signal the side channel thread to exit...
  */

  if (have_sidechannel)
  {
    close(CUPS_SC_FD);
    pthread_mutex_lock(&g.readwrite_lock_mutex);
    g.readwrite_lock = 0;
    pthread_cond_signal(&g.readwrite_lock_cond);
    pthread_mutex_unlock(&g.readwrite_lock_mutex);

    g.sidechannel_thread_stop = 1;
    pthread_mutex_lock(&g.sidechannel_thread_mutex);

    if (!g.sidechannel_thread_done)
    {
      gettimeofday(&tv, NULL);
      cond_timeout.tv_sec  = tv.tv_sec + WAIT_SIDE_DELAY;
      cond_timeout.tv_nsec = tv.tv_usec * 1000;

      while (!g.sidechannel_thread_done)
      {
	if (pthread_cond_timedwait(&g.sidechannel_thread_cond,
				   &g.sidechannel_thread_mutex,
				   &cond_timeout) != 0)
	  break;
      }
    }

    pthread_mutex_unlock(&g.sidechannel_thread_mutex);
  }

 /*
  * Signal the read thread to exit then wait 7 seconds for it to complete...
  */

  g.read_thread_stop = 1;

  pthread_mutex_lock(&g.read_thread_mutex);

  if (!g.read_thread_done)
  {
    fputs("DEBUG: Waiting for read thread to exit...\n", stderr);

    gettimeofday(&tv, NULL);
    cond_timeout.tv_sec  = tv.tv_sec + WAIT_EOF_DELAY;
    cond_timeout.tv_nsec = tv.tv_usec * 1000;

    while (!g.read_thread_done)
    {
      if (pthread_cond_timedwait(&g.read_thread_cond, &g.read_thread_mutex,
				 &cond_timeout) != 0)
	break;
    }

   /*
    * If it didn't exit abort the pending read and wait an additional second...
    */
  
    if (!g.read_thread_done)
    {
      fputs("DEBUG: Read thread still active, aborting the pending read...\n", 
	    stderr);

      g.wait_eof = 0;

      gettimeofday(&tv, NULL);
      cond_timeout.tv_sec  = tv.tv_sec + 1;
      cond_timeout.tv_nsec = tv.tv_usec * 1000;
  
      while (!g.read_thread_done)
      {
	if (pthread_cond_timedwait(&g.read_thread_cond, &g.read_thread_mutex,
				   &cond_timeout) != 0)
	  break;
      }
    }
  }

  pthread_mutex_unlock(&g.read_thread_mutex);

  if (print_fd)
    close(print_fd);

 /*
  * Close the connection and input file and general clean up...
  */

  close_device(g.printer);

 /*
  * Clean up ....
  */

  libusb_free_device_list(list, 1);
  libusb_exit(NULL);

  return (status);
}


/*
 * 'close_device()' - Close the connection to the USB printer.
 */

static int				/* I - 0 on success, -1 on failure */
close_device(usb_printer_t *printer)	/* I - Printer */
{
  struct libusb_device_descriptor devdesc;
                                        /* Current device descriptor */
  struct libusb_config_descriptor *confptr;
                                        /* Pointer to current configuration */


  if (printer->handle)
  {
   /*
    * Release interfaces before closing so that we know all data is written
    * to the device...
    */

    int number;				/* Interface number */

    libusb_get_device_descriptor (printer->device, &devdesc);
    libusb_get_config_descriptor (printer->device, printer->conf, &confptr);
    number = confptr->interface[printer->iface].
                 altsetting[printer->altset].bInterfaceNumber;
    libusb_release_interface(printer->handle, number);
    if (number != 0)
      libusb_release_interface(printer->handle, 0);

   /*
    * Re-attach "usblp" kernel module if it was attached before using this
    * device
    */

    if (printer->usblp_attached == 1)
    {
      if (libusb_attach_kernel_driver(printer->handle, printer->iface) < 0)
	fprintf(stderr,
	        "DEBUG: Failed to re-attach \"usblp\" kernel module to "
	        "%04x:%04x\n", devdesc.idVendor, devdesc.idProduct);
    }

    libusb_free_config_descriptor(confptr);

   /*
    * Close the interface and return...
    */

    libusb_close(printer->handle);
    printer->handle = NULL;
  }

  return (0);
}


/*
 * 'find_device()' - Find or enumerate USB printers.
 */

static usb_printer_t *			/* O - Found printer */
find_device(usb_cb_t   cb,		/* I - Callback function */
            const void *data)		/* I - User data for callback */
{
  libusb_device         **list;         /* List of connected USB devices */
  libusb_device         *device = NULL;	/* Current device */
  struct libusb_device_descriptor devdesc;
                                        /* Current device descriptor */
  struct libusb_config_descriptor *confptr = NULL;
                                        /* Pointer to current configuration */
  const struct libusb_interface *ifaceptr = NULL;
                                        /* Pointer to current interface */
  const struct libusb_interface_descriptor *altptr = NULL;
					/* Pointer to current alternate setting */
  const struct libusb_endpoint_descriptor *endpptr = NULL;
					/* Pointer to current endpoint */
  ssize_t               numdevs,        /* number of connected devices */
                        i = 0;
  uint8_t		conf,		/* Current configuration */
			iface,		/* Current interface */
			altset,		/* Current alternate setting */
			protocol,	/* Current protocol */
			endp,		/* Current endpoint */
			read_endp,	/* Current read endpoint */
			write_endp;	/* Current write endpoint */
  char			device_id[1024],/* IEEE-1284 device ID */
			device_uri[1024];
					/* Device URI */
  static usb_printer_t	printer;	/* Current printer */


 /*
  * Initialize libusb...
  */

  libusb_init(NULL);
  numdevs = libusb_get_device_list(NULL, &list);
  fprintf(stderr, "DEBUG: libusb_get_device_list=%d\n", (int)numdevs);

 /*
  * Then loop through the devices it found...
  */

  if (numdevs > 0)
    for (i = 0; i < numdevs; i++)
    {
      device = list[i];

     /*
      * Ignore devices with no configuration data and anything that is not
      * a printer...
      */

      libusb_get_device_descriptor (device, &devdesc);

      if (!devdesc.bNumConfigurations || !devdesc.idVendor ||
          !devdesc.idProduct)
	continue;

      for (conf = 0; conf < devdesc.bNumConfigurations; conf ++)
      {
	if (libusb_get_config_descriptor (device, conf, &confptr) < 0)
	  continue;
        for (iface = 0, ifaceptr = confptr->interface;
	     iface < confptr->bNumInterfaces;
	     iface ++, ifaceptr ++)
        {
	 /*
	  * Some printers offer multiple interfaces...
	  */

          protocol   = 0;

	  for (altset = 0, altptr = ifaceptr->altsetting;
	       altset < ifaceptr->num_altsetting;
	       altset ++, altptr ++)
          {
	   /*
	    * Currently we only support unidirectional and bidirectional
	    * printers.  Future versions of this code will support the
	    * 1284.4 (packet mode) protocol as well.
	    */

	    if (altptr->bInterfaceClass != LIBUSB_CLASS_PRINTER ||
	        altptr->bInterfaceSubClass != 1 ||
		(altptr->bInterfaceProtocol != 1 &&	/* Unidirectional */
		 altptr->bInterfaceProtocol != 2) ||	/* Bidirectional */
		altptr->bInterfaceProtocol < protocol)
	      continue;

	    read_endp  = -1;
	    write_endp = -1;

	    for (endp = 0, endpptr = altptr->endpoint;
	         endp < altptr->bNumEndpoints;
		 endp ++, endpptr ++)
              if ((endpptr->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
	              LIBUSB_TRANSFER_TYPE_BULK)
	      {
	        if (endpptr->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)
		  read_endp = endp;
		else
		  write_endp = endp;
	      }

            if (write_endp >= 0)
	    {
	     /*
	      * Save the best match so far...
	      */

              protocol           = altptr->bInterfaceProtocol;
	      printer.altset     = altset;
	      printer.write_endp = write_endp;
	      printer.read_endp  = read_endp;
	    }
	  }

	  if (protocol > 0)
	  {
	    printer.device   = device;
	    printer.conf     = conf;
	    printer.iface    = iface;
	    printer.protocol = protocol;
	    printer.handle   = NULL;

            if (!open_device(&printer, data != NULL))
	    {
	      get_device_id(&printer, device_id, sizeof(device_id));
	      make_device_uri(&printer, device_id, device_uri,
			      sizeof(device_uri));

	      if ((*cb)(&printer, device_uri, device_id, data))
	      {
		printer.read_endp  = confptr->interface[printer.iface].
					   altsetting[printer.altset].
					   endpoint[printer.read_endp].
					   bEndpointAddress;
		printer.write_endp = confptr->interface[printer.iface].
					   altsetting[printer.altset].
					   endpoint[printer.write_endp].
					   bEndpointAddress;
		return (&printer);
              }

              close_device(&printer);
	    }
	  }
	}
	libusb_free_config_descriptor(confptr);
      }
    }

 /*
  * If we get this far without returning, then we haven't found a printer
  * to print to...
  */

 /*
  * Clean up ....
  */

  libusb_free_device_list(list, 1);
  libusb_exit(NULL);

  return (NULL);
}


/*
 * 'get_device_id()' - Get the IEEE-1284 device ID for the printer.
 */

static int				/* O - 0 on success, -1 on error */
get_device_id(usb_printer_t *printer,	/* I - Printer */
              char          *buffer,	/* I - String buffer */
              size_t        bufsize)	/* I - Number of bytes in buffer */
{
  int	length;				/* Length of device ID */


  if (libusb_control_transfer(printer->handle,
			      LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_IN |
			      LIBUSB_RECIPIENT_INTERFACE,
			      0, printer->conf,
			      (printer->iface << 8) | printer->altset,
			      (unsigned char *)buffer, bufsize, 5000) < 0)
  {
    *buffer = '\0';
    return (-1);
  }

 /*
  * Extract the length of the device ID string from the first two
  * bytes.  The 1284 spec says the length is stored MSB first...
  */

  length = (((unsigned)buffer[0] & 255) << 8) |
	   ((unsigned)buffer[1] & 255);

 /*
  * Check to see if the length is larger than our buffer or less than 14 bytes
  * (the minimum valid device ID is "MFG:x;MDL:y;" with 2 bytes for the length).
  *
  * If the length is out-of-range, assume that the vendor incorrectly
  * implemented the 1284 spec and re-read the length as LSB first,..
  */

  if (length > bufsize || length < 14)
    length = (((unsigned)buffer[1] & 255) << 8) |
	     ((unsigned)buffer[0] & 255);

  if (length > bufsize)
    length = bufsize;

  if (length < 14)
  {
   /*
    * Invalid device ID, clear it!
    */

    *buffer = '\0';
    return (-1);
  }

  length -= 2;

 /*
  * Copy the device ID text to the beginning of the buffer and
  * nul-terminate.
  */

  memmove(buffer, buffer + 2, length);
  buffer[length] = '\0';

  return (0);
}


/*
 * 'list_cb()' - List USB printers for discovery.
 */

static int				/* O - 0 to continue, 1 to stop */
list_cb(usb_printer_t *printer,		/* I - Printer */
        const char    *device_uri,	/* I - Device URI */
        const char    *device_id,	/* I - IEEE-1284 device ID */
        const void    *data)		/* I - User data (not used) */
{
  char	make_model[1024];		/* Make and model */


 /*
  * Get the device URI and make/model strings...
  */

  if (backendGetMakeModel(device_id, make_model, sizeof(make_model)))
    strlcpy(make_model, "Unknown", sizeof(make_model));

 /*
  * Report the printer...
  */

  cupsBackendReport("direct", device_uri, make_model, make_model, device_id,
                    NULL);

 /*
  * Keep going...
  */

  return (0);
}


/*
 * 'make_device_uri()' - Create a device URI for a USB printer.
 */

static char *				/* O - Device URI */
make_device_uri(
    usb_printer_t *printer,		/* I - Printer */
    const char    *device_id,		/* I - IEEE-1284 device ID */
    char          *uri,			/* I - Device URI buffer */
    size_t        uri_size)		/* I - Size of device URI buffer */
{
  struct libusb_device_descriptor devdesc;
                                        /* Current device descriptor */
  char		options[1024];		/* Device URI options */
  int		num_values;		/* Number of 1284 parameters */
  cups_option_t	*values;		/* 1284 parameters */
  const char	*mfg,			/* Manufacturer */
		*mdl,			/* Model */
		*des = NULL,		/* Description */
		*sern;			/* Serial number */
  size_t	mfglen;			/* Length of manufacturer string */
  char		tempmfg[256],		/* Temporary manufacturer string */
		tempsern[256],		/* Temporary serial number string */
		*tempptr;		/* Pointer into temp string */


 /*
  * Get the make, model, and serial numbers...
  */

  num_values = _cupsGet1284Values(device_id, &values);

  if ((sern = cupsGetOption("SERIALNUMBER", num_values, values)) == NULL)
    if ((sern = cupsGetOption("SERN", num_values, values)) == NULL)
      if ((sern = cupsGetOption("SN", num_values, values)) == NULL &&
	  ((libusb_get_device_descriptor (printer->device, &devdesc) >= 0) &&
	   devdesc.iSerialNumber))
      {
       /*
        * Try getting the serial number from the device itself...
	*/

        int length =
	  libusb_get_string_descriptor_ascii(printer->handle,
					     devdesc.iSerialNumber,
					     (unsigned char *)tempsern,
					     sizeof(tempsern) - 1);
        if (length > 0)
	{
	  tempsern[length] = '\0';
	  sern             = tempsern;
	}
      }

  if ((mfg = cupsGetOption("MANUFACTURER", num_values, values)) == NULL)
    mfg = cupsGetOption("MFG", num_values, values);

  if ((mdl = cupsGetOption("MODEL", num_values, values)) == NULL)
    mdl = cupsGetOption("MDL", num_values, values);

 /*
  * To maintain compatibility with the original character device backend on
  * Linux and *BSD, map manufacturer names...
  */

  if (mfg)
  {
    if (!_cups_strcasecmp(mfg, "Hewlett-Packard"))
      mfg = "HP";
    else if (!_cups_strcasecmp(mfg, "Lexmark International"))
      mfg = "Lexmark";
  }
  else
  {
   /*
    * No manufacturer?  Use the model string or description...
    */

    if (mdl)
      _ppdNormalizeMakeAndModel(mdl, tempmfg, sizeof(tempmfg));
    else if ((des = cupsGetOption("DESCRIPTION", num_values, values)) != NULL ||
             (des = cupsGetOption("DES", num_values, values)) != NULL)
      _ppdNormalizeMakeAndModel(des, tempmfg, sizeof(tempmfg));
    else
      strlcpy(tempmfg, "Unknown", sizeof(tempmfg));

    if ((tempptr = strchr(tempmfg, ' ')) != NULL)
      *tempptr = '\0';

    mfg = tempmfg;
  }

  if (!mdl)
  {
   /*
    * No model?  Use description...
    */
    if (des)
      mdl = des; /* We remove the manufacturer name below */
    else if (!strncasecmp(mfg, "Unknown", 7))
      mdl = "Printer";
    else
      mdl = "Unknown Model";
  }

  mfglen = strlen(mfg);

  if (!strncasecmp(mdl, mfg, mfglen) && _cups_isspace(mdl[mfglen]))
  {
    mdl += mfglen + 1;

    while (_cups_isspace(*mdl))
      mdl ++;
  }

 /*
  * Generate the device URI from the manufacturer, model, serial number,
  * and interface number...
  */

  if (sern)
  {
    if (printer->iface > 0)
      snprintf(options, sizeof(options), "?serial=%s&interface=%d", sern,
               printer->iface);
    else
      snprintf(options, sizeof(options), "?serial=%s", sern);
  }
  else if (printer->iface > 0)
    snprintf(options, sizeof(options), "?interface=%d", printer->iface);
  else
    options[0] = '\0';

  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, uri_size, "usb", NULL, mfg, 0,
		   "/%s%s", mdl, options);

  cupsFreeOptions(num_values, values);

  return (uri);
}


/*
 * 'open_device()' - Open a connection to the USB printer.
 */

static int				/* O - 0 on success, -1 on error */
open_device(usb_printer_t *printer,	/* I - Printer */
            int           verbose)	/* I - Update connecting-to-device state? */
{
  struct libusb_device_descriptor devdesc;
                                        /* Current device descriptor */
  struct libusb_config_descriptor *confptr = NULL;
                                        /* Pointer to current configuration */
  int	number1 = -1,			/* Configuration/interface/altset */
        number2 = -1,			/* numbers */
        errcode = 0;
  char	current;			/* Current configuration */


 /*
  * Return immediately if we are already connected...
  */

  if (printer->handle)
    return (0);

 /*
  * Try opening the printer...
  */

  if (libusb_open(printer->device, &printer->handle) < 0)
    return (-1);

  if (verbose)
    fputs("STATE: +connecting-to-device\n", stderr);

 /*
  * Set the desired configuration, but only if it needs changing. Some
  * printers (e.g., Samsung) don't like libusb_set_configuration. It will
  * succeed, but the following print job is sometimes silently lost by the
  * printer.
  */

  if (libusb_control_transfer(printer->handle,
                LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_ENDPOINT_IN |
		LIBUSB_RECIPIENT_DEVICE,
		8, /* GET_CONFIGURATION */
		0, 0, (unsigned char *)&current, 1, 5000) < 0)
    current = 0;			/* Assume not configured */

  libusb_get_device_descriptor (printer->device, &devdesc);
  libusb_get_config_descriptor (printer->device, printer->conf, &confptr);
  number1 = confptr->bConfigurationValue;

  if (number1 != current)
  {
    if ((errcode = libusb_set_configuration(printer->handle, number1)) < 0)
    {
     /*
      * If the set fails, chances are that the printer only supports a
      * single configuration.  Technically these printers don't conform to
      * the USB printer specification, but otherwise they'll work...
      */

      if (errcode != LIBUSB_ERROR_BUSY)
        fprintf(stderr, "DEBUG: Failed to set configuration %d for %04x:%04x\n",
		number1, devdesc.idVendor, devdesc.idProduct);
    }
  }

 /*
  * Get the "usblp" kernel module out of the way. This backend only
  * works without the module attached.
  */

  errcode = libusb_kernel_driver_active(printer->handle, printer->iface);
  if (errcode == 0)
    printer->usblp_attached = 0;
  else if (errcode == 1)
  {
    printer->usblp_attached = 1;
    if ((errcode =
	 libusb_detach_kernel_driver(printer->handle, printer->iface)) < 0)
    {
      fprintf(stderr, "DEBUG: Failed to detach \"usblp\" module from %04x:%04x\n",
	      devdesc.idVendor, devdesc.idProduct);
      goto error;
    }
  }
  else
  {
    printer->usblp_attached = 0;
    fprintf(stderr, "DEBUG: Failed to check whether %04x:%04x has the \"usblp\" kernel module attached\n",
	      devdesc.idVendor, devdesc.idProduct);
    goto error;
  }

 /*
  * Claim interfaces as needed...
  */

  number1 = confptr->interface[printer->iface].
    altsetting[printer->altset].bInterfaceNumber;

  while ((errcode = libusb_claim_interface(printer->handle, number1)) < 0)
  {
    if (errcode != LIBUSB_ERROR_BUSY)
      fprintf(stderr,
              "DEBUG: Failed to claim interface %d for %04x:%04x: %s\n",
              number1, devdesc.idVendor, devdesc.idProduct, strerror(errno));

    goto error;
  }

 /*
  * Set alternate setting, but only if there is more than one option.  Some
  * printers (e.g., Samsung) don't like usb_set_altinterface.
  */

  if (confptr->interface[printer->iface].num_altsetting > 1)
  {
    number1 = confptr->interface[printer->iface].
                 altsetting[printer->altset].bInterfaceNumber;
    number2 = confptr->interface[printer->iface].
                 altsetting[printer->altset].bAlternateSetting;

    while ((errcode =
	    libusb_set_interface_alt_setting(printer->handle, number1, number2))
	   < 0)
    {
      if (errcode != LIBUSB_ERROR_BUSY)
        fprintf(stderr,
                "DEBUG: Failed to set alternate interface %d for %04x:%04x: "
                "%s\n",
                number2, devdesc.idVendor, devdesc.idProduct, strerror(errno));

      goto error;
    }
  }

  libusb_free_config_descriptor(confptr);

  if (verbose)
    fputs("STATE: -connecting-to-device\n", stderr);

  return (0);

 /*
  * If we get here, there was a hard error...
  */

  error:

  if (verbose)
    fputs("STATE: -connecting-to-device\n", stderr);

  libusb_close(printer->handle);
  printer->handle = NULL;

  return (-1);
}


/*
 * 'print_cb()' - Find a USB printer for printing.
 */

static int				/* O - 0 to continue, 1 to stop (found) */
print_cb(usb_printer_t *printer,	/* I - Printer */
         const char    *device_uri,	/* I - Device URI */
         const char    *device_id,	/* I - IEEE-1284 device ID */
         const void    *data)		/* I - User data (make, model, S/N) */
{
  char	requested_uri[1024],		/* Requested URI */
	*requested_ptr,			/* Pointer into requested URI */
	detected_uri[1024],		/* Detected URI */
	*detected_ptr;			/* Pointer into detected URI */


 /*
  * If we have an exact match, stop now...
  */

  if (!strcmp((char *)data, device_uri))
    return (1);

 /*
  * Work on copies of the URIs...
  */

  strlcpy(requested_uri, (char *)data, sizeof(requested_uri));
  strlcpy(detected_uri, device_uri, sizeof(detected_uri));

 /*
  * libusb-discovered URIs can have an "interface" specification and this
  * never happens for usblp-discovered URIs, so remove the "interface"
  * specification from the URI which we are checking currently. This way a
  * queue for a usblp-discovered printer can now be accessed via libusb.
  *
  * Similarly, strip "?serial=NNN...NNN" as needed.
  */

  if ((requested_ptr = strstr(requested_uri, "?interface=")) == NULL)
    requested_ptr = strstr(requested_uri, "&interface=");
  if ((detected_ptr = strstr(detected_uri, "?interface=")) == NULL)
    detected_ptr = strstr(detected_uri, "&interface=");

  if (!requested_ptr && detected_ptr)
  {
   /*
    * Strip "[?&]interface=nnn" from the detected printer.
    */

    *detected_ptr = '\0';
  }
  else if (requested_ptr && !detected_ptr)
  {
   /*
    * Strip "[?&]interface=nnn" from the requested printer.
    */

    *requested_ptr = '\0';
  }

  if ((requested_ptr = strstr(requested_uri, "?serial=?")) != NULL)
  {
   /*
    * Strip "?serial=?" from the requested printer.  This is a special
    * case, as "?serial=?" means no serial number and not the serial
    * number '?'.  This is not covered by the checks below...
    */

    *requested_ptr = '\0';
  }

  if ((requested_ptr = strstr(requested_uri, "?serial=")) == NULL &&
      (detected_ptr = strstr(detected_uri, "?serial=")) != NULL)
  {
   /*
    * Strip "?serial=nnn" from the detected printer.
    */

    *detected_ptr = '\0';
  }
  else if (requested_ptr && !detected_ptr)
  {
   /*
    * Strip "?serial=nnn" from the requested printer.
    */

    *requested_ptr = '\0';
  }

  return (!strcmp(requested_uri, detected_uri));
}


/*
 * 'read_thread()' - Thread to read the backchannel data on.
 */

static void *read_thread(void *reference)
{
  unsigned char		readbuffer[512];
  int			rbytes;
  int			readstatus;
  struct timeval	now,
			delay,
			end,
			timeleft;


  (void)reference;

 /*
  * Read frequency: once every 250 milliseconds.
  */

  delay.tv_sec = 0;
  delay.tv_usec = 250000;

  do
  {
   /*
    * Remember when we started so we can throttle the loop after the read
    * call...
    */

    gettimeofday(&now, NULL);

   /*
    * Calculate what 250 milliSeconds are in absolute time...
    */

    timeradd(&now, &delay, &end);

    rbytes     = sizeof(readbuffer);
    readstatus = libusb_bulk_transfer(g.printer->handle,
				      g.printer->read_endp,
				      readbuffer, rbytes,
				      &rbytes, 60000);
    if (readstatus == LIBUSB_SUCCESS && rbytes > 0)
    {
      fprintf(stderr, "DEBUG: Read %d bytes of back-channel data...\n",
              (int)rbytes);
      cupsBackChannelWrite((const char *)readbuffer, rbytes, 1.0);
    }
    else if (readstatus == LIBUSB_ERROR_TIMEOUT)
      fputs("DEBUG: Got USB transaction timeout during read.\n", stderr);
    else if (readstatus == LIBUSB_ERROR_PIPE)
      fputs("DEBUG: Got USB pipe stalled during read.\n", stderr);
    else if (readstatus == LIBUSB_ERROR_INTERRUPTED)
      fputs("DEBUG: Got USB return aborted during read.\n", stderr);

   /*
    * Make sure this loop executes no more than once every 250 miliseconds...
    */

    if ((readstatus != LIBUSB_SUCCESS || rbytes == 0) &&
        (g.wait_eof || !g.read_thread_stop))
    {
      gettimeofday(&now, NULL);
      if (timercmp(&now, &end, <))
      {
	timersub(&end, &now, &timeleft);
	usleep(1000000 * timeleft.tv_sec + timeleft.tv_usec);
      }
    }
  } while (g.wait_eof || !g.read_thread_stop);

 /*
  * Let the main thread know that we have completed the read thread...
  */

  pthread_mutex_lock(&g.read_thread_mutex);
  g.read_thread_done = 1;
  pthread_cond_signal(&g.read_thread_cond);
  pthread_mutex_unlock(&g.read_thread_mutex);

  return (NULL);
}


/*
 * 'sidechannel_thread()' - Handle side-channel requests.
 */

static void*
sidechannel_thread(void *reference)
{
  cups_sc_command_t	command;	/* Request command */
  cups_sc_status_t	status;		/* Request/response status */
  char			data[2048];	/* Request/response data */
  int			datalen;	/* Request/response data size */


  (void)reference;

  do
  {
    datalen = sizeof(data);

    if (cupsSideChannelRead(&command, &status, data, &datalen, 1.0))
    {
      if (status == CUPS_SC_STATUS_TIMEOUT)
	continue;
      else
	break;
    }

    switch (command)
    {
      case CUPS_SC_CMD_SOFT_RESET:	/* Do a soft reset */
	  fputs("DEBUG: CUPS_SC_CMD_SOFT_RESET received from driver...\n",
		stderr);

	  soft_reset();
	  cupsSideChannelWrite(command, CUPS_SC_STATUS_OK, NULL, 0, 1.0);
	  fputs("DEBUG: Returning status CUPS_STATUS_OK with no bytes...\n",
		stderr);
	  break;

      case CUPS_SC_CMD_DRAIN_OUTPUT:	/* Drain all pending output */
	  fputs("DEBUG: CUPS_SC_CMD_DRAIN_OUTPUT received from driver...\n",
		stderr);

	  g.drain_output = 1;
	  break;

      case CUPS_SC_CMD_GET_BIDI:	/* Is the connection bidirectional? */
	  fputs("DEBUG: CUPS_SC_CMD_GET_BIDI received from driver...\n",
		stderr);

	  data[0] = (g.printer->protocol >= 2 ? 1 : 0);
	  cupsSideChannelWrite(command, CUPS_SC_STATUS_OK, data, 1, 1.0);

	  fprintf(stderr,
	          "DEBUG: Returned CUPS_SC_STATUS_OK with 1 byte (%02X)...\n",
		  data[0]);
	  break;

      case CUPS_SC_CMD_GET_DEVICE_ID:	/* Return IEEE-1284 device ID */
	  fputs("DEBUG: CUPS_SC_CMD_GET_DEVICE_ID received from driver...\n",
		stderr);

	  datalen = sizeof(data);
	  if (get_device_id(g.printer, data, sizeof(data)))
	  {
	    status  = CUPS_SC_STATUS_IO_ERROR;
	    datalen = 0;
	  }
	  else
	  {
	    status  = CUPS_SC_STATUS_OK;
	    datalen = strlen(data);
	  }
	  cupsSideChannelWrite(command, CUPS_SC_STATUS_OK, data, datalen, 1.0);

          if (datalen < sizeof(data))
	    data[datalen] = '\0';
	  else
	    data[sizeof(data) - 1] = '\0';

	  fprintf(stderr,
	          "DEBUG: Returning CUPS_SC_STATUS_OK with %d bytes (%s)...\n",
		  datalen, data);
	  break;

      case CUPS_SC_CMD_GET_STATE:	/* Return device state */
	  fputs("DEBUG: CUPS_SC_CMD_GET_STATE received from driver...\n",
		stderr);

	  data[0] = CUPS_SC_STATE_ONLINE;
	  cupsSideChannelWrite(command, CUPS_SC_STATUS_OK, data, 1, 1.0);

	  fprintf(stderr,
	          "DEBUG: Returned CUPS_SC_STATUS_OK with 1 byte (%02X)...\n",
		  data[0]);
	  break;

      case CUPS_SC_CMD_GET_CONNECTED:	/* Return whether device is
					   connected */
	  fputs("DEBUG: CUPS_SC_CMD_GET_CONNECTED received from driver...\n",
		stderr);

	  data[0] = (g.printer->handle ? 1 : 0);
	  cupsSideChannelWrite(command, CUPS_SC_STATUS_OK, data, 1, 1.0);

	  fprintf(stderr,
	          "DEBUG: Returned CUPS_SC_STATUS_OK with 1 byte (%02X)...\n",
		  data[0]);
	  break;

      default:
	  fprintf(stderr, "DEBUG: Unknown side-channel command (%d) received "
			  "from driver...\n", command);

	  cupsSideChannelWrite(command, CUPS_SC_STATUS_NOT_IMPLEMENTED,
			       NULL, 0, 1.0);

	  fputs("DEBUG: Returned CUPS_SC_STATUS_NOT_IMPLEMENTED with no bytes...\n",
		stderr);
	  break;
    }
  }
  while (!g.sidechannel_thread_stop);

  pthread_mutex_lock(&g.sidechannel_thread_mutex);
  g.sidechannel_thread_done = 1;
  pthread_cond_signal(&g.sidechannel_thread_cond);
  pthread_mutex_unlock(&g.sidechannel_thread_mutex);

  return (NULL);
}


/*
 * 'soft_reset()' - Send a soft reset to the device.
 */

static void soft_reset(void)
{
  fd_set	  input_set;		/* Input set for select() */
  struct timeval  tv;			/* Time value */
  char		  buffer[2048];		/* Buffer */
  struct timespec cond_timeout;		/* pthread condition timeout */

 /*
  * Send an abort once a second until the I/O lock is released by the main
  * thread...
  */

  pthread_mutex_lock(&g.readwrite_lock_mutex);
  while (g.readwrite_lock)
  {
    gettimeofday(&tv, NULL);
    cond_timeout.tv_sec  = tv.tv_sec + 1;
    cond_timeout.tv_nsec = tv.tv_usec * 1000;

    while (g.readwrite_lock)
    {
      if (pthread_cond_timedwait(&g.readwrite_lock_cond,
				 &g.readwrite_lock_mutex,
				 &cond_timeout) != 0)
	break;
    }
  }

  g.readwrite_lock = 1;
  pthread_mutex_unlock(&g.readwrite_lock_mutex);

 /*
  * Flush bytes waiting on print_fd...
  */

  g.print_bytes = 0;

  FD_ZERO(&input_set);
  FD_SET(g.print_fd, &input_set);

  tv.tv_sec  = 0;
  tv.tv_usec = 0;

  while (select(g.print_fd+1, &input_set, NULL, NULL, &tv) > 0)
    if (read(g.print_fd, buffer, sizeof(buffer)) <= 0)
      break;

 /*
  * Send the reset...
  */

  libusb_reset_device (g.printer->handle);

 /*
  * Release the I/O lock...
  */

  pthread_mutex_lock(&g.readwrite_lock_mutex);
  g.readwrite_lock = 0;
  pthread_cond_signal(&g.readwrite_lock_cond);
  pthread_mutex_unlock(&g.readwrite_lock_mutex);
}


/*
 * End of "$Id: usb-libusb.c 3648 2012-02-13 17:37:38Z msweet $".
 */

