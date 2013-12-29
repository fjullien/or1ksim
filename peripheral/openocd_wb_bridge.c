/* openocd_wb_bridge.c -- openocd_wb_bridge external peripheral

   Copyright (C) 2013 Franck Jullien, franck.jullien@gmail.com

   This file is part of OpenRISC 1000 Architectural Simulator.
  
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3 of the License, or (at your option)
   any later version.
  
   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.
  
   You should have received a copy of the GNU General Public License along
   with this program.  If not, see <http://www.gnu.org/licenses/>. */

/* This program is commented throughout in a fashion suitable for processing
   with Doxygen. */

/* Autoconf and/or portability configuration */
#include "config.h"

/* System includes */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/types.h>

/* Package includes */
#include "arch.h"
#include "sim-config.h"
#include "abstract.h"
#include "toplevel-support.h"
#include "sim-cmd.h"

#define EOC	"\x1a"		/* End Of Command delimiter */

/*! State associated with the wb_bridge device. */
struct dev_wb_bridge
{
  uint32_t value;		/* The value to read/write */

  /* Configuration */

  int enabled;			/* Device enabled */
  char *name;			/* Name of the device */
  oraddr_t baseaddr;		/* Base address of device */
  uint32_t size;		/* Address space size (bytes) */
  int sock;
  char *server_addr;		/* OpenOCD TCL server address */
  int server_port;		/* OpenOCD TCL server port */
};

int
init_connection (char *server_ip, int server_port, int *sock)
{
  struct sockaddr_in openocd_server;
  int ret;

  /* Create the TCP socket */
  *sock = socket (PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*sock < 0)
    {
      printf ("Failed to create socket\n");
      return -1;
    }

  /* Construct the server sockaddr_in structure */
  memset (&openocd_server, 0, sizeof (openocd_server));
  openocd_server.sin_family = AF_INET;
  openocd_server.sin_addr.s_addr = inet_addr (server_ip);
  openocd_server.sin_port = htons (server_port);

  /* Establish connection */
  ret =
    connect (*sock, (struct sockaddr *) &openocd_server,
	     sizeof (openocd_server));
  if (ret)
    {
      printf ("Failed to connect with server\n");
      return ret;
    }

  return 0;
}

char *
build_write_cmd (uint32_t address, void *data, int size)
{
  char *cmd_buffer;
  char *write_cmd;
  char temp[12];
  int value;

  int cmd_len = strlen ("mww ") + strlen ("0x00000000 ") + strlen (EOC);

  switch (size)
    {
    case 1:
      write_cmd = "mwb ";
      cmd_len += strlen ("0x00 ");
      value = *((uint8_t *) data);
      break;
    case 2:
      write_cmd = "mwh ";
      cmd_len += strlen ("0x0000 ");
      value = *((uint16_t *) data);
      break;
    case 4:
      write_cmd = "mww ";
      cmd_len += strlen ("0x00000000 ");
      value = *((uint32_t *) data);
      break;
    default:
      return NULL;
    }

  cmd_buffer = calloc (cmd_len + 1, 1);
  cmd_buffer = strcat (cmd_buffer, write_cmd);

  sprintf (temp, "0x%08X ", address);
  cmd_buffer = strcat (cmd_buffer, temp);

  sprintf (temp, "0x%0*X ", size * 2, value);
  cmd_buffer = strcat (cmd_buffer, temp);

  cmd_buffer = strcat (cmd_buffer, EOC);

  return cmd_buffer;
}

char *
build_read_cmd (uint32_t address, int size)
{
  char *cmd_buffer;
  char *read_cmd;
  char temp[12];

  int cmd_len = strlen ("mdw ") + strlen ("0x00000000 ") + strlen (EOC);

  switch (size)
    {
    case 1:
      read_cmd = "mdb ";
      break;
    case 2:
      read_cmd = "mdh ";
      break;
    case 4:
      read_cmd = "mdw ";
      break;
    default:
      return NULL;
    }

  cmd_buffer = calloc (cmd_len + 1, 1);
  cmd_buffer = strcat (cmd_buffer, read_cmd);

  sprintf (temp, "0x%08X ", address);
  cmd_buffer = strcat (cmd_buffer, temp);

  cmd_buffer = strcat (cmd_buffer, EOC);

  return cmd_buffer;
}

int
send_cmd (int sock, char *cmd)
{
  int ret = send (sock, cmd, strlen (cmd), 0);

  if (ret != strlen (cmd))
    ret = -1;

  free (cmd);
  return ret;
}

