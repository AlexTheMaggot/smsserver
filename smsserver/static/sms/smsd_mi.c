/*
SMS sender 1.0
Copyright (C) Vladimir Oleynik <dzo@simtreas.ru> 2018-2019

Based by SMS server tool 2.0 Copyright (C) Stefan Frings <stefan@stefanfrings.de>

This program is free software unless you got it under another license directly
from the author. You can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation.
Either version 2 of the License, or (at your option) any later version.
*/

#define smsd_version "1.0"
#define ENABLE_FLASH_SMS 0
/* Tells the first memory space number for received messages.
   This is normally 1, Vodafone Mobile Connect Card starts with 0. */
#define READ_MEMORY_START 1
/* Forces smsd to empty the first SIM card memory before sending SM.
   This is a workaround for modems that cannot send SM with a full SIM card. */
#define RECEIVE_BEFORE_SEND 0

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#define maxsms_ucs2 140

#ifndef MinGW_NOFORK
#include <sys/ioctl.h>
#include <termios.h>
#include <syslog.h>
#include <iconv.h>
#else
#define LOG_CRIT        2       /* critical conditions */
#define LOG_ERR         3       /* error conditions */
#define LOG_WARNING     4       /* warning conditions */
#define LOG_NOTICE      5       /* normal but significant condition */
#define LOG_INFO        6       /* informational */
#define LOG_DEBUG       7       /* debug-level messages */

#include "win-iconv-0.0.8/iconv.h"
#include <windows.h>
#endif

typedef struct PHONES {
	struct PHONES *next;
	char phone[1];
} *phone_list;

static const char *program_name;

static char *port;
static int cfg_baudrate = 9600;
static char *initstring;
static int rtscts;
static char *message_text;
static char *pin;
static int loglevel = LOG_ERR;
static phone_list pl;
static phone_list *last = &pl;
static int textlen;

static FILE *logfile;
static int error_def_val = 2;       // 2 - usage, 1 - crit

static void writelogfile(int severity, char *s, ...)
{
	va_list p;
	FILE *lf;

	if (severity > loglevel)
		return;
	lf = logfile == NULL ? stderr : logfile;
	va_start(p, s);
	if(lf == stderr)
		fprintf(stderr, "%s: ", program_name);
	else {
		time_t now;
		char timestamp[64];

		time(&now);
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
		fprintf(lf, "%s: ", timestamp);
	}
	vfprintf(lf, s, p);
	putc('\n', lf);
	fflush(lf);
	va_end(p);
	if(severity == LOG_CRIT)
		exit(error_def_val);
}


#ifndef MinGW_NOFORK
static int modem;

static int
RS232_IsCTSEnabled (void)
{
  int status;

  ioctl (modem, TIOCMGET, &status);
  return status & TIOCM_CTS;
}

static int
RS232_SendByte (unsigned char byte)
{

  int n;

  while (1) {
	n = write (modem, &byte, 1);
	if (n < 0 && errno == EAGAIN)
	  continue;
	return n;
  }
}

static int
getNbrOfBytes (void)
{
  unsigned long available = 0;

  ioctl (modem, FIONREAD, &available);
  return (int) available;
}

#define ReadFromSerial(buffer, len) read(modem, buffer, len)

static void RS232_flushRX(void)
{
  tcflush(modem, TCIFLUSH);
}

#else

static HANDLE handle;

static int
RS232_IsCTSEnabled (void)
{
  int status;

  GetCommModemStatus (handle, (LPDWORD) ((void *) &status));

  return status & MS_CTS_ON;
}

static int
RS232_SendByte (unsigned char byte)
{
  int n;

  WriteFile (handle, &byte, 1, (LPDWORD) ((void *) &n), NULL);

  if (n <= 0)
	return -1;

  return 1;
}

static int
getNbrOfBytes (void)
{
  struct _COMSTAT status;
  unsigned long etat;

  ClearCommError (handle, &etat, &status);
  return status.cbInQue;
}

static int
ReadFromSerial (char *buffer, int len)
{
  unsigned long read_nbr = 0;

  ReadFile (handle, buffer, len, &read_nbr, NULL);
  return ((int) read_nbr);
}

static void RS232_flushRX(void)
{
  PurgeComm(handle, PURGE_RXCLEAR | PURGE_RXABORT);
}
#endif /* MinGW_NOFORK */


static void cut_emptylines(char *text)
{
  char *posi;
  char *found;

  posi = text;
  while (posi[0] && (found=strchr(posi,'\n')))
  {
    if ((found[1]=='\n') || (found==text))
      memmove(found,found+1,strlen(found));
    else
      posi++;
  }
}


