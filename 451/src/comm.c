/*---------------------------------------------------------------------------
 * Gamedriver Communications module.
 *
 *---------------------------------------------------------------------------
 * Throughout the module the fact is used that valid socket numbers
 * are always >= 0. Unused sockets are therefore marked with negative
 * numbers.
 *
 * All information needed for an interactive object are stored in
 * a 'interactive_t'. This struct is linked to by the shadow sentence
 * of the interactive object.
 *
 * Sending data is performed through the function add_message().
 * The function collects the data in interactive.message_buf[] until
 * it is filled (or a special 'flush' message is passed), then the
 * data is written en-bloc to the network.
 *
 * Incoming data is collected in interactive.text[]. interactive.text_end
 * indexes the first free character in the buffer where new data is to
 * be appended. The new data is not passed directly to the command parser,
 * instead it is processed by a dfa implementing the important parts
 * of the telnet protocol. The dfa analyses the data read, interprets
 * any telnet commands and stores the remaining 'pure' data starting
 * from the beginning of .text[].
 *
 * Initialized to start working in the state TS_DATA, the dfa does its
 * thing until it either reaches a line end or the end of the current
 * data. If it is a line end, it terminates the pure data collected so
 * far with a '\0', goes into state TS_READY, and lets .tn_end index
 * the next unprocessed raw data char. If it is the end of the current
 * data, the dfa stays in whatever state it was and indexes the current end
 * of the pure data gathered so far with interactive.command_end. Once
 * a new chunk of data has been read, the dfa simply continues where it
 * took off.
 *
 * There are some variations to this scheme, but you get the idea. It is
 * possible to do all telnet negotiations on mudlib level, but it must
 * be either the driver or the mudlib, not both.
 *
 * To understand get_message() itself fully, think of it as a coroutine
 * with its own state. It does not really return to the caller (though
 * that is how it is implemented), in merely yields control back to
 * caller in order to process the found command or the pending heartbeat.
 * TODO: Obvious possibility for implementation multi-threading.
 *
 * Timing is implemented this way: The driver usually stays in the input
 * loop, waiting in 1 second intervals for incoming data. An alarm() is
 * triggered by backend.c every 2 seconds and sets the flag variable
 * comm_time_to_call_heart_beat. The loop checks this variable every second
 * and, if it is set, aborts its input loop and returns to the backend.
 * To mark the cause of the return, the variable time_to_call_heart_beat is
 * set before return.
 *
 * TODO: The noecho/charmode logic, especially in combination with
 * TODO:: the telnet machine is frustratingly underdocumented.
 *
#ifdef USE_PTHREAD
 * The data is not written directly to the sockets, but instead to
 * an intermediate buffer, from which a secondary thread does the actual
 * writing. The buffers are stored in a linked list in the interactive-s
 * structure. Advantage is that the driver is no longer bothered by blocking
 * sockets.
 *
 * TODO: Generalize the background buffer and either use pthreads, or a call
 * TODO:: from the backend loop to write the data. Also allow to set the
 * TODO:: PTHREAD_MAX_WRITE from the commandline (incl. unlimited), and don't
 * TODO:: immediately discard EWOULDBLOCK-failed messages.
#endif
 *---------------------------------------------------------------------------
 */

#include "driver.h"
#include "typedefs.h"

#include "my-alloca.h"

#include <stdio.h>
#include <ctype.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stddef.h>

#if defined(AMIGA) && !defined(__SASC) && !defined(__GNUC__)
#    include <ioctl.h>
#else
#    include <sys/ioctl.h>
#endif

#define TELOPTS
#include "telnet.h"

#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif

#if !defined(AMIGA) || defined(__GNUC__)
#    include <signal.h>
#else
#    include "hosts/amiga/nsignal.h"
#endif

#if defined(_AIX) || defined(__EMX__) || defined(OS2)
#    include <sys/select.h>
#endif

#ifdef HAVE_FCNTL
#    include <fcntl.h>
#endif

#ifdef SOCKET_INC
#    include SOCKET_INC
#endif

#define USES_SVALUE_STRLEN

#include "comm.h"
#include "access_check.h"
#include "actions.h"
#include "array.h"
#include "backend.h"
#include "closure.h"
#include "ed.h"
#include "exec.h"
#include "filestat.h"
#include "gcollect.h"
#include "interpret.h"
#include "main.h"
#include "object.h"
#include "sent.h"
#include "simulate.h"
#include "smalloc.h"  /* svalue_strlen() */
#include "stdstrings.h"
#include "stralloc.h"
#include "swap.h"
#include "wiz_list.h"
#include "xalloc.h"

#include "../mudlib/sys/input_to.h"
#include "../mudlib/sys/driver_hook.h"

/* if driver is compiled for ERQ demon then include the necessary file
 */
#ifdef ERQ_DEMON
#    ifdef ERQ_INCLUDE
#        include ERQ_INCLUDE
#    else
#        include "util/erq/erq.h"
#    endif
#endif

/* When no special networking code is needed, define the
 * socket function to their normal Unix names.
 */
#if !defined (SOCKET_LIB) && !defined(SOCKET_INC)
#    define socket_number(s) (s)
#    define socket_ioctl  ioctl
#    ifndef hpux
#        define socket_select select
#    else
#        define socket_select(n,r,w,x,t) select(n, (int *)r, (int *)w, (int *)x, t)
         /* Silences the compiler */
#    endif
#    define socket_read   read
#    define socket_write  write
#    define socket_close  close
#endif /* SOCKET_LIB */

#if defined(SunOS4) || defined(atarist)
struct timeval;
extern SOCKET_T socket(int, int, int);
extern int getpeername(SOCKET_T, struct sockaddr *, int *);
extern void  shutdown(SOCKET_T, int);
extern int setsockopt(SOCKET_T, int, int, char *, int);
extern int bind(SOCKET_T, struct sockaddr *, int);
extern int listen(SOCKET_T, int);
extern SOCKET_T accept(SOCKET_T, struct sockaddr *, int *);
extern int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#endif /* SunOS4 */

#if defined(_AIX)
typedef unsigned long length_t;
#elif defined(__INTEL_COMPILER)
typedef socklen_t length_t;
#else
typedef int length_t;
#endif

#if defined(CYGWIN)
extern int socketpair(int, int, int, int[2]);
#endif

#ifndef EPROTO
#    define EPROTO EINTR
#endif
#ifndef SIGCLD
#    define SIGCLD SIGCHLD
#endif

#ifndef MAXHOSTNAMELEN
#    define MAXHOSTNAMELEN 64
#endif

#ifndef INET_ADDRSTRLEN
#    define INET_ADDRSTRLEN 16
#endif

/* Amazing how complicated networking can be, hm? */

/*-------------------------------------------------------------------------*/

interactive_t *all_players[MAX_PLAYERS];
  /* Pointers to the structures of the interactive users.
   * Unused entries are NULL.
   * TODO: A list would be nicer
   */

static int max_player = -1;
  /* Index of the last used entry in all_players[]. */

int num_player = 0;
  /* The current number of active users */

char *message_flush = NULL;
  /* Special flush message for add_message().
   * It is a variable instead of a define to keep gcc from complaining about
   * a 'null format string'.
   */

#ifdef COMM_STAT

/* The statistics were originally introduced to measure the efficiency
 * of the message buffering in comparison to the unbuffered sending of
 * data. Nowadays, it's just interesting to know how much bandwidth you
 * use.
 */

int add_message_calls = 0;
  /* Number of calls to add_message() */

int inet_packets = 0;
  /* Number packets sent to the users */

int inet_volume = 0;
  /* Amount of data sent to the users */

#endif

/*-------------------------------------------------------------------------*/

#ifdef ERQ_DEMON

#define MAX_PENDING_ERQ  32  /* Max number of pending requests */

#define FLAG_NO_ERQ   -2    /* No ERQ connected */
#define FLAG_ERQ_STOP -1    /* Severing connection */

static SOCKET_T erq_demon = FLAG_NO_ERQ;
  /* Socket of the connection to the erq demon. */

static SOCKET_T erq_proto_demon = -1;
  /* Socket to hold the connection to an aspiring new erq demon
   * while the connection to the current one is being severed.
   */

static char buf_from_erq[ERQ_MAX_REPLY];
  /* Buffer for the data received from the erq */

static char * input_from_erq = &buf_from_erq[0];
  /* Pointer to the first free byte in buf_from_erq. */

static unsigned long erq_pending_len = 0;
  /* erq_pending_len is used by send_erq(), but needs
   * to be cleared by stop_erq_demon().
   */

static svalue_t pending_erq[MAX_PENDING_ERQ+1];
  /* ERQ callback handles. The last one is reserved for callback-free
   * requests.
   * The free entries are organised in a singly linked list of
   * T_INVALID svalues, using the u.lvalue to point to the next
   * free entry.
   */

static svalue_t *free_erq;
  /* The first free entry in the freelist in pending_erq[] */

/* The size of the IPTABLE depends on the number of users,
 * and is at least 200.
 */
#if MAX_PLAYERS > 700
#    define IPSIZE MAX_PLAYERS
#else
#    if MAX_PLAYERS > 100
#        define IPSIZE (MAX_PLAYERS*2)
#    else
#        define IPSIZE 200
#    endif
#endif

static struct ipentry {
    struct in_addr  addr;  /* The address (only .s_addr is significant) */
    char           *name;  /* shared string with the hostname for <addr> */
} iptable[IPSIZE] = { { { 0 }, 0}, };
  /* Cache of known names for given IP addresses.
   * It is used as a ringbuffer, indexed by ipcur.
   * TODO: Null all entries in the table? Std-C should do that automatically.
   * TODO: Instead of a simple circular buffer, the lookup should be
   * TODO:: hashed over the IP address. Worst case would still be O(IPSIZE),
   * TODO:: but best case would be O(1).
   */

static int ipcur = 0;
  /* Index of the next entry to use in the iptable[].
   */

#endif /* ERQ_DEMON */

/*-------------------------------------------------------------------------*/

/* --- Communication sockets --- */

static SOCKET_T sos[MAXNUMPORTS];
  /* The login sockets.
   */

static SOCKET_T udp_s = -1;
  /* The UDP socket */

/* --- Networking information --- */

static struct in_addr host_ip_number;
  /* This computer's numeric IP address only, used for
   * the query_host_name() efun. */

static struct sockaddr_in host_ip_addr;
  /* This computer's full IP address, used with varying port numbers
   * to open the driver's ports.
   * TODO: Can't this be local?
   */

char * domain_name = NULL;
  /* This computer's domain name, as needed by lex.c::get_domainname().
   */

static int min_nfds = 0;
  /* The number of fds used by the driver's sockets (udp, erq, login).
   * It is the number of the highest fd plus one.
   */

/* --- Telnet handling --- */

#ifdef DEBUG_TELNET

#define DT(x) printf("%s TDEBUG: ", time_stamp()); printf x 
#define DTF(x) printf x 

#else

#define DT(x) 
#define DTF(x) 

#endif

static Bool sending_telnet_command = MY_FALSE;
  /* Mutex queried in add_message() to hide telnet commands
   * from snoopers and shadows.
   */

#define SEND_TELNET_COMMAND(TEXT) {\
        sending_telnet_command = MY_TRUE;\
        TEXT\
        sending_telnet_command = MY_FALSE;\
}
  /* Use this macro to safely send telnet commands with TEXT
   */

static void (*telopts_do  [NTELOPTS])(int);
static void (*telopts_dont[NTELOPTS])(int);
static void (*telopts_will[NTELOPTS])(int);
static void (*telopts_wont[NTELOPTS])(int);

  /* Tables with the telnet statemachine handlers.
   */

#define TS_DATA       0
#define TS_IAC        1
#define TS_WILL       2
#define TS_WONT       3
#define TS_DO         4
#define TS_DONT       5
#define TS_SB         6
#define TS_SB_IAC     7
#define TS_READY      8
#define TS_CR         9
#define TS_SYNCH     10
#define TS_INVALID   11

  /* Telnet states
   */

#define TN_START_VALID(x) ((x & ~1) == TS_SB)


/* --- Misc --- */

static volatile Bool urgent_data = MY_FALSE;
  /* Flag set when a SIGURG/SIGIO announces the arrival of
   * OOB data.
   */

static volatile mp_int urgent_data_time;
  /* The backend::current_time when urgent_data was set last.
   */

static object_t *first_player_for_flush = NULL;
  /* First interactive user object to flush. Marks the head
   * of the list formed by interactive.{next,previous}_player_for_flush
   */

static char destruct_add_message_format[]
  = { '%', 's', '\0' };
  /* To allow messages to go to a interactive player who's object
   * is currently destructed, there is this special format string which
   * is guaranteed to be an unshareable string.
   */


/* Bitflags for interactive.do_close
 *
 * Putting PROTO_ERQ into do_close looks strange, but actually makes
 * sense because some of the steps to be taken for both are the
 * same.
 */

#define FLAG_DO_CLOSE   0x1
#define FLAG_PROTO_ERQ  0x2


/*-------------------------------------------------------------------------*/

/* Forward declarations */

static void free_input_to(input_to_t *);
static void telnet_neg(interactive_t *);
static void send_will(int);
static void send_wont(int);
static void send_do(int);
static void send_dont(int);
static void remove_flush_entry(interactive_t *ip);
static void new_player(SOCKET_T new_socket, struct sockaddr_in *addr, size_t len, int login_port);

#ifdef ERQ_DEMON

static long read_32(char *);
static Bool send_erq(int handle, int request, char *arg, size_t arglen);
static void shutdown_erq_demon(void);
static void stop_erq_demon(Bool);
static char * lookup_ip_entry (struct in_addr addr, Bool useErq);
static void add_ip_entry(struct in_addr addr, const char *name);
#ifdef USE_IPV6
static void update_ip_entry(const char *oldname, const char *newname);
#endif

#endif /* ERQ_DEMON */

#ifdef USE_PTHREAD
static void *writer_thread(void *arg);
static void writer_thread_cleanup(void *arg);
#endif

#ifdef USE_IPV6

/*-------------------------------------------------------------------------*/
#ifndef CREATE_IPV6_MAPPED  /* not defined on AIX 4.3 */

#define CREATE_IPV6_MAPPED(v6,v4) \
    ((v6).s6_addr32[0] = 0, \
     (v6).s6_addr32[1] = 0, \
     (v6).s6_addr32[2] = 0x0000ffff, \
     (v6).s6_addr32[3] = v4 )

#endif

/* These are the typical IPv6 structures - we use them transparently.
 *
 * --- arpa/inet.h ---
 *
 * struct in6_addr {
 *         union {
 *                 uint32_t u6_addr32[4];
 * #ifdef notyet
 *                 uint64_t u6_addr64[2];
 * #endif
 *                 uint16_t u6_addr16[8];
 *                 uint8_t  u6_addr8[16];
 *         } u6_addr;
 * };
 * #define s6_addr32       u6_addr.u6_addr32
 * #ifdef notyet
 * #define s6_addr64       u6_addr.u6_addr64
 * #endif
 * #define s6_addr16       u6_addr.u6_addr16
 * #define s6_addr8        u6_addr.u6_addr8
 * #define s6_addr         u6_addr.u6_addr8
 *
 * --- netinet/in.h ---
 *
 * struct sockaddr_in6 {
 *    u_char		sin6_len;
 *    u_char		sin6_family;
 *    u_int16_t	sin6_port;
 *    u_int32_t	sin6_flowinfo;
 *    struct 		in6_addr	sin6_addr;
 * };
 *
 */

/*-------------------------------------------------------------------------*/
static char *
inet6_ntoa (struct in6_addr in)

/* Convert the ipv6 address <in> into a string and return it.
 * Note: the string is stored in a local buffer.
 */

{
    static char str[INET6_ADDRSTRLEN+1];

    if (NULL == inet_ntop(AF_INET6, &in, str, INET6_ADDRSTRLEN))
    {
        perror("inet_ntop");
    }
    return str;
} /* inet6_ntoa() */

/*-------------------------------------------------------------------------*/
static struct in6_addr
inet6_addr (char *to_host)

/* Convert the name <to_host> into a ipv6 address and return it.
 */

{
    struct in6_addr addr;

    inet_pton(AF_INET6, to_host, &addr);
    return addr;
} /* inet6_addr() */

#endif /* USE_IPV6 */

/*-------------------------------------------------------------------------*/
static char *
decode_noecho (char noecho)

/* Decode the <noecho> flag byte into a string.
 * Result is a pointer to a static buffer.
 */

{
    static char buf[100];
    strcpy(buf, "(");
    if (noecho & NOECHO_REQ) strcat(buf, "NOECHO_REQ, ");
    if (noecho & CHARMODE_REQ) strcat(buf, "CHARMODE_REQ, ");
    if (noecho & NOECHO) strcat(buf, "NOECHO, ");
    if (noecho & CHARMODE) strcat(buf, "CHARMODE, ");
    if (noecho & NOECHO_ACK) strcat(buf, "NOECHO_ACK, ");
    if (noecho & CHARMODE_ACK) strcat(buf, "CHARMODE_ACK, ");
    if (noecho & NOECHO_STALE) strcat(buf, "NOECHO_STALE, ");
    if (noecho & IGNORE_BANG) strcat(buf, "IGNORE_BANG");
    strcat(buf, ")");

    return buf;
} /* decode_noecho() */

/*-------------------------------------------------------------------------*/
static void
dump_bytes (void * data, size_t length, int indent)

/* Write the datablock starting at <data> of size <length> to stderr.
 * If it spans more than one line, indent the following lines by <indent>.
 */

{
    int cur_indent = 0;
    unsigned char * datap = (unsigned char *)data;

    while (length > 0)
    {
        size_t count;

        if (cur_indent)
            fprintf(stderr, "%*.*s", cur_indent, cur_indent, " ");
        else
            cur_indent = indent;
        fprintf(stderr, " %p:", datap);

        for (count = 0; count < 16 && length > 0; ++count, --length, ++datap)
        {
            fprintf(stderr, " %02x", *datap);
        }
        putc('\n', stderr);
    }
} /* dump_bytes() */

/*-------------------------------------------------------------------------*/
static void
comm_fatal (interactive_t *ip, char *fmt, ...)

/* The telnet code ran into a fatal error.
 * Dump the data from the current interactive structure and disconnect
 * the user (we have to assume that the interactive structure is
 * irrecoverably hosed).
 * TODO: Make similar functions comm_error(), comm_perror() which prefix
 * TODO:: the error message with the ip %p and obj name.
 */

{
    va_list va;
    static Bool in_fatal = MY_FALSE;
    char * ts;
    char * msg = "\r\n=== Internal communications error in mud driver.\r\n"
                   "=== Please log back in and inform the administration.\r\n"
                   "\r\n";

    /* Prevent double fatal. */
    if (in_fatal)
        fatal("Recursive call to comm_fatal().");
    in_fatal = MY_TRUE;
    ts = time_stamp();
    
    /* Print the error message */

    va_start(va, fmt);

    fflush(stdout);
    fprintf(stderr, "%s ", ts);
    vfprintf(stderr, fmt, va);
    fflush(stderr);
    if (current_object)
        fprintf(stderr, "%s Current object was %s\n"
                      , ts, current_object->name
                            ? current_object->name : "<null>");
    debug_message("%s ", ts);
    vdebug_message(fmt, va);
    if (current_object)
        debug_message("%s Current object was %s\n"
                     , ts, current_object->name
                           ? current_object->name : "<null>");
    debug_message("%s Dump of the call chain:\n", ts);
    (void)dump_trace(MY_TRUE, NULL); fflush(stdout);

    va_end(va);
    
    /* Dump the interactive structure */

    fprintf(stderr, "--- Dump of current interactive structure (%p..%p) --- \n"
                  , ip, ip + sizeof(*ip) - 1);
    fprintf(stderr, "  .socket:            %d\n", ip->socket);
    fprintf(stderr, "  .ob:                %p", ip->ob);
      if (ip->ob)  fprintf(stderr, " (%s)", ip->ob->name);
      putc('\n', stderr);
    fprintf(stderr, "  .input_to:          %p\n", ip->input_to);
    fprintf(stderr, "  .modify_command:    %p", ip->modify_command);
      if (ip->modify_command)  fprintf(stderr, " (%s)", ip->modify_command->name);
      putc('\n', stderr);
    fprintf(stderr, "  .prompt:           ");
      dump_bytes(&(ip->prompt), sizeof(ip->prompt), 21);
    fprintf(stderr, "  .addr:             ");
      dump_bytes(&(ip->addr), sizeof(ip->addr), 21);
    fprintf(stderr, "  .msg_discarded:     %02x\n", (unsigned char)ip->msg_discarded);
    fprintf(stderr, "  .set_input_to:      %02x\n", (unsigned char)ip->set_input_to);
    fprintf(stderr, "  .closing:           %02x\n", (unsigned char)ip->closing);
    fprintf(stderr, "  .do_close:          %02x", (unsigned char)ip->do_close);
      if (ip->do_close & (FLAG_DO_CLOSE|FLAG_PROTO_ERQ)) fprintf(stderr, " (");
      if (ip->do_close & FLAG_DO_CLOSE) fprintf(stderr, "DO_CLOSE");
      if (ip->do_close & (FLAG_DO_CLOSE|FLAG_PROTO_ERQ)) fprintf(stderr, ", ");
      if (ip->do_close & FLAG_PROTO_ERQ) fprintf(stderr, "PROTO_ERQ");
      if (ip->do_close & (FLAG_DO_CLOSE|FLAG_PROTO_ERQ)) fprintf(stderr, ")");
      putc('\n', stderr);
    fprintf(stderr, "  .noecho:            %02x", (unsigned char)ip->noecho);
      fprintf(stderr, " %s\n", decode_noecho(ip->noecho));
    fprintf(stderr, "  .tn_state:          %d", ip->tn_state);
      switch(ip->tn_state) {
      case TS_DATA:    fprintf(stderr, " (TS_DATA)\n"); break;
      case TS_IAC:     fprintf(stderr, " (TS_IAC)\n"); break;
      case TS_WILL:    fprintf(stderr, " (TS_WILL)\n"); break;
      case TS_WONT:    fprintf(stderr, " (TS_WONT)\n"); break;
      case TS_DO:      fprintf(stderr, " (TS_DO)\n"); break;
      case TS_DONT:    fprintf(stderr, " (TS_DONT)\n"); break;
      case TS_SB:      fprintf(stderr, " (TS_SB)\n"); break;
      case TS_SB_IAC:  fprintf(stderr, " (TS_SB_IAC)\n"); break;
      case TS_READY:   fprintf(stderr, " (TS_READY)\n"); break;
      case TS_CR:      fprintf(stderr, " (TS_CR)\n"); break;
      case TS_SYNCH:   fprintf(stderr, " (TS_SYNCH)\n"); break;
      case TS_INVALID: fprintf(stderr, " (TS_INVALID)\n"); break;
      default: putc('\n', stderr);
      }
    fprintf(stderr, "  .save_tn_state:     %d", ip->save_tn_state);
      switch(ip->save_tn_state) {
      case TS_DATA:    fprintf(stderr, " (TS_DATA)\n"); break;
      case TS_IAC:     fprintf(stderr, " (TS_IAC)\n"); break;
      case TS_WILL:    fprintf(stderr, " (TS_WILL)\n"); break;
      case TS_WONT:    fprintf(stderr, " (TS_WONT)\n"); break;
      case TS_DO:      fprintf(stderr, " (TS_DO)\n"); break;
      case TS_DONT:    fprintf(stderr, " (TS_DONT)\n"); break;
      case TS_SB:      fprintf(stderr, " (TS_SB)\n"); break;
      case TS_SB_IAC:  fprintf(stderr, " (TS_SB_IAC)\n"); break;
      case TS_READY:   fprintf(stderr, " (TS_READY)\n"); break;
      case TS_CR:      fprintf(stderr, " (TS_CR)\n"); break;
      case TS_SYNCH:   fprintf(stderr, " (TS_SYNCH)\n"); break;
      case TS_INVALID: fprintf(stderr, " (TS_INVALID)\n"); break;
      default: putc('\n', stderr);
      }
    fprintf(stderr, "  .supress_go_ahead:  %02x\n", (unsigned char)ip->supress_go_ahead);
    fprintf(stderr, "  .text_end:          %hd (%p)\n", ip->text_end, ip->text+ip->text_end);
    fprintf(stderr, "  .command_start:     %hd (%p)\n", ip->command_start, ip->text+ip->command_start);
    fprintf(stderr, "  .command_end:       %hd (%p)\n", ip->command_end, ip->text+ip->command_end);
    fprintf(stderr, "  .tn_start:          %hd (%p)\n", ip->tn_start, ip->text+ip->tn_start);
    fprintf(stderr, "  .tn_end:            %hd (%p)\n", ip->tn_end, ip->text+ip->tn_end);
    fprintf(stderr, "  .chars_ready:       %ld\n", (long)ip->chars_ready);
    fprintf(stderr, "  .snoop_on:          %p", ip->snoop_on);
      if (ip->snoop_on && ip->snoop_on->ob) fprintf(stderr, " (%s)", ip->snoop_on->ob->name);
      putc('\n', stderr);
    fprintf(stderr, "  .snoop_by:          %p", ip->snoop_by);
      if (ip->snoop_by) fprintf(stderr, " (%s)", ip->snoop_by->name);
      putc('\n', stderr);
    fprintf(stderr, "  .last_time:         %ld\n", (long)ip->last_time);
    fprintf(stderr, "  .trace_level:       %d\n", ip->trace_level);
    fprintf(stderr, "  .trace_prefix:      %p", ip->trace_prefix);
      if (ip->trace_prefix) fprintf(stderr, " '%s'", ip->trace_prefix);
      putc('\n', stderr);
    fprintf(stderr, "  .message_length:    %d (%p)\n", ip->message_length, ip->message_buf+ip->message_length);
    fprintf(stderr, "  .next_for_flush:    %p", ip->next_player_for_flush);
      if (ip->next_player_for_flush) fprintf(stderr, " (%s)", ip->next_player_for_flush->name);
      putc('\n', stderr);
    fprintf(stderr, "  .prev_for_flush:    %p", ip->previous_player_for_flush);
      if (ip->previous_player_for_flush) fprintf(stderr, " (%s)", ip->previous_player_for_flush->name);
      putc('\n', stderr);
    fprintf(stderr, "  .access_class:      %ld\n", ip->access_class);
    fprintf(stderr, "  .charset:          ");
      dump_bytes(&(ip->charset), sizeof(ip->charset), 21);
    fprintf(stderr, "  .combine_cset:     ");
      dump_bytes(&(ip->combine_cset), sizeof(ip->combine_cset), 21);
    fprintf(stderr, "  .quote_iac:         %02x\n", (unsigned char)ip->quote_iac);
    fprintf(stderr, "  .catch_tell_activ:  %02x\n", (unsigned char)ip->catch_tell_activ);
    fprintf(stderr, "  .gobble_char:       %02x\n", (unsigned char)ip->gobble_char);
    fprintf(stderr, "  .ts_data:           %02x\n", (unsigned char)ip->ts_data);
    fprintf(stderr, "  .text:             ");
      dump_bytes(&(ip->text), sizeof(ip->text), 21);
    fprintf(stderr, "  .message_buf:      ");
      dump_bytes(&(ip->message_buf), sizeof(ip->message_buf), 21);
    fprintf(stderr, "------\n");

    /* Disconnect the user */
    socket_write(ip->socket, msg, strlen(msg));
    remove_interactive(ip->ob, MY_TRUE);

    /* Unset mutex */
    in_fatal = MY_FALSE;
} /* comm_fatal() */

