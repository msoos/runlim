#include <asm/param.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*------------------------------------------------------------------------*/

#define SAMPLE_RATE 10000	/* in milliseconds */
#define REPORT_RATE 100		/* in terms of sampling */

/*------------------------------------------------------------------------*/

#ifndef PID_MAX			/* usually set by 'configure.sh' */
#define PID_MAX 32768		/* default on Linux it seems */
#endif

/*------------------------------------------------------------------------*/

typedef struct Process Process;
typedef enum Status Status;

/*------------------------------------------------------------------------*/

enum Status
{
  OK,
  OUT_OF_MEMORY,
  OUT_OF_TIME,
  SEGMENTATION_FAULT,
  BUS_ERROR,
  OTHER_SIGNAL,
  FORK_FAILED,
  INTERNAL_ERROR,
  EXEC_FAILED
};

/*------------------------------------------------------------------------*/

struct Process
{
  char active;
  char cyclic_sampling;
  char cyclic_killing;
  long pid;
  long ppid;
  long sampled;
  double time;
  double memory;
  Process * next;
  Process * child;
  Process * parent;
  Process * sibbling;
};

/*------------------------------------------------------------------------*/

#define USAGE \
"usage: runlim [option ...] program [arg ...]\n" \
"\n" \
"  where option is from the following list:\n" \
"\n" \
"    -h                         print this command line summary\n" \
"    --help\n" \
"\n" \
"    --version                  print version number\n" \
"\n" \
"    --space-limit=<number>     set space limit to <number> MB\n" \
"    -s <number>\n"\
"\n" \
"    --time-limit=<number>      set time limit to <number> seconds\n" \
"    -t <number>\n"\
"\n" \
"    --real-time-limit=<number> set real time limit to <number> seconds\n" \
"    -r <number>\n"\
"\n" \
"    -k|--kill                  propagate signals\n" \
"\n" \
"The program is the name of an executable followed by its arguments.\n"

/*------------------------------------------------------------------------*/

static void
usage (void)
{
  printf (USAGE);
  fflush (stdout);
}

/*------------------------------------------------------------------------*/

static FILE *log = 0;
static int close_log = 0;

/*------------------------------------------------------------------------*/