// The following functions are for internal use of put_command and therefore not in the modeminit.h.
static int
write_to_modem (char *command, int timeout)
{
  int timeoutcounter = 0;
  size_t x = 0;

  if (command && command[0]) {
	if (rtscts) {
	  while (1) {
		if (RS232_IsCTSEnabled ())
		  break;
		timeoutcounter++;
		if (timeoutcounter > timeout) {
		  writelogfile (LOG_ERR, "Modem is not clear to send");
		  return 0;
		}
		usleep (100000);
	  }
	}
	writelogfile (LOG_DEBUG, "-> %s", command);
	for (x = 0; x < strlen (command); x++) {
	  if (RS232_SendByte ((unsigned char) command[x]) < 1) {
		writelogfile (LOG_ERR, "Could not send character %c, cause: %s", command[x],
					  strerror (errno));
		return 0;
	  }
#ifndef MinGW_NOFORK
	  tcdrain (modem);
#endif
	}
  }
  return 1;
}

// Read max characters from modem. The function returns when it received at
// least 1 character and then the modem is quiet for timeout*0.1s.
// The answer might contain already a string. In this case, the answer
// gets appended to this string.
static int
read_from_modem (char *answer, int max, int timeout)
{
  int got;
  int timeoutcounter = 0;
  int success = 0;

  do {
	// How many bytes do I want to read maximum? Not more than buffer size -1 for termination character.
	int toread = max - success - 1;
	if (toread <= 0)
	  break;
	got = getNbrOfBytes ();
	if (got > toread)
	  got = toread;
	if (got > 0) {
	  // read data
	  got = ReadFromSerial (answer + success, got);
	}
	// if nothing received ...
	if (got <= 0) {
	  // wait a litte bit and then repeat this loop
	  got = 0;
	  usleep (100000);
	  timeoutcounter++;
	} else {
	  // restart timout counter
	  timeoutcounter = 0;
	  // append a string termination character
	  success += got;
	}
  }
  while (timeoutcounter < timeout);
  answer[success] = 0;
  return success;
}

/* Is a character a space or tab? */
static int is_blank(char c)
{
  return (c==9) || (c==32);
}

/* Is a character a space or tab? */
static int is_cntrl(char c)
{
  return c < ' ';
}

static void cutspaces(char *text)
{
  int count;
  int Length;
  int i;
  int omitted;
  /* count ctrl chars and spaces at the beginning */
  count=0;
  while ( text[count] != 0 && (is_blank(text[count]) || is_cntrl(text[count])) )
    count++;
  /* remove ctrl chars at the beginning and \r within the text */
  omitted=0;
  Length=strlen(text);
  for (i=0; i<=(Length-count); i++)
    if (text[i+count] == '\r')
      omitted++;
    else
      text[i-omitted] = text[i+count];
  Length=strlen(text);
  while ( Length > 0 && (is_blank(text[Length-1]) || is_cntrl(text[Length-1])) ) {
    text[Length-1] = 0;
    Length--;
  }
}

// Exported functions:
static void
put_command (char *command, char *answer, int max, int timeout, char *add)
{
  int timeoutcounter = 0;
  int l = 0;
  char *s;

  // clean input buffer
  // It seems that this command does not do anything because actually it
  // does not clear the input buffer. However I do not remove it until I
  // know why it does not work.
  RS232_flushRX();

  // send command
  if (write_to_modem (command, 3) == 0) {
	sleep (1);
	return;
  }
  writelogfile (LOG_DEBUG, "Command is sent, waiting for the answer");

  // wait for the modem-answer
  timeoutcounter = 0;
  do {
	l += read_from_modem (answer + l, max, 2);     // One read attempt is 200ms
	s = strstr(answer, add == NULL ? "OK" : add);
	if(s != NULL)
		break;
	s = strstr(answer, "ERROR");
	if(s != NULL)
		break;
	timeoutcounter += 2;
  }
  // repeat until timout
  while (timeoutcounter < timeout);

  cutspaces (answer);
  cut_emptylines (answer);
  writelogfile (LOG_DEBUG, "<- %s", answer);
}