/*-------------------------------------------------------------------------*/
static void
set_socket_nonblocking (SOCKET_T new_socket)

/* Set the <new_socket> into non-blocking mode.
 * Abort on error.
 */

{
    int tmp;

#ifndef __BEOS__

    tmp = 1;

# ifdef USE_IOCTL_FIONBIO
    if (socket_ioctl(new_socket, FIONBIO, &tmp) == -1) {
        perror("ioctl socket FIONBIO");
        abort();
    }
# else /* !USE_IOCTL_FIONBIO */
# ifdef USE_FCNTL_O_NDELAY
    if (fcntl(new_socket, F_SETFL, O_NDELAY) == -1) {
# else
    if (fcntl(new_socket, F_SETFL, FNDELAY) == -1) {
# endif
        perror("fcntl socket FNDELAY");
        abort();
    }
# endif /* !USE_IOCTL_FIONBIO */

#else /* if __BEOS__ */

    /* BeOS up to R4 uses different filedescriptors for files and sockets;
     * so a fcntl() modifies the regular file with the number <new_socket>,
     * but not the socket itself. setsockopt() comes to our rescue.
     * TODO: Add setsockopt() to configure to test?
     */
    tmp = 1;
    if (setsockopt(new_socket, SOL_SOCKET, SO_NONBLOCK, &tmp, sizeof tmp))
        perror("setsockopt SO_NONBLOCK");

#endif /* if !__BEOS__ */

}

/*-------------------------------------------------------------------------*/
static void
set_close_on_exec (SOCKET_T new_socket)

/* Set that <new_socket> is closed when the driver performs an exec()
 * (i.e. when starting the ERQ).
 * Failure is acceptable as this is just a nicety.
 */

{
#ifdef HAVE_FCNTL
    fcntl(new_socket, F_SETFD, 1L);
#endif
}

/*-------------------------------------------------------------------------*/
static void
set_socket_own (SOCKET_T new_socket)

/* Enable OOB communication on <new_socket>: the driver is set to
 * receive SIGIO and SIGURG signals, and OOBINLINE is enabled.
 * Failure is acceptable as both facilities are not available everywhere.
 */

{
#if defined(F_SETOWN) && defined(USE_FCNTL_SETOWN)
    if (0 > fcntl(new_socket, F_SETOWN, getpid()))
    {
        perror("fcntl SETOWN");
    }
#endif
#if defined(SO_OOBINLINE) && defined(USE_OOBINLINE)
    {
        int on = 1;
        if (0 > setsockopt(new_socket, SOL_SOCKET, SO_OOBINLINE, (char *)&on, sizeof on))
        {
            perror("setsockopt SO_OOBINLINE");
        }
    }
#endif
    new_socket = 0; /* Prevent 'not used' warning */
}

/*-------------------------------------------------------------------------*/
void
initialize_host_ip_number (void)

/* Initialise the globals host_ip_number and host_ip_address.
 * Open the UDP port if requested so that it can be used
 * in inaugurate_master().
 * exit() on failure.
 */

{
    char host_name[MAXHOSTNAMELEN+1];
    struct hostent *hp;
    char *p;
    length_t tmp;

    if (gethostname(host_name, sizeof host_name) == -1) {
        perror("gethostname");
        exit(1);
    }
    hp = gethostbyname(host_name);
    if (!hp) {
        fprintf(stderr, "%s gethostbyname: unknown host '%s'.\n", time_stamp(), host_name);
        exit(1);
    }
    memset(&host_ip_addr, 0, sizeof host_ip_addr);
    memcpy(&host_ip_addr.sin_addr, hp->h_addr, (size_t)hp->h_length);
    host_ip_addr.sin_family = (unsigned short)hp->h_addrtype;
    host_ip_number = host_ip_addr.sin_addr;

    /* Initialize domain_name for the lexer */
    p = strchr(hp->h_name, '.');
    if (p)
    {
        p++;
        domain_name = strdup(p);
    }
    else
        domain_name = strdup("unknown");

    /* Initialize upd at an early stage so that the master object can use
     * it in inaugurate_master() , and the port number is known.
     */
    if (udp_port >= 0)
    {
        memset(&host_ip_addr.sin_addr, 0, sizeof(host_ip_addr.sin_addr));
#ifndef USE_IPV6
        host_ip_addr.sin_addr.s_addr = INADDR_ANY;
        host_ip_addr.sin_family = AF_INET;
#else
        host_ip_addr.sin_addr = in6addr_any;
        host_ip_addr.sin_family = AF_INET6;
#endif
        host_ip_addr.sin_port = htons((u_short)udp_port);
        debug_message("%s UDP recv-socket requested for port: %d\n"
                     , time_stamp(), udp_port);
        udp_s = socket(host_ip_addr.sin_family, SOCK_DGRAM, 0);
        if (udp_s == -1)
        {
            perror("socket(udp_socket)");
            exit(1);
        }
        tmp = 1;
        if (setsockopt (udp_s, SOL_SOCKET, SO_REUSEADDR,
                        (char *) &tmp, sizeof (tmp)) < 0)
        {
            perror ("setsockopt(udp_s, SO_REUSEADDR)");
            exit(1);
        }

        /* Bind the UDP socket to an address.
         * First, try the given port number, if that one is in use
         * already, let bind() select one. If that one is in use, too,
         * close the socket again and pretend that we never had one.
         * Other errors abort the driver.
         */
        for(;;) {
            if (-1 == bind(udp_s, (struct sockaddr *)&host_ip_addr
                          , sizeof(host_ip_addr)))
            {
                if (errno == EADDRINUSE) {
                    fprintf(stderr, "%s UDP Socket already bound!\n", time_stamp());
                    debug_message("%s UDP Socket already bound!\n", time_stamp());
                    if (host_ip_addr.sin_port) {
                        host_ip_addr.sin_port = 0;
                        continue;
                    }
                    close(udp_s);
                    udp_s = -1;
                } else {
                    perror("udp-bind");
                    exit(1);
                }
            }
            break;
        }
    }

    /* If we got the UDP socket, get query it's real address and
     * initialise it.
     */
    if (udp_s >= 0) {
        tmp = sizeof(host_ip_addr);
        if (!getsockname(udp_s, (struct sockaddr *)&host_ip_addr, &tmp))
        {
            int oldport = udp_port;

            udp_port = ntohs(host_ip_addr.sin_port);
            if (oldport != udp_port)
                debug_message("%s UDP recv-socket on port: %d\n"
                             , time_stamp(), udp_port);
        }
        set_socket_nonblocking(udp_s);
        set_close_on_exec(udp_s);
        if (socket_number(udp_s) >= min_nfds)
            min_nfds = socket_number(udp_s)+1;
    }

} /* initialize_host_ip_number() */


/*-------------------------------------------------------------------------*/
static void
ignore_handler (int signo)

/* Signal handler for ignored signals: it just reinitializes the signal
 * handler for this signal. It is used for OS where a signal(,SIG_IGN)
 * is implemented with a one-shot handler (e.g. Linux).
 */

{
#ifdef DEBUG
    if (signo != SIGPIPE) /* the only ignored signal so far */
    {
        fprintf(stderr, "%s Error: OS passes signo %d instead of SIGPIPE (%d) to handler.\n", time_stamp(), signo, SIGPIPE);
        signo = SIGPIPE;
    }
#endif
    signal(signo, (RETSIGTYPE(*)(int))ignore_handler);
}

/*-------------------------------------------------------------------------*/
static void
urgent_data_handler (int signo)

/* Signal handler for SIGURG/SIGIO: set the urgent_data flag and
 * note the time.
 */

{
    if (d_flag)
        write(2, "received urgent data\n", 21);
    urgent_data = MY_TRUE;
    urgent_data_time = current_time;
    signal(signo, (RETSIGTYPE(*)(int))urgent_data_handler);
}

/*-------------------------------------------------------------------------*/
void
prepare_ipc(void)

/* Open all login sockets on driver startup, exit() on a failure.
 */

{
    length_t tmp;
    int i;

    /* Initialize the telnet machine unless mudlib_telopts() already
     * did that.
     */
    if (!telopts_do[0])
      init_telopts();

    /* Loop over all given port numbers.
     * Remember: positive number are actual port numbers to be opened,
     * negative numbers are the fd numbers of already existing sockets.
     */
    for (i = 0; i < numports; i++) {
        if (port_numbers[i] > 0) {

            /* Real port number */

            memset(&host_ip_addr.sin_addr, 0, sizeof(host_ip_addr.sin_addr));
#ifndef USE_IPV6
            host_ip_addr.sin_addr.s_addr = INADDR_ANY;
            host_ip_addr.sin_family = AF_INET;
#else
            host_ip_addr.sin_addr = in6addr_any;
            host_ip_addr.sin_family = AF_INET6;
#endif
            host_ip_addr.sin_port = htons((u_short)port_numbers[i]);
            sos[i] = socket(host_ip_addr.sin_family, SOCK_STREAM, 0);
            if ((int)sos[i] == -1) {
                perror("socket");
                exit(1);
            }
            tmp = 1;
            if (setsockopt(sos[i], SOL_SOCKET, SO_REUSEADDR
                          , (char *) &tmp, sizeof (tmp)) < 0) {
                perror ("setsockopt");
                exit (1);
            }
            if (bind(sos[i], (struct sockaddr *)&host_ip_addr, sizeof host_ip_addr) == -1) {
                if (errno == EADDRINUSE) {
                    fprintf(stderr, "%s Socket already bound!\n", time_stamp());
                    debug_message("%s Socket already bound!\n", time_stamp());
                    exit(errno);
                } else {
                    perror("bind");
                    exit(1);
                }
            }
        }
        else {

            /* Existing socket */

            sos[i] = -port_numbers[i];
            tmp = sizeof(host_ip_addr);
            if (!getsockname(sos[i], (struct sockaddr *)&host_ip_addr, &tmp))
                port_numbers[i] = ntohs(host_ip_addr.sin_port);
        }

        /* Initialise the socket */
        if (listen(sos[i], 5) == -1) {
            perror("listen");
            exit(1);
        }
#ifndef USE_PTHREAD
        set_socket_nonblocking(sos[i]);
#endif
        set_close_on_exec(sos[i]);

        if (socket_number(sos[i]) >= min_nfds)
            min_nfds = socket_number(sos[i])+1;
    } /* for(i = 0..numports) */

    /* We handle SIGPIPEs ourself */
#if defined(__linux__)
    signal(SIGPIPE, (RETSIGTYPE(*)(int))ignore_handler);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

#if defined(SIGURG)
    signal(SIGURG, (RETSIGTYPE(*)(int))urgent_data_handler);
#endif
#if defined(SIGIO)
    signal(SIGIO, (RETSIGTYPE(*)(int))urgent_data_handler);
#endif
} /* prepare_ipc() */

/*-------------------------------------------------------------------------*/
void
ipc_remove (void)

/* Called when the driver is shutting down, this function closes all
 * open sockets.
 */

{
    int i;

    printf("%s Shutting down ipc...\n", time_stamp());
    for (i = 0; i < numports; i++)
        socket_close(sos[i]);

    if (udp_s >= 0)
        socket_close(udp_s);

#ifdef ERQ_DEMON
    shutdown_erq_demon();
#endif

} /* ipc_remove() */

/*-------------------------------------------------------------------------*/
#ifdef USE_PTHREAD
#define PTHREAD_WRITE_MAX_SIZE 100000

static int
thread_socket_write(SOCKET_T socket, char *msg, size_t size, interactive_t *ip)

/* Stand in for socket_write(): take the data to be written and append
 * it to the buffer list of <ip>.
 */

{
    struct write_buffer_s *b;

    if (size == 0)
        return 0;

    /* Get a new buffer for the data to be written */
    xallocate(b, sizeof(struct write_buffer_s) + size - 1, "thread_socket_write()");
    b->length = size;
    b->next = NULL;

    memcpy(b->buffer, msg, size);

    /* Chain in the new buffer */

    pthread_mutex_lock(&ip->write_mutex);

    if(ip->write_first)
        ip->write_last = ip->write_last->next = b;
    else
        ip->write_first = ip->write_last = b;
    ip->write_size += size;

    /* Make sure that the amount of data pending never exceeds
     * PTHREAD_MAX_SIZE.
     */
    while (ip->write_size >= PTHREAD_WRITE_MAX_SIZE)
    {
        struct write_buffer_s *tmp = ip->write_first;
        ip->write_first = tmp->next;
        ip->write_size -= tmp->length;
        xfree(tmp);
    }

    pthread_mutex_unlock(&ip->write_mutex);
    pthread_cond_signal(&ip->write_cond);

    errno = 0;
    return size;
} /* thread_socket_write() */

/*-------------------------------------------------------------------------*/
static void
writer_thread_cleanup(void *arg)

/* The given thread is canceled - remove all associated data structures.
 */

{
    interactive_t * ip = (interactive_t *) arg;
    struct write_buffer_s *buf = ip->write_first, *tmp;

    while (buf)
    {
	tmp = buf->next;
	if (buf)
            xfree(buf);
	buf = tmp;
    }
    ip->write_first = NULL;

    fprintf(stderr, "Thread %d canceled and cleaned up!\n", pthread_self());
} /* writer_thread_cleanup() */

/*-------------------------------------------------------------------------*/
void *
writer_thread (void *arg)

/* The thread to write the pending data for the given interactive <arg>.
 * The buffer to be written is removed from the chain before the write
 * is attempted, so that a block here won't block add_message().
 */

{
    interactive_t *ip = (interactive_t *) arg;
    struct write_buffer_s *buf;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); /* make us cancelable */
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    pthread_cleanup_push(writer_thread_cleanup, ip);
      /* MacOS X: pthread_cleanup_push() is a macro which opens a new scope
       * and uses scope-local variable to store the cleanup handler.
       * To close the scope, pthread_cleanup_pop() needs to be 'invoked'
       * below.
       */

    while (MY_TRUE)
    {
	/* cancellation point */
	pthread_testcancel();

	/* mutex protected getting of first write_buffer */
	pthread_mutex_lock(&ip->write_mutex);
	if  (!ip->write_first)
        {
            /* if no first write_buffer -> wait on signal from mainthread */
	    pthread_cond_wait(&ip->write_cond, &ip->write_mutex);
        }
	buf = ip->write_first;
	ip->write_first = buf->next;
        ip->write_size -= buf->length;
	pthread_mutex_unlock(&ip->write_mutex);

	/* write the stuff to socket */
        if ((socket_write(ip->socket, buf->buffer, buf->length)) == -1) 
        {
            switch (errno) {
              case EINTR:
                fprintf(stderr, "%s comm: write EINTR. Message discarded.\n", time_stamp());

              case EWOULDBLOCK:
                fprintf(stderr, "%s comm: write EWOULDBLOCK. Message discarded.\n", time_stamp());

              case EMSGSIZE:
                fprintf(stderr, "%s comm: write EMSGSIZE.\n", time_stamp());

              case EINVAL:
                fprintf(stderr, "%s comm: write EINVAL.\n", time_stamp());

              case ENETUNREACH:
                fprintf(stderr, "%s comm: write ENETUNREACH.\n", time_stamp());

              case EHOSTUNREACH:
                fprintf(stderr, "%s comm: write EHOSTUNREACH.\n", time_stamp());

              case EPIPE:
                fprintf(stderr, "%s comm: write EPIPE detected\n", time_stamp());

              default:
                {
                  int e = errno;
                  fprintf(stderr, "%s comm: write: unknown errno %d\n"
                                , time_stamp(), e);
                }
            } /* switch (errno) */
        } /* if socket_write() == -1 */

	if (buf)
        {
	    xfree(buf);
        }
    } /* while forever */

    /* Remove the thread cleanup handler */
    pthread_cleanup_pop(0);
} /* writer_thread() */

#endif /* USE_PTHREAD */

/*-------------------------------------------------------------------------*/
void
add_message (char *fmt, ...)

/* Send a message to the current command_giver. The message is composed
 * from the <fmt> string and the following arguments using the normal
 * printf() semantics. The format string '%s' is special in that it
 * bypasses the normal printf() handling and uses the given string
 * argument directly as data source, allowing to send strings of
 * arbitrary length. All other format strings compose the message to
 * send in a local buffer and are therefore subject to a length
 * restriction.
 *
 * This function also does the telnet, snooping, and shadow handling.
 * If an interactive player is shadowed, object.c::shadow_catch_message()
 * is called to give the shadows the opportunity to intercept the message.
 *
 * All messages are accumulated in interactive.message_buf, which is
 * flushed when it is full. This flush can be forced by passing the
 * special 'string' message_flush (which is actually just a NULL pointer)
 * as <fmt> string to this function.
 *
 * Messages which can't be send (e.g. because the command_giver was
 * destructed or disconnected) are printed on stdout, preceeded by ']'.
 *
 * If an error other than EINTR occured while sending the data to
 * the network, the message is discarded and the socket is marked
 * for disconnection.
 *
 * Note that add_message() might be called recursively.
 */

{
    char  buff[2130];
      /* Composition buffer for the final message.
       * We hope that it's big enough, but to be sure the code will
       * check for buffer overruns.
       * Message is composed starting from buff[1] on, buff[0] is
       * set to '%' for easier snooper-message generation.
       */
    int   length;
    int   min_length;
      /* When accumulating data in ip.message_buf[], this is the
       * threshold over which the buffer will be written to the
       * socket.
       * TODO: Actually, it is used just as a flag for flush/non-flush.
       */
    int   old_message_length;  /* accumulated message length so far */
    char *source;              /* Pointer to the final message to add */
    char *end;                 /* One char past the end of .message_buf[] */
    char *dest;                /* First free char in .message_buf[] */
    va_list va;
    interactive_t *ip;       /* The interactive user */
    object_t      *snooper;  /* Snooper of <ip> */
    int   n;

    va_start(va, fmt);

    /* Test if the command_giver is a real, living, undestructed user,
     * and not disconnected, closing or actually a new ERQ demon.
     * If the command_giver fails the test, the message is printed
     * to stdout and the function returns.
     */
    if ( command_giver == NULL
     || (   command_giver->flags & O_DESTRUCTED
         && fmt != destruct_add_message_format
         && fmt != message_flush )
     || !(O_SET_INTERACTIVE(ip, command_giver))
     || (ip->do_close && fmt != message_flush)
       )
    {
        putchar(']');
        if ( fmt != message_flush ) {
            vprintf(fmt, va);
        }
        fflush(stdout);
        va_end(va);
        return;
    }

    /* First, if a previous call had to discard the message, inform the user.
     */
    if (ip->msg_discarded)
    {
        ip->msg_discarded = MY_FALSE;
        add_message("%s", "\n*** Text lost in transmission ***\n");
        /* msg_discarded might be TRUE again now */
    }

    old_message_length = ip->message_length;

    /* --- Compose the final message --- */

    /* Create the final message and handle snoopers and shadows.
     */

    min_length = MAX_SOCKET_PACKET_SIZE-1;
      /* Allow some wiggle room for source characters like NL which
       * expand into two characters.
       */

    if ( fmt == message_flush )
    {
        /* Just flush, nothing to add */

        min_length = 1;
        source = "";
    }
    else /* add the message */
    {
#ifdef COMM_STAT
        add_message_calls++;
#endif

        /* Compose the final message in buff[] (while checking for overruns)
         * and point source to it.
         * Recognize the special format '%s' to bypass buff[] for messages
         * of arbitrary length.
         */

        if (fmt[0] == '%' && fmt[1] == 's' && !fmt[2])
        {
            source = va_arg(va, char *);
            va_end(va);
        }
        else
        {
            buff[(sizeof buff) - 1] = '\0'; /* Overrun sentinel */
            vsprintf(buff+1,fmt,va);
            va_end(va);
            if (buff[(sizeof buff) - 1])
            {
                comm_fatal(ip, "To long message!\n");
                return;
            }
            source = buff+1;
        }

        /* If we're not sending a telnet command with this message,
         * pass on the new data to any snooper and/or shadow
         */

        if (!sending_telnet_command)
        {

            /* If there's a shadow successfully handling the
             * message, return.
             * This may cause a recursive call to add_message()!.
             */

            if (shadow_catch_message(command_giver, source))
                return;

            /* If there's a snooper, send it the new message prepended
             * with a '%'.
             * For interactive snoopers this means a recursion with
             * the command_giver set to the snooper, for non-interactive
             * snoopers it's a simple call to tell_npc(), with an
             * adaption of the global trace_level to this users trace
             * settings.
             */

            if ( NULL != (snooper = ip->snoop_by)
             && !(snooper->flags & O_DESTRUCTED))
            {
                buff[0] = '%';
                if (O_IS_INTERACTIVE(snooper))
                {
                    object_t *save;

                    save = command_giver;
                    command_giver = snooper;
                    if (source != buff+1) {
                        if (strlen(source) >= sizeof buff - 1) {
                            add_message("%s", "%");
                            add_message("%s", source);
                        } else {
                            strcpy(buff+1, source);
                            add_message("%s", buff);
                        }
                    }
                    else
                    {
                        add_message("%s", buff);
                    }
                    command_giver = save;
                }
                else
                {
                    trace_level |= ip->trace_level;
                    if (source != buff+1) {
                        if (strlen(source) >= sizeof buff - 1)
                        {
                            tell_npc(snooper, "%");
                            tell_npc(snooper, source);
                        } else
                        {
                            strcpy(buff+1, source);
                            tell_npc(snooper, buff);
                        }
                    } else
                    {
                        tell_npc(snooper, buff);
                    }
                }
            } /* if (snooper) */
        } /* if (!sending_telnet_command */
    } /* if (flush or not) */

#ifdef DEBUG
    if (d_flag > 1)
        debug_message("%s [%s(%ld)]: %s"
                     , time_stamp(), command_giver->name
                     , (long)strlen(source), source);
#endif

    /* --- Send the final message --- */

    /* Append the final message to the .message_buf[], taking
     * care of all the necessary charset and telnet translations.
     */

    dest = &ip->message_buf[old_message_length];
    end  = &ip->message_buf[sizeof ip->message_buf];

    /* This loop advances source until it reaches a '\0' character.
     * Every character encountered is copied, translated or fed
     * into the telnet machine.
     */

#ifdef DEBUG_TELNET
if (sending_telnet_command)
{
    char *cp;
    printf("%s TDEBUG: '%s' Sending telnet (%ld bytes): "
          , time_stamp(), ip->ob->name, (long)strlen(source));
    for (cp = source; *cp; cp++)
        printf(" %02x", (unsigned char)*cp);
    printf("\n");
}
#endif

    do {
        int    retries;   /* Number of retries left when sending data */
        ptrdiff_t chunk;  /* Current size of data in .message_buf[] */
        char   c;         /* Currently processed character */

        /* TODO: a simple while (dest != end) should do */

        if (dest == end)
        {
            c = '\0';  /* buffer (again) full: just send the data */
        }
        else for (;;) {
            c = *source++;

            /* Process the character:
             *  - copy it if the corresponding .charset bit is set,
             *    or if it's part of a telnet command.
             *  - translate a '\n' into '\r\n'
             *  - double an IAC if quote_iac is active.
             *  - stop this loop if a '\0' is encountered or if
             *    the buffer is full.
             *
             * If c is not '\0' after this loop, it will be added
             * immediately to .message_buf[] after data has been sent.
             */
            if (ip->charset[(c&0xff)>>3] & 1<<(c&7)
             || (c && sending_telnet_command)
               )
            {
                *dest++ = c;
            }
            else if (c == '\0')
            {
                source--; /* Recreate exit condition for outer loop */
                break;
            }
            else if (c == '\n')
            {
                /* Insert CR before NL */
                *dest++ = '\r';
                if (dest == end)
                    break;
                *dest++ = c;
            }
            else if ( (unsigned char)c == IAC && ip->quote_iac)
            {
                *dest++ = c;
                if (dest == end)
                    break;
                *dest++ = c;
            }

            /* If the buffer is full: exit loop and send the data */
            if (dest == end)
            {
                c = '\0';
                break;
            }
        } /* for() */

        /* Check how much data there is in .message_buf[].
         * If it is enough, send it, else terminate the outer loop
         * (because *source must be exhausted for this to happen).
         */
        chunk = dest - ip->message_buf;
        if (chunk < min_length)
        {
            break;
        }

        /* Write .message_buf[] to the network. */

        for (retries = 6;;) {

#ifdef USE_PTHREAD
            if ((n = (int)thread_socket_write(ip->socket, ip->message_buf, (size_t)chunk, ip)) != -1)
#else
            if ((n = (int)socket_write(ip->socket, ip->message_buf, (size_t)chunk)) != -1)
#endif
            {
                break;
            }
            switch (errno) {
              case EINTR:
                if (--retries)
                    continue;
                ip->msg_discarded = MY_TRUE;
                fprintf(stderr,
                  "%s comm: write EINTR. Message discarded.\n", time_stamp());
                if (old_message_length)
                    remove_flush_entry(ip);
                return;

              case EWOULDBLOCK:
                ip->msg_discarded = MY_TRUE;
                if (d_flag)
                    fprintf(stderr,
                      "%s comm: write EWOULDBLOCK. Message discarded.\n", time_stamp());
                if (old_message_length)
                    remove_flush_entry(ip);
                return;

              case EMSGSIZE:
                fprintf(stderr, "%s comm: write EMSGSIZE.\n", time_stamp());
                return;

              case EINVAL:
                fprintf(stderr, "%s comm: write EINVAL.\n", time_stamp());
                break;

              case ENETUNREACH:
                fprintf(stderr, "%s comm: write ENETUNREACH.\n", time_stamp());
                break;

              case EHOSTUNREACH:
                fprintf(stderr, "%s comm: write EHOSTUNREACH.\n", time_stamp());
                break;

              case EPIPE:
                fprintf(stderr, "%s comm: write EPIPE detected\n", time_stamp());
                break;

              default:
                {
                  int e = errno;
                  perror("write");
                  fprintf(stderr, "%s comm: write: unknown errno %d\n"
                                , time_stamp(), e);
                }
            }
            if (old_message_length)
                remove_flush_entry(ip);
            ip->do_close = FLAG_DO_CLOSE;
            return;

        } /* for (retries) */

#ifdef COMM_STAT
        inet_packets++;
        inet_volume += n;
#endif
        if (n != chunk)
            fprintf(stderr, "%s write socket: wrote %ld, should be %ld.\n"
                          , time_stamp(), (long)n, (long)chunk);

        /* Continue with the processing of source */
        dest = &ip->message_buf[0];
        if (c)
            *dest++ = c;
    } while (*source);

    /* --- Final touches --- */

    ip->message_length = length = dest - ip->message_buf;

    /* Update the ring of interactives with pending data */

    if ( length && !old_message_length )
    {
        /* Buffer became 'dirty': add this interactive to the list */

        if ( NULL != (ip->next_player_for_flush = first_player_for_flush) )
        {
            O_GET_INTERACTIVE(first_player_for_flush)->
              previous_player_for_flush =
                command_giver;
        }
        ip->previous_player_for_flush = 0;
        first_player_for_flush = command_giver;
    }
    if ( !length && old_message_length ) /* buffer has become empty */
    {
        remove_flush_entry(ip);
    }
}