static void
error (const char * fmt, ...)
{
  va_list ap;
  assert (log);
  fputs ("runlim error: ", log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  fputc ('\n', log);
  va_end (ap);
  fflush (log);
  exit (1);
}

static void
warning (const char * fmt, ...)
{
  va_list ap;
  assert (log);
  fputs ("runlim warning: ", log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  fputc ('\n', log);
  va_end (ap);
  fflush (log);
}

static void
message (const char * type, const char * fmt, ...)
{
  size_t len;
  va_list ap;
  assert (log);
  fputs ("[runlim] ", log);
  fputs (type, log);
  fputc (':', log);
  for (len = strlen (type); len < 22; len += 8)
    fputc ('\t', log);
  fputc ('\t', log);
  va_start (ap, fmt);
  vfprintf (log, fmt, ap);
  va_end (ap);
  fputc ('\n', log);
  fflush (log);
}

/*------------------------------------------------------------------------*/

static int
isposnumber (const char *str)
{
  const char *p;
  int res;

  if (*str)
    {
      for (res = 1, p = str; res && *p; p++)
	res = isdigit ((int) *p);
    }
  else
    res = 0;

  return res;
}

/*------------------------------------------------------------------------*/

static unsigned
parse_number_argument (int *i, int argc, char **argv)
{
  unsigned res;

  if (argv[*i][2])
    {
      if (isposnumber (argv[*i] + 2))
	res = (unsigned) atoi (argv[*i] + 2);
      else
	goto ARGUMENT_IS_MISSING;
    }
  else if (*i + 1 < argc && isposnumber (argv[*i + 1]))
    {
      res = (unsigned) atoi (argv[*i + 1]);
      *i += 1;
    }
  else
    {
ARGUMENT_IS_MISSING:
      error ("number argument for '-%c' missing");
      res = 0;
    }

  return res;
}

/*------------------------------------------------------------------------*/

static void
print_long_command_line_option (FILE * file, char *str)
{
  const char *p;

  for (p = str; *p && *p != ' '; p++)
    fputc (*p, file);
}

/*------------------------------------------------------------------------*/

static unsigned
parse_number_rhs (char *str)
{
  unsigned res;
  char *p;

  p = strchr (str, '=');
  assert (p);

  if (!p[1])
    {
      fputs ("*** runlim: argument to ", stderr);
      print_long_command_line_option (stderr, str);
      fputs (" is missing\n", stderr);
      exit (1);

      res = 0;
    }

  if (!isposnumber (p + 1))
    {
      fputs ("*** runlim: argument to ", stderr);
      print_long_command_line_option (stderr, str);
      fputs (" is not a positive number\n", stderr);
      exit (1);

      res = 0;
    }

  res = (unsigned) atoi (p + 1);

  return res;
}

/*------------------------------------------------------------------------*/

static unsigned
get_physical_mb ()
{
  unsigned res;
  long tmp;

  tmp = sysconf (_SC_PAGE_SIZE) * sysconf (_SC_PHYS_PAGES);
  tmp >>= 20;
  res = (unsigned) tmp;

  return res;
}

/*------------------------------------------------------------------------*/

static int child_pid = -1;
static int parent_pid = -1;

/*------------------------------------------------------------------------*/

static long num_samples_since_last_report = 0;
static long num_samples = 0;

static double max_time = 0;
static double max_memory = 0;

/*------------------------------------------------------------------------*/

static int propagate_signals = 0;
static int children = 0;

/*------------------------------------------------------------------------*/

#define PID_POS 0
#define PPID_POS 3
#define STIME_POS 13
#define UTIME_POS 14
#define RSIZE_POS 23
#define MAX_POS 23

/*------------------------------------------------------------------------*/

static unsigned start_time;
static unsigned time_limit;
static unsigned real_time_limit;
static unsigned space_limit;

/*------------------------------------------------------------------------*/

static char * buffer;
static size_t size_buffer;
static size_t pos_buffer;

/*------------------------------------------------------------------------*/

static char * path;
static size_t size_path;

/*------------------------------------------------------------------------*/

static void
push_buffer (int ch)
{
  if (size_buffer == pos_buffer)
    {
      size_buffer = size_buffer ? 2*size_buffer : 128;
      buffer = realloc (buffer, size_buffer);
      if (!buffer)
	error ("out-of-memory reallocating buffer");
    }

  buffer[pos_buffer++] = ch;
}

static void
fit_path (size_t len)
{
  if (len > size_path)
    {
      size_path = 2*len;
      path = realloc (path, size_path);
      if (!path)
	error ("out-of-memory reallocating path");
    }
}

/*------------------------------------------------------------------------*/

static long pid_max;
static long page_size;
static Process process[PID_MAX];
static Process * active;
static pthread_mutex_t active_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
add_process (pid_t pid, pid_t ppid, double time, double memory)
{
  Process * p;

  assert (0 < pid);
  assert (pid < pid_max);
  assert (0 <= ppid); 
  assert (ppid < pid_max);

  p = process + pid;
  
  if (p->active)
    {
      assert (p->pid == pid);
      p->time = time;
      if (p->memory < memory)
	p->memory = memory;
    }
  else
    {
      assert (!p->active);
      p->active = 1;
      p->pid = pid;
      p->ppid = ppid;
      p->time = time;
      p->memory = memory;
      p->next = 0;
      if (active) active->next = p;
      else active = p;
    }

  p->sampled = num_samples;
}

/*------------------------------------------------------------------------*/

static long
read_processes (void)
{
  long ujiffies, sjiffies, rss;
  const char * proc = "/proc";
  long pid, ppid, tmpid;
  double time, memory;
  struct dirent *de;
  char *token;
  FILE *file;
  int i, ch;
  DIR *dir;

  long res = 0;

  if (!(dir = opendir (proc)))
    error ("can not open directory '%s'", proc);

NEXT:

  while ((de = readdir (dir)) != NULL)
    {
      pid = (pid_t) atoi (de->d_name);
      if (pid == 0) goto NEXT;
      if (pid >= pid_max) goto NEXT;

      fit_path (strlen (proc) + strlen (de->d_name) + 20);
      sprintf (path, "%s/%ld/stat", proc, pid);
      file = fopen (path, "r");
      if (!file) goto NEXT;

      pos_buffer = 0;
      
      while ((ch = getc (file)) != EOF)
	push_buffer (ch);
      
      (void) fclose (file);	/* ignore return value */

      push_buffer (0);
      
      ppid = -1;
      tmpid = -1;
      rss = -1;
      ujiffies = -1;
      sjiffies = -1;
      
      token = strtok (buffer, " ");
      i = 0;
      
      while (token && i <= MAX_POS)
	{
	  switch (i++)
	    {
	    case PID_POS:
	      if (sscanf (token, "%ld", &tmpid) != 1) goto NEXT;
	      if (tmpid != pid) goto NEXT;
	      break;
	    case PPID_POS:
	      if (sscanf (token, "%ld", &ppid) != 1) goto NEXT;
	      if (ppid < 0) goto NEXT;
	      if (ppid >= pid_max) goto NEXT;
	      break;
	    case RSIZE_POS:
	       if (sscanf (token, "%ld", &rss) != 1) goto NEXT;
	       break;
	    case STIME_POS:
	      if (sscanf (token, "%ld", &sjiffies) != 1) goto NEXT;
	      break;
	    case UTIME_POS:
	      if (sscanf (token, "%ld", &ujiffies) != 1) goto NEXT;
	      break;
	    default:
	      break;
	    }
	  
	  token = strtok (0, " ");
	}

      if (tmpid < 0) goto NEXT;
      assert (tmpid == pid);
      if (ppid < 0) goto NEXT;
      if (ujiffies < 0) goto NEXT;
      if (sjiffies < 0) goto NEXT;
      if (rss < 0) goto NEXT;


      time = (ujiffies + sjiffies) / (double) HZ;
      memory = rss / (double) page_size;

      add_process (pid, ppid, time, memory);
      res++;
    }
  
  (void) closedir (dir);

  message ("read", "%ld", res);

  return res;
}

static Process *
find_process (long pid)
{
  assert (0 <= pid);
  assert (pid < pid_max);
  return process + pid;
}

static void
clear_tree_connections (Process * p)
{
  p->parent = p->child = p->sibbling = 0;
}

static void
connect_process_tree (void)
{
  Process * p, * parent;

  for (p = active; p; p = p->next)
    {
      assert (p->active);
      assert (find_process (p->pid) == p);
      parent = find_process (p->ppid);
      clear_tree_connections (parent);
      clear_tree_connections (p);
    }

  for (p = active; p; p = p->next)
    {
      parent = find_process (p->ppid);
      p->parent = parent;
      if (parent->child) parent->child->sibbling = p;
      else parent->child = p;
    }
}

/*------------------------------------------------------------------------*/

static double accumulated_time;

/*------------------------------------------------------------------------*/

static long
flush_inactive_processes (void)
{
  Process * prev = 0, * p, * next;
  long res = 0;

  for (p = active; p; p = next)
    {
      assert (p->active);
      next = p->next;
      if (p->sampled == num_samples)
	{
	  prev = p;
	}
      else
	{
	  if (prev) prev->next = next;
	  else active = next;

	  accumulated_time += p->time;
	  p->active = 0;
	  res++;
	}
    }

  message ("flushed", "%ld", res);

  return res;
}

/*------------------------------------------------------------------------*/

static double sampled_time;
static double sampled_memory;

/*------------------------------------------------------------------------*/

static long
sample_recursively (Process * p)
{
  Process * child;
  long res = 0;

  if (p->cyclic_sampling)
    {
      warning ("cyclic process dependencies during sampling");
      return 0;
    }

  if (p->sampled == num_samples)
    {
      sampled_time += p->time;
      sampled_memory += p->memory;
      res++;
    }

  p->cyclic_sampling = 1;

  for (child = p->child; child; child = child->sibbling)
    res += sample_recursively (child);

  assert (p->cyclic_sampling);
  p->cyclic_sampling = 0;
  
  return res;
}

static long
sample_all_child_processes (void)
{
  long read, sampled;
  Process * p;

  pthread_mutex_lock (&active_mutex);

  read = read_processes ();
  connect_process_tree ();

  if (read > 0)
    {
      p = find_process (child_pid);
      sampled = sample_recursively (p);
      message ("sampled", "%ld", sampled);
    }
  else
    sampled = 0;

  sampled += flush_inactive_processes ();

  pthread_mutex_unlock (&active_mutex);

  return sampled;
}

/*------------------------------------------------------------------------*/

static struct itimerval timer;
static struct itimerval old_timer;

static int caught_out_of_memory;
static int caught_out_of_time;

static pthread_mutex_t caught_out_of_memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t caught_out_of_time_mutex = PTHREAD_MUTEX_INITIALIZER;

/*------------------------------------------------------------------------*/

static void
term_process (Process * p)
{
  assert (p->pid != parent_pid);
  kill (p->pid, SIGTERM);
}

static void
kill_process (Process * p)
{
  assert (p->pid != parent_pid);
  kill (p->pid, SIGKILL);
}

static long
kill_recursively (Process * p, void(*killer)(Process *))
{
  Process * child;
  long res = 0;

  if (p->cyclic_killing)
    return 0;

  p->cyclic_killing = 1;
  for (child = p->child; child; child = child->sibbling)
    res += kill_recursively (child, killer);
  assert (p->cyclic_killing);
  p->cyclic_killing = 0;
  usleep (100);

  killer (p);
  res++;

  return res;
}

static pthread_mutex_t kill_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
kill_all_child_processes (void)
{
  long ms = 160000;
  long rounds = 0;
  Process * p;
  long killed;
  long read;

  static void (*killer) (Process *);

  do 
    {
      usleep (ms);

      if (ms > 2000) killer = term_process;
      else killer = kill_process;

      pthread_mutex_lock (&kill_mutex);
      pthread_mutex_lock (&active_mutex);

      read = read_processes ();

      if (read > 0)
	{
	  connect_process_tree ();
	  p = find_process (child_pid);
	  killed = kill_recursively (p, killer);
	}
      else killed = 0;

      pthread_mutex_unlock (&active_mutex);
      pthread_mutex_unlock (&kill_mutex);

      if (ms > 1000) ms /= 2;
    }
  while (killed > 0 && rounds++ < 10);
}

/*------------------------------------------------------------------------*/

static double
wall_clock_time (void)
{
  double res = -1;
  struct timeval tv;
  if (!gettimeofday (&tv, 0))
    {
      res = 1e-6 * tv.tv_usec;
      res += tv.tv_sec;
    }
  return res;
}

/*------------------------------------------------------------------------*/

static double
real_time (void) 
{
  double res;
  if (start_time < 0) return -1;
  res = wall_clock_time() - start_time;
  return res;
}

/*------------------------------------------------------------------------*/

static void
report (double time, double mb)
{
  message ("sample", "%.1f time, %1.f real, %.1f MB");
}

/*------------------------------------------------------------------------*/

static void
sampler (int s)
{
  long sampled;

  assert (s == SIGALRM);
  num_samples++;

  sampled_time = sampled_memory = 0;
  sampled = sample_all_child_processes ();

  if (sampled > 0)
    { 
      if (sampled_memory > max_memory)
	max_memory = sampled_memory;

      if (sampled_time > max_time)
	max_time = sampled_time;
    }

  if (++num_samples_since_last_report >= REPORT_RATE)
    {
      num_samples_since_last_report = 0;
      if (sampled > 0)
	report (sampled_time, sampled_memory);
    }

  if (sampled > 0)
    {
      if (sampled_time > time_limit || real_time () > real_time_limit)
	{
	  int already_caught;
	  pthread_mutex_lock (&caught_out_of_time_mutex);
	  already_caught = caught_out_of_time;
	  caught_out_of_time = 1;
	  pthread_mutex_unlock (&caught_out_of_time_mutex);
	  if (!already_caught) kill_all_child_processes ();
	}
      else if (sampled_memory > space_limit)
	{
	  int already_caught;
	  pthread_mutex_lock (&caught_out_of_memory_mutex);
	  already_caught = caught_out_of_memory;
	  caught_out_of_memory = 1;
	  pthread_mutex_unlock (&caught_out_of_memory_mutex);
	  if (!already_caught) kill_all_child_processes ();
	}
    }
}

/*------------------------------------------------------------------------*/

static volatile int caught_usr1_signal = 0;
static volatile int caught_other_signal = 0;

static pthread_mutex_t caught_other_signal_mutex = PTHREAD_MUTEX_INITIALIZER;

/*------------------------------------------------------------------------*/

static void
sig_usr1_handler (int s)
{
  assert (s == SIGUSR1);
  caught_usr1_signal = 1;
}

static void (*old_sig_int_handler);
static void (*old_sig_segv_handler);
static void (*old_sig_kill_handler);
static void (*old_sig_term_handler);
static void (*old_sig_abrt_handler);

static void restore_signal_handlers ()
{
  (void) signal (SIGINT, old_sig_int_handler);
  (void) signal (SIGSEGV, old_sig_segv_handler);
  (void) signal (SIGKILL, old_sig_kill_handler);
  (void) signal (SIGTERM, old_sig_term_handler);
  (void) signal (SIGABRT, old_sig_abrt_handler);
}

static void
sig_other_handler (int s)
{
  int already_caught;
  pthread_mutex_lock (&caught_other_signal_mutex);
  already_caught = caught_other_signal;
  caught_other_signal = 1;
  pthread_mutex_unlock (&caught_other_signal_mutex);
  if (already_caught) return;
  restore_signal_handlers ();
  kill_all_child_processes ();
  usleep (10000);
  raise (s);
}

/*------------------------------------------------------------------------*/

static const char *
get_host_name ()
{
  const char * path = "/proc/sys/kernel/hostname";
  FILE * file;
  int ch;

  file = fopen (path, "r");
  if (!file)
    error ("can not open '%s' for reading", path);

  pos_buffer = 0;
  while ((ch = getc (file)) != EOF && ch != '\n')
    push_buffer (ch);

  push_buffer (0);

  (void) fclose (file);

  return buffer;
}

static long
get_pid_max ()
{
  const char * path = "/proc/sys/kernel/pid_max";
  FILE * file;
  long res;

  file = fopen (path, "r");
  if (!file)
    error ("can not open '%s' for reading", path);

  if (fscanf (file, "%ld", &res) != 1)
    error ("failed to read maximum process id from '%s'", path);

  if (res < 32768)
    error ("tiny maximum process id '%ld' in '%s'", res, path);

  if (res > (1l << 22))
    error ("huge maximum process id '%ld' in '%s'", res, path);

  if (fclose (file))
    warning ("failed to close file '%s'", path);

  return res;
}

/*------------------------------------------------------------------------*/

static const char *
ctime_without_new_line (time_t * t)
{
  const char * str, * p;
  str = ctime (t);
  pos_buffer = 0;
  for (p = str; *p && *p != '\n'; p++)
    push_buffer (*p);
  push_buffer (0);
  return buffer;
}

/*------------------------------------------------------------------------*/

int
main (int argc, char **argv)
{
  int i, j, res, status, s, ok;
  char signal_description[80];
  const char * description;
  struct rlimit l;
  double real;
  time_t t;

  log = stderr;
  assert (!close_log);

  pid_max = get_pid_max ();
  if (pid_max > PID_MAX)
    error ("maximum process id '%ld' exceeds limit '%ld' (recompile)",
      pid_max, (long) PID_MAX);

  page_size = getpagesize ();

  ok = OK;				/* status of the runlim */
  s = 0;				/* signal caught */

  time_limit = 60 * 60 * 24 * 3600;	/* one year */
  real_time_limit = time_limit;		/* same as time limit by default */
  space_limit = get_physical_mb ();	/* physical memory size */

  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] == '-')
	{
	  if (argv[i][1] == 't')
	    {
	      time_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--time-limit=") == argv[i])
	    {
	      time_limit = parse_number_rhs (argv[i]);
	    }
	  else if (argv[i][1] == 'r')
	    {
	      real_time_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--real-time-limit=") == argv[i])
	    {
	      real_time_limit = parse_number_rhs (argv[i]);
	    }
	  else if (argv[i][1] == 's')
	    {
	      space_limit = parse_number_argument (&i, argc, argv);
	    }
	  else if (strstr (argv[i], "--space-limit=") == argv[i])
	    {
	      space_limit = parse_number_rhs (argv[i]);
	    }
	  else if (strcmp (argv[i], "-v") == 0 ||
	           strcmp (argv[i], "--version") == 0)
	    {
	      printf ("%g\n", VERSION);
	      fflush (stdout);
	      exit (0);
	    }
	  else if (strcmp (argv[i], "-k") == 0 ||
	           strcmp (argv[i], "--kill") == 0)
	    {
	      propagate_signals = 1;
	    }
	  else if (strcmp (argv[i], "-h") == 0 ||
	           strcmp (argv[i], "--help") == 0)
	    {
	      usage ();
	      exit (0);
	    }
	  else
	    error ("invalid option '%s' (try '-h')", argv[1]);
	}
      else
	break;
    }

  if (i >= argc)
    error ("no program specified (try '-h')");

  message ("version", "%g", VERSION);
  message ("host", "%s", get_host_name ());
  message ("time limit", "%u seconds", time_limit);
  message ("real time limit", "%u seconds", real_time_limit);
  message ("space limit", "%u MB", space_limit);

  for (j = i; j < argc; j++)
    {
      char argstr[80];
      sprintf (argstr, "argv[%d]", j - i);
      message (argstr, "%s", argv[j]);
    }

  t = time (0);
  message ("start", "%s", ctime_without_new_line (&t));

  (void) signal (SIGUSR1, sig_usr1_handler);

  start_time = wall_clock_time();

  parent_pid = getpid ();
  child_pid = fork ();

  if (child_pid != 0)
    {
      if (child_pid < 0)
	{
	  ok = FORK_FAILED;
	  res = 1;
	}
      else
	{
	  status = 0;

	  old_sig_int_handler = signal (SIGINT, sig_other_handler);
	  old_sig_segv_handler = signal (SIGSEGV, sig_other_handler);
	  old_sig_kill_handler = signal (SIGKILL, sig_other_handler);
	  old_sig_term_handler = signal (SIGTERM, sig_other_handler);
	  old_sig_abrt_handler = signal (SIGABRT, sig_other_handler);

	  message ("parent pid", "%d", (int) child_pid);
	  message ("child pid", "%d", (int) parent_pid);

	  assert (SAMPLE_RATE < 1000000);
	  timer.it_interval.tv_sec = 0;
	  timer.it_interval.tv_usec = SAMPLE_RATE;
	  timer.it_value = timer.it_interval;
	  signal (SIGALRM, sampler);
	  setitimer (ITIMER_REAL, &timer, &old_timer);

	  (void) wait (&status);

	  setitimer (ITIMER_REAL, &old_timer, &timer);

	  if (WIFEXITED (status))
	    res = WEXITSTATUS (status);
	  else if (WIFSIGNALED (status))
	    {
	      s = WTERMSIG (status);
	      res = 128 + s;
	      switch (s)
		{
		case SIGXFSZ:
		  ok = OUT_OF_MEMORY;
		  break;
		case SIGXCPU:
		  ok = OUT_OF_TIME;
		  break;
		case SIGSEGV:
		  ok = SEGMENTATION_FAULT;
		  break;
		case SIGBUS:
		  ok = BUS_ERROR;
		  break;
		default:
		  ok = OTHER_SIGNAL;
		  break;
		}
	    }
	  else
	    {
	      ok = INTERNAL_ERROR;
	      res = 1;
	    }
	}
    }
  else
    {
      unsigned hard_time_limit;

      if (time_limit < real_time_limit)
	{
	  hard_time_limit = time_limit;
	  hard_time_limit = (hard_time_limit * 101 + 99) / 100;	// + 1%
	  l.rlim_cur = l.rlim_max = hard_time_limit;
	  setrlimit (RLIMIT_CPU, &l);
	}

      execvp (argv[i], argv + i);
      kill (getppid (), SIGUSR1);		// TODO DOES THIS WORK?
      exit (1);
    }

  real = real_time ();

  if (caught_usr1_signal)
    ok = EXEC_FAILED;
  else if (caught_out_of_memory)
    ok = OUT_OF_MEMORY;
  else if (caught_out_of_time)
    ok = OUT_OF_TIME;

  kill_all_child_processes ();

  t = time (0);
  message ("end", "%s", ctime_without_new_line (&t));

  if (max_time >= time_limit || real_time () >= real_time_limit)
    goto FORCE_OUT_OF_TIME_ENTRY;

  switch (ok)
    {
    case OK:
      description = "ok";
      res = 0;
      break;
    case OUT_OF_TIME:
FORCE_OUT_OF_TIME_ENTRY:
      description = "out of time";
      res = 2;
      break;
    case OUT_OF_MEMORY:
      description = "out of memory";
      res = 3;
      break;
    case SEGMENTATION_FAULT:
      description = "segmentation fault";
      res = 4;
      break;
    case BUS_ERROR:
      description = "bus error";
      res = 5;
      break;
    case FORK_FAILED:
      description = "fork failed";
      res = 6;
      break;
    case INTERNAL_ERROR:
      description = "internal error";
      res = 7;
      break;
    case EXEC_FAILED:
      description = "execvp failed";
      res = 1;
      break;
    default:
      sprintf (signal_description, "signal(%d)", s);
      description = signal_description;
      res = 11;
      break;
    }

  message ("status", description);
  message ("result", "%d", res);
  message ("children", "%d", children);
  message ("real", "%.2f seconds", real);
  message ("time", "%.2f seconds", max_time);
  message ("space", "%.1f MB", max_memory);
  message ("samples", "%ld", num_samples);

  if (close_log)
    {
      log = stderr;
      if (fclose (log))
	warning ("could not close log file");
    }

  if (buffer)
    free (buffer);

  if (path)
    free (path);

  restore_signal_handlers ();

  if (propagate_signals)
    {
      switch (ok)
	{
	case OK:
	case OUT_OF_TIME:
	case OUT_OF_MEMORY:
	case FORK_FAILED:
	case INTERNAL_ERROR:
	case EXEC_FAILED:
	  break;
	default:
	  raise (s);
	  break;
	}
    }

  return res;
}