static int
initmodem (int error_sleeptime)
{
  char command[100];
  char answer[500];
  int retries = 0;
  int success = 0;

  writelogfile (LOG_INFO, "Checking if modem is ready");
  retries = 0;
  do {
	retries++;
	put_command ("AT\r", answer, sizeof (answer), 50, NULL);
	if (strstr (answer, "OK") == NULL && strstr (answer, "ERROR") == NULL) {
	  // if Modem does not answer, try to send a PDU termination character
	  put_command ("\x1A\r", answer, sizeof (answer), 50, NULL);
	}
  }
  while ((retries <= 10) && (!strstr (answer, "OK")));
  if (!strstr (answer, "OK")) {
	writelogfile (LOG_ERR, "Modem is not ready to answer commands");
	return 1;
  }

  if (pin) {
	writelogfile (LOG_INFO, "Checking if modem needs PIN");
	put_command ("AT+CPIN?\r", answer, sizeof (answer), 50, NULL);
	if (strstr (answer, "+CPIN: SIM PIN")) {
	  writelogfile (LOG_NOTICE, "Modem needs PIN, entering PIN...");
	  sprintf (command, "AT+CPIN=\"%s\"\r", pin);
	  put_command (command, answer, sizeof (answer), 300, NULL);
	  put_command ("AT+CPIN?\r", answer, sizeof (answer), 50, NULL);
	  if (strstr (answer, "+CPIN: SIM PIN")) {
		writelogfile (LOG_ERR, "Modem did not accept this PIN");
		return 2;
	  } else if (strstr (answer, "+CPIN: READY"))
		writelogfile (LOG_INFO, "PIN Ready");
	}
	if (strstr (answer, "+CPIN: SIM PUK")) {
	  writelogfile (LOG_CRIT, "Your PIN is locked. Unlock it manually");
	  return 2;
	}
  }

  if (initstring) {
	writelogfile (LOG_INFO, "Initializing modem");
	put_command (initstring, answer, sizeof (answer), 100, NULL);
	if (strstr (answer, "OK") == 0) {
	  writelogfile (LOG_ERR, "Modem did not accept the init string");
	  return 3;
	}
  }

  writelogfile (LOG_INFO, "Checking if Modem is registered to the network");
  success = 0;
  retries = 0;
  do {
	retries++;
	put_command ("AT+CREG?\r", answer, sizeof (answer), 100, NULL);
	if (strstr (answer, "1")) {
	  writelogfile (LOG_INFO, "Modem is registered to the network");
	  success = 1;
	} else if (strstr (answer, "5")) {
	  // added by Thomas Stoeckel
	  writelogfile (LOG_INFO, "Modem is registered to a roaming partner network");
	  success = 1;
	} else if (strstr (answer, "ERROR")) {
	  writelogfile (LOG_INFO, "Ignoring that modem does not support +CREG command.");
	  success = 1;
	} else if (strstr (answer, "+CREG:")) {
	  writelogfile (LOG_NOTICE, "Modem is not registered, waiting %i sec. before retrying", error_sleeptime);
	  sleep (error_sleeptime);
	  success = 0;
	} else {
	  writelogfile (LOG_ERR, "Error: Unexpected answer from Modem after +CREG?, waiting %i sec. before retrying", error_sleeptime);
	  sleep (error_sleeptime);
	  success = 0;
	}
  }
  while ((success == 0) && (retries < 10));


  if (success == 0) {
	writelogfile (LOG_ERR, "Error: Modem is not registered to the network");
	return 4;
  }


  writelogfile (LOG_INFO, "Selecting PDU mode");
  put_command ("AT+CMGF=0\r", answer, sizeof (answer), 50, NULL);
  if (strstr (answer, "ERROR")) {
	  writelogfile (LOG_ERR, "Error: Modem did not accept mode selection");
	  return 5;
  }

  return 0;
}