/*-------------------------------------------------------------------------*/
static INLINE void
reset_input_buffer (interactive_t *ip)

/* When returning from CHARMODE to LINEMODE, the input buffer variables
 * need to be reset. This function takes care of it.
 */

{
    if (ip->command_start)
    {
        DT(("'%s' reset input buffer: cmd_start %d, tn_start %d, tn_end %d\n", ip->ob->name,  ip->command_start, ip->tn_start, ip->tn_end));
        ip->tn_start -= ip->command_start;
        ip->tn_end -= ip->command_start;
        if (ip->tn_start < 0)
            ip->tn_start = 0;
        if (ip->tn_end <= 0)
            ip->tn_end = 0;
        else
        {
            move_memory( ip->text, ip->text + ip->command_start
                       , ip->tn_end
                       );
        }
        ip->text_end = ip->tn_end;
        if (ip->command_end)
            ip->command_end = ip->tn_end;
        ip->command_start = 0;
    }
} /* reset_input_buffer() */

/*-------------------------------------------------------------------------*/
static void
remove_flush_entry (interactive_t *ip)

/* Remove the given interactive <ip> from the list of 'dirty' interactives
 * and make sure it is really clean.
 *
 * This function is called after an interactive sent all pending data (or
 * failing while doing so).
 */

{
    ip->message_length = 0;
    if ( ip->previous_player_for_flush )
    {
        O_GET_INTERACTIVE(ip->previous_player_for_flush)->next_player_for_flush
          = ip->next_player_for_flush;
    }
    else
    {
        first_player_for_flush = ip->next_player_for_flush;
    }

    if ( ip->next_player_for_flush )
    {
        O_GET_INTERACTIVE(ip->next_player_for_flush)->previous_player_for_flush
          = ip->previous_player_for_flush;
    }
}

/*-------------------------------------------------------------------------*/
void
flush_all_player_mess (void)

/* Flush all pending data from the interactives. Usually called before
 * every input loop, after a user logged in, or after an LPC runtime
 * error was processed.
 */

{
    object_t *p, *np;
    object_t *save = command_giver;

    for ( p = first_player_for_flush; p != NULL; p = np)
    {
        np = O_GET_INTERACTIVE(p)->next_player_for_flush;
          /* add_message() will clobber (p)->next_player_for_flush! */
        command_giver = p;
        add_message(message_flush);
    }
    command_giver = save;
}

/*-------------------------------------------------------------------------*/
Bool
get_message (char *buff)

/* Get a message from a user, or wait until it is time for the next
 * heartbeat/callout. You can tell this apart by the result:
 *
 *   true: a user message was received and placed into buff; the user
 *         object is set as command_giver.
 *   false: it is just time to call the heart_beat.
 *
 * In both cases, time_to_call_heart_beat is set if a heartbeat is due.
 *
 * Internally, get_message() scans the array of interactive users in
 * search for one with a complete message in its incoming buffer. If
 * an earlier select() marked the socket for the current user as pending
 * with data, this data is read into the buffer before the check for
 * a message is performed. get_message() returns for the first user found
 * with a complete message. Since get_message() keeps its own
 * status of which user was looked at last, the next call to get_message()
 * will continue the scan where it left off.
 *
 * If no user has a complete message, a call to select() waits for more
 * incoming data. If this succeeds (and no heartbeat requires an
 * immediate return), the cycle begins again. If a heart_beat is due
 * even before select() executed, the waiting time for select() is
 * set to 0 so that only the status of the sockets is recorded and
 * get_message returns (almost) immediately.
 *
 * Normally, users can give only one command per cycle. The exception
 * is when they are editing, then they can give up to ALLOWED_ED_CMDS.
 *
 * Heartbeats are detected by checking the backend variable comm_time_-
 * to_call_heart_beat, which is set by the SIGALRM handler. If it is
 * true, get_message() sets the variable time_to_call_heart_beat to
 * inform the backend and returns.
 *
 * If a heart_beat occurs during the reading and returning of player
 * commands, the comm_time_c_h_b variable is set, but not evaluated.
 * This evaluation happens only when a select() is performed (therefore
 * the second variable time_to_c_h_b). This way every user can issure
 * at least one command in one backend cycle, even if that takes longer
 * than one heart_beat time. This makes it also legal for comm_to_c_h_b
 * to be set upon entering get_message().
 *
 * For short latency, the UDP socket is checked on every call to
 * get_message(), even if a previous select() did not mark it as ready
 * (this is disabled under BeOS and Windows).
 */