#define VAL_OFFSET	12
#define BUFFER_SIZE	1024

int
recv_cmd (int sock, void *buffer, int size)
{
  char _buffer[BUFFER_SIZE];
  int i = 0;
  uint8_t *ptr = (uint8_t *) buffer;
  char *inptr = _buffer;
  char byte[3] = { 0, 0, 0 };
  int eoc = 0;

  while (eoc == 0)
    {
      int bytes = recv (sock, inptr, BUFFER_SIZE - 1, 0);
      if (bytes < 0)
	{
	  printf ("Failed to receive bytes from server");
	  return -1;
	}

      /* Sometimes the EOC is available on the next frame. If we don't
       * receive the EOC in this frame, received another one. */
      for (i = 0; i < bytes; i++)
	{
	  if (inptr[i] == 0x1A)
	    eoc = 1;
	}
      inptr += bytes;
    }

  if (!buffer)
    return 0;

  for (i = 0; i < size; i++)
    {
      strncpy (byte, &_buffer[VAL_OFFSET + (i * 2)], 2);
      *ptr++ = strtoul (byte, NULL, 16);
    }

  return 0;
}

int
wb_read (int sock, uint32_t address, void *buffer, int size)
{
  char *cmd = build_read_cmd (address, size);

  int ret = send_cmd (sock, cmd);
  if (ret == -1)
    {
      printf ("Error while sending the command\n");
      return -1;
    }

  ret = recv_cmd (sock, buffer, size);
  if (ret)
    {
      printf ("Error while receiving the command\n");
      return -1;
    }

  return 0;
}

int
wb_write (int sock, uint32_t address, void *buffer, int size)
{
  char *cmd = build_write_cmd (address, buffer, size);

  int ret = send_cmd (sock, cmd);
  if (ret == -1)
    {
      printf ("Error while sending the command\n");
      return -1;
    }

  ret = recv_cmd (sock, NULL, 0);
  if (ret)
    {
      printf ("Error while receiving the command\n");
      return -1;
    }

  return 0;
}

/* --------------------------------------------------------------------------*/
/*!Read a byte from an external device

   To model Wishbone accurately, we always do this as a 4-byte access, with a
   mask for the bytes we don't want.

   Since this is only a byte, the endianess of the result is irrelevant.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to read from (host endian).
   @param[in] dat   The device data structure

   @return  The byte read.                                                   */
/* --------------------------------------------------------------------------*/
static uint8_t
wb_bridge_read_byte (oraddr_t addr, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr, "Byte read  out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
      return 0;
    }
  else
    {
      unsigned long int fulladdr = (unsigned long int) (addr + dev->baseaddr);
      int buffer;
      wb_read (dev->sock, fulladdr, &buffer, 1);
      return buffer;		/* read value */
    }
}				/* wb_bridge_read_byte() */


/* --------------------------------------------------------------------------*/
/*!Write a byte to an external device

   To model Wishbone accurately, we always do this as a 4-byte access, with a
   mask for the bytes we don't want.

   Since this is only a byte, the endianess of the value is irrelevant.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to write to (host endian)
   @param[in] value The byte value to write
   @param[in] dat   The device data structure                                */
/* --------------------------------------------------------------------------*/
static void
wb_bridge_write_byte (oraddr_t addr, uint8_t value, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr, "Byte written out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
    }
  else
    {
      unsigned long int fulladdr = (unsigned long int) (addr + dev->baseaddr);
      wb_write (dev->sock, fulladdr, &value, 1);
    }
}				/* wb_bridge_write_byte() */


/* --------------------------------------------------------------------------*/
/*!Read a half word from an external device

   To model Wishbone accurately, we always do this as a 4-byte access, with a
   mask for the bytes we don't want.

   Since this is a half word, the result must be converted to host endianess.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to read from (host endian).
   @param[in] dat   The device data structure.

   @return  The half word read (host endian).                                */
/* --------------------------------------------------------------------------*/
static uint16_t
wb_bridge_read_hw (oraddr_t addr, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr, "Half-word read  out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
      return 0;
    }
  else if (addr & 0x1)
    {
      /* This should be trapped elsewhere - here for safety. */
      fprintf (stderr,
	       "Unaligned half word read from 0x%" PRIxADDR " ignored\n",
	       addr);
      return 0;
    }
  else
    {
      unsigned long int fulladdr = (unsigned long int) (addr + dev->baseaddr);
      unsigned char res[2];
      wb_read (dev->sock, fulladdr, res, 2);

      /* Result converted according to endianess */
#ifdef OR32_BIG_ENDIAN
      return (unsigned short int) res[0] << 8 | (unsigned short int) res[1];
#else
      return (unsigned short int) res[1] << 8 | (unsigned short int) res[0];
#endif
    }
}				/* wb_bridge_read_hw() */