static int
openmodem (void)
{
#ifndef MinGW_NOFORK
  struct termios newtio;
  speed_t baudrate;

  modem = open (port, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (modem < 0) {
	writelogfile (LOG_ERR, "Cannot open serial port, cause: %s", strerror (errno));
	return -1;
  }

  bzero (&newtio, sizeof (newtio));
  newtio.c_cflag = CS8 | CLOCAL | CREAD | O_NDELAY | O_NONBLOCK;
  if (rtscts) {
	if (CRTSCTS != 0)
	  newtio.c_cflag |= CRTSCTS;
  }
  newtio.c_iflag = IGNPAR;
  newtio.c_oflag = 0;
  newtio.c_lflag = 0;
  newtio.c_cc[VTIME] = 0;
  newtio.c_cc[VMIN] = 0;
  switch (cfg_baudrate) {
  case 300: baudrate = B300; break;
  case 1200: baudrate = B1200; break;
  case 2400: baudrate = B2400; break;
  case 9600: baudrate = B9600; break;
  case 19200: baudrate = B19200; break;
  case 38400: baudrate = B38400; break;
#ifdef B57600
  case 57600: baudrate = B57600; break;
#endif
#ifdef B115200
  case 115200: baudrate = B115200; break;
#endif
#ifdef B230400
  case 230400: baudrate = B230400; break;
#endif
  default:
	writelogfile (LOG_ERR, "Baudrate not supported, using 9600");
	baudrate = B9600;
	break;
  }
  cfsetispeed (&newtio, baudrate);
  cfsetospeed (&newtio, baudrate);
  tcsetattr (modem, TCSANOW, &newtio);
  return modem;
#else
  DCB port_settings;
  char mode_str[128];
  COMMTIMEOUTS Cptimeouts;

  handle = CreateFile (port,                   // Name of the Port to be Opened
				GENERIC_READ | GENERIC_WRITE, // Read/Write Access
				0,            // No Sharing, ports cant be shared
				NULL,         // No Security
				OPEN_EXISTING,        // Open existing port only
				0,            // Non Overlapped I/O
				NULL);        // Null for Comm Devices

  if (handle == INVALID_HANDLE_VALUE) {
	writelogfile (LOG_ERR, "Cannot open serial port");
	return -1;
  }

  switch (cfg_baudrate) {
  case 110: strcpy (mode_str, "baud=110"); break;
  case 300: strcpy (mode_str, "baud=300"); break;
  case 600: strcpy (mode_str, "baud=600"); break;
  case 1200: strcpy (mode_str, "baud=1200"); break;
  case 2400: strcpy (mode_str, "baud=2400"); break;
  case 4800: strcpy (mode_str, "baud=4800"); break;
  case 9600: strcpy (mode_str, "baud=9600"); break;
  case 19200: strcpy (mode_str, "baud=19200"); break;
  case 38400: strcpy (mode_str, "baud=38400"); break;
  case 57600: strcpy (mode_str, "baud=57600"); break;
  case 115200: strcpy (mode_str, "baud=115200"); break;
  case 128000: strcpy (mode_str, "baud=128000"); break;
  case 256000: strcpy (mode_str, "baud=256000"); break;
  default: writelogfile (LOG_ERR, "Baudrate not supported, using 9600");
	strcpy (mode_str, "baud=9600");
	break;
  }
  strcat (mode_str, " data=8");
  strcat (mode_str, " parity=n");
  strcat (mode_str, " stop=1");
  strcat (mode_str, " dtr=on rts=on");

  memset (&port_settings, 0, sizeof (port_settings));   /* clear the new struct  */
  port_settings.DCBlength = sizeof (port_settings);

  if (!BuildCommDCBA (mode_str, &port_settings)) {
	writelogfile (LOG_ERR, "Unable to build comport mode settings");
	return -1;
  }

  if (!SetCommState (handle, &port_settings)) {
	writelogfile (LOG_ERR, "Unable to set comport mode settings");
	return -1;
  }

  Cptimeouts.ReadIntervalTimeout = MAXDWORD;
  Cptimeouts.ReadTotalTimeoutMultiplier = 0;
  Cptimeouts.ReadTotalTimeoutConstant = 0;
  Cptimeouts.WriteTotalTimeoutMultiplier = 0;
  Cptimeouts.WriteTotalTimeoutConstant = 0;

  if (!SetCommTimeouts (handle, &Cptimeouts)) {
	writelogfile (LOG_ERR, "Unable to SetCommTimeouts");
	return -1;
  }
  return 0;
#endif
}

static void help (int e)
{
  printf ("SMS sender.\n\n");
  printf ("Usage:\n");
  printf ("         %s -d DEVICE -p|P PHONES [options]\n\n", program_name);
  printf ("Options:\n");
  printf ("         -l LOGFILE. Recomended set first\n");
  printf ("         -L LOGLEVEL\n");
  printf ("         -d DEVICE  set serial port name\n");
  printf ("         -s BAUDRATE  set serial port speed. Default - 9600\n");
  printf ("         -I INITSTR  set init string for your modem\n");
  printf ("         -r   use rtscts modem control\n");
  printf ("         -p PHONE  set phone number. Can multiply usage.\n");
  printf ("         -P FILE_PHONES  load file with phone numbers\n");
  printf ("         -N PIN  need pin for this SIM card\n");
  printf ("         -t \"TEXT\"  text of message\n");
  printf ("         -e ENCODENAME  need encode message from this international codetable\n");
  printf ("         -h  this help\n");
  printf ("         -V  print copyright and version\n\n");
  exit (e);
}

#define C_SPACE 1
#define C_EOL 2
#define C_TN 4

static inline int num_class_char(char c)
{
	int n = (unsigned char)c;

	if(n == ' ' || n == '\t')
		return C_SPACE;
	if(n == '\r' || n == '\n' || n == '\0')
		return C_EOL;
	if((n >= '0' && n <= '9') || n == '*' || n == '#')
		return C_TN;
	return 0;
}

static void add_phone_list(const char *number)
{
	phone_list n;
	char *s;
	const char *num = number;

	if(*num == '#' && (num_class_char(num[1]) & (C_SPACE|C_EOL)))
		return; /* # rem */
	while(num_class_char(*num) == C_SPACE)
		num++;
	if(num_class_char(*num) == C_EOL)
		return; /* <empty line> */
	if(*num == '+')
		num++;
	number = num;
	while(num_class_char(*num) == C_TN)
		num++;
	if(number == num || (num_class_char(*num) & (C_SPACE|C_EOL)) == 0)
		writelogfile(LOG_CRIT, "Invalid phone numer '%s'", number);
	n = malloc(sizeof(*n) + strlen(number));
	if(n == NULL)
		writelogfile(LOG_CRIT, "Memory exhausted");
	n->next = NULL;
	s = strcpy(n->phone, number) + (num - number);
	*s = '\0';
	*last = n;
	last = &n->next;
}

static void load_phones(const char *name)
{
	FILE *p = fopen(name, "r");
	char buf[128];

	if(p == NULL) {
		writelogfile(LOG_ERR, "can not open file '%s': %s", name, strerror (errno));
		return;
	}
	while(fgets(buf, sizeof(buf), p) != NULL)
		add_phone_list(buf);
	fclose(p);
}

static long safe_strtol(const char *arg)
{
	char *endptr;
	long value = value;     /* make gcc happy */

	if(arg == NULL || *arg == '\0')
		goto eret;

	errno = 0;
	value = strtol(arg, &endptr, 0);
	if (*endptr != '\0') {
eret:
		errno = EINVAL;
	}
	return value;
}


static void open_logfile(const char *name)
{
	if(logfile)
		writelogfile(LOG_CRIT, "Can not multiply set logfiles");
	logfile = fopen(name, "a");
	if(logfile == NULL) {
		fprintf(stderr, "Can not set logfile '%s': %s\n", name, strerror (errno));
		exit(1);
	}
}

#define WANT_HEX_ESCAPES 1

/* Usual "this only works for ascii compatible encodings" disclaimer. */
#undef _tolower
#define _tolower(X) ((X)|((char) 0x20))

static char process_escape_sequence(const char **ptr)
{
	static const char charmap[] = {
		'a',  'b',  'e',    'f',  'n',  'r',  't',  'v',  '\\', 0,
		'\a', '\b', '\033', '\f', '\n', '\r', '\t', '\v', '\\', '\\' };

	const char *p;
	const char *q;
	unsigned int num_digits;
	unsigned int r;
	unsigned int n;
	unsigned int d;
	unsigned int base;

	num_digits = n = 0;
	base = 8;
	q = *ptr;

#ifdef WANT_HEX_ESCAPES
	if (*q == 'x') {
		++q;
		base = 16;
		++num_digits;
	}
#endif

	do {
		d = (unsigned int)(*q - '0');
#ifdef WANT_HEX_ESCAPES
		if (d >= 10) {
			d = ((unsigned int)(_tolower(*q) - 'a')) + 10;
		}
#endif

		if (d >= base) {
#ifdef WANT_HEX_ESCAPES
			if ((base == 16) && (!--num_digits)) {
/*                              return '\\'; */
				--q;
			}
#endif
			break;
		}

		r = n * base + d;
		if (r > 255) {
			break;
		}

		n = r;
		++q;
	} while (++num_digits < 3);

	if (num_digits == 0) {  /* mnemonic escape sequence? */
		p = charmap;
		do {
			if (*p == *q) {
				q++;
				break;
			}
		} while (*++p);
		n = *(p+(sizeof(charmap)/2));
	}

	*ptr = q;

	return (char) n;
}

static void precess_init_str(const char *arg)
{
	const char *p = arg;
	char *q;

	q = initstring = malloc(strlen(arg) + 2);
	if(q == NULL)
		writelogfile(LOG_CRIT, "Memory exhausted");
	while (*p) {
		if (*p == '\\') {
			p++;
			*q++ = process_escape_sequence(&p);
		} else {
			*q++ = *p++;
		}
	}
	if(q == initstring) {
		initstring = NULL;
		return;
	}
	if(q[-1] != '\r')
		*q++ = '\r';
	*q = '\0';
}

static void
parsearguments (int argc, char **argv)
{
  int result;
  long lt;
  const char *s;
  const char *src_txt = NULL;
  char *encode = NULL;

  s = program_name = argv[0];
#ifdef MinGW_NOFORK
  if(((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) && s[1] == ':')
	s += 2;
  while (*s) {
	int c = *s++;
	if (c == '\\' || c == '/') program_name = s;
  }
#else
  while (*s)
	if (*(s++) == '/') program_name = s;
#endif

  while((result = getopt (argc, argv, "l:L:d:s:I:rp:P:N:t:e:hV")) > 0) {
	switch (result) {
	case 'l':
	  open_logfile(optarg);
	  break;
	case 'L':
	  lt = safe_strtol(optarg);
	  if(errno || lt < 0 || lt > 7)
		writelogfile(LOG_CRIT, "loglevel must be 0..7");
	  loglevel = lt;
	  break;
	case 'd':
	  port = optarg;
	  break;
	case 's':
	  lt = safe_strtol(optarg);
	  if(errno || cfg_baudrate <= 0)
		writelogfile(LOG_CRIT, "Invalid baudrate %s", optarg);
	  cfg_baudrate = lt;
	  break;
	case 'I':
	  precess_init_str(optarg);
	  break;
	case 'r':
	  rtscts = 1;
	  break;
	case 'p':
	  add_phone_list(optarg);
	  break;
	case 'P':
	  load_phones(optarg);
	  break;
	case 'N':
	  if(pin)
		writelogfile(LOG_CRIT, "Can not set multiply pin");
	  pin = optarg;
	  safe_strtol(pin);
	  if(errno)
		writelogfile(LOG_CRIT, "Invalid pin '%s'", pin);
	  break;
	case 't':
	  src_txt = optarg;
	  break;
	case 'e':
	  encode = optarg;
	  break;
	case 'h':
	  help (0);
	case 'V':
	  printf("Version %s, Copyright (c) Vladimir Oleynik and Stefan Frings\n", smsd_version);
	  exit (0);
	default:
		help(2);
	}
  }

  if(port == NULL) {
	writelogfile(LOG_ERR, "-d DEVICE unspecified, exiting");
	help(2);
  }
  if(src_txt != NULL) {
	iconv_t cd;
	size_t f, t;
	char *st;

	f = strlen(src_txt);
	t = (f + 1) * 2;
	st = message_text = malloc(t);
	if(message_text == NULL)
		writelogfile(LOG_CRIT, "Memory exhausted");
	cd = iconv_open("UCS-2BE", encode == NULL ? "ascii" : encode);
	if( cd == (iconv_t)-1 ) {
		if(encode == NULL)
			writelogfile(LOG_CRIT, "Internall iconv error");
		writelogfile(LOG_CRIT, "Can not recode from codetable '%s'", encode);
	}
	if(iconv(cd, &src_txt, &f, &st, &t) == (size_t)-1 || f != 0) {
		if(encode == NULL)
			writelogfile(LOG_CRIT, "SMS text is not ascii, need use '-e input_code_table' option");
		writelogfile(LOG_CRIT, "Can not recode from codetable '%s' to UCS-2BE", encode);
	}
	*st = 0;
	textlen = (st - message_text);
	if(textlen > maxsms_ucs2)
		writelogfile(LOG_CRIT, "Message too long, exiting");
	if(st == message_text)
		message_text = NULL;
  }
  if(message_text == NULL && pl != NULL)
	writelogfile(LOG_CRIT, "Message do not found, nothing to do, exiting");
  error_def_val = 1;
}

/* Swap every second character */
static void swapchars(char *string)
{
  int Length;
  int position;
  char c;
  Length = strlen(string);

  for (position = 0; position < Length - 1; position += 2) {
    c = string[position];
    string[position] = string[position + 1];
    string[position + 1] = c;
  }
}

/* Converts binary to PDU string, this is basically a hex dump. */
static void binary2pdu(const char *binary, int length, char* pdu)
{
  int character;

  for (character = 0; character < length; character++)
    sprintf(pdu + character * 2, "%02X", (unsigned char)binary[character]);
}

// Make the PDU string from a mesage text and destination phone number.
// The destination variable pdu has to be big enough.
static void
make_pdu(const char *number, char* message, int messagelen, char* pdu)
{
  int coding;
  int flags;
  char *tmp;
  char tmp2[500];
  int numberformat;
  int numberlength;

  numberformat = 145;
  numberlength = strlen(number);
  tmp = alloca(numberlength + 2);

  // terminate the number with F if the length is odd
  if (numberlength%2)
    sprintf(tmp, "%sF", number);
  else
    strcpy(tmp, number);
  // Swap every second character
  swapchars(tmp);

  flags = 1; // SMS-Sumbit MS to SMSC
  coding = 24; // UCS2
  if (!ENABLE_FLASH_SMS)
    coding += 1; // Class1

  flags += 16; // Validity field
  /* report ? flags+=32; Request Status Report */
  /* Create the PDU string of the message */
  binary2pdu(message, messagelen, tmp2);
  sprintf(pdu, "00%02X00%02X%02X%s00%02XFF%02X%s",
		flags, numberlength, numberformat, tmp, coding, messagelen, tmp2);
}

/* =======================================================================
   Get a field from a modem answer, remove quotes
   ======================================================================= */

static void
getfield (char *line, int field, char *result)
{
  char *start;
  char *end;
  int i;
  int length;
#ifdef DEBUGMSG
  printf ("!! getfield(line=%s, field=%i, ...)\n", line, field);
#endif
  *result = 0;
  start = strstr (line, ":");
  if (start == 0)
	return;
  for (i = 1; i < field; i++) {
	start = strchr (start + 1, ',');
	if (start == 0)
	  return;
  }
  start++;
  while (start[0] == '\"' || start[0] == ' ')
	start++;
  if (start[0] == 0)
	return;
  end = strstr (start, ",");
  if (end == 0)
	end = start + strlen (start) - 1;
  while ((end[0] == '\"' || end[0] == '\"' || end[0] == ',')
		 && (end >= start))
	end--;
  length = end - start + 1;
  strncpy (result, start, length);
  result[length] = 0;
#ifdef DEBUGMSG
  printf ("!! result=%s\n", result);
#endif
}

/* =======================================================================
   Delete message on the SIM card
   ======================================================================= */
static void
deletesms (int sim)
{
  char command[100];
  char answer[500];

  writelogfile (LOG_INFO, "deleting message %i", sim);
  sprintf (command, "AT+CMGD=%i\r", sim);
  put_command (command, answer, sizeof (answer), 50, NULL);
}

/* =======================================================================
   Check size of SIM card
   ======================================================================= */
static void
check_memory (int *used_memory, int *max_memory)
{
  char answer[500];
  char *start;
  char tmp[100];
  // Set default values in case that the modem does not support the +CPMS command
  *used_memory = 1;
  *max_memory = 10;

  writelogfile (LOG_INFO, "checking memory size");
  put_command ("AT+CPMS?\r", answer, sizeof (answer), 50, NULL);
  if ((start = strstr (answer, "+CPMS:"))) {
	  getfield (start, 2, tmp);
	  if (tmp[0])
		*used_memory = atoi (tmp);
	  getfield (start, 3, tmp);
	  if (tmp[0])
		*max_memory = atoi (tmp);
	  writelogfile (LOG_INFO, "used memory is %i of %i", *used_memory, *max_memory);
	  return;
  }
  writelogfile (LOG_INFO, "command failed, using defaults.");
}

static void cut_ctrl(char* message) /* removes all ctrl chars */
{
  char tmp[500];
  int posdest=0;
  int possource;
  int count;
  count=strlen(message);
  for (possource=0; possource<=count; possource++)
  {
    if ((message[possource]>=' ') || (message[possource]==0))
      tmp[posdest++]=message[possource];
  }
  strcpy(message,tmp);
}




/* =======================================================================
   Read a memory space from SIM card
   ======================================================================= */

static int
readsim (int sim, char *line1, char *line2)
/* returns number of SIM memory if successful, otherwise 0 */
/* line1 contains the first line of the modem answer */
/* line2 contains the pdu string */
{
  char command[500];
  char answer[1024];
  char *begin1;
  char *begin2;
  char *end1;
  char *end2;
  line2[0] = 0;
  line1[0] = 0;
#ifdef DEBUGMSG
  printf ("!! readsim(sim=%i, ...)\n", sim);
#endif
  writelogfile (LOG_INFO, "trying to get stored message %i", sim);
  sprintf (command, "AT+CMGR=%i\r", sim);
  put_command (command, answer, sizeof (answer), 50, NULL);
  if (strstr (answer, ",,0\nOK")) // No SMS,  because Modem answered with +CMGR: 0,,0
	return -1;
  if (strstr (answer, "ERROR")) // No SMS,  because Modem answered with ERROR
	return -1;
  begin1 = strstr (answer, "+CMGR:");
  if (begin1 == 0)
	return -1;
  end1 = strstr (begin1, "\n");
  if (end1 == 0)
	return -1;
  begin2 = end1 + 1;
  end2 = strstr (begin2 + 1, "\n");
  if (end2 == 0)
	return -1;
  strncpy (line1, begin1, end1 - begin1);
  line1[end1 - begin1] = 0;
  strncpy (line2, begin2, end2 - begin2);
  line2[end2 - begin2] = 0;
  cutspaces (line1);
  cut_ctrl (line1);
  cutspaces (line2);
  cut_ctrl (line2);
  if (strlen (line2) == 0)
	return -1;
#ifdef DEBUGMSG
  printf ("!! line1=%s, line2=%s\n", line1, line2);
#endif
  return sim;
}


/* =======================================================================
   Receive one SMS or as many as the modem holds in memory
   ======================================================================= */

static int
receivesms (int quick, int only1st)
// if quick=1 then no initstring
// if only1st=1 then checks only 1st memory space
// Returns 1 if successful
// Returns -1 on error
{
  int max_memory, used_memory;
  int found;
  int sim;
  char line1[1024];
  char line2[1024];
#ifdef DEBUGMSG
  printf ("receivesms(quick=%i, only1st=%i)\n", quick, only1st);
#endif
  int r = 0;

  writelogfile (LOG_INFO, "checking device for incoming SMS");

  if (quick == 0) {
	// Initialize modem
	if (initmodem (1) > 0)
	  return -1;
  }
  // Check how many memory spaces we really can read
  check_memory (&used_memory, &max_memory);
  found = 0;
  if (used_memory > 0) {
	for (sim = READ_MEMORY_START; sim <= READ_MEMORY_START + max_memory - 1; sim++) {
	  found = readsim (sim, line1, line2);
	  if (found >= 0) {
		r++;
		deletesms (found);
		used_memory--;
		if (used_memory < 1)
		  break;                                // Stop reading memory if we got everything
	  }
	  if (only1st)
		break;
	}
  }
  return r;
}

/* ==========================================================================================
   Send a part of a message, this is physically one SM with max. 160 characters or 14 bytes
   ========================================================================================== */

static int
send_part (const char *to, int quick)
{
  char pdu[1024];
  int retries;
  char command[128];
  char command2[1024];
  char answer[1024];

  // Initialize messageid
  writelogfile (LOG_INFO, "Sending SMS to %s", to);

  if (quick == 0) {
	// Initialize modem
	if (initmodem (1) > 0)
	  return 0;
  }
  // Compose the modem command
  make_pdu (to, message_text, textlen, pdu);
  sprintf (command, "AT+CMGS=%i\r", (int)(strlen (pdu) / 2 - 1));

  sprintf (command2, "%s\x1A", pdu);

  retries = 0;
  while (1) {
	// Send modem command
	put_command (command, answer, sizeof (answer), 50, ">");
	// Send message if command was successful
	if (!strstr (answer, "ERROR"))
	  put_command (command2, answer, sizeof (answer), 300, NULL);
	// Check answer
	if (strstr (answer, "OK")) {
	  writelogfile (LOG_NOTICE, "SMS sent, To: %s", to);
	  return 1;
	} else {
	  writelogfile (LOG_ERR, "the modem said ERROR or did not answer.");
	  retries += 1;
	  if (retries <= 2) {
		writelogfile (LOG_NOTICE, "waiting %i sec. before retrying", 1);
		sleep (1);
		// Initialize modem after error
		if (initmodem (1) > 0) {
		  // Cancel if initializing failed
		  writelogfile (LOG_WARNING, "sending SMS to %s failed", to);
		  return 0;
		}
	  } else {
		// Cancel if too many retries
		writelogfile (LOG_WARNING, "Sending SMS to %s failed", to);
		return 0;
	  }
	}
  }
}

/* =======================================================================
   Main
   ======================================================================= */

int
main (int argc, char **argv)
{
  phone_list n;
  int quick = 0;

  parsearguments (argc, argv);
  // Open serial port or return if not successful
#ifdef DEBUGMSG
  printf ("!! Opening serial port %s\n", port);
#endif
  if (openmodem () == -1)
	return 1;
#ifdef DEBUGMSG
  printf ("!! Entering endless send/receive loop\n");
#endif
  // Start main program
  for(n = pl; n != NULL; n = n->next) {
	if (RECEIVE_BEFORE_SEND)
		receivesms (quick, 1);
	  // Try to send the sms
	if (send_part (n->phone, quick)) {
		// Sending was successful
		quick = 1;
	} else {
		// Sending failed
		quick = 0;
	}
  }
  if (!RECEIVE_BEFORE_SEND)
	receivesms (quick, 0);
  return quick == 0;
}