{
    /* State information: */
    static fd_set readfds;
      /* List of sockets with pending data.
       * You can ignore a 'could be used uninitialized' warning.
       */
    static int NextCmdGiver = -1;
      /* Index of current user to check */
    static int CmdsGiven = 0;
      /* Number of commands the current user gave in this cycle. */

#   define StartCmdGiver  (max_player)
#   define IncCmdGiver    NextCmdGiver--

    int    i;
    interactive_t * ip = NULL;
    fd_set exceptfds;


    /* The endless loop */

    while(MY_TRUE)
    {
        struct sockaddr_in addr;
        length_t length; /* length of <addr> */
        struct timeval timeout;

        /* --- select() on the sockets and handle ERQ --- */

        /* This also removes users which connection is marked
         * as to be closed.
         */

        if (NextCmdGiver < 0)
        {
            int nfds;     /* number of fds for select() */
            int res;      /* result from select() */
            int twait;    /* wait time in seconds for select() */
            int retries;  /* retries of select() after EINTR */

            flush_all_player_mess();
            twait = comm_time_to_call_heart_beat ? 0 : 1;
              /* If the heart_beat is due, just check the state
               * of the sockets, but don't wait.
               */

            /* Set up readfds */

            FD_ZERO(&readfds);
            for (i = 0; i < numports; i++) {
                FD_SET(sos[i], &readfds);
            } /* for */
            nfds = min_nfds;
            for (i = max_player + 1; --i >= 0;)
            {
                ip = all_players[i];
                if (!ip)
                    continue;

                if (ip->do_close)
                {
                    ip->do_close &= FLAG_PROTO_ERQ;
                    remove_interactive(ip->ob, MY_FALSE);
                    continue;
                }

                if (ip->tn_state == TS_READY)
                {
                    /* If telnet is ready for commands, react quickly. */
                    twait = 0;
                }
                else
                {
                    FD_SET(ip->socket, &readfds);
                    if (socket_number(ip->socket) >= nfds)
                        nfds = socket_number(ip->socket)+1;
                }
            } /* for (all players) */
#ifdef ERQ_DEMON
            if (erq_demon >= 0)
            {
                FD_SET(erq_demon, &readfds);
            }
#endif
            if (udp_s >= 0)
            {
                FD_SET(udp_s, &readfds);
            }

            /* select() until time is up or there is data */

            for (retries = 6;;)
            {
                check_alarm();
                timeout.tv_sec = twait;
                timeout.tv_usec = 0;
                res = socket_select(nfds, &readfds, 0, 0, &timeout);
                if (res == -1)
                {
                    /* BeOS <= PR2 returns errno -1 instead of EINTR :-( */
                    if (errno == EINTR || errno == -1)
                    {
                        /* We got an alarm, probably need heart_beat.
                         * But try to finish the select call since we already
                         * have prepared readfds.
                         */
                        if (comm_time_to_call_heart_beat)
                            twait = 0;
                        if (--retries >= 0)
                            continue;
                    }
                    else
                    {
                        perror("select");
                    }

                    /* Despite the failure, pretend select() suceeded with
                     * zero sockets to read, and process heart_beat / buffered
                     * commands.
                     */
                    FD_ZERO(&readfds);
                }
                break;
            } /* for (retries) */

            /* If we got a SIGIO/SIGURG, telnet wants to synch with us.
             */
            if (urgent_data)
            {
                DT(("telnet wants to sync\n"));
                check_alarm();
                urgent_data = MY_FALSE;
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                memset((char *)&exceptfds, 255, (size_t)(nfds + 7) >> 3);
                if (socket_select(nfds, 0, 0, &exceptfds, &timeout) > 0)
                {
                    for (i = max_player + 1; --i >= 0;)
                    {
                        ip = all_players[i];
                        if (!ip)
                            continue;
                        if (FD_ISSET(ip->socket, &exceptfds))
                        {
                            DT(("'%s' ts_data = TS_SYNCH\n", ip->ob->name));
                            ip->ts_data = TS_SYNCH;
                            switch (ip->tn_state)
                            {
                              case TS_DATA:
                              case TS_CR:
                              case TS_READY:
                                ip->tn_state = TS_SYNCH;
                                ip->gobble_char = '\0';
                                DT(("'%s' tn_state = TS_SYNCH\n", ip->ob->name));
                            }
                        }
                    } /* for (all players) */
                }
                /* Maybe the data didn't arrive yet, so try again later.
                 * But don't waste time doing it for too long.
                 */
                else if (current_time - urgent_data_time < 600)
                {
                    urgent_data = MY_TRUE;
                }
            } /* if (urgent_data) */

            /* Initialise the user scan */
            CmdsGiven = 0;
            NextCmdGiver = StartCmdGiver;

#ifdef ERQ_DEMON

            /* --- Handle data from the ERQ ---
             * TODO: This should be a function on its own.
             * TODO: Define the erq messages as structs.
             */
            if (erq_demon >= 0 && FD_ISSET(erq_demon, &readfds))
            {
                mp_int l;
                mp_int msglen;  /* Length of the current erq message */
                mp_int rest;
                int32  handle;
                char  *rp;      /* Read pointer into buf_from_erq[] */

                FD_CLR(erq_demon, &readfds);

                /* Try six times to read data from the ERQ, appending
                 * it to what is already in buf_from_erq[].
                 */
                retries = 6;
                do {
                    l = socket_read(
                      erq_demon,
                      input_from_erq,
                      (size_t)(&buf_from_erq[sizeof buf_from_erq] - input_from_erq)
                    );
                } while(l < 0 && errno == EINTR && --retries >= 0);

                /* If there is no data, stop the erq, else handle it. */

                if (l <= 0)
                {
#ifdef DEBUG_ERQ
                    fprintf(stderr, "%s read %ld bytes from erq demon\n"
                                  , time_stamp(), l);
                    if (l)
                        perror("");
#endif /* DEBUG_ERQ */
                    stop_erq_demon(MY_TRUE);
                }
                else
                {
                    input_from_erq += l;
                    l = input_from_erq - &buf_from_erq[0];
                    rp = buf_from_erq;

                    /* l is the amount of data left to consider,
                     * rp points to the data to be considered next.
                     *
                     * Loop while there are messages left in the buffer.
                     */

                    for (; l >= 8 && l >= (msglen = read_32(rp))
                         ; rp += msglen, l -= msglen)
                    {
                        Bool keep_handle;

                        /* Is the message length valid?
                         * TODO: use sizeof(struct) here
                         */
                        if (msglen < 8) {
#ifdef DEBUG_ERQ
                            fprintf( stderr
                                   , "%s invalid length of message from"
                                     "erq demon: %ld bytes\n"
                                   , time_stamp(), msglen);
#endif /* DEBUG_ERQ */
                            stop_erq_demon(MY_TRUE);
                            break;
                        }

                        handle = read_32(rp+4); /* get the handle */

                        if (handle == ERQ_HANDLE_KEEP_HANDLE
                         && msglen >= 8)
                        {
                            /* _KEEP_HANDLE wrapper are used when
                             * more messages for the (real) handle
                             * are to be expected. The real message
                             * follows after the _KEEP_HANDLE.
                             */
                            handle = read_32(rp+8); /* the real handle */
                            keep_handle = MY_TRUE;
                            msglen -= 4;  /* adjust the message parameters */
                            l -= 4;
                            rp += 4;
                        }
                        else if (handle == ERQ_HANDLE_RLOOKUP)
                        {
                            /* The result of a hostname lookup. */

                            if (msglen < 13 || rp[msglen-1]) {
#ifdef DEBUG
                              if (msglen == 12) {
                                if (d_flag > 1)
                                  debug_message("%s Host lookup failed\n"
                                               , time_stamp());
                              } else {
                                debug_message("%s Bogus reverse name lookup.\n"
                                             , time_stamp());
                              }
#endif
                            } else {
                                int32 naddr;
                                struct in_addr net_addr;

                                memcpy((char*)&naddr, rp+8, 4);
#ifndef USE_IPV6
                                net_addr.s_addr = naddr;
#else
                                CREATE_IPV6_MAPPED(net_addr, naddr);
#endif
                                add_ip_entry(net_addr, rp+12);
                            }
                            continue;
                        }
#ifdef USE_IPV6
                        else if (handle == ERQ_HANDLE_RLOOKUPV6)
                        {
                            /* The result of a hostname lookup. */

                            if (msglen < 9 || rp[msglen-1]) {
#ifdef DEBUG
                                debug_message("%s Bogus reverse name lookup.\n"
                                             , time_stamp());
#else
                                NOOP;
#endif
                            } else {
                                char * space;

                                space = strchr(rp+8, ' ');

                                if (space == NULL)
                                {
                                    debug_message("%s IP6 Host lookup failed: %s\n"
                                                 , time_stamp(), rp+8);
                                }
                                else if (strlen(space+1))
                                {
                                    *space = '\0';
                                    update_ip_entry(rp+8, space+1);
                                }
                            }
                            continue;
                        }
#endif /* USE_IPV6 */
                        else
                        {
                            /* remove the callback handle after processing
                             * the message.
                             */
                            keep_handle = MY_FALSE;
                        }

                        /* We have an ERQ message for a user supplied
                         * handle - execute it (after some sanity checks).
                         */

                        if ((uint32)handle < MAX_PENDING_ERQ
                         && (   (rest = msglen - 8) <= max_array_size
                             || !max_array_size)
                         && pending_erq[handle].type != T_INVALID)
                        {
                            svalue_t *erqp = &pending_erq[handle];
                            char *cp;
                            vector_t *v;
                            svalue_t *svp;
                            object_t *ob;
                            wiz_list_t *user;

                            command_giver = 0;
                            current_interactive = 0;
                            ob = !CLOSURE_MALLOCED(erqp->x.closure_type)
                                 ? erqp->u.ob
                                 : erqp->u.lambda->ob;
                            current_object = ob;
                            v = allocate_array(rest);
                            current_object = NULL;
                            push_referenced_vector(v);
                            push_number(rest);
                            cp = rp + 8;
                            for (svp = v->item; --rest >=0; svp++)
                            {
                                svp->u.number = *cp++;
                            }
                            user = ob->user;
                            if (user->last_call_out != current_time)
                            {
                                user->last_call_out = current_time;
                                CLEAR_EVAL_COST;
                            } else {
                                assigned_eval_cost = eval_cost = user->call_out_cost;
                            }
                            RESET_LIMITS;
                            secure_call_lambda(erqp, 2);
                            user->call_out_cost = eval_cost;
                            if (!keep_handle || (ob->flags & O_DESTRUCTED))
                            {
                                free_svalue(erqp);
                                erqp->type = T_INVALID;
                                erqp->u.lvalue = free_erq;
                                free_erq = erqp;
                            }
                        } /* if(valid handle) */

                        /* Messages for invalid handles are no error: e.g. the
                         * object could have gone away unexpectantly before
                         * the erq had time to answer.
                         */

                    } /* for (l,rp in buf_from_erq) */

                    /* Delete the processed data from the buffer */
                    if (rp != buf_from_erq)
                    {
                        move_memory(buf_from_erq, rp, (size_t)l);
                        input_from_erq = &buf_from_erq[l];
                    }
                } /* if (read data from erq) */
            } /* if (erq socket ready) */

#endif /* ERQ_DEMON */

            /* --- Try to get a new player --- */
            for (i = 0; i < numports; i++)
            {
                if (FD_ISSET(sos[i], &readfds))
                {
                    SOCKET_T new_socket;

                    length = sizeof addr;
                    new_socket = accept(sos[i], (struct sockaddr *)&addr
                                              , &length);
                    if ((int)new_socket != -1)
                        new_player(new_socket, &addr, (size_t)length
                                  , port_numbers[i]);
                    else if ((int)new_socket == -1
                      && errno != EWOULDBLOCK && errno != EINTR
                      && errno != EAGAIN && errno != EPROTO )
                    {
                        /* EBADF would be a valid cause for an abort,
                         * same goes for ENOTSOCK, EOPNOTSUPP, EFAULT.
                         * However, don't abort() because that tends to
                         * leave Mud admins baffled (and would opens the 
                         * door for DoS attacks).
                         */
                        int errorno = errno;
                        fprintf( stderr
                               , "%s comm: Can't accept on socket %d "
                                 "(port %d): %s\n"
                               , time_stamp(), sos[i], port_numbers[i]
                               , strerror(errorno)
                               );
                        debug_message("%s comm: Can't accept on socket %d "
                                      "(port %d): %s\n"
                                     , time_stamp(), sos[i], port_numbers[i]
                                     , strerror(errorno)
                                     );
                        /* TODO: Was: perror(); abort(); */
                    }
                }
            } /* for */
            /* check for alarm signal (heart beat) */
            if (comm_time_to_call_heart_beat)
            {
                time_to_call_heart_beat = MY_TRUE;
                return MY_FALSE;
            }
        } /* if (no NextCmdGiver) */

        /* See if we got any udp messages.
         * We don't test readfds so that we can accept udp messages with
         * short latency. But for the same reason, it was necessary to
         * include the descriptor number in the set to be selected on.
         * Note for BeOS and Cygwin: since making sockets non-blocking
         *   is a bit tricky, we check if the socket is actually ready,
         *   to prevent freezing.
         * TODO: Always use the readfds information.
         */
#if !defined(__BEOS__) && !defined(CYGWIN)
        if (udp_s >= 0)
#else
        if (udp_s >= 0 && FD_ISSET(udp_s, &readfds))
#endif
        {
            char udp_buf[1024+1], *st;
            int cnt;

            length = sizeof addr;
            cnt = recvfrom(udp_s, udp_buf, sizeof(udp_buf)-1, 0
                          , (struct sockaddr *)&addr, &length);
            if (cnt != -1)
            {
                command_giver = NULL;
                current_interactive = NULL;
                current_object = NULL;
                trace_level = 0;
                udp_buf[sizeof(udp_buf) - 1] = '\0';
                udp_buf[cnt] = '\0';
#ifndef USE_IPV6
                st = inet_ntoa(addr.sin_addr);
#else
                st = inet6_ntoa(addr.sin_addr);
#endif
                push_string_malloced(st);
                push_string_malloced(udp_buf);
                push_number(ntohs(addr.sin_port));
                RESET_LIMITS;
#ifndef USE_DEPRECATED
                apply_master_ob(STR_RECEIVE_UDP, 3);
#else
                if (!apply_master_ob(STR_RECEIVE_UDP, 3))
                {
                    push_string_malloced(st);
                    push_string_malloced(udp_buf);
                    push_number(ntohs(addr.sin_port));
                    apply_master_ob(STR_RECEIVE_IMP, 3);
                }
#endif
                CLEAR_EVAL_COST;
            }
        } /* if (upd_s) */

        /* --- The Scan for User Commands --- */

        for (; NextCmdGiver >= 0; IncCmdGiver)
        {
            object_t *snooper;

            ip = all_players[NextCmdGiver];
            if (ip == 0) {
                continue;
            }

            /* Get the data (if any), at max enough to fill .text[] */

            if (FD_ISSET(ip->socket, &readfds)) {
                int l;

                l = MAX_TEXT - ip->text_end;

                l = socket_read(ip->socket, ip->text + ip->text_end, (size_t)l);
                if (l == -1) {
                    if (errno == ENETUNREACH) {
                        debug_message("%s Net unreachable detected.\n"
                                     , time_stamp());
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == EHOSTUNREACH) {
                        debug_message("%s Host unreachable detected.\n"
                                     , time_stamp());
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == ETIMEDOUT) {
                        debug_message("%s Connection timed out detected.\n"
                                     , time_stamp());
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == ECONNRESET) {
                        debug_message("%s Connection reset by peer detected.\n"
                                     , time_stamp());
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == EWOULDBLOCK) {
                        debug_message("%s read would block socket %d!\n"
                                     , time_stamp(), ip->socket);
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == EMSGSIZE) {
                        debug_message("%s read EMSGSIZE\n", time_stamp());
                        continue;
                    }
                    if (errno == ESHUTDOWN) {
                        debug_message("%s Connection to socket %d lost.\n"
                                     , time_stamp(), ip->socket);
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    if (errno == EBADF) {
                        if (ip->ob)
                            debug_message("%s Socket %d (ip %p '%s') is a bad descriptor.\n"
                                         , time_stamp(), ip->socket, ip
                                         , ip->ob->name);
                        else
                            debug_message("%s Socket %d (ip %p) is a bad descriptor.\n"
                                         , time_stamp(), ip->socket, ip);
                        remove_interactive(ip->ob, MY_FALSE);
                        continue;
                    }
                    perror("read");
                    debug_message("%s Unknown errno %d\n", time_stamp(), errno);
                    remove_interactive(ip->ob, MY_FALSE);
                    continue;
                }
                if (l == 0) {
                    if (ip->closing)
                        comm_fatal(ip, "Tried to read from closing socket.\n");
                        /* This will forcefully disconnect the user */
                    else
                        remove_interactive(ip->ob, MY_FALSE);
                    continue;
                }
                ip->text_end += l;

                /* Here would be the place to send data through an
                 * outportal instead of returning it.
                 */

                telnet_neg(ip);
            } /* if (cmdgiver socket ready) */

            /* if ip->text[0] does not hold a valid character, the outcome
             * of the comparison to '!' does not matter.
             */
            if ((ip->noecho & (CHARMODE_REQ|CHARMODE)) == (CHARMODE_REQ|CHARMODE))
            {
                DT(("'%s' CHARMODE_REQ\n", ip->ob->name));
                if (ip->text[0] != '!' || find_no_bang(ip) & IGNORE_BANG )
                {
                    /* Unescaped input.
                     * Puts the next character(s) (addressed by
                     * .command_start) into buff[0] and return the data.
                     */

                    int destix;  /* Save index */
                    DT(("'%s'   Unescaped input\n", ip->ob->name));
                    
                    if (ip->tn_state != TS_READY)
                    {
                        /* .text[] contains an incomplete negotiation.
                         * Set .chars_ready the amount of pure data available
                         * and temporarily suspend the telnet machine.
                         */
                        length = (TN_START_VALID(ip->tn_state)
                                   ? ip->tn_start
                                   : ip->command_end
                                 ) - ip->command_start;
                        DT(("'%s'     incomplete negotiation: length %d\n"
                           , ip->ob->name, length));
                        if (!length)
                            continue;
                        if (length < 0)
                        {
                            comm_fatal(ip, "comm: data length < 0: %ld\n", (long)length);
                            continue;
                        }
                        DT(("'%s'     save machine state %d, set to %d (READY)\n"
                          , ip->ob->name, ip->tn_state, TS_READY));
                        ip->save_tn_state = ip->tn_state;
                        ip->chars_ready = length;
                        ip->tn_state = TS_READY;
                    }
                    else if (!ip->chars_ready)
                    {
                        /* Empty input: we received an end of line.
                         * The telnet machine is already suspended, but
                         * we have to set the state for it to return to.
                         * At the moment it is TS_INVALID, so the next
                         * character received would be thrown away.
                         */
                        DT(("'%s'     save machine state %d (DATA)\n"
                          , ip->ob->name, TS_DATA));
                        ip->save_tn_state = TS_DATA;
                        /* tn_state is TS_READY */
                    }

                    /* Copy as many characters from the text[] into
                     * the buff[] as possible.
                     */
                    DT(("'%s'   %ld chars ready\n", ip->ob->name
                       , (long)ip->chars_ready));
                    for (destix = 0; destix < ip->chars_ready; )
                    {
                        char ch;

                        ch = ip->text[ip->command_start++];
                        buff[destix++] = ch;
                        if (!(ip->combine_cset[(ch&0xff) / 8] & (1 << (ch % 8)))
                         || !ch
                           )
                        {
                            /* This character can't be combined (or it is the
                             * end of the line).
                             * If it is not the first character encountered,
                             * undo the previous store; in either case break
                             * the loop.
                             */
                            if (destix != 1)
                            {
                                destix--;
                                ip->command_start--;
                            }
                            break;
                        }
                    }

                    /* destix is now the number of characters stored in
                     * buff[], and is usually at least 1. If it's not, it's
                     * the end of the line and the telnet machine is correct
                     * already.
                     */
                    
                    if (destix > 0 && !buff[destix-1])
                    {
                        /* End of line. Reinitialise the telnet machine
                         */
                        DT(("'%s'     end of line: reinit telnet machine\n", ip->ob->name));
                        buff[destix-1] = '\n';
                        ip->command_start = 0;
                        ip->tn_state = TS_DATA;
                        telnet_neg(ip);
                    }

                    buff[destix] = '\0';

                    ip->chars_ready -= destix;
                    DT(("'%s'   %ld chars left ready\n", ip->ob->name
                       , (long)ip->chars_ready));
                    if (!ip->chars_ready)
                    {
                        /* All the pure data was read, now restore the
                         * old telnet machine state.
                         * Leave the first char in to make '!' possible
                         */
                        DT(("'%s'     restore old telnet machine state %d\n"
                           , ip->ob->name, ip->save_tn_state));
                        ip->tn_state = ip->save_tn_state;
                        ip->save_tn_state = TS_INVALID;
                        ip->tn_start -= ip->command_start - 1;
                        ip->command_end -= ip->command_start - 1;

                        if (ip->command_end > 0)
                        {
                            move_memory( ip->text, ip->text+ip->command_start
                                       , ip->command_end
                                       );
                        }

                        ip->command_start = 1;

                        /* When receiving a pure data line in charmode, starting
                         * with the second char, these two values may become
                         * negative. We have to correct them then to point
                         * to ip->command_start.
                         */
                        if (ip->tn_start < 1)
                            ip->tn_start = 1;
                        if (ip->command_end < 1)
                            ip->command_end = 1;

                        ip->text_end = ip->tn_end = ip->command_end;
                    }

                    command_giver = ip->ob;
                    trace_level = ip->trace_level;
                    IncCmdGiver;
                    CmdsGiven = 0;
                    ip->last_time = current_time;

                    DT(("'%s' --- return with char command %02x '%c' ---\n", ip->ob->name, buff[0], buff[0]));
                    return MY_TRUE;
                }
                else if (ip->tn_state != TS_READY)
                {
                    length = (TN_START_VALID(ip->tn_state)
                              ? ip->tn_start
                              : ip->command_end
                             ) - ip->command_start;
                    DT(("'%s'   data length %d\n", ip->ob->name, length));
                    if (length < 0)
                    {
                        comm_fatal(ip, "comm: data length < 0: %ld\n", (long)length);
                        continue;
                    }
                    if (length > ip->chars_ready)
                    {
#ifdef USE_PTHREAD
                        thread_socket_write(ip->socket, ip->text + ip->chars_ready
                                    , (size_t)(length - ip->chars_ready), ip);
#else
                        socket_write(ip->socket, ip->text + ip->chars_ready
                                    , (size_t)(length - ip->chars_ready));
#endif
                        ip->chars_ready = length;
                    }
                }
            } /* if (CHARMODE_REQ) */

            /* The telnet negotiation produces the commands starting at
             * the beginning of .text[] and terminated with a '\0'. Whenever
             * a command is complete, the tn_state is TS_READY.
             */
            if (ip->tn_state == TS_READY)
            {
                /* We have a command: copy it into buff, handle a
                 * possible snooper and return.
                 */

                DT(("'%s' telnet machine ready\n", ip->ob->name));
                strcpy(buff, ip->text);
                command_giver = ip->ob;
                trace_level = ip->trace_level;

                /* Reinitialize the telnet machine, possibly already
                 * producing the next command in .text[].
                 */
                ip->tn_state = TS_DATA;
                telnet_neg(ip);

                /* If the user is not in ed, don't let him issue another command
                 * before the poll comes again.
                 */
                if (O_GET_SHADOW(ip->ob)->ed_buffer
                 && CmdsGiven < ALLOWED_ED_CMDS)
                {
                    CmdsGiven++;
                    FD_CLR(ip->socket, &readfds);
                }
                else
                {
                    IncCmdGiver;
                    CmdsGiven = 0;
                }

                /* Manage snooping - should the snooper see type ahead?
                 * Well, he doesn't here.
                 */
                if (NULL != (snooper = ip->snoop_by)
                 && !(snooper->flags & O_DESTRUCTED)
                 && !(ip->noecho & NOECHO_REQ)
                   )
                {
                    if (O_IS_INTERACTIVE(snooper))
                    {
                        command_giver = snooper;
                        add_message("%% %s\n", buff);
                    }
                    else
                    {
                        char *snoop_message = alloca(strlen(buff) + 4);
                        sprintf(snoop_message, "%% %s\n", buff);
                        tell_npc(snooper, snoop_message);
                    }
                    command_giver = ip->ob;
                }
                ip->last_time = current_time;

                if ((ip->noecho & (CHARMODE_REQ|CHARMODE)) == CHARMODE_REQ)
                {
                    DT(("'%s'   clear CHARMODE as it was refused anyway\n", ip->ob->name));
                    ip->noecho &= ~(CHARMODE_REQ|CHARMODE|CHARMODE_ACK);
                    reset_input_buffer(ip);
                }

                DT(("'%s' --- return with line command ---\n", ip->ob->name));
                return MY_TRUE;
            } /* if (have a command) */

        } /* for (NextCmdGiver) */

        /* If we come here, we couldn't find any commandsd:
         * loop and select (on timeout) again.
         */

    } /* while(forever) */

    /* NOTREACHED */
#   undef StartCmdGiver
#   undef IncCmdGiver

} /* get_message() */

/*-------------------------------------------------------------------------*/
void
remove_interactive (object_t *ob, Bool force)

/* Remove the interactive user <ob> immediately.
 * If <force> is true, the user is removed under all circumstances and
 * without even flushing the outgoing buffer.
 * This function should not be called from within a LPC command execution.
 */

{
    object_t *save = command_giver;
    int i;
    interactive_t *interactive;
    int save_privilege;

    interactive = O_GET_INTERACTIVE(ob);

    /* Proper call? */
    for (i = 0; i < MAX_PLAYERS && all_players[i] != interactive; i++) NOOP;
    if (i >= MAX_PLAYERS)
    {
        fatal("Could not find and remove player %s\n", ob->name);
        abort();
    }
    if (interactive->closing && !force)
        fatal("Double call to remove_interactive()\n");

    interactive->closing = MY_TRUE;
    current_object = ob;
    save_privilege = malloc_privilege;

    /* If the object is not destructed, inform the master */

    if ( !(ob->flags & O_DESTRUCTED) )
    {
        command_giver = NULL;
        current_interactive = NULL;
        push_object(ob);
        malloc_privilege = MALLOC_MASTER;
        apply_master_ob(STR_DISCONNECT, 1);
        /* master might have used exec() */
        ob = interactive->ob;
    }

    interactive->catch_tell_activ = MY_FALSE;

    /* Untie eventual snooping relations */

    if (interactive->snoop_by)
    {
        if (O_IS_INTERACTIVE(interactive->snoop_by))
        {
            O_GET_INTERACTIVE(interactive->snoop_by)->snoop_on = NULL;
        }
        else
        {
            free_object(interactive->snoop_by, "remove_interactive");
        }
        interactive->snoop_by = NULL;
    }
    if (interactive->snoop_on)
    {
        interactive->snoop_on->snoop_by = NULL;
        interactive->snoop_on = NULL;
    }

    command_giver = ob;

#ifdef ERQ_DEMON
    /* If this object is disconnected because it was used to connect
     * a new ERQ, put the connection into place and greet the ERQ.
     */
    if (interactive->do_close & FLAG_PROTO_ERQ
     && interactive->socket == erq_proto_demon
     && !force)
    {
        static unsigned char erq_welcome[] = { IAC, TELOPT_BINARY };

        add_message(message_flush);
        erq_demon = interactive->socket;
        erq_proto_demon = -1;
        socket_write(erq_demon, erq_welcome, sizeof erq_welcome);
    }
    else
#endif
    {
        if (!force)
        {
            /* Say goodbye to the user. */
            trace_level |= interactive->trace_level;
            add_message(message_flush);
        }
#ifdef USE_PTHREAD
        /* buffer list is cleaned up by thread */
        pthread_cancel(interactive->write_thread);
#endif
        shutdown(interactive->socket, 2);
        socket_close(interactive->socket);
    } /* if (erq or user) */

#if defined(ACCESS_CONTROL)
    release_host_access(interactive->access_class);
      /* One user less in this class */
#endif

    num_player--;

    /* Release all associated resources */

    while (interactive->input_to)
    {
        input_to_t * it = interactive->input_to;

        interactive->input_to = it->next;
        free_input_to(it);
    }

    if (interactive->modify_command)
    {
        free_object(interactive->modify_command, "remove_interactive");
    }

#ifdef USE_PTHREAD
    pthread_mutex_destroy(&interactive->write_mutex);
    pthread_cond_destroy(&interactive->write_cond);
#endif
    free_svalue(&interactive->prompt);

    /* Unlink the interactive structure from the shadow sentence
     * of the object.
     */
    O_GET_INTERACTIVE(ob) = NULL;
    check_shadow_sent(ob);

    xfree(interactive);
    if (i < MAX_PLAYERS)
        all_players[i] = NULL;
    while (max_player && !all_players[max_player])
        max_player--;
    free_object(ob, "remove_interactive");

    command_giver = check_object(save);
    current_object = NULL;
    malloc_privilege = save_privilege;
} /* remove_interactive() */

#ifdef ACCESS_CONTROL

/*-------------------------------------------------------------------------*/
void
refresh_access_data(void (*add_entry)(struct sockaddr_in *, int, long*) )

/* Called from access_check after the ACCESS_FILE has been (re)read, this
 * function has to call the passed callback function add_entry for every
 * user currently logged in.
 */

{
    interactive_t **user, *this;
    int n;

    user = all_players;
    for (n = max_player + 2; --n; user++)
    {
        this = *user;
        if (this)
        {
            struct sockaddr_in addr;
            int port;
#           ifndef _AIX
              int length;
#           else
              size_t length;
#           endif

            length = sizeof(addr);
            getsockname(this->socket, (struct sockaddr *)&addr, &length);
            port = ntohs(addr.sin_port);
            (*add_entry)(&this->addr, port, &this->access_class);
        }
    }
}

#endif /* ACCESS_CONTROL */

/*-------------------------------------------------------------------------*/
vector_t *
users (void)

/* EFUN users()
 *
 * Return a (possibly empty) vector of all interactive user objects.
 */

{
    object_t *ob;
    int n, num;
    vector_t *ret;
    interactive_t **user;
    svalue_t *svp;

    /* Count the active users */
    num = 0;
    user = all_players;
    for (n = max_player + 2; --n; user++)
    {
        if (*user && !((*user)->ob->flags & O_DESTRUCTED))
            num++;
    }

    /* Get the result array and fill it */

    ret = allocate_array(num);
    svp = ret->item;
    user = all_players;
    for (n = max_player + 2; --n; user++)
    {
        if (*user && !((ob = (*user)->ob)->flags & O_DESTRUCTED))
        {
            put_ref_object(svp, ob, "users");
            svp++;
        }
    }

    return ret;
}

/*-------------------------------------------------------------------------*/
static void
new_player (SOCKET_T new_socket, struct sockaddr_in *addr, size_t addrlen
#if !defined(ACCESS_CONTROL)
           , int login_port UNUSED
#else
           , int login_port
#endif
           )

/* Accept (or reject) a new connection on <new_socket> from <addr> (length
 * of structure is <addrlen>), accepted on port <login_port>.
 *
 * Called when get_message() detects a new connection on one of the
 * login ports, this function checks if the user may access the mud.
 * If yes, a new interactive structure is generated and bound to the
 * master, then master->connect() is called. This call is expected
 * to return an object and the interactive structure is rebound to
 * this object. Finally, logon() is called in this object. Alternatively,
 * master->connect() may exec() the connection away from the master,
 * in which case no further action will be taken after the return
 * from that call.
 *
 * If the connection can't be accepted for some reason, a failure
 * message will be send back to the user and the socket will be
 * closed.
 */

{
#if defined(__MWERKS__) && !defined(ACCESS_CONTROL)
#    pragma unused(login_port)
#endif

    int   i;             /* Index of free slot in all_players[] */
    char *message;       /* Failure message */
    object_t *ob;        /* Login object */
    svalue_t *ret;       /* LPC call results */
    interactive_t *new_interactive;
                         /* The new interactive structure */
#ifdef ACCESS_CONTROL
    long class;     /* Access class */
#endif

    /* Set some useful socket options */
#ifndef USE_PTHREAD
    set_socket_nonblocking(new_socket);
#endif
    set_close_on_exec(new_socket);
    set_socket_own(new_socket);

#ifdef ACCESS_CONTROL
    /* Check for access restrictions for this connection */
    message = allow_host_access(addr, login_port, &class);
#ifdef ACCESS_LOG
    {
        FILE *log_file = fopen (ACCESS_LOG, "a");

        if (log_file) {
            FCOUNT_WRITE(log_file);
            fprintf(log_file, "%s %s: %s\n"
                   , time_stamp()
#ifndef USE_IPV6
                   , inet_ntoa(addr->sin_addr)
#else
                   , inet6_ntoa(addr->sin_addr)
#endif
                   , message ? "denied" : "granted");
            fclose(log_file);
        }
    }
#endif
    if (message)
    {
        socket_write(new_socket, message, strlen(message));
        socket_write(new_socket, "\r\n", 2);
        socket_close(new_socket);
        return;
    }
#endif /* ACCESS_CONTROL */

    if (d_flag > 1)
        debug_message("%s New player at socket %d.\n"
                     , time_stamp(), new_socket);

    /* Look for an empty slot in all_players[] */
    for (i = 0; i < MAX_PLAYERS && all_players[i] != NULL; i++) NOOP;
    if (i >= MAX_PLAYERS)
    {
        message = "The mud is full. Come back later.\r\n";
        /* calling closures here would need special error handling */
        if (driver_hook[H_NO_IPC_SLOT].type == T_STRING)
        {
            message = driver_hook[H_NO_IPC_SLOT].u.string;
        }
        socket_write(new_socket, message, strlen(message));
        socket_close(new_socket);
        return;
    }

    /* The master must be loaded and free to accept a login */
    assert_master_ob_loaded();
    if (O_IS_INTERACTIVE(master_ob))
    {
        message = "Cannot accept connections. Come back later.\r\n";
        socket_write(new_socket, message, strlen(message));
        socket_close(new_socket);
        return;
    }

    command_giver = master_ob;
    trace_level = 0;
    new_interactive = xalloc(sizeof (interactive_t));
    if (!new_interactive)
    {
        message = "Cannot accept connection (out of memory). Come back later.\r\n";
        socket_write(new_socket, message, strlen(message));
        socket_close(new_socket);
        debug_message("%s Out of memory (%lu bytes) for new connection.\n"
                     , time_stamp(), (unsigned long) sizeof(interactive_t));
        return;
    }

    /* Link the interactive to the master */

    assert_shadow_sent(master_ob);
    O_GET_INTERACTIVE(master_ob) = new_interactive;
    master_ob->flags |= O_ONCE_INTERACTIVE;
    new_interactive->ob = master_ob;

    /* Initialize the rest of the interactive structure */

    new_interactive->input_to = NULL;
    put_volatile_string(&new_interactive->prompt, "> ");
    new_interactive->modify_command = NULL;
    new_interactive->msg_discarded = MY_FALSE;
    new_interactive->set_input_to = MY_FALSE;
    new_interactive->closing = MY_FALSE;
    new_interactive->do_close = 0;
    new_interactive->noecho = 0;
    new_interactive->gobble_char = 0;
    new_interactive->catch_tell_activ = MY_TRUE;
    new_interactive->text_end = 0;
    new_interactive->command_start = 0;
    new_interactive->command_end = 0;
    new_interactive->chars_ready = 0;
    new_interactive->save_tn_state = TS_INVALID;
    new_interactive->tn_start = 0;
    new_interactive->tn_end = 0;
    new_interactive->tn_state = TS_DATA;
    new_interactive->ts_data = TS_DATA;
    new_interactive->snoop_on = NULL;
    new_interactive->snoop_by = NULL;
    new_interactive->last_time = current_time;
    new_interactive->trace_level = 0;
    new_interactive->trace_prefix = 0;
    new_interactive->message_length = 0;
    memset(new_interactive->charset, 255, sizeof new_interactive->charset);
    new_interactive->charset['\n'/8] &= ~(1 << '\n' % 8);
    new_interactive->charset['\0'/8] &= ~(1 << '\0' % 8);
    memset(new_interactive->combine_cset, 0, sizeof new_interactive->combine_cset);
    new_interactive->text[0] = '\0';
    memcpy(&new_interactive->addr, addr, addrlen);
#if defined(ACCESS_CONTROL)
    new_interactive->access_class = class;
#endif
    new_interactive->socket = new_socket;

#ifdef USE_PTHREAD
    pthread_mutex_init(&new_interactive->write_mutex, NULL);
    pthread_cond_init(&new_interactive->write_cond, NULL);
    new_interactive->write_first = new_interactive->write_last = NULL;
    new_interactive->write_size = 0;
    pthread_create(&new_interactive->write_thread, NULL, writer_thread, new_interactive);
#endif

    /* Add the new interactive structure to the list of users */

    all_players[i] = new_interactive;
    if (i > max_player)
        max_player = i;
    num_player++;

    /* The player object has one extra reference. */
    ref_object(master_ob, "new_player");

    /* Call master->connect() and evaluate the result.
     */
    ret = apply_master_ob(STR_CONNECT, 0);
    if (new_interactive != O_GET_INTERACTIVE(master_ob))
        return;
    if (ret == NULL
     || ret->type != T_OBJECT
     || (ob = ret->u.ob, O_IS_INTERACTIVE(ob)))
    {
        remove_interactive(master_ob, MY_FALSE);
        return;
    }
    command_giver = master_ob;
    add_message(message_flush);

    /* There was an non-interactive object returned from connect().
     * Relink the interactive from the master to this as the user object.
     */

    O_GET_INTERACTIVE(master_ob) = NULL;
    master_ob->flags &= ~O_ONCE_INTERACTIVE;
    check_shadow_sent(master_ob);

    assert_shadow_sent(ob);
    O_GET_INTERACTIVE(ob) = new_interactive;
    new_interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;

    free_object(master_ob, "new_player");

    /* Prepare to call logon() in the new user object.
     */
    command_giver = ref_object(ob, "new_player");
    current_interactive = ob;
    if (new_interactive->snoop_on)
    {
        new_interactive->snoop_on->snoop_by = ob;
    }
#ifdef ERQ_DEMON
    (void) lookup_ip_entry(new_interactive->addr.sin_addr, MY_TRUE);
    /* TODO: We could pass the retrieved hostname right to login */
#endif
    logon(ob);
    flush_all_player_mess();
} /* new_player() */

/*-------------------------------------------------------------------------*/
void
set_noecho (interactive_t *i, char noecho)

/* Change the input mode <i>->noecho to the given <noecho>, performing all
 * necessary telnet negotiations. If the driverhook H_NOECHO is set,
 * the hook function is expected to do all the negotiations.
 */

{
    char old, confirm;
    object_t *ob;

    old = i->noecho;

    confirm = (char)(
      noecho | CHARMODE_REQ_TO_CHARMODE(noecho & (NOECHO_REQ|CHARMODE_REQ)));
    DT(("'%s' set_noecho(%02x) old %02x %s\n"
       , i->ob->name, noecho, old, decode_noecho(old)));
    DT(("'%s'   -> confirm: %02x %s\n"
       , i->ob->name, confirm, decode_noecho(confirm)));
    DT(("'%s'            -> %02x %s\n"
       , i->ob->name, confirm | NOECHO_ACKSHIFT(confirm)
       , decode_noecho(confirm | NOECHO_ACKSHIFT(confirm))
       ));

    i->noecho = confirm;

    confirm |= NOECHO_ACKSHIFT(confirm);
    if ((confirm ^ old) & (NOECHO_MASK|CHARMODE_MASK) )
    {
        ob = i->ob;
        if (driver_hook[H_NOECHO].type == T_STRING)
        {
            DT(("'%s' set_noecho():   calling H_NOECHO\n", i->ob->name));
            push_number(noecho);
            push_object(ob);
            secure_apply(driver_hook[H_NOECHO].u.string, ob, 2);
        }
        else if (driver_hook[H_NOECHO].type == T_CLOSURE)
        {
            DT(("'%s' set_noecho():   calling H_NOECHO\n", i->ob->name));
            if (driver_hook[H_NOECHO].x.closure_type == CLOSURE_LAMBDA)
                driver_hook[H_NOECHO].u.lambda->ob = ob;
            push_number(noecho);
            push_object(ob);
            secure_call_lambda(&driver_hook[H_NOECHO], 2);
        }
        else
        {
            object_t *save;

            save = command_giver;
            command_giver = ob;
            if (~confirm & old & NOECHO)
            {
                DT(("'%s' set_noecho():   WONT TELOPT_ECHO\n", i->ob->name));
                send_wont(TELOPT_ECHO);
            }
            else if (confirm & ~old & NOECHO_MASK)
            {
                DT(("'%s' set_noecho():   WILL TELOPT_ECHO\n", i->ob->name));
                send_will(TELOPT_ECHO);
            }
            else /* No change in NOECHO mode */ if (confirm & NOECHO)
            {
                /* Since we stay in NOECHO mode, we need the ACK flag set. */
                DT(("'%s' set_noecho():   Staying in NOECHO mode\n", i->ob->name));
                i->noecho |= NOECHO_ACKSHIFT(NOECHO);
            }

            if (i->supress_go_ahead && !(confirm & (NOECHO|CHARMODE)))
            {
                DT(("'%s' set_noecho():   WONT TELOPT_SGA\n", i->ob->name));
                i->supress_go_ahead = MY_FALSE;
                send_wont(TELOPT_SGA);
            }
            /* Only using SGA for charmode is supported hardcoded.
             * To make more sophisticated negotiations, e.g. using LINEMODE,
             * use the H_NOECHO hook.
             */
            if ((~confirm & old & CHARMODE_MASK)
            ||  ((~confirm & old & NOECHO_STALE) && (old & CHARMODE_MASK))
               )
            {
                if(~confirm & old & CHARMODE_MASK)
                {
                    DT(("'%s' set_noecho():   turn off charmode\n", i->ob->name));
                    if (old & CHARMODE)
                    {
                        DT(("'%s' set_noecho():     DONT TELOPT_SGA\n", i->ob->name));
                        send_dont(TELOPT_SGA);
                    }
                    if (i->save_tn_state != TS_INVALID)
                    {
                        DT(("'%s' set_noecho():     0 chars ready, saved state %d\n", i->ob->name,  i->save_tn_state));
                        i->chars_ready = 0;
                        i->tn_state = i->save_tn_state;
                    }
                }

                reset_input_buffer(i);
            }
            else if (confirm & ~old & CHARMODE_MASK)
            {
                DT(("'%s' set_noecho():   turn on charmode\n", i->ob->name));
                DT(("'%s' set_noecho():     DO+WILL TELOPT_SGA\n", i->ob->name));
                send_do(TELOPT_SGA);
                /* some telnet implementations (Windows' telnet is one) mix
                 * up DO and WILL SGA, thus we send WILL SGA as well.
                 */
                send_will(TELOPT_SGA);
                i->supress_go_ahead = MY_TRUE;
            }
            else /* No change in CHARMODE mode */ if (confirm & CHARMODE)
            {
                /* Since we stay in CHARMODE mode, we need the ACK flag set. */
                DT(("'%s' set_noecho():   Staying in CHARMODE mode\n", i->ob->name));
                i->noecho |= NOECHO_ACKSHIFT(CHARMODE);
            }

            command_giver = save;
        }
    }
    else
    {
        /* No change in modes.
         * However, if we stay in NOECHO/CHARMODE, we need to set
         * the ACK flags.
         */
        if (confirm & CHARMODE)
        {
            /* Since we stay in CHARMODE mode, we need the ACK flag set. */
            DT(("'%s' set_noecho():   Staying in CHARMODE mode\n", i->ob->name));
            i->noecho |= NOECHO_ACKSHIFT(CHARMODE);
        }
        if (confirm & NOECHO)
        {
            /* Since we stay in NOECHO mode, we need the ACK flag set. */
            DT(("'%s' set_noecho():   Staying in NOECHO mode\n", i->ob->name));
            i->noecho |= NOECHO_ACKSHIFT(NOECHO);
        }
    }

} /* set_noecho() */

/*-------------------------------------------------------------------------*/
int
find_no_bang (interactive_t *ip)

/* Find the most recent input_to in *<ip> which specified "IGNORE_BANG" and
 * return its full "noecho" flags. This may be the ip->noecho itself!
 * If there is none, return 0.
 */

{
    input_to_t *it;

    if (ip->noecho & IGNORE_BANG)
        return ip->noecho;
        
    for (it = ip->input_to; it; it = it->next)
        if (it->noecho & IGNORE_BANG)
            return it->noecho;
    return 0;
} /* find_no_bang() */

/*-------------------------------------------------------------------------*/
Bool
call_function_interactive (interactive_t *i, char *str)

/* Perform a pending input_to() for this user <i> and the input <str>
 * Return TRUE if an input_to() was pending and executed, and FALSE
 * if the input was not processed.
 *
 * This function is called by the backend as part of the input processing.
 */

{
    static input_to_t current_it;
      /* Current input_to, static so that longjmp() won't clobber it. */

    struct error_recovery_info error_recovery_info;

    input_to_t *it;
    object_t   *ob;   /* object holding <function> */

    it = i->input_to;
    
    /* _Are_ there an input_to() pending? */
    if (!it)
        return MY_FALSE;

    /* Yes, there are. Check if we have to handle '!'. */
    if (*str == '!')
    {
        input_to_t * prev;

        for (prev = NULL
            ; it && !(it->noecho & IGNORE_BANG)
            ; prev = it, it = it->next)
            NOOP;

        if (it)
        {
            /* Move this 'IGNORE_BANG' input_to to the top of list
             * since it's the one we're going to execute.
             */
            if (prev)
            {
                prev->next = it->next;
                it->next = i->input_to;
                i->input_to = it;
            }

            if (!(i->noecho & NOECHO) != !(it->noecho & NOECHO_REQ)) {
                /* !message for ECHO-context  while in NOECHO - simulate the
                 * echo by sending the (remaining) raw data we got.
                 */
                add_message("%s\n", str + i->chars_ready);
                i->chars_ready = 0;
            }

            /* Don't hide the leading '!' */
        }
        else
        {
            /* Bang-input but no matching input_to(): return */
            return MY_FALSE;
        }
    }

    /* We got the right input_to_t. Check if it's still valid. */
    ob = callback_object(&(it->fun));
    if (!ob)
    {
        /* Sorry, the object has selfdestructed ! */
        set_noecho(i, it->next ? it->next->noecho : 0);
        i->input_to = it->next;
        free_input_to(it);
        return MY_FALSE;
    }

    if (O_PROG_SWAPPED(ob)
     && load_ob_from_swap(ob) < 0)
    {
        set_noecho(i, it->next ? it->next->noecho : 0);
        i->input_to = it->next;
        free_input_to(it);
        error("Out of memory: unswap object '%s'.\n", ob->name);
        return MY_FALSE;
    }

    /* if there is a series of noecho/charmode input, we should only
     * negotiate when we know that the state actually should change.
     * In other words: should the input_to function request NOECHO
     * again, the NOECHO_STALE bit will be cleared and we will not
     * turn NOECHO off after the call.
     */
    if (i->noecho)
    {
        i->noecho |= NOECHO_STALE;
    }

    /* Clear the input_to() reference in case the function called
     * sets up a new one.
     */
    current_it = *it;
    i->input_to = it->next;
    xfree(it);
    free_svalue(&current_it.prompt); /* Don't need this anymore */

    /* Activate the local error recovery context */

    error_recovery_info.rt.last = rt_context;
    error_recovery_info.rt.type = ERROR_RECOVERY_BACKEND;
    rt_context = (rt_context_t *)&error_recovery_info;

    if (setjmp(error_recovery_info.con.text))
    {
        /* An error occured: free the remaining data,
         * restore the error stack and return
         */

        clear_state();
        debug_message("%s Error in input_to().\n", time_stamp());
        free_callback(&(current_it.fun));
        rt_context = error_recovery_info.rt.last;
        return MY_TRUE;
    }

    /* Call the input_to() function with the newly input string */

    push_volatile_string(str);
    (void)backend_callback(&(current_it.fun), 1);

    rt_context = error_recovery_info.rt.last;

    /* If NOECHO is no longer needed, turn it off. */

    if (i->noecho & NOECHO_STALE)
    {
        set_noecho(i, i->input_to ? i->input_to->noecho : 0);
    }

    /* Done */
    return MY_TRUE;
} /* call_function_interactive() */

/*-------------------------------------------------------------------------*/
static Bool
set_call (object_t *ob, input_to_t *it, char noecho)

/* Set a a new input_to <it> with the flags <noecho> (mainly really NOECHO,
 * but also IGNORE_BANG or not) to the interactive object <ob>.
 * Return TRUE on success.
 *
 * Called for efun input_to().
 */

{
    interactive_t *ip;

    if (ob == NULL || it == NULL)
        return MY_FALSE;
    if (!(O_SET_INTERACTIVE(ip, ob))
     || ip->closing || ip->set_input_to)
    {
        return MY_FALSE;
    }

    it->noecho = noecho;
    it->next = ip->input_to;
    ip->input_to = it;
    ip->set_input_to = MY_TRUE;

    if (noecho || ip->noecho)
        set_noecho(ip, noecho);
    return MY_TRUE;
} /* set_call() */

/*-------------------------------------------------------------------------*/
void
remove_all_players (void)

/* Destruct all user objects. This is first tried by calling master->remove()
 * for every object. If this doesn't destruct the user object,
 * destruct() is used.
 * The function is called when the game is shut down.
 */

{
    int i;

    for (i = 0; i < MAX_PLAYERS; i++) {
        if (all_players[i] == 0 || (all_players[i]->ob->flags & O_DESTRUCTED))
            continue;
        command_giver = all_players[i]->ob;
        trace_level |= all_players[i]->trace_level;
        RESET_LIMITS;
        CLEAR_EVAL_COST;
        push_object(all_players[i]->ob);
        (void)apply_master_ob(STR_REMOVE_PL, 1);
        if ( !(all_players[i]->ob->flags & O_DESTRUCTED) ) {
            destruct(all_players[i]->ob);
        }
    }
}

/*-------------------------------------------------------------------------*/
void
print_prompt (void)

/* Print the prompt of the current command_giver, unless disabled
 * by input_to. If the prompt is set to a closure, the closure
 * is called and expected to return the actual prompt string or
 * to print the prompt itself.
 */

{
    interactive_t *ip;
    svalue_t *prompt = NULL;

#ifdef DEBUG
    if (command_giver == 0)
        fatal("command_giver == 0.\n");
#endif

    if (!(O_SET_INTERACTIVE(ip, command_giver)))
        fatal("print_prompt() of non-interactive object\n");

    if (ip->input_to != NULL)
    {
        prompt = &ip->input_to->prompt;
    }
    else if (NULL == (prompt = get_ed_prompt(ip)))
    {
        prompt = &ip->prompt;
    }

    if (prompt->type == T_CLOSURE)
    {
        object_t *ob;
        
        /* Needed for clean error recovery */

        previous_ob = 0;
        current_object = command_giver;

        /* Check if the object the closure is bound to still exists.
         * If not, restore the prompt, then throw an error.
         */
        ob = !CLOSURE_MALLOCED(prompt->x.closure_type)
             ? prompt->u.ob
             : prompt->u.lambda->ob;

        if (ob && ob->flags & O_DESTRUCTED)
        {
            free_svalue(prompt);
            put_volatile_string(prompt, "> ");
            add_message("%s", prompt->u.string);
            error("Prompt of %s was a closure bound to a now-destructed object - default prompt restored.\n", command_giver->name);
            /* NOTREACHED */
        }

        call_lambda(prompt, 0);
        prompt = inter_sp;

        if (prompt->type != T_STRING)
        {
            free_svalue(prompt);
        }
        else
        {
            /* beware: add_message() might cause an error. Thus, the LPC
             * stack has to include the prompt to free it then.
             */
            add_message("%s", prompt->u.string);
            free_svalue(prompt);
        }
        inter_sp--;
    }
    else if (prompt->type == T_STRING)
    {
        add_message("%s", prompt->u.string);
    }
} /* print_prompt() */

/*-------------------------------------------------------------------------*/
int
set_snoop (object_t *me, object_t *you)

/* Set a snoop from <me> on the IO of <you>. If <you> is NULL, an
 * existing snoop is terminated. <me> need not to be an interactive
 * user.
 *
 * Return 1 on success, -1 if a snooping loop would be caused, 0 for
 * any other failure.
 *
 * The function calls master->valid_snoop() to test if the snoop
 * is allowed.
 */

{
    interactive_t *on = NULL; /* interactive struct of <you> */
    interactive_t *by = NULL; /* interactive struct of <me> */
    interactive_t *tmp;
    svalue_t *ret;

    /* Stop if people managed to quit before we got this far */
    if (me->flags & O_DESTRUCTED)
        return 0;
    if (you && (you->flags & O_DESTRUCTED))
        return 0;

    /* Check for permissions with valid_snoop in master */
    push_object(me);
    if (you == NULL)
        push_number(0);
    else
        push_object(you);
    ret = apply_master_ob(STR_VALID_SNOOP, 2);

    if (!ret || ret->type != T_NUMBER || ret->u.number == 0)
        return 0;

    if (me->flags & O_DESTRUCTED)
        return 0;

    /* Test is <me> is able to snoop anyway.
     * Set <by> to <me>'s interactive struct if yes.
     */
    if (O_SET_INTERACTIVE(by, me) && by->closing)
        return 0;

    if (you)
    {
        /* Test if <you> can be snooped at all.
         * Set <on> to <you>'s interactive struct if yes.
         */
        if (you->flags & O_DESTRUCTED)
            return 0;
        if (!(O_SET_INTERACTIVE(on, you)) || on->closing)
            return 0;
    }
    else
    {
        /* Stop snoop.
         * For this, set <on> to the interactive struct of the snoops
         * victim. If <by> is NULL, <me> is propably a netdead user
         * or a NPC and we have to scan the list of users for the victim.
         */

        if (!by)
        {
            int i;

            for (i = max_player+1;;)
            {
                if (--i < 0)
                    return 0;
                if (NULL != (on = all_players[i]) && on->snoop_by == me)
                    break;
            }
            if (on->closing)
                return 0;
            free_object(me, "set_snoop");
        }
        else
        {
            on = by->snoop_on;
            if (!on || on->closing)
                return 0;
            by->snoop_on = NULL;
        }
        on->snoop_by = NULL;
        return 1;
    }

    /* If we come here, a snoop on <you> by <me> is possible.
     * Now protect against snooping loops.
     */

    for (tmp = on; tmp; tmp = tmp->snoop_on)
    {
        if (tmp == by)
            return -1;
    }

    /* Terminate previous snoop, if any */
    if (on->snoop_by)
    {
        interactive_t *ip;

        if (O_SET_INTERACTIVE(ip, on->snoop_by))
        {
            if (ip->closing)
                return 0;
            ip->snoop_on = 0;
        }
        else
        {
            free_object(on->snoop_by, "set_snoop");
        }
        on->snoop_by = NULL;
    }

    /* Initialise the new snoop */
    if (by)
    {
        if (by->snoop_on)
        {
            if (by->snoop_on->closing)
                return 0;
            by->snoop_on->snoop_by = 0;
            by->snoop_on = 0;
        }
        by->snoop_on = on;
    }
    else
    {
        ref_object(me, "set_snoop");
    }

    on->snoop_by = me;
    return 1;
} /* set_snoop() */

/*=========================================================================*/
/*                      Telnet Support
 */

/* Note: when stored in char variables, IAC can be equal to EOF.
 * This can cause sprintf(), which is used in add_message(), to abort
 * output after EOF. Therefore, don't try to send anything after the IAC
 * in the same call to add_message().
 */

/*-------------------------------------------------------------------------*/
static void
send_wont (int option)

/* Send IAC WONT <option> */

{
    DTF(("%s TDEBUG: send IAC WONT %02x\n", time_stamp(), option));
    SEND_TELNET_COMMAND(
      add_message("%c", IAC);
      add_message("%c%c", WONT, option);
      add_message(message_flush);
    )
}

/*-------------------------------------------------------------------------*/
static void
send_dont (int option)

/* Send IAC DONT <option> */

{
    DTF(("%s TDEBUG: send IAC DONT %02x\n", time_stamp(), option));
    SEND_TELNET_COMMAND(
      add_message("%c", IAC);
      add_message("%c%c", DONT, option);
      add_message(message_flush);
    )
}

/*-------------------------------------------------------------------------*/
static void
send_will (int option)

/* Send IAC WILL <option> */

{
    DTF(("%s TDEBUG: send IAC WILL %02x\n", time_stamp(), option));
    SEND_TELNET_COMMAND(
      add_message("%c", IAC);
      add_message("%c%c", WILL, option);
      add_message(message_flush);
    )
}

/*-------------------------------------------------------------------------*/
static void
send_do (int option)

/* Send IAC DO <option> */

{
    DTF(("%s TDEBUG: send IAC DO %02x\n", time_stamp(), option));
    SEND_TELNET_COMMAND(
      add_message("%c", IAC);
      add_message("%c%c", DO, option);
      add_message(message_flush);
    )
}

/*-------------------------------------------------------------------------*/
static void
reply_nil (int option UNUSED)

/* Dummy function which does nothing. */

{
#ifdef __MWERKS__
#    pragma unused(option)
#endif
}

/*-------------------------------------------------------------------------*/
static void
reply_to_do_echo (int option)

/* Send IAC WILL <option> if we were told before to not echo, or
 * send IAC WONT <option> if we are echoing.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to DO ECHO\n", ip->ob->name));
    if (ip->noecho & NOECHO_MASK) {
        if ( !(ip->noecho & NOECHO) ) {
            /* We were previously told not to echo */
            send_will(option);
        }
        DT(("'%s'   we don't need to say WILL\n", ip->ob->name));
        /* If we already said that we will echo, be quiet */
        ip->noecho |= NOECHO_MASK;
    } else {
        send_wont(option);
    }
}