/* --------------------------------------------------------------------------*/
/*!Write a half word to an external device

   To model Wishbone accurately, we always do this as a 4-byte access, with a
   mask for the bytes we don't want.

   Since this is a half word, the value must be converted from host endianess.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to write to (host endian).
   @param[in] value The half word value to write (model endian).
   @param[in] dat   The device data structure.                               */
/* --------------------------------------------------------------------------*/
static void
wb_bridge_write_hw (oraddr_t addr, uint16_t value, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr,
	       "Half-word written  out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
    }
  else if (addr & 0x1)
    {
      fprintf (stderr,
	       "Unaligned half word write to 0x%" PRIxADDR " ignored\n",
	       addr);
    }
  else
    {
      unsigned long int fulladdr = (unsigned long int) (addr + dev->baseaddr);
      wb_write (dev->sock, fulladdr, &value, 2);
    }
}				/* wb_bridge_write_hw() */


/* --------------------------------------------------------------------------*/
/*!Read a full word from an external device

   Since this is a full word, the result must be converted to host endianess.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to read from (host endian).
   @param[in] dat   The device data structure.

   @return  The full word read (host endian).                                */
/* --------------------------------------------------------------------------*/
static uint32_t
wb_bridge_read_word (oraddr_t addr, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr, "Full word read  out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
      return 0;
    }
  else if (0 != (addr & 0x3))
    {
      fprintf (stderr,
	       "Unaligned full word read from 0x%" PRIxADDR " ignored\n",
	       addr);
      return 0;
    }
  else
    {
      unsigned long int wordaddr = (unsigned long int) (addr + dev->baseaddr);
      unsigned char res[4];
      wb_read (dev->sock, wordaddr, res, 4);

#ifdef OR32_BIG_ENDIAN
      return (unsigned long int) res[0] << 24 |
	(unsigned long int) res[1] << 16 |
	(unsigned long int) res[2] << 8 | (unsigned long int) res[3];
#else
      return (unsigned long int) res[3] << 24 |
	(unsigned long int) res[2] << 16 |
	(unsigned long int) res[1] << 8 | (unsigned long int) res[0];
#endif
    }
}				/* wb_bridge_read_word() */


/* --------------------------------------------------------------------------*/
/*!Write a full word to an external device

   Since this is a half word, the value must be converted from host endianess.

   @note We are passed the device address, but we must convert it to a full
         address for external use, to allow the single upcall handler to
         decode multiple wb_bridge devices.

   @param[in] addr  The device address to write to (host endian).
   @param[in] value The full word value to write (host endian).
   @param[in] dat   The device data structure.                               */
/* --------------------------------------------------------------------------*/
static void
wb_bridge_write_word (oraddr_t addr, uint32_t value, void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  if (addr >= dev->size)
    {
      fprintf (stderr,
	       "Full word written  out of range for wb_bridge device %s "
	       "(addr %" PRIxADDR ")\n", dev->name, addr);
    }
  else if (0 != (addr & 0x3))
    {
      fprintf (stderr,
	       "Unaligned full word write to 0x%" PRIxADDR " ignored\n",
	       addr);
    }
  else
    {
      unsigned long int wordaddr = (unsigned long int) (addr + dev->baseaddr);
      wb_write (dev->sock, wordaddr, &value, 4);
    }
}				/* wb_bridge_write_word() */


/* Reset is a null operation */

static void
wb_bridge_reset (void *dat)
{
  return;

}				/* wb_bridge_reset() */


/* Status report can only advise of configuration. */

static void
wb_bridge_status (void *dat)
{
  struct dev_wb_bridge *dev = (struct dev_wb_bridge *) dat;

  PRINTF ("\nwb_bridge device \"%s\" at 0x%" PRIxADDR ":\n", dev->name,
	  dev->baseaddr);
  PRINTF ("  Size 0x%" PRIx32 "\n", dev->size);

  PRINTF ("\n");

}				/* wb_bridge_status() */


/* Functions to set configuration */