/*-------------------------------------------------------------------------*/
static void
reply_to_dont_echo (int option)

/* Send IAC WONT <option> if we were granted the option before.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to DONT ECHO\n", ip->ob->name));
    if (ip->noecho & NOECHO_MASK) {
        if (!~(ip->noecho | ~NOECHO_MASK)) {
            /* We were granted the option before */
            send_wont(option);
        }
        DT(("'%s'   we don't need to say WONT\n", ip->ob->name));
        ip->noecho = (char)((ip->noecho & ~NOECHO) | NOECHO_ACK);
    }
}

/*-------------------------------------------------------------------------*/
static void
reply_to_do_sga (int option)

/* Send IAC WILL <option> if Suppress Go Ahead is not already active and
 * mark it as active, send IAC WONT <option> if neither in NOECHO or
 * CHARMODE mode.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to DO SGA\n", ip->ob->name));
    if (ip->noecho & (NOECHO_MASK|CHARMODE_MASK)) {
        if (!ip->supress_go_ahead) {
            ip->supress_go_ahead = MY_TRUE;
            send_will(option);
        }
        DT(("'%s'   we don't need to say WILL\n", ip->ob->name));
    } else {
        send_wont(option);
    }
}

/*-------------------------------------------------------------------------*/
static void
reply_to_dont_sga (int option)

/* Send IAC WONT <option> if Suppress Go Ahead is active and mark it as
 * inactive.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to DONT SGA\n", ip->ob->name));
    if (ip->supress_go_ahead) {
        ip->supress_go_ahead = 0;
        send_wont(option);
    }
}

/*-------------------------------------------------------------------------*/
static void
reply_to_will_sga (int option)

/* Send IAC DO <option> if CHARMODE is requested but not active yet,
 * send IAC DONT <option> if CHARMODE is neither requested nor active.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to WILL SGA\n", ip->ob->name));
    if (ip->noecho & CHARMODE_MASK) {
        if ( !(ip->noecho & CHARMODE) ) {
            send_do(option);
        }
        DT(("'%s'   we don't need to say DO\n", ip->ob->name));
        DT(("'%s'   noecho: %02x -> %02x\n", ip->ob->name, ip->noecho, (unsigned char)(ip->noecho  | CHARMODE_MASK)));
        ip->noecho |= CHARMODE_MASK;
    } else {
        send_dont(option);
    }
}

/*-------------------------------------------------------------------------*/
static void
reply_to_wont_sga (int option)

/* Send IAC DONT <option> if CHARMODE was granted before.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);

    DT(("'%s' reply to WONT SGA\n", ip->ob->name));
    if (ip->noecho & CHARMODE_MASK) {
        if (!~(ip->noecho | ~CHARMODE_MASK)) {
            /* We were granted the option before */
            send_dont(option);
        }
        DT(("'%s'   we don't need to say DONT\n", ip->ob->name));
        DT(("'%s'   noecho: %02x -> %02x\n", ip->ob->name, ip->noecho, (unsigned char)((ip->noecho & ~CHARMODE) | CHARMODE_ACK)));
        ip->noecho = (char)((ip->noecho & ~CHARMODE) | CHARMODE_ACK);
          /* Don't reset CHARMODE_REQ here: this WONT can be the answer
           * to the DO SGA we sent before, and the client can still answer
           * with DO SGA to the WILL SGA we sent as well (Windows' telnet
           * for example does this).
           * Besides, the variables are now set up to treat the input
           * in charmode, and changing the flag without the variables
           * will do Bad Things(tm).
           */
    }
}

/*-------------------------------------------------------------------------*/
static svalue_t *
h_telnet_neg (int n)

/* Call the H_TELNET_NEG driverhook with <n> arguments on the interpreter
 * stack. Return the result from that call, or NULL if the hook isn't
 * set. The arguments are removed from the stack in any case.
 */

{
    svalue_t *svp;

    RESET_LIMITS;
    CLEAR_EVAL_COST;
    if (driver_hook[H_TELNET_NEG].type == T_STRING)
    {
        svp =
          secure_apply(driver_hook[H_TELNET_NEG].u.string, command_giver, n);
    }
    else if (driver_hook[H_TELNET_NEG].type == T_CLOSURE)
    {
        if (driver_hook[H_TELNET_NEG].x.closure_type == CLOSURE_LAMBDA)
            driver_hook[H_TELNET_NEG].u.lambda->ob = command_giver;
        svp = secure_call_lambda(&driver_hook[H_TELNET_NEG], n);
    }
    else
    {
        while (--n >= 0)
            pop_stack();
        svp = NULL;
    }
    return svp;
}

/*-------------------------------------------------------------------------*/
static void
reply_h_telnet_neg (int option)

/* Call the H_TELNET_NEG driver hook with <tn_state> <option> as
 * arguments. If the hook is not defined, send WONT <option> if
 * the state is TS_DO, or send DONT <option> if the state is TS_WILL.
 */

{
    interactive_t *ip = O_GET_INTERACTIVE(command_giver);
    int i = 0;

    switch(ip->tn_state) {
      case TS_DO:
        DT(("'%s' reply to telnet_neg: DO %02x\n", ip->ob->name, option));
        i = DO;
        break;
      case TS_DONT:
        DT(("'%s' reply to telnet_neg: DONT %02x\n", ip->ob->name, option));
        i = DONT;
        break;
      case TS_WILL:
        DT(("'%s' reply to telnet_neg: WILL %02x\n", ip->ob->name, option));
        i = WILL;
        break;
      case TS_WONT:
        DT(("'%s' reply to telnet_neg: WONT %02x\n", ip->ob->name, option));
        i = WONT;
        break;
      default:
        debug_message("%s Invalid tn_state %d for interactive '%s'\n"
             , time_stamp(), ip->tn_state, ip->ob->name);
        break;
    }
    push_number(i);
    push_number(option);
    if (!h_telnet_neg(2)) {
        DT(("'%s'   using default methods\n", ip->ob->name));
        switch(ip->tn_state) {
          case TS_DO:
            DT(("'%s'     -> WONT %02x\n", ip->ob->name, option));
            send_wont(option);
            break;
          case TS_WILL:
            DT(("'%s'     -> DONT %02x\n", ip->ob->name, option));
            send_dont(option);
            break;
        }
    }
} /* reply_h_telnet_neg() */

/*-------------------------------------------------------------------------*/
void
init_telopts (void)

/* Initialise the telopts_xxx[] tables.
 * The default setting is such that requests are ignored or rejected.
 */

{
    int i;

    for (i = NTELOPTS; --i >= 0; ) {
        telopts_do[i] = send_wont;
    }
    telopts_do[TELOPT_ECHO] = reply_to_do_echo;
    for (i = NTELOPTS; --i >= 0; ) {
        telopts_dont[i] = reply_nil;
    }
    telopts_dont[TELOPT_ECHO] = reply_to_dont_echo;
    for (i = NTELOPTS; --i >= 0; ) {
        telopts_will[i] = send_dont;
    }
    telopts_will[TELOPT_SGA] = reply_to_will_sga;
    for (i = NTELOPTS; --i >= 0; ) {
        telopts_wont[i] = reply_nil;
    }
    telopts_do[TELOPT_TM] = reply_h_telnet_neg;
    telopts_dont[TELOPT_TM] = reply_h_telnet_neg;
    telopts_will[TELOPT_TM] = reply_h_telnet_neg;
    telopts_wont[TELOPT_TM] = reply_h_telnet_neg;
    telopts_wont[TELOPT_SGA] = reply_to_wont_sga;
    telopts_do[TELOPT_NEWENV] = reply_h_telnet_neg;
    telopts_dont[TELOPT_NEWENV] = reply_h_telnet_neg;
    telopts_will[TELOPT_NEWENV] = reply_h_telnet_neg;
    telopts_wont[TELOPT_NEWENV] = reply_h_telnet_neg;
    telopts_do[TELOPT_ENVIRON] = reply_h_telnet_neg;
    telopts_dont[TELOPT_ENVIRON] = reply_h_telnet_neg;
    telopts_will[TELOPT_ENVIRON] = reply_h_telnet_neg;
    telopts_wont[TELOPT_ENVIRON] = reply_h_telnet_neg;
    telopts_do[TELOPT_XDISPLOC] = reply_h_telnet_neg;
    telopts_dont[TELOPT_XDISPLOC] = reply_h_telnet_neg;
    telopts_will[TELOPT_XDISPLOC] = reply_h_telnet_neg;
    telopts_wont[TELOPT_XDISPLOC] = reply_h_telnet_neg;
    telopts_do[TELOPT_LINEMODE] = reply_h_telnet_neg;
    telopts_dont[TELOPT_LINEMODE] = reply_h_telnet_neg;
    telopts_will[TELOPT_LINEMODE] = reply_h_telnet_neg;
    telopts_wont[TELOPT_LINEMODE] = reply_h_telnet_neg;
    telopts_do[TELOPT_NAWS] = reply_h_telnet_neg;
    telopts_dont[TELOPT_NAWS] = reply_h_telnet_neg;
    telopts_will[TELOPT_NAWS] = reply_h_telnet_neg;
    telopts_wont[TELOPT_NAWS] = reply_h_telnet_neg;
    telopts_do[TELOPT_TTYPE] = reply_h_telnet_neg;
    telopts_dont[TELOPT_TTYPE] = reply_h_telnet_neg;
    telopts_will[TELOPT_TTYPE] = reply_h_telnet_neg;
    telopts_wont[TELOPT_TTYPE] = reply_h_telnet_neg;
    telopts_do[TELOPT_TSPEED] = reply_h_telnet_neg;
    telopts_dont[TELOPT_TSPEED] = reply_h_telnet_neg;
    telopts_will[TELOPT_TSPEED] = reply_h_telnet_neg;
    telopts_wont[TELOPT_TSPEED] = reply_h_telnet_neg;

    telopts_do[TELOPT_EOR] = reply_h_telnet_neg;
    telopts_dont[TELOPT_EOR] = reply_h_telnet_neg;
    telopts_will[TELOPT_EOR] = reply_h_telnet_neg;
    telopts_wont[TELOPT_EOR] = reply_h_telnet_neg;

    /* Tinyfugue can do bad things to your health */
    telopts_do[EOR] = reply_h_telnet_neg;
    telopts_dont[EOR] = reply_h_telnet_neg;
    telopts_will[EOR] = reply_h_telnet_neg;
    telopts_wont[EOR] = reply_h_telnet_neg;

    /* Go Ahead does not make any sense when coupling multiple
     * interactive users. It is debatable if we are sending
     * Go Ahead every time it is appropriate (i.e. , never),
     * or we supress it all the time.
     * Unfortunately, SGA is also often associated with
     * character-at-a-time mode - the RFC even mandates this
     * double meaning - which we certainly don't want.
     * It might cause problems when we reject Supress Go Ahead
     * when some stupid client thinks that the ECHO option need
     * be coupled with SGA .
     * Thus, reject SGA in general, but not while
     * ip->noecho & NOECHO_MASK is true.
     */
    telopts_do[TELOPT_SGA] = reply_to_do_sga;
    telopts_dont[TELOPT_SGA] = reply_to_dont_sga;

    /* Mud specific protocols */

    telopts_do[TELOPT_COMPRESS] = reply_h_telnet_neg;
    telopts_dont[TELOPT_COMPRESS] = reply_h_telnet_neg;
    telopts_will[TELOPT_COMPRESS] = reply_h_telnet_neg;
    telopts_wont[TELOPT_COMPRESS] = reply_h_telnet_neg;

    telopts_do[TELOPT_COMPRESS2] = reply_h_telnet_neg;
    telopts_dont[TELOPT_COMPRESS2] = reply_h_telnet_neg;
    telopts_will[TELOPT_COMPRESS2] = reply_h_telnet_neg;
    telopts_wont[TELOPT_COMPRESS2] = reply_h_telnet_neg;

    telopts_do[TELOPT_MSP] = reply_h_telnet_neg;
    telopts_dont[TELOPT_MSP] = reply_h_telnet_neg;
    telopts_will[TELOPT_MSP] = reply_h_telnet_neg;
    telopts_wont[TELOPT_MSP] = reply_h_telnet_neg;

    telopts_do[TELOPT_MXP] = reply_h_telnet_neg;
    telopts_dont[TELOPT_MXP] = reply_h_telnet_neg;
    telopts_will[TELOPT_MXP] = reply_h_telnet_neg;
    telopts_wont[TELOPT_MXP] = reply_h_telnet_neg;
} /* init_telopts() */

/*-------------------------------------------------------------------------*/
void
mudlib_telopts (void)

/* Set all telopts_xxx[] entries to reply_h_telnet_neg().
 * This means that the mudlib does all the telnet negotiation.
 * It is called whenever driver hook H_NOECHO is set.
 */

{
    int i;

    DT(("All telnet options set to the mudlib.\n"));
    for (i = NTELOPTS; --i >= 0; ) {
        telopts_do[i] = telopts_dont[i] =
          telopts_will[i] = telopts_wont[i] = reply_h_telnet_neg;
    }
} /* mudlib_telopts() */

/*-------------------------------------------------------------------------*/
static void
telnet_neg (interactive_t *ip)

/* Process the data read from the socket, performing any telnet negotiations
 * necessary, and extract the 'pure' command text. When the function returns,
 * all new data in .text[] has been used and .text_end set back as far
 * as possible.
 *
 * The start state for the telnet machine is TS_DATA, and whenever a command
 * text has been completed, it assumes the TS_READY state.
 */

{
    fd_set exceptfds;
    char *from;   /* Next char to process */
    char *to;     /* Where to store the extracted command text */
    int   state;
    int   ch;     /* Current character */
    char *first;  /* Begin of the last pure command text */
    char *end;    /* End of data in text[] */

    first = ip->text;
    from = &first[ip->tn_end];
    end = &first[ip->text_end];

    DT(("'%s' telnet_neg: state %d\n", ip->ob->name, ip->tn_state));

    /* Gobble the character *from if gobble_char is set.
     * Also test for the end of current buffer content.
     */
    for (;;)
    {
        if (from >= end) {
#if 0 /* cannot happen with the current calling pattern */
            if (ip->state == TS_READY) return;
#endif
            ip->text_end = ip->tn_end = ip->command_end;
            return;
        }
        if (ip->gobble_char) {
            DT(("'%s' t_n: gobble char %02x (in buf: %02x)\n"
               , ip->ob->name, ip->gobble_char, *from));
            if (*from == ip->gobble_char) {
                from++;
            }
            ip->gobble_char = '\0';
            continue;
        }
        break;
    }
    to = &first[ip->command_end];

    /* The processing loop */

    do {
        ch = (*from++ & 0xff);
        DT(("'%s' t_n: processing %02x '%c'\n"
            , ip->ob->name, (unsigned char)ch, ch));
        switch(ip->tn_state)
        {
        case TS_READY:
            DT(("'%s' t_n: still in TS_READY - return\n", ip->ob->name));
            /* Previous command hasn't been read yet - don't clobber it! */
            return;

        ts_data:
            /* Most state functions end with a jump here to check if they
             * exhausted their input.
             */
            if (from >= end)
            {
        data_exhausted:
                ip->text_end = ip->tn_end = ip->command_end = (short)(to - first);
                if (ip->text_end >= MAX_TEXT)
                {
                    /* this looks like a super-long command.
                     * Return the text so far as partial command and restart
                     * input from the beginning.
                     * In charmode, we must not reset command_end, otherwise
                     * it might fall under command_start.
                     */
                    ip->text_end = ip->tn_end = 0;
                    if (!(ip->noecho & CHARMODE_REQ))
                        ip->command_end = 0;
                    *to = '\0';
                    ip->tn_state = TS_READY;
                    return;
                }
                return;
            }
            ch = (*from++ & 0xff);
            /* FALLTHROUGH */

        case TS_DATA: /* --- Copy/interpret plain data --- */
            switch(ch)
            {
            case IAC:
            new_iac:
                  state = TS_IAC;
        change_state:
                  DT(("'%s' t_n: new state %d\n", ip->ob->name, state));
                  ip->tn_state = (char)state;
                  continue;

            case '\b':        /* Backspace */
            case 0x7f:        /* Delete */
                /* In Linemode, just move to one char back.
                 * In Charmode with escaped input, write the data gathered
                 * so far and add a rubout sequence ('\b \b').
                 * In Charmode with unescaped input, just pass it on to
                 * the mudlib.
                 */
                if ( !(ip->noecho & CHARMODE_REQ) )
                {
                    if (to > first)
                        to--;
                    goto ts_data;
                }

                if (ip->text[0] == '!' && ! (find_no_bang(ip) & IGNORE_BANG) )
                {
#ifdef USE_PTHREAD
                    if (to > &ip->text[ip->chars_ready])
                    {
                        thread_socket_write(ip->socket, &ip->text[ip->chars_ready],
                          (size_t)(to - &ip->text[ip->chars_ready]), ip);
                        ip->chars_ready = to - ip->text;
                    }
                    if (to > first) {
                        thread_socket_write(ip->socket, "\b \b", 3, ip);
                        to--;
                        ip->chars_ready--;
                    }
#else
                    if (to > &ip->text[ip->chars_ready])
                    {
                        socket_write(ip->socket, &ip->text[ip->chars_ready],
                          (size_t)(to - &ip->text[ip->chars_ready]));
                        ip->chars_ready = to - ip->text;
                    }
                    if (to > first) {
                        socket_write(ip->socket, "\b \b", 3);
                        to--;
                        ip->chars_ready--;
                    }
#endif
                    goto ts_data;
                }
                /* FALLTHROUGH */

            default:
                *to++ = (char)ch;
                /* FALLTHROUGH */

            case '\0':
                goto ts_data;

            case '\r':
                if (from >= end)
                {
                    /* This might be a fragmented CR NL, CR NUL, or
                     * a broken client that ends lines with CR only.
                     * If we are just looking for a line of text, we
                     * can proceed now, else we have to wait for the
                     * next character to make our decisions.
                     */
                    if ( !(ip->noecho & CHARMODE_REQ)
                     || (ip->text[0] == '!' && ! (find_no_bang(ip) & IGNORE_BANG))
                       )
                    {
                        ip->gobble_char = '\n';
                        goto full_newline;
                    }
                    ip->tn_state = TS_CR;
                    goto data_exhausted;
                }
                else
                {
                    ch = (*from++ & 0xff);
        ts_cr:
                    if (ch != '\n')
                        from--;

                    if ((ip->noecho & CHARMODE_REQ) &&
                        (ip->text[0] != '!' || find_no_bang(ip) & IGNORE_BANG))
                    {
                        if (from == to)
                        {
                            /* The client sent a single CR in CHARMODE,
                             * there were no previous negotiations and thus
                             * we have to make space to insert the CR.
                             */
                            char * cp;

                            if (ip->text_end < MAX_TEXT-1)
                            {
                                ip->text_end++;
                                end++;
                            }
                                
                            from++;
                            for (cp = end; cp != from-1; cp--)
                                *cp = *(cp-1);
                        }
                            
                        if ((ip->noecho & (CHARMODE_REQ|CHARMODE)) != (CHARMODE_REQ))
                        {
                            *to++ = '\r';
                            ip->tn_state = TS_DATA;
                            goto ts_data;
                        }
                        else
                        {
                            /* The client refused to go into char mode
                             * and instead sent us a complete line.
                             * Handle it as usual.
                             */
                            /* FALLTHROUGH to full_newline */
                        }
                    }
                } /* if (from >= end) or not */

        full_newline:
                /* Proper line end found: set telnet machine into TS_READY,
                 * terminate the command with \0 and return.
                 */
                {
                    ip->tn_state = TS_READY;
                    ip->command_end = 0;
                    ip->tn_end = (short)(from - first);
                    *to = '\0';
                    return;
                }

            case '\n':
                if ( !(ip->noecho & CHARMODE_REQ) ||
                     (ip->text[0] == '!' && ! (find_no_bang(ip) & IGNORE_BANG)) )
                {
                    ip->gobble_char = '\r';
                }
                goto full_newline;
            } /* switch(ch) */

            /* NOTREACHED */

        case TS_CR:
            /* Complete a CR-?? combination. */
            goto ts_cr;

        ts_iac:
        case TS_IAC:
            DT(("'%s' t_n: state IAC\n", ip->ob->name));
            /* Begin a telnet negotiation */
            switch(ch)
            {
            case IAC:
                DT(("'%s' t_n: got IAC\n", ip->ob->name));
                *to++ = ch;
                ip->tn_state = state = TS_DATA;
                goto ts_data;
            case WILL:
                DT(("'%s' t_n: got WILL\n", ip->ob->name));
                state = TS_WILL;
                goto change_state;
            case WONT:
                DT(("'%s' t_n: got WONT\n", ip->ob->name));
                state = TS_WONT;
                goto change_state;
            case DO:
                DT(("'%s' t_n: got DO\n", ip->ob->name));
                state = TS_DO;
                goto change_state;
            case DONT:
                DT(("'%s' t_n: got DONT\n", ip->ob->name));
                state = TS_DONT;
                goto change_state;
            case SB:
                DT(("'%s' t_n: got SB\n", ip->ob->name));
                ip->tn_start = (short)(to - first);
                state = TS_SB;
                goto change_state;
            case DM:
                DT(("'%s' t_n: got DM\n", ip->ob->name));
            data_mark:
                if (ip->ts_data == TS_SYNCH)
                {
                    struct timeval timeout;

                    FD_ZERO(&exceptfds);
                    FD_SET(ip->socket, &exceptfds);
                    timeout.tv_sec = 0;
                    timeout.tv_usec = 0;
                    if (! socket_select(ip->socket + 1, 0, 0, &exceptfds,
                        &timeout))
                    {
                        if (d_flag)
                            debug_message("%s Synch operation finished.\n"
                                         , time_stamp());
                        ip->ts_data = TS_DATA;
                    }
                }
                break;
            case NOP:
                DT(("'%s' t_n: got NOP\n", ip->ob->name)); break;
            case GA:
                DT(("'%s' t_n: got GA\n", ip->ob->name)); break;
            default:
                DT(("'%s' t_n: got %02x\n", ip->ob->name, ch)); break;
                break;
            } /* switch(ch) */
            state = ip->ts_data;
            goto change_state;

        case TS_WILL:
            command_giver = ip->ob;
            if (ch < NTELOPTS) {
                DT(("'%s' t_n: state WILL got %s (%02x)\n"
                   , ip->ob->name, telopts[ch], ch));
                if (d_flag)
                    debug_message("%s Will %s\n", time_stamp(), telopts[ch]);
                (*telopts_will[ch])(ch);
            } else {
                debug_message("%s Unknown telnet option Will %d\n"
                             , time_stamp(), ch);
                send_dont(ch);
            }
            state = ip->ts_data;
            goto change_state;

        case TS_WONT:
            command_giver = ip->ob;
            if (ch < NTELOPTS) {
                DT(("'%s' t_n: state WONT got %s (%02x)\n"
                   , ip->ob->name, telopts[ch], ch));
                if (d_flag)
                    debug_message("%s Wont %s\n", time_stamp(), telopts[ch]);
                (*telopts_wont[ch])(ch);
            } else {
                debug_message("%s Unknown telnet option Wont %d\n"
                             , time_stamp(), ch);
            }
            state = ip->ts_data;
            goto change_state;

        case TS_DO:
            command_giver = ip->ob;
            if (ch < NTELOPTS) {
                DT(("'%s' t_n: state DO got %s (%02x)\n"
                   , ip->ob->name, telopts[ch], ch));
                if (d_flag)
                    debug_message("%s Do %s\n", time_stamp(), telopts[ch]);
                (*telopts_do[ch])(ch);
            } else {
                debug_message("%s Unknown telnet option Do %d\n"
                             , time_stamp(), ch);
                send_wont(ch);
            }
            state = ip->ts_data;
            goto change_state;

        case TS_DONT:
            command_giver = ip->ob;
            if (ch < NTELOPTS) {
                DT(("'%s' t_n: state DONT got %s (%02x)\n"
                   , ip->ob->name, telopts[ch], ch));
                if (d_flag)
                    debug_message("%s Dont %s\n", time_stamp(), telopts[ch]);
                (*telopts_dont[ch])(ch);
            } else {
                debug_message("%s Unknown telnet option Dont %d\n"
                             , time_stamp(), ch);
            }
            state = ip->ts_data;
            goto change_state;

        case TS_SB:
            DT(("'%s' t_n: state TS_SB got %02x\n", ip->ob->name, ch));
            if (ch == IAC) {
                state = TS_SB_IAC;
                goto change_state;
            }
            *to++ = (char)ch;
            continue;

        case TS_SB_IAC:
          {
            mp_int size;
            vector_t *v;

            DT(("'%s' t_n: state TS_SB_IAC got %02x\n", ip->ob->name, ch));
            if (ch == IAC) {
                DT(("'%s' t_n: that is: state TS_SB_IAC got IAC\n"
                   , ip->ob->name));
                *to++ = (char)ch;
                state = TS_SB;
                goto change_state;
            } else if ((ch == SE || ch == SB)
                && (  (size = (to - first) - ip->tn_start - 1) <= max_array_size
                    || !max_array_size)
                && size >= 0
                && (current_object = ip->ob,  v = allocate_array(size)) )
            {
                unsigned char *str;
                svalue_t *svp;

                str = (unsigned char *)&ip->text[ip->tn_start];
                DT(("'%s' t_n: that is: state TS_SB_IAC got useful SE or SB: neg SB %02x (%ld bytes)\n", ip->ob->name, *str, (long)size));
                push_number(SB);
                push_number(*str++);
                svp = v->item;
                while (--size >= 0) {
                    svp->u.number = *str++;
                    svp++;
                }
                push_referenced_vector(v);
                command_giver = ip->ob;
                h_telnet_neg(3);
            }
            to = &first[ip->tn_start];
            if (ch != SE)
                goto ts_iac;
            state = ip->ts_data;
            goto change_state;
          }

        case TS_SYNCH:
            DT(("'%s' t_n: state TS_SYNCH got %02x\n", ip->ob->name, ch));
            if (ch == IAC) goto new_iac;
            if (ch == DM) goto data_mark;
            continue;

        default:
            if (d_flag)
                debug_message("%s Bad state: 0x%x\n", time_stamp(), ip->tn_state);
            state = TS_DATA;
            goto change_state;
        } /* switch (ip->tn_state) */

    } while(from < end);

    /* We used all the new data in .text[] but found no complete command.
     * Reset all pointers necessary to read new data.
     */

    ip->text_end = ip->tn_end = ip->command_end = (short)(to - first);
    if (ip->text_end == MAX_TEXT)
    {
        /* telnet negotiation shouldn't have such large data chunks.
         * Ignore all data altogether and return to text mode.
         */
        ip->text_end = ip->tn_end = ip->command_end = 0;
        ip->tn_start = ip->command_start = 0;
        ip->tn_state = TS_DATA;
    }
} /* telnet_neg() */

/* End of Telnet support */
/*=========================================================================*/
/*                      ERQ Support
 */

#ifdef ERQ_DEMON
/*-------------------------------------------------------------------------*/
void
start_erq_demon (char *suffix)

/* Start the ERQ demon from the path 'ERQFILE<suffix>' and setup
 * the pending_erq[] array.
 */

{
    svalue_t *erqp;
    char path[MAXPATHLEN+1];
    int sockets[2];
    int pid;
    char c;

    /* Create the freelist in pending_erq[] */
    pending_erq[0].type = T_INVALID;
    pending_erq[0].u.lvalue = NULL;

    erqp = pending_erq + 1;
    while (erqp < &pending_erq[MAX_PENDING_ERQ])
    {
        erqp->u.lvalue = erqp - 1;
        erqp->type = T_INVALID;
        erqp++;
    }
    free_erq = &pending_erq[MAX_PENDING_ERQ-1];


    /* Create the sockets to talk to the ERQ */
/* TODO: Add tests to configure if the system really implements AF_UNIX or socketpair() */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
    {
        perror("socketpair");
        return;
    }

    (void)signal(SIGCLD, SIG_IGN); /* don't create zombie processes */

    if ((pid = fork()) == 0)
    {
        /* Child */
        dup2(sockets[0], 0);
        dup2(sockets[0], 1);
        close(sockets[0]);
        close(sockets[1]);

        if (strlen(erq_file) + 1 + strlen(suffix) <= sizeof path)
        {
            sprintf(path, "%s%s", erq_file, suffix);
            if (erq_args)
                execv((char *)path, erq_args);
            else
                execl((char *)path, "erq", "--forked", 0);
        }
        write(1, "0", 1);  /* indicate failure back to the driver */
        fprintf(stderr, "%s exec of erq demon '%s' failed.\n", time_stamp(), path);
        _exit(1);
    }

    close(sockets[0]);
    if (pid == -1) {
        close(sockets[1]);
        return;
    }

    /* Read the first character from the ERQ. If it's '0', the ERQ
     * didn't start.
     */
    read(sockets[1], &c, 1);
    if (c == '0') {
        close(sockets[1]);
        fprintf(stderr, "%s start of erq demon failed.\n", time_stamp());
        return;
    }

    /* ERQ is up and running */
    erq_demon = sockets[1];
    set_socket_nonblocking(erq_demon);
    if (socket_number(erq_demon) >= min_nfds)
        min_nfds = socket_number(erq_demon)+1;
}

/*-------------------------------------------------------------------------*/
static void
shutdown_erq_demon (void)

/* Close the connection to the ERQ.
 * This method is to be used directly only on game shutdown, otherwise
 * use stop_erq_demon() instead.
 */

{
    if (erq_demon < 0)
        return;

    socket_close(erq_demon);
    erq_demon = FLAG_NO_ERQ;
    erq_pending_len = 0;
    input_from_erq = &buf_from_erq[0];
} /* shutdown_erq_demon() */

/*-------------------------------------------------------------------------*/
static void
stop_erq_demon (Bool notify)

/* Close the connection to the ERQ and inform all pending requests
 * about this. If <notify> is set, the hook H_ERQ_STOP is called.
 */

{
    svalue_t *erqp;
    int i;

    if (erq_demon < 0)
        return;

    shutdown_erq_demon();

    /* Inform all pending requests about the loss.
     */
    erqp = pending_erq;
    i = MAX_PENDING_ERQ;
    do {
        if (erqp->type == T_CLOSURE)
        {
            *++inter_sp = *erqp;
            erqp->type = T_INVALID;
            erqp->u.lvalue = free_erq;
            free_erq = erqp;
            CLEAR_EVAL_COST;
            RESET_LIMITS;
            apply_master_ob(STR_STALE_ERQ, 1);
        }
        erqp++;
    } while (--i);

    /* If desired, call H_ERQ_STOP to notify the situation.
     */
    if (notify)
    {
        RESET_LIMITS;
        CLEAR_EVAL_COST;
        if (driver_hook[H_ERQ_STOP].type == T_CLOSURE) {
            secure_call_lambda(&driver_hook[H_ERQ_STOP], 0);
        }
    }
} /* stop_erq_demon() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_attach_erq_demon (svalue_t *sp)

/* TEFUN: attach_erq_demon()
 *
 *   int attach_erq_demon(object ob, int do_close)
 *   int attach_erq_demon(string name, int do_close)
 *
 * In the first form, take away the connection from <ob> and store it as
 * _the_ erq connection. <ob> thus becomes a normal non-interactive object.
 * In the second form, try to start the ERQ demon from the path
 * 'ERQFILE<name>' (ERQFILE defaults to BINDIR/erq). <name> must not
 * contain '/..' sequences.
 *
 * If there is already an ERQ demon connected to the driver, the function
 * will fail unless <do_close> is set to 1 or any other odd integer; in
 * this case the connection to the old ERQ will be closed first.
 *
 * Return svalue.number 1 on success, 0 else.
 */