static void
wb_bridge_enabled (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->enabled = val.int_val;

  init_connection (((struct dev_wb_bridge *) dat)->server_addr,
		   ((struct dev_wb_bridge *) dat)->server_port,
		   &((struct dev_wb_bridge *) dat)->sock);

}				/* wb_bridge_enabled() */


static void
wb_bridge_name (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->name = strdup (val.str_val);

  if (!((struct dev_wb_bridge *) dat)->name)
    {
      fprintf (stderr, "Peripheral openocd_wb_bridge: Run out of memory\n");
      exit (-1);
    }
}				/* wb_bridge_name() */


static void
wb_server_addr (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->server_addr = strdup (val.str_val);

  if (!((struct dev_wb_bridge *) dat)->server_addr)
    {
      fprintf (stderr, "Peripheral openocd_wb_bridge: Run out of memory\n");
      exit (-1);
    }
}				/* wb_server_addr() */

static void
wb_bridge_baseaddr (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->baseaddr = val.addr_val;

}				/* wb_bridge_baseaddr() */


static void
wb_bridge_size (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->size = val.int_val;

}				/* wb_bridge_size() */


static void
wb_server_port (union param_val val, void *dat)
{
  ((struct dev_wb_bridge *) dat)->server_port = val.int_val;

}				/* wb_server_port() */

/* Start of new wb_bridge section */

static void *
wb_bridge_sec_start ()
{
  struct dev_wb_bridge *new =
    (struct dev_wb_bridge *) malloc (sizeof (struct dev_wb_bridge));

  if (0 == new)
    {
      fprintf (stderr, "Peripheral openocd_wb_bridge: Run out of memory\n");
      exit (-1);
    }

  /* Default names */

  new->enabled = 1;
  new->name = "anonymous external peripheral";
  new->server_addr = "127.0.0.1";
  new->server_port = 6666;
  new->baseaddr = 0;
  new->size = 0;

  return new;

}				/* wb_bridge_sec_start() */


/* End of new wb_bridge section */

static void
wb_bridge_sec_end (void *dat)
{
  struct dev_wb_bridge *wb_bridge = (struct dev_wb_bridge *) dat;
  struct mem_ops ops;

  /* Give up if not enabled, or if size is zero, or if no access size is
     enabled. */

  if (!wb_bridge->enabled)
    {
      free (dat);
      return;
    }

  if (0 == wb_bridge->size)
    {
      fprintf (stderr, "wb_bridge peripheral \"%s\" has size 0: ignoring",
	       wb_bridge->name);
      free (dat);
      return;
    }

  /* Zero all the ops, then set the ones we care about. Read/write delays will
   * come from the peripheral if desired.
   */

  memset (&ops, 0, sizeof (struct mem_ops));

  ops.readfunc8 = wb_bridge_read_byte;
  ops.writefunc8 = wb_bridge_write_byte;
  ops.read_dat8 = dat;
  ops.write_dat8 = dat;

  ops.readfunc16 = wb_bridge_read_hw;
  ops.writefunc16 = wb_bridge_write_hw;
  ops.read_dat16 = dat;
  ops.write_dat16 = dat;

  ops.readfunc32 = wb_bridge_read_word;
  ops.writefunc32 = wb_bridge_write_word;
  ops.read_dat32 = dat;
  ops.write_dat32 = dat;

  /* Register everything */

  reg_mem_area (wb_bridge->baseaddr, wb_bridge->size, 0, &ops);

  reg_sim_reset (wb_bridge_reset, dat);
  reg_sim_stat (wb_bridge_status, dat);

}				/* wb_bridge_sec_end() */


/* Register a wb_bridge section. */

void
reg_wb_bridge_sec (void)
{
  struct config_section *sec = reg_config_sec ("openocd_wb_bridge",
					       wb_bridge_sec_start,
					       wb_bridge_sec_end);

  reg_config_param (sec, "enabled", PARAMT_INT, wb_bridge_enabled);
  reg_config_param (sec, "name", PARAMT_STR, wb_bridge_name);
  reg_config_param (sec, "server_addr", PARAMT_STR, wb_server_addr);
  reg_config_param (sec, "server_port", PARAMT_INT, wb_server_port);
  reg_config_param (sec, "baseaddr", PARAMT_ADDR, wb_bridge_baseaddr);
  reg_config_param (sec, "size", PARAMT_INT, wb_bridge_size);

}				/* reg_wb_bridge_sec */