{
    object_t *ob;
    interactive_t *ip;
    char *suffix;

    /* Test for the first form: (object ob, int do_close) */
    if (sp[-1].type == T_OBJECT
     && (ob = sp[-1].u.ob, O_SET_INTERACTIVE(ip, ob)))
    {
        if (sp->type != T_NUMBER)
        {
            bad_xefun_arg(2, sp);
            /* NOTREACHED */
        }
        sp--;
        deref_object(ob, "attach_erq_demon");
        put_number(sp, 0);
        /* we need to read sp[1] below, thus don't overwrite it now. */
        if (privilege_violation4("attach_erq_demon",
            ob, 0, sp[1].u.number, sp+1))
        {
            if (erq_demon != FLAG_NO_ERQ) {
                if (sp[1].u.number & 1) {
                    stop_erq_demon(0);
                    erq_demon = FLAG_ERQ_STOP;
                } else {
                    return sp;
                }
            }
            erq_proto_demon = ip->socket;
            ip->do_close = FLAG_PROTO_ERQ;
            sp->u.number = 1;
        }
        return sp;
    }
    else

    /* Test for the second form: (string name, int do_close) */
    if (sp[-1].type == T_STRING
          && !strstr((suffix = sp[-1].u.string), "/.."))
    {
        int n;

        if (sp->type != T_NUMBER)
        {
            bad_xefun_arg(2, sp);
            /* NOTREACHED */
        }
        sp--;
        n = 0;
        if (privilege_violation4("attach_erq_demon",
            0, suffix, sp[1].u.number, sp+1))
        {
            if (erq_demon != FLAG_NO_ERQ)
            {
                if (sp[1].u.number & 1) {
                    stop_erq_demon(0);
                } else {
                    goto return_result;
                }
                erq_proto_demon = -1;
            }
            start_erq_demon(suffix);
            n = 1;
        }
return_result:
        free_svalue(sp);
        put_number(sp, n);
        return sp;
    }
    else
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    /* NOTREACHED */
    return NULL;
}

/*-------------------------------------------------------------------------*/
static Bool
send_erq (int handle, int request, char *arg, size_t arglen)

/* Send compose an ERQ message out of <handle>, <request> and <arg>
 * and send it to the ERQ. If all the data can't be sent now, the
 * next call to send_erq() will send the rest.
 *
 * Return FALSE if the data couldn't be sent, TRUE on success.
 *
 * SOCK_SEQPACKET is not portable enough, thus make special provisions
 * to deliver messages in an atomic fashion.
 */

{
    static char buf[ERQ_MAX_SEND], *pending;
    long wrote;

    if (erq_demon < 0)
        return MY_FALSE;

    /* Try to send the pending data */
    if (erq_pending_len) {
        wrote = socket_write(erq_demon, pending, erq_pending_len);
        if (wrote > 0) {
            pending += wrote;
            erq_pending_len -= wrote;
        }
        if (erq_pending_len)
            return MY_FALSE;
    }

    if (arglen + 9 > sizeof buf)
        return MY_FALSE;

    /* Create the message and add it to buf[] */
    erq_pending_len = arglen + 9;
    *(uint32*)buf = htonl(erq_pending_len);
    *(uint32*)(buf+4) = htonl(handle);
    buf[8] = (char)request;
    memcpy(buf + 9, arg, arglen);

    /* Send as much of buf[] as possible */
    pending = buf;
    wrote = socket_write(erq_demon, buf, erq_pending_len);
    if (wrote > 0) {
        pending += wrote;
        erq_pending_len -= wrote;
    }

    return MY_TRUE;
}

/*-------------------------------------------------------------------------*/
svalue_t *
f_send_erq (svalue_t *sp)

/* TEFUN: send_erq()
 *
 *   int send_erq(int request, string|int* data, closure callback)
 *
 * Send a request of type <request> and the data <data> to the ERQ>
 * If <callback> is set to a closure, it will be called with the
 * response from the ERQ.
 *
 * The function returns svalue.number 1 on success, and 0 on failure.
 *
 * The function causes a privilege violation "erq".
 */

{
    char  *arg;
    size_t arglen;
    svalue_t *new_erq;
    int i;

    if (sp[-2].type != T_NUMBER)
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    /* Set arg with the data to send. */

    if (sp[-1].type == T_STRING) {
        arg = sp[-1].u.string;
        arglen = strlen(arg);
    }
    else if (sp[-1].type == T_POINTER)
    {
        vector_t *v;
        svalue_t *svp;
        char *cp;
        mp_int j;

        v = sp[-1].u.vec;
        arglen = VEC_SIZE(v);
        cp = arg = alloca(arglen);
        svp = &v->item[0];
        for (j = (mp_int)arglen; --j >= 0; )
            *cp++ = (char)(*svp++).u.number;
    }
    else
    {
        bad_xefun_arg(2, sp);
        /* NOTREACHED */
    }

    /* Test if this call is allowed. */

    if (!privilege_violation4("erq", 0, "", sp[-2].u.number, sp)) {
        goto failure;
    }

    /* Store the callback closure. If none is given, use the
     * default callback.
     */

    if (sp->type == T_NUMBER && !sp->u.number) {
        new_erq = &pending_erq[MAX_PENDING_ERQ];
        new_erq->u.lvalue = free_erq;
    }
    else if (sp->type == T_CLOSURE
          && sp->x.closure_type != CLOSURE_UNBOUND_LAMBDA)
    {
        new_erq = free_erq;
    }
    else
    {
        bad_xefun_arg(3, sp);
        /* NOTREACHED */
    }

    /* Send the request and make up the result. */

    if (new_erq
     && 0 != (i = send_erq(new_erq - pending_erq, sp[-2].u.number, arg, arglen)) )
    {
        free_erq = new_erq->u.lvalue;
        *new_erq = *sp;
    }
    else
    {
failure:
        i = 0;
        free_svalue(sp);
    }

    free_svalue(--sp);
    (*--sp).u.number = i;

    return sp;
}

/*-------------------------------------------------------------------------*/
static long
read_32 (char *str)

/* Read a 32 bit value from a possibly unaligned network byte order
 * representation.
 */

{
    unsigned char *p = (unsigned char *)str;

    return (long)p[0]<<24 | (long)p[1]<<16 | (long)p[2]<<8 | p[3];
}

/*-------------------------------------------------------------------------*/
static void
add_ip_entry (struct in_addr addr, const char *name)

/* Add a new IP address <addr>/hostname <name> pair to the cache iptable[].
 * If the <addr> already exists in the table, replace the old tabled name
 * with the new one.
 */

{
    int i, ix;
    Bool new_entry;

    ix = -1;
    new_entry = MY_FALSE;
    for (i = 0; i < IPSIZE; i++)
    {
        if (!memcmp(&(iptable[i].addr.s_addr), &addr.s_addr, sizeof(iptable[i].addr.s_addr)))
        {
            ix = i;
            break;
        }
    }

    if (ix < 0)
    {
        ix = ipcur;
        new_entry = MY_TRUE;
    }

    iptable[ix].addr = addr;
    if (iptable[ix].name)
        free_string(iptable[ix].name);
    iptable[ix].name = make_shared_string(name);

    if (new_entry)
        ipcur = (ipcur+1) % IPSIZE;
} /* add_ip_entry() */

/*-------------------------------------------------------------------------*/
#ifdef USE_IPV6

static void
update_ip_entry (const char *oldname, const char *newname)

/* Change the IP name <oldname> in the iptable[] to <newname>.
 * If the <oldname> is not in the table, nothing happens.
 */

{
    int i, ix;
    Bool new_entry;

    ix = -1;
    new_entry = MY_FALSE;
    for (i = 0; i < IPSIZE; i++)
    {
        if (iptable[i].name && !strcmp(iptable[i].name, oldname))
        {
            free_string(iptable[i].name);
            iptable[i].name = make_shared_string(newname);
        }
    }
} /* update_ip_entry() */

#endif

/*-------------------------------------------------------------------------*/
static char *
lookup_ip_entry (struct in_addr addr, Bool useErq)

/* Lookup the IP address <addr> and return an uncounted pointer to
 * a shared string with the hostname. The function looks first in the
 * iptable[], then, if not found there and <useErq> is true, asks the ERQ.
 * If the hostname can not be found, NULL is returned.
 */

{
    int i;
    char *ipname;
    struct in_addr tmp;

    /* Search for the address backwards from the last added entry,
     * hoping that its one of the more recently added ones.
     */
    i = ipcur;
    do {
        i--;
        if (i < 0)
            i += IPSIZE;

        if (!memcmp(&(iptable[i].addr.s_addr), &addr.s_addr, sizeof(iptable[i].addr.s_addr))
         && iptable[i].name)
        {
            return iptable[i].name;
        }
    } while (i != ipcur );

    /* The address is new to us.
     * Add a temporary entry into the iptable[] to bridge
     * the time until the erq has finished the lookup.
     * This also handles the case of an unresolvable hostname.
     */

    iptable[ipcur].addr = addr;
    if (iptable[ipcur].name)
        free_string(iptable[ipcur].name);

    memcpy(&tmp, &addr, sizeof(tmp));
#ifndef USE_IPV6
    ipname = make_shared_string(inet_ntoa(tmp));
#else
    ipname = make_shared_string(inet6_ntoa(tmp));
#endif

    iptable[ipcur].name = ipname;

    ipcur = (ipcur+1) % IPSIZE;

    /* If we have the erq and may use it, lookup the real hostname */
    if (erq_demon >= 0 && useErq)
    {
#ifndef USE_IPV6
        send_erq(ERQ_HANDLE_RLOOKUP, ERQ_RLOOKUP, (char *)&addr.s_addr, sizeof(addr.s_addr));
#else
        send_erq(ERQ_HANDLE_RLOOKUPV6, ERQ_RLOOKUPV6, ipname, strlen(ipname));
#endif
    }

    return iptable[ipcur].name;
}

#endif /* ERQ_DEMON */

/* End of ERQ Support */
/*=========================================================================*/

/*-------------------------------------------------------------------------*/
void
remove_stale_player_data (void)

/* GC and statistics support: Remove all input_to and prompt infos
 * referencing destructed objects.
 */

{
    int i;

    for(i = 0 ; i < MAX_PLAYERS; i++)
    {
        input_to_t * it, * prev;
        object_t *ob;

        if (all_players[i] == NULL)
            continue;

        /* Remove stale input_to data */
        for ( prev = NULL, it = all_players[i]->input_to; it != NULL; )
        {
            input_to_t *tmp;
            ob = callback_object(&(it->fun));
            if (ob)
            {
                prev = it;
                it = it->next;
            }
            else
            {
                /* The object has selfdestructed */

                if (prev == NULL)
                {
                    set_noecho(all_players[i], it->next ? it->next->noecho : 0);
                    all_players[i]->input_to = it->next;
                }
                else
                {
                    prev->next = it->next;
                }

                tmp = it;
                it = it->next;

                free_input_to(tmp);
            }
        }

        /* Remove stale snooping monsters */
        ob = all_players[i]->snoop_by;
        if (ob && !O_IS_INTERACTIVE(ob) && !check_object(ob))
        {
            free_object(ob, "remove_stale_player_data");
            all_players[i]->snoop_by = NULL;
        }

        /* Remove a stale modify_command object */
        ob = all_players[i]->modify_command;
        if (ob && !check_object(ob))
        {
            free_object(ob, "remove_stale_player_data");
            all_players[i]->modify_command = NULL;
        }
    } /* for (i) */
} /* remove_stale_player_data() */

/*-------------------------------------------------------------------------*/
size_t
show_comm_status (strbuf_t * sbuf, Bool verbose UNUSED)

/* Return the amount of memory used by the comm module.
 */

{
#if defined(__MWERKS__)
#    pragma unused(verbose)
#endif
    size_t sum;
    int i;

    remove_stale_player_data();

    sum = 0;

    for (i = 0; i <= max_player; i++)
    {
        interactive_t *pl;
        input_to_t *it;

        pl = all_players[i];
        if (!pl)
            continue;

        sum += sizeof(*pl);

        for (it = pl->input_to; it != NULL; it = it->next)
            sum += sizeof(*it);

        sum += ed_buffer_size(O_GET_EDBUFFER(pl->ob));
    }

    if (sbuf)
        strbuf_addf(sbuf, "Comm structures\t\t\t\t %9lu\n", sum);
    return sum;
} /* show_comm_status() */

#ifdef GC_SUPPORT

/*-------------------------------------------------------------------------*/
void
clear_comm_refs (void)

/* GC support: Clear all refs the module might have.
 */

{
#ifdef ERQ_DEMON
    clear_ref_in_vector(
      pending_erq, sizeof pending_erq / sizeof (svalue_t)
    );
#endif /* ERQ_DEMON */
}

/*-------------------------------------------------------------------------*/
void
count_comm_refs (void)

/* GC support: count any ref the module has.
 */
{
#ifdef ERQ_DEMON
    int i;

    for(i = 0; i < IPSIZE; i++) {
        if (iptable[i].name)
            count_ref_from_string(iptable[i].name);
    }
    count_ref_in_vector(
      pending_erq, sizeof pending_erq / sizeof (svalue_t)
    );
#endif /* ERQ_DEMON */
}

#endif /* GC_SUPPORT */


/*=========================================================================*/

/*-------------------------------------------------------------------------*/
svalue_t *
query_ip_name (svalue_t *sp, Bool lookup)

/* Lookup the IP address (<lookup> is false) or IP hostname (<lookup> is
 * true) of object <sp> and return it. If <sp> is the number 0 or a
 * non-interactive object, the number 0 is returned.
 *
 * The hostname is read from the iptable[], so if it hasn't been
 * resolved yet, we return the number in any case.
 *
 * If <sp> is a reference to an interactive object, it will be replaced
 * on return with an array of integers with the full sockaddr_in:
 *    array[0.. 1]: sin_family
 *    array[2.. 3]: sin_port
 *    array[4.. 7]: sin_addr
 *    array[8..15]: undefined (ideally 0).
 *
 * The function is used to implement the efuns query_ip_number() and
 * query_ip_name().
 */

{
    object_t *ob;
    int i;
    interactive_t *ip;
    char *str;

    /* Set <ob> to the object passed on the stack. */

    if (sp->type != T_OBJECT)
    {
        svalue_t *svp;

        if (sp->type == T_NUMBER && !sp->u.number)
            return sp;
        svp = sp;
        while (svp->type == T_LVALUE || svp->type == T_PROTECTED_LVALUE)
            svp = svp->u.lvalue;
        if (svp->type != T_OBJECT)
        {
            bad_xefun_arg(1, sp);
            /* NOTREACHED */
        }
        ob = svp->u.ob;
    }
    else
    {
        ob = sp->u.ob;
        deref_object(ob, "query_ip_name");
        sp->type = T_INVALID;
    }

    /* Return 0 for non-interactive objects */
    if (!(O_SET_INTERACTIVE(ip, ob)))
    {
        free_svalue(sp);
        put_number(sp, 0);
        return sp;
    }

    /* If the object was passed as reference, replace it with an array
     * with the full sockaddr_in.
     */
    if (sp->type == T_LVALUE)
    {
        svalue_t array, *svp;
        vector_t *v;
        char *cp;

        v = allocate_array(sizeof ip->addr);
        if (v)
        {
            put_array(&array, v);
            i = sizeof ip->addr;
            svp = v->item;
            cp = (char *)&ip->addr;
            do {
                svp->u.number = *cp++;
                svp++;
            } while(--i);
            transfer_svalue(sp, &array);
        }
        else
        {
            assign_svalue(sp, &const0);
        }
    }

    /* If the hostname is requested and we indeed have it in our table,
     * return it.
     */
    if (lookup)
    {
#ifdef ERQ_DEMON
        char * hname;

        hname = lookup_ip_entry(ip->addr.sin_addr, MY_FALSE);
        if (hname)
        {
            put_ref_string(sp, hname);
            return sp;
        }
#else
        /* The if(lookup) gets rid of a 'lookup unused' warning. */
#endif
    }

    /* Return the IP address as string.
     */

#ifndef USE_IPV6
    str = string_copy(inet_ntoa(ip->addr.sin_addr));
#else
    str = string_copy(inet6_ntoa(ip->addr.sin_addr));
#endif
    if (!str)
    {
        inter_sp = sp - 1;
        error("Out of memory for IP address\n");
    }
    put_malloced_string(sp, str);
    return sp;
}

/*-------------------------------------------------------------------------*/
char *
query_host_name (void)

/* Return the hostname (and just the hostname, not the full domain name).
 * The result is a pointer to a static array!
 */

{
    static char name[MAXHOSTNAMELEN+1];
    char *p;

    gethostname(name, sizeof name);
    name[sizeof name - 1] = '\0';        /* Just to make sure */
    /* some platforms return the FQHN, but we don't want it. */
    p = strchr(name, '.');
    if (p)
        *p = '\0';
    return name;
}

/*-------------------------------------------------------------------------*/
char *
get_host_ip_number (void)

/* Return the IP address of the host.
 * The result is a newly allocated string.
 */

{
#ifndef USE_IPV6
    char buf[INET_ADDRSTRLEN+3];

    sprintf(buf, "\"%s\"", inet_ntoa(host_ip_number));
#else
    char buf[INET6_ADDRSTRLEN+3];

    sprintf(buf, "\"%s\"", inet6_ntoa(host_ip_number));
#endif
    return string_copy(buf);
}

/*-------------------------------------------------------------------------*/
svalue_t *
f_query_snoop (svalue_t *sp)

/* TEFUN: query_snoop()
 *
 *   object query_snoop(object victim)
 *
 * Return the object which is snooping <victim>, or 0 if there is none.
 * The call must be allowed by master->valid_query_snoop().
 */

{
    svalue_t *arg1;
    object_t *ob;

    if (sp->type != T_OBJECT)
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    /* Do some test and set ob to the snooper (if any) */
    switch (0) /* try {...} */
    {
      default:
        ob = sp->u.ob;
        if ((ob->flags & (O_DESTRUCTED|O_SHADOW)) != O_SHADOW
         || O_GET_SHADOW(ob)->ip == NULL)
        {
            zero_object_svalue(sp);
            return sp;
        }
        inter_sp = sp;
        assert_master_ob_loaded();
        if (current_object != master_ob)
        {
            assign_eval_cost();
            arg1 = apply_master_ob(STR_VALID_QSNOOP, 1);
            if (arg1 == 0 || arg1->type != T_NUMBER || !arg1->u.number)
            {
                ob = NULL;
                break;
            }
        }
        else
        {
            deref_object(ob, "query_snoop");
        }
        ob = O_GET_INTERACTIVE(ob)->snoop_by;
    }

    /* Return the result */
    if (ob)
        put_ref_object(sp, ob, "query_snoop");
    else
        put_number(sp, 0);
    return sp;
}

/*-------------------------------------------------------------------------*/
svalue_t *
f_query_idle (svalue_t *sp)


/* TEFUN: query_idle()
 *
 *   int query_idle(object ob)
 *
 * Return how many seconds a user object has been idle.
 */

{
    int i;
    object_t *ob;

    if (sp->type != T_OBJECT)
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    ob = sp->u.ob;
    if (!O_IS_INTERACTIVE(ob))
    {
        inter_sp = sp;
        error("query_idle() of non-interactive object.\n");
        return sp;
    }

    i = current_time - O_GET_INTERACTIVE(ob)->last_time;
    deref_object(ob, "query_idle");
    put_number(sp, i);
    return sp;
}

/*-------------------------------------------------------------------------*/
svalue_t *
f_remove_interactive (svalue_t *sp)

/* TEFUN: remove_interactive()
 *
 *   void remove_interactive(object ob)
 *
 * Close the connection to the interactive object ob.
 *
 * In fact, the connection is only flushed and marked for closing,
 * as a remove_interactive() here can upset some other code.
 * The actual remove will be done by get_message().
 */

{
    interactive_t *victim;

    if (sp->type != T_OBJECT)
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    if (O_SET_INTERACTIVE(victim, sp->u.ob)
     && !victim->closing
     && !victim->do_close)
    {
        if (victim->message_length) {
            command_giver = victim->ob;
            add_message(message_flush);

            /* message_flush takes always directly effect on the
             * socket. No apply() is involved.
             */
        }
        victim->do_close = FLAG_DO_CLOSE;
    }
    free_svalue(sp);
    return sp - 1;
}

/*-------------------------------------------------------------------------*/
int
replace_interactive (object_t *ob, object_t * obfrom, char *name)

/* EFUN: exec()
 *
 * Switch the network connection from <obfrom> to <ob>. If <ob> is already
 * interactive, its connection will be switched to <obfrom>. The efun is
 * called from the program <name>.
 *
 * If <obfrom> was command_giver, <ob> will be the new command_giver.
 *
 * The call is validated by master->valid_exec() and has to return 0 on
 * failure, and 1 on success.
 */

{
    svalue_t *v;
    interactive_t *stale_interactive, *ip;
    object_t *save_command;

    /* Ask the master if this exec() is ok. */
    push_volatile_string(name);
    push_object(ob);
    push_object(obfrom);
    v = apply_master_ob(STR_VALID_EXEC, 3);
    if (!v || v->type != T_NUMBER || v->u.number == 0)
        return 0;

    /* stale_interactive becomes the former interactive _if_ it
     * still is an interactive_t.
     */
    if (!(O_SET_INTERACTIVE(stale_interactive, ob)))
    {
        stale_interactive = NULL;
    }

    if (!(O_SET_INTERACTIVE(ip, obfrom)))
        error("Bad argument 2 to exec()\n");

    /* When we have to have an out of memory error, have it before pointers
     * get changed.
     */
    assert_shadow_sent(ob);

    save_command = command_giver;

    /* If <ob> has a connection, flush it */
    if (stale_interactive)
    {
        if (stale_interactive->message_length)
        {
            command_giver = ob;
            add_message(message_flush);
        }
    }

    /* Flush the connection of <obfrom> */

    if (ip->message_length) {
        command_giver = obfrom;
        add_message(message_flush);
    }
    command_giver = save_command;

    /* Switch a possible snooper */

    if (ip->snoop_on)
        ip->snoop_on->snoop_by = ob;

    /* Switch the interactive */

    O_GET_INTERACTIVE(ob) = ip;
    O_GET_INTERACTIVE(obfrom) = NULL;
    ob->flags |= O_ONCE_INTERACTIVE;
    ip->ob = ob;
    ip->catch_tell_activ = MY_TRUE;

    if (stale_interactive)
    {
        /* Tie <ob>s stale connection to <obfrom>. */

        O_GET_INTERACTIVE(obfrom) = stale_interactive;
        stale_interactive->ob = obfrom;
        if (stale_interactive->snoop_on)
            stale_interactive->snoop_on->snoop_by = obfrom;
        stale_interactive->catch_tell_activ = MY_TRUE;
    }
    else
    {
        /* Clean up <obfrom> after the loss of connection */

        obfrom->flags &= ~O_ONCE_INTERACTIVE;
        check_shadow_sent(obfrom);

        ref_object(ob, "exec");
        free_object(obfrom, "exec");
    }

    /* If this_player() or this_interactive() point to one of the
     * involved objects, switch it too.
     */
    if (obfrom == command_giver)
        command_giver = ob;
    else if (ob == command_giver)
        command_giver = obfrom;

    if (obfrom == current_interactive)
        current_interactive = ob;
    else if (ob == current_interactive)
        current_interactive = obfrom;

    return 1;
}

/*-------------------------------------------------------------------------*/
#ifdef DEBUG

void
count_comm_extra_refs (void)

/* Count all the refs to verify the normal refcounting. */

{
    int i;

#ifdef ERQ_DEMON
    count_extra_ref_in_vector(
      pending_erq, sizeof pending_erq / sizeof (svalue_t)
    );
#endif /* ERQ_DEMON */

    for (i = 0; i < MAX_PLAYERS; i++)
    {
        object_t *ob;
        input_to_t *it;

        if (all_players[i] == 0)
            continue;
        all_players[i]->ob->extra_ref++;
        if ( NULL != (ob = all_players[i]->snoop_by) ) {
            interactive_t *ip;

            if (!(O_SET_INTERACTIVE(ip, current_object)))
            {
                /* snooping monster */
                ob->extra_ref++;
            }
        } /* end of snoop-processing */

        for ( it = all_players[i]->input_to; it; it = it->next)
        {
            count_callback_extra_refs(&(it->fun));
            count_extra_ref_in_vector(&it->prompt, 1);

        }
        if ( NULL != (ob = all_players[i]->modify_command) )
            count_extra_ref_in_object(ob);
        count_extra_ref_in_vector(&all_players[i]->prompt, 1);
    }
} /* count_comm_extra_refs() */

#endif /* DEBUG */

/*-------------------------------------------------------------------------*/
svalue_t *
f_send_udp (svalue_t *sp)

/* TEFUN: send_udp()
 *
 *   int send_udp(string host, int port, string message)
 *   int send_udp(string host, int port, int * message)
 *
 * Sends The message in an UDP packet to the given host and port
 * number. Causes a privilege violation.
 * The message can be given either as string, or as array of
 * bytes. The latter variant allows to send binary data as well.
 * Returns 1 on success, 0 on failure.
 *
 * Note: On some machines a failed send_udp() will not be registered
 * until the next send_udp() - the latter one might return '0' even
 * if itself was successful.
 */

{
    char *to_host;
    int to_port;
    char *msg;
    size_t msglen;
    int ip1, ip2, ip3, ip4;
    struct sockaddr_in name;
    struct hostent *hp;
    int ret = 0;

    if ((sp-2)->type != T_STRING) bad_xefun_arg(1, sp);
    if ((sp-1)->type != T_NUMBER) bad_xefun_arg(2, sp);

    switch(0) { default: /* try {...} */

        /* Set msg/msglen to the data of the message to send */

        if (sp->type == T_STRING)
        {
            msg = sp->u.string;
            msglen = strlen(msg);
        }
        else if (sp->type == T_POINTER)
        {
            vector_t *v;
            svalue_t *svp;
            char *cp;
            mp_int j;

            v = sp->u.vec;
            msglen = VEC_SIZE(v);
            cp = msg = alloca(msglen);
            if (!msg)
                break;
            svp = &v->item[0];
            for (j = (mp_int)msglen; --j >= 0; )
                *cp++ = (char)(*svp++).u.number;
        }
        else
        {
            bad_xefun_arg(3, sp);
            /* NOTREACHED */
        }

        /* Is this call valid? */

        if (
#ifdef USE_DEPRECATED
            !_privilege_violation("send_imp", sp-2, sp) &&
#endif
            !_privilege_violation("send_udp", sp-2, sp)
           )
            break;

        if (udp_s < 0)
            break;

        /* Determine the destination address */

        to_host = (sp-2)->u.string;
        to_port = (sp-1)->u.number;

#ifndef USE_IPV6
        if (sscanf(to_host, "%d.%d.%d.%d", &ip1, &ip2, &ip3, &ip4) == 4)
        {
            name.sin_addr.s_addr = inet_addr(to_host);
            name.sin_family = AF_INET;
        }
        else
        {
            /* TODO: Uh-oh, blocking DNS in the execution thread */
            hp = gethostbyname(to_host);
            if (hp == 0)
                break;
            memcpy(&name.sin_addr, hp->h_addr, (size_t)hp->h_length);
            name.sin_family = AF_INET;
        }

#else /* USE_IPV6 */
        
        /* TODO: Uh-oh, blocking DNS in the execution thread */
        hp = gethostbyname2(to_host, AF_INET6);
        if (hp == 0) hp = gethostbyname2(to_host, AF_INET);
        if (hp == 0) break;
        memcpy(&name.sin_addr, hp->h_addr, (size_t)hp->h_length);

        if (hp->h_addrtype == AF_INET)
        {
            CREATE_IPV6_MAPPED(name.sin_addr, (uint32_t)hp->h_addr_list[0]);
        }
        name.sin_family = AF_INET6;
#endif /* USE_IPV6 */
        
        name.sin_port = htons(to_port);

        /* Send the message. */
#ifndef SENDTO_BROKEN
        if (sendto(udp_s, msg, msglen, 0,
               (struct sockaddr *)&name, sizeof(name)) != msglen)
#endif
            break;
        ret = 1;
    }

    /* Return the result */

    free_svalue(sp);
    free_svalue(--sp);
    free_svalue(--sp);
    put_number(sp, ret);
    return sp;
} /* f_send_udp() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_set_buffer_size (svalue_t *sp)

/* TEFUN: set_buffer_size()
 *
 *   int set_buffer_size(int size)
 *
 * Changes the socket buffer size for this_interactive() to size,
 * up to a preconfigured maximum, result is the old buffer size
 * (or -1 on systems which aren't able to change the socket
 * buffer).
 * Modifying the buffer size may result in a better IO
 * throughput, but can also worsen it.
 */

{
    int new;

    /* Get the desired buffer size */

    if (sp->type != T_NUMBER || sp->u.number > SET_BUFFER_SIZE_MAX)
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }
    new = sp->u.number;

    sp->u.number = -1; /* Default result */

#ifdef SO_SNDBUF
    {
        int old;
        length_t optlen;
        interactive_t *ip;

        if (!(O_SET_INTERACTIVE(ip, current_object))
         || ip->do_close)
        {
            return sp;
        }


        optlen = sizeof old;
        if (getsockopt(ip->socket, SOL_SOCKET, SO_SNDBUF, (char *)&old, &optlen) < 0)
            return sp;
        if (setsockopt(ip->socket, SOL_SOCKET, SO_SNDBUF, (char *)&new, sizeof new) < 0)
            return sp;
        sp->u.number = old;
    }
#endif /* SO_SNDBUF */

    return sp;
}

/*-------------------------------------------------------------------------*/
svalue_t *
f_binary_message (svalue_t *sp)

/* TEFUN: binary_message()
 *
 *   int binary_message(int *|string message, int flags)
 *
 * Flush output and send output directly with write WITHOUT IAC QUOTING.
 * The message may contain zeroes if given as int *.
 * The messages goes to this_object(), but only if interactive.
 * Returned value: number of characters actually written.
 * Any 'allowed charset' setting is ignored.
 *
 * Flag settings are interpreted bitwise and may be ored
 * together:
 *
 *   Bit 0 (value 1): when set, add_message() is used instead of
 *     write(). Thus no previous flushing of the buffer is
 *     needed, but the output is not immediate, nor can the
 *     number of bytes actually sent be determined - the return
 *     value is undefined.
 *   Bit 1 (value 2): The buffer is flushed _after_ adding the
 *     message. Useful only in conjunction with Bit 0.
 *
 * The idea behind the flag settings is that sending command
 * codes for colours and other things needs to bypass the allowed
 * charset filters, but isn't important enough to waste bandwith
 * on a synchronous transmission.
 */

{
    char *message, *p;
    size_t size;
    mp_int wrote = 0, i;
    svalue_t *svp;
    interactive_t *ip;
    object_t *save_command_giver;

    /* Set message to the data to be sent, and size to its length. */

    if (sp[-1].type == T_POINTER)
    {
        size = VEC_SIZE(sp[-1].u.vec);
        message = alloca(size + 1);
        if (!message)
            fatal("Stack overflow in binary_message()");
        for (i = (mp_int)size, svp = sp[-1].u.vec->item, p = message; --i >= 0; svp++)
        {
            if (svp->type != T_NUMBER)
            {
                bad_xefun_arg(1, sp);
                /* NOTREACHED */
            }
            *p++ = (char)svp->u.number;
        }
        *p = '\0';
    }
    else if (sp[-1].type == T_STRING)
    {
        message = sp[-1].u.string;
        size = strlen(message);
    }
    else
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    if (sp->type != T_NUMBER)
    {
        bad_xefun_arg(2, sp);
        /* NOTREACHED */
    }

    /* Send the message */

    i = 0;
    if (O_SET_INTERACTIVE(ip, current_object)
     && !ip->do_close)
    {
        save_command_giver = command_giver;
        command_giver = current_object;

        if (sp->u.number & 1)
        {
            /* Write before flush... */

            sending_telnet_command = MY_TRUE; /* turn of IAC quoting */

            /* Loop until the whole message is sent.
             * The loop is necessary because the message may contain
             * embedded '\0' which the normal add_message() doesn't
             * add.
             */
            while (size)
            {
                if (*message) {
                    add_message("%s", message);
                    if (ip->do_close)
                        break;
                    size -= (wrote = (mp_int)strlen(message));
                    message += wrote;
                }
                else
                {
                    /* Possibly an embedded '\0'. Fake the basics
                     * of add_message()
                     */

                    if (ip->message_length >= MAX_SOCKET_PACKET_SIZE)
                    {
                        add_message(message_flush);
                        if (ip->do_close)
                            break;
                    }
                    if (!ip->message_length )
                    {
                        if ( NULL != (ip->next_player_for_flush = first_player_for_flush) )
                        {
                            O_GET_INTERACTIVE(first_player_for_flush)->
                              previous_player_for_flush =
                                command_giver;
                        }
                        ip->previous_player_for_flush = NULL;
                        first_player_for_flush = command_giver;
                    }
                    ip->message_buf[ip->message_length++] = '\0';
                    size--;
                }
            } /* while(size) */
            sending_telnet_command = MY_FALSE;

            if (sp->u.number & 2)
                add_message(message_flush);
            wrote = 0;

        }
        else
        {
            /* Flush, then write. */
            add_message(message_flush);

            /* Since all pending data was flushed, we can write directly
             * to the socket now.
             */

            for (i = 6;;) {
#ifdef USE_PTHREAD
                wrote = (mp_int)thread_socket_write(ip->socket, message
                                                   , (size_t)size, ip);
#else
                wrote = (mp_int)socket_write(ip->socket, message, (size_t)size);
#endif
                if (wrote != -1)
                    break;
                switch(errno)
                {
                case EINTR:
                    if (--i)
                        continue;
                    fprintf(stderr
                           , "%s comm: write EINTR. Message discarded.\n"
                           , time_stamp());
                    break;
                case EWOULDBLOCK:
                    fprintf(stderr,
                      "%s comm: write EWOULDBLOCK. Message discarded.\n"
                           , time_stamp());
                    size = 0;
                    break;
                case EMSGSIZE:
                    fprintf(stderr, "%s comm: write EMSGSIZE.\n"
                           , time_stamp());
                    break;
                default:
                    perror("write");
                    ip->do_close = FLAG_DO_CLOSE;
                    break;
                }
                break;
            } /* end for on retry count */
        } /* if (type of write) */

        command_giver = save_command_giver;
    } /* end if interactive */

    sp--;
    free_svalue(sp);
    put_number(sp, wrote);
    return sp;
} /* f_binary_message() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_set_combine_charset (svalue_t *sp)

/* TEFUN: set_combine_charset()
 *
 *   void set_combine_charset (int* bitvector)
 *   void set_combine_charset (string chars)
 *
 * Set the set of characters which can be combined into a single string
 * when received en-bloc in charmode from the current interactive user.
 * Non-combinable characters and single received characters are returned
 * in separate strings as usual. The function must be called with the
 * interactive user being the command giver.
 *
 * The newline '\n' and the NUL character '\0' are always non-combinable.
 *
 * The charset can be given either directly as a string, or indirectly
 * as a bitvector.
 *
 * The bitvector is interpreted as an array of 8-bit-values and might
 * contain up to 32 elements. Character n is "combinable"
 * if sizeof(bitvector) > n/8 && bitvector[n/8] & (1 << n%8) .
 */

{
    mp_int i;
    svalue_t *svp;
    char *p;
    interactive_t *ip;

    i = 0;
    if (sp->type != T_STRING
     && (sp->type != T_POINTER || (i = (mp_int)VEC_SIZE(sp->u.vec)) > 32))
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    if (command_giver && O_SET_INTERACTIVE(ip, command_giver))
    {
        if (sp->type == T_STRING)
        {
            memset(ip->combine_cset, 0, sizeof ip->combine_cset);
            for ( i = svalue_strlen(sp), p = sp->u.string
                ; i > 0
                ; i--, p++)
                ip->combine_cset[(*p & 0xff) / 8] |= 1 << (*p % 8);
        }
        else
        {
            /* i was set in the typecheck above */
            for ( svp = sp->u.vec->item, p = ip->combine_cset
                ; --i >= 0
                ; svp++, p++)
            {
                if (svp->type == T_NUMBER)
                    *p = (char)svp->u.number;
            }
            memset(p, 0, (size_t)(&ip->combine_cset[sizeof ip->combine_cset] - p));
        }

        ip->combine_cset['\n'/8] &= ~(1 << '\n' % 8);
        ip->combine_cset['\0'/8] &= ~(1 << '\0' % 8);
    }
    free_svalue(sp);
    sp--;
    return sp;
} /* f_set_combine_charset() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_set_connection_charset (svalue_t *sp)

/* TEFUN: set_connection_charset()
 *
 *   void set_connection_charset (int* bitvector, int quote_iac)
 *   void set_connection_charset (string charset, int quote_iac)
 *
 * Set the set of characters that can be output to the interactive user
 * (this does not apply to binary_message() ). The function must be called
 * by the interactive user object itself.
 *
 * The charset can be given either directly as a string, or indirectly
 * as a bitvector.
 *
 * The bitvector is interpreted as an array of 8-bit-values and might
 * contain up to 32 elements. Character n is allowed to be output
 * if sizeof(bitvector) > n/8 && bitvector[n/8] & (1 << n%8) .
 *
 * If quote_iac is 0 and char 255 is allowed to be output, IAC
 * will be output unmodified.
 * If quote_iac is 1 and char 255 is allowed to be output,
 * char 255 will be quoted so that it is not interpreted as IAC
 * by the telnet protocol.
 */

{
    mp_int i;
    svalue_t *svp;
    char *p;
    interactive_t *ip;

    i = 0;
    if ( sp[-1].type != T_STRING
     && (sp[-1].type != T_POINTER || (i = (mp_int)VEC_SIZE(sp[-1].u.vec)) > 32))
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }
    if (sp->type != T_NUMBER)
    {
        bad_xefun_arg(2, sp);
        /* NOTREACHED */
    }

    if (O_SET_INTERACTIVE(ip, current_object))
    {
        if (sp[-1].type == T_STRING)
        {
            memset(ip->charset, 0, sizeof ip->charset);
            for ( i = svalue_strlen(sp-1), p = sp[-1].u.string
                ; i > 0
                ; i--, p++)
                ip->charset[(*p & 0xff) / 8] |= 1 << (*p % 8);
        }
        else
        {
            /* i was set in the typecheck above */
            for ( svp = sp[-1].u.vec->item, p = ip->charset
                ; --i >= 0
                ; svp++, p++)
            {
                if (svp->type == T_NUMBER)
                    *p = (char)svp->u.number;
            }
            memset(p, 0, (size_t)(&ip->charset[sizeof ip->charset] - p));
        }

        ip->charset['\n'/8] &= ~(1 << '\n' % 8);
        ip->charset['\0'/8] &= ~(1 << '\0' % 8);

        if ( 0 != (ip->quote_iac = (char)sp->u.number) )
        {
            if (ip->charset[IAC/8] & (1 << IAC % 8))
                ip->charset[IAC/8] &= ~(1 << IAC % 8);
            else
                ip->quote_iac = MY_FALSE;
        }
    }
    sp--;
    free_svalue(sp);
    sp--;
    return sp;
} /* f_set_connection_charset() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_set_prompt (svalue_t *sp)

/* TEFUN set_prompt()
 *
 *       string set_prompt(mixed prompt, object ob)
 *
 * Set the prompt given by the first argument for the interactive object
 * instead of the default ``> ''. If the second argument is omitted,
 * this_player() is used as default. The first arg can be a string or a
 * closure.
 *
 * The result returned is the old prompt.
 */

{
    svalue_t *prompt;
    interactive_t *ip;

    /* Make sure the object is interactive */
    if (sp->type != T_OBJECT
     || !(O_SET_INTERACTIVE(ip, sp->u.ob))
     || ip->closing)
    {
        bad_xefun_arg(2, sp);
    }

    /* Get the address of the prompt svalue */
    prompt = &O_GET_INTERACTIVE(sp->u.ob)->prompt;

    free_object_svalue(sp);
    sp--;

    if (sp->type == T_STRING || sp->type == T_CLOSURE)
    {
        if (sp->type == T_CLOSURE && sp->x.closure_type == CLOSURE_UNBOUND_LAMBDA)
        {
            inter_sp = sp;
            error("Bad argument 1 for set_prompt(): unbound lambda closure\n");
            /* NOTREACHED */
        }

        if (sp->type == T_STRING
         && sp->x.string_type == STRING_VOLATILE)
        {
            char *str = make_shared_string(sp->u.string);

            if (!str)
            {
                inter_sp = sp;
                error("(set_prompt) Out of memory (%lu bytes) for prompt\n"
                     , (unsigned long) strlen(sp->u.string));
            }
            else
            {
                sp->u.string = str;
                sp->x.string_type = STRING_SHARED;
            }
        }

        /* Three-way exchange to set the new prompt and put
         * the old one onto the stack.
         */
        sp[1] = *prompt;
        *prompt = *sp;
        *sp = sp[1];
#ifdef USE_FREE_CLOSURE_HOOK
        if (sp->type == T_CLOSURE)
        {
            /* In case the prompt is changed from within the prompt
             * closure.
             */
            addref_closure(sp, "unset_prompt");
            free_closure_hooks(sp, 1);
        }
#endif
    }
    else if (sp->type == T_NUMBER
          && (sp->u.number == 0 || sp->u.number == -1) )
    {
        assign_svalue(sp, prompt);
    }
    else
    {
        bad_xefun_arg(1, sp);
        /* NOTREACHED */
    }

    return sp;
} /* f_set_prompt() */

/*-------------------------------------------------------------------------*/
svalue_t *
e_input_to (svalue_t *sp, int num_arg)

/* EFUN: input_to()
 *
 * Set up a function in the current object to be called with the
 * next user input.
 *
 * This function can do a lot - I won't repeat the external
 * man page here :-)
 */

{
    svalue_t *arg;       /* Pointer to the arguments of the efun */
    svalue_t *extra_arg; /* Pointer to the extra arguments of the efun */
    int iflags;          /* The flags passed to input_to() */
    int flags;           /* The flags as required for .noecho */
    input_to_t *it;
    int extra;           /* Number of extra arguments */
    int error_index;

    arg = sp - num_arg + 1;
    if (arg[0].type != T_STRING && arg[0].type != T_CLOSURE)
    {
        bad_efun_vararg(1, sp);
        /* NOTREACHED */
    }

    /* Extract the arguments */

    iflags = 0;
    extra = 0;
    extra_arg = arg + 1;

    if (num_arg > 1)
    {
        if (arg[1].type != T_NUMBER)
        {
            bad_efun_vararg(2, sp);
            /* NOTREACHED */
        }
        iflags = arg[1].u.number;
        extra = num_arg - 2;
        extra_arg = arg + 2;
    }

    /* Setup the flags required for 'noecho' */
    flags =   ((iflags & INPUT_NOECHO)      ? NOECHO_REQ   : 0)
            | ((iflags & INPUT_CHARMODE)    ? CHARMODE_REQ : 0)
            | ((iflags & INPUT_IGNORE_BANG) ? IGNORE_BANG  : 0)
          ;

    /* Allocate and setup the input_to structure */

    xallocate(it, sizeof *it, "new input_to");
    init_empty_callback(&(it->fun));
    put_number(&(it->prompt), 0);

    /* If SET_PROMPT was specified, collect it */

    if (iflags & INPUT_PROMPT)
    {
        if (num_arg <= 2)
        {
            error("Missing prompt argument to input_to().\n");
            /* NOTREACHED */
        }

        if (arg[2].type != T_STRING && arg[2].type != T_CLOSURE)
        {
            free_input_to(it);
            bad_efun_vararg(3, sp);
            /* NOTREACHED */
        }

        transfer_svalue(&(it->prompt), arg+2);
        extra--;
        extra_arg++;
    }
    else
        put_number(&(it->prompt), 0);

    /* Parse the extra args for the call */

    if (arg[0].type == T_STRING)
    {
        error_index = setup_function_callback(&(it->fun), current_object
                                             , arg[0].u.string
                                             , extra, extra_arg
                                             , MY_TRUE
                                             );
        free_string_svalue(arg);
    }
    else
        error_index = setup_closure_callback(&(it->fun), arg
                                            , extra, extra_arg
                                            , MY_TRUE
                                            );

    if (error_index >= 0)
    {
        free_input_to(it);
        bad_efun_vararg(error_index, arg - 1);
        /* NOTREACHED */
    }

    /* If the master agrees (only in case of IGNORE_BANG) the
     * the input_to can be set - return 1.
     */

    sp->type = T_NUMBER;
    if (!(flags & IGNORE_BANG)
     || privilege_violation4("input_to", command_giver, 0, flags, sp))
    {
        if (set_call(command_giver, it, (char)flags))
        {
            put_number(arg, 1);
            return arg;
        }
    }

    /* input_to() was not allowed - return 0. */

    free_input_to(it);
    put_number(arg, 0);
    return arg;
} /* e_input_to() */

/*-------------------------------------------------------------------------*/
static void
free_input_to (input_to_t *it)

/* Deallocate the input_to structure <it> and all referenced memory.
 */

{
    free_callback(&(it->fun));
    free_svalue(&(it->prompt));
    xfree(it);
} /* free_input_to() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_remove_input_to (svalue_t *sp, int num_arg)

/* VEFUN: remove_input_to()
 *
 *   int remove_input_to (object player)
 *   int remove_input_to (object player, string|closure|object fun)
 *
 * Remove a pending input_to from the interactive <player> object.
 * If the optional <fun> is not given, the most recently added input_to
 * is removed.
 *
 * If the optional <fun> is given, the efun tries to find and remove the
 * most recently added input_to matching the <fun> argument:
 *  - <fun> is a string: the input_to functionname has to match
 *  - <fun> is an object: the object the input_to function is bound to has
 *                        to match
 *  - <fun> is a closure: the input_to closure has to match.
 * 
 * Return 1 on success, or 0 on failure (no input_to found, object is not
 * interactive or has no input_to pending).
 */

{
    svalue_t      *arg;  /* Pointer to the arguments of the efun */
    int            rc;   /* Resultvalue */

    /* Check the arguments */
    arg = sp - num_arg + 1;
    if (arg[0].type != T_OBJECT)
    {
        bad_xefun_vararg(1, sp);
        /* NOTREACHED */
        return NULL;
    }

    if (num_arg > 1
     && arg[1].type != T_OBJECT && arg[1].type != T_STRING
     && arg[1].type != T_CLOSURE)
    {
        bad_xefun_vararg(2, sp);
        /* NOTREACHED */
        return NULL;
    }

    /* If the second arg exists and is a string, we need it shared */
    if (num_arg > 1
     && arg[1].type == T_STRING
     && arg[1].x.string_type != STRING_SHARED)
    {
        char * str = make_shared_string(arg[1].u.string);
        if (arg[1].x.string_type == STRING_MALLOC)
            xfree(arg[1].u.string);
        arg[1].x.string_type = STRING_SHARED;
        arg[1].u.string = str;
    }

    /* Process the command, bailing out whenever necessary */
    do {
        input_to_t * prev;
        input_to_t    *it;
        interactive_t *ip;

        /* Get the interactive object.
         * If there is none, or if it is closing down or doesn't have
         * an input_to set, fail.
         */
        if (!(O_SET_INTERACTIVE(ip, arg[0].u.ob))
         || ip->closing || ip->input_to == NULL
           )
        {
            rc = 0;
            break;
        }

        /* If no filter argument has been given, just remove
         * the first input to.
         */
        if (num_arg < 2)
        {
            it = ip->input_to;
            ip->input_to = it->next;
            free_input_to(it);
            ip->set_input_to = (ip->input_to != NULL);
            rc = 1;
            break;
        }

        /* There is a filter argument: search for the right input_to */

        for (prev = NULL, it = ip->input_to
            ; it != NULL
            ; prev = it, it = it->next)
        {
            Bool found = MY_FALSE;

            switch (arg[1].type)
            {
            case T_STRING:
                if (!it->fun.is_lambda
                 && it->fun.function.named.name == arg[1].u.string)
                    found = MY_TRUE;
                break;
                
            case T_OBJECT:
                if (callback_object(&(it->fun)) == arg[1].u.ob)
                    found = MY_TRUE;
                break;

            case T_CLOSURE:
                if (it->fun.is_lambda
                 && closure_eq(&(it->fun.function.lambda), arg+1))
                    found = MY_TRUE;
                break;

            default:
                fatal("Unsupported argument type %d\n", arg[1].type);
                break;
            }

            if (found)
                break;
        }

        if (it != NULL)
        {
            /* We found the input_to: remove it */
            if (prev == NULL)
                ip->input_to = it->next;
            else
                prev->next = it->next;

            free_input_to(it);
            ip->set_input_to = (ip->input_to != NULL);
            rc = 1;
            break;
        }

        /* At this point, nothing worked: failure */
        rc = 0;
    } while (0);

    /* Return the result */
    sp = pop_n_elems(num_arg, sp);
    sp++;
    put_number(sp, rc);

    return sp;
} /* f_remove_input_to() */

/*-------------------------------------------------------------------------*/
svalue_t *
query_ip_port (svalue_t *sp)

/* EFUN: query_mud_port()
 *
 * Returns the port number the parser uses for user connections.
 *
 *   int query_mud_port(void)
 *
 * If no argument is given, the port for this_player() is
 * returned. If this_player() is not existing or not interactive,
 * the first port number open for connections is returned.
 *
 *   int query_mud_port(object user)
 *   int query_mud_port(int num)
 *
 * If an user object is given, the port used for its connection
 * is returned.
 * If a positive number is given, the <num>th port number the
 * parser uses for connections is returned (given that there are
 * that many ports).
 * If -1 is given, the number of ports open for connections is
 * returned.
 */

{
    object_t *ob;
    interactive_t *ip;
    struct sockaddr_in addr;
    length_t length;

    length = sizeof(addr);

    if (sp->type != T_OBJECT) {
        if (   sp->type != T_NUMBER
            || sp->u.number < -1 || sp->u.number >= numports
           )
        {
            bad_xefun_arg(1, sp);
            /* NOTREACHED */
        }
        sp->u.number = sp->u.number < 0 ? numports : port_numbers[sp->u.number];
        return sp;
    }

    ob = sp->u.ob;
    deref_object(ob, "query_ip_port");

    if ( !(O_SET_INTERACTIVE(ip, ob))) {
        put_number(sp, port_numbers[0]);
        return sp;
    }

    getsockname(ip->socket, (struct sockaddr *)&addr, &length);
    put_number(sp, ntohs(addr.sin_port));
    return sp;
}

/***************************************************************************/
