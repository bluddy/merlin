#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(__linux)
#include <linux/limits.h>
#elif defined(__APPLE__)
#include <sys/syslimits.h>
#elif defined(__OpenBSD__)
#include <sys/param.h>
#endif

/** Portability information **/

/* Determine OS, http://stackoverflow.com/questions/6649936
   __linux__       Defined on Linux
   __sun           Defined on Solaris
   __FreeBSD__     Defined on FreeBSD
   __NetBSD__      Defined on NetBSD
   __OpenBSD__     Defined on OpenBSD
   __APPLE__       Defined on Mac OS X
   __hpux          Defined on HP-UX
   __osf__         Defined on Tru64 UNIX (formerly DEC OSF1)
   __sgi           Defined on Irix
   _AIX            Defined on AIX
*/

/* Compute executable path, http://stackoverflow.com/questions/1023306
   Mac OS X       _NSGetExecutablePath() (man 3 dyld)
   Linux          readlink /proc/self/exe
   Solaris        getexecname()
   FreeBSD        sysctl CTL_KERN KERN_PROC KERN_PROC_PATHNAME -1
   NetBSD         readlink /proc/curproc/exe
   DragonFly BSD  readlink /proc/curproc/file
   Windows        GetModuleFileName() with hModule = NULL
*/

#define NO_EINTR(var, command) \
  do { (var) = command; } while ((var) == -1 && errno == EINTR)

static void dumpinfo(void);

static void failwith_perror(const char *msg)
{
  perror(msg);
  dumpinfo();
  exit(EXIT_FAILURE);
}

static void failwith(const char *msg)
{
  fprintf(stderr, "%s\n", msg);
  dumpinfo();
  exit(EXIT_FAILURE);
}

#define PATHSZ (PATH_MAX+1)

#define BEGIN_PROTECTCWD \
  { char previous_cwd[PATHSZ]; \
    if (!getcwd(previous_cwd, PATHSZ)) previous_cwd[0] = '\0';

#define END_PROTECTCWD \
    if (previous_cwd[0] != '\0') chdir(previous_cwd); }

static const char *path_socketdir(void)
{
  static const char *tmpdir = NULL;
  if (tmpdir == NULL)
    tmpdir = getenv("TMPDIR");
  if (tmpdir == NULL)
    tmpdir = "/tmp";
  return tmpdir;
}

/** Deal with UNIX IPC **/

static void ipc_send(int fd, unsigned char *buffer, size_t len, int fds[3])
{
  struct iovec iov = { .iov_base = buffer, .iov_len = len };
  struct msghdr msg = {
    .msg_iov = &iov, .msg_iovlen = 1,
    .msg_controllen = CMSG_SPACE(3 * sizeof(int)),
  };
  msg.msg_control = alloca(msg.msg_controllen);
  memset(msg.msg_control, 0, msg.msg_controllen);

  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(3 * sizeof(int));

  int *fds0 = (int*)CMSG_DATA(cm);
  fds0[0] = fds[0];
  fds0[1] = fds[1];
  fds0[2] = fds[2];

  ssize_t sent;
  NO_EINTR(sent, sendmsg(fd, &msg, 0));

  if (sent == -1)
    failwith_perror("sendmsg");

  while (sent < len)
  {
    ssize_t sent_;
    NO_EINTR(sent_, send(fd, buffer + sent, len - sent, 0));

    if (sent_ == -1)
      failwith_perror("sent");

    sent += sent_;
  }
}

/* Serialize arguments */

#define byte(x,n) ((unsigned)((x) >> (n * 8)) & 0xFF)

static const char *envvars[] = {
  "OCAMLLIB",
  "OCAMLFIND_CONF",
  "MERLIN_LOG",
  NULL
};

static void append_argument(unsigned char *buffer, size_t len, ssize_t *pos, const char *p)
{
  ssize_t j = *pos;
  while (*p && j < len)
  {
    buffer[j] = *p;
    j += 1;
    p += 1;
  }

  if (j >= len)
    failwith("maximum number of arguments exceeded");

  buffer[j] = 0;
  j += 1;
  *pos = j;
}

static ssize_t prepare_args(unsigned char *buffer, size_t len, int argc, char **argv)
{
  int i = 0;
  ssize_t j = 4;

  /* Append env var */
  for (i = 0; envvars[i] != NULL; ++i)
  {
    append_argument(buffer, len, &j, envvars[i]);

    const char *v = getenv(envvars[i]);
    if (v != NULL)
    {
      j -= 1; /* Overwrite delimiting 0 */
      append_argument(buffer, len, &j, "=");
      j -= 1; /* Overwrite delimiting 0 */
      append_argument(buffer, len, &j, v);
    }
  }

  /* Env var delimiter */
  append_argument(buffer, len, &j, "");

  /* Append arguments */
  for (i = 0; i < argc && j < len; ++i)
  {
    append_argument(buffer, len, &j, argv[i]);
  }

  /* Put size at the beginning */
  buffer[0] = byte(j,0);
  buffer[1] = byte(j,1);
  buffer[2] = byte(j,2);
  buffer[3] = byte(j,3);
  return j;
}

static int connect_socket(const char *socketname, int fail)
{
  int sock = socket(PF_UNIX, SOCK_STREAM, 0);
  if (sock == -1) failwith_perror("socket");

  int err;

  BEGIN_PROTECTCWD
    struct sockaddr_un address;
    int address_len;

    chdir(path_socketdir());
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, 104, "./%s", socketname);
    address_len = strlen(address.sun_path) + sizeof(address.sun_family) + 1;

    NO_EINTR(err, connect(sock, (struct sockaddr*)&address, address_len));
  END_PROTECTCWD

  if (err == -1)
  {
    if (fail) failwith_perror("connect");
    close(sock);
    return -1;
  }

  return sock;
}

static void make_daemon(int sock)
{
  /* On success: The child process becomes session leader */
  if (setsid() < 0)
    failwith_perror("setsid");

  /* Close all open file descriptors */
  close(0);
  if (open("/dev/null", O_RDWR, 0) != 0)
    failwith_perror("open");
  dup2(0,1);
  dup2(0,2);

  /* Change directory to root, so that process still works if directory
   * is delete. */
  if (chdir("/") != 0)
    failwith_perror("chdir");

  //int x;
  //for (x = sysconf(_SC_OPEN_MAX); x>2; x--)
  //{
  //  if (x != sock)
  //    close(x);
  //}

  pid_t child = fork();
  signal(SIGHUP, SIG_IGN);

  /* An error occurred */
  if (child < 0)
    failwith_perror("fork");

  /* Success: Let the parent terminate */
  if (child > 0)
    exit(EXIT_SUCCESS);
}

static void start_server(const char *socketname, const char *exec_path)
{
  int sock = socket(PF_UNIX, SOCK_STREAM, 0);
  if (sock == -1)
    failwith_perror("socket");

  int err;

  BEGIN_PROTECTCWD
    struct sockaddr_un address;
    int address_len;

    chdir(path_socketdir());
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, 104, "./%s", socketname);
    address_len = strlen(address.sun_path) + sizeof(address.sun_family) + 1;
    unlink(address.sun_path);

    NO_EINTR(err, bind(sock, (struct sockaddr*)&address, address_len));
  END_PROTECTCWD

  if (err == -1)
    failwith_perror("bind");

  if (listen(sock, 5) == -1)
    failwith_perror("listen");

  pid_t child = fork();

  if (child == -1)
    failwith_perror("fork");

  if (child == 0)
  {
    make_daemon(sock);

    char socket_fd[50], socket_path[PATHSZ];
    sprintf(socket_fd, "%d", sock);
    snprintf(socket_path, PATHSZ, "%s/%s", path_socketdir(), socketname);
    //execlp("nohup", "nohup", exec_path, "server", socket_path, socket_fd, NULL);
    execlp(exec_path, exec_path, "server", socket_path, socket_fd, NULL);
    failwith_perror("execlp");
  }

  close(sock);
  wait(NULL);
}

static int connect_and_serve(const char *socket_path, const char *exec_path)
{
  int sock = connect_socket(socket_path, 0);

  if (sock == -1)
  {
    start_server(socket_path, exec_path);
    sock = connect_socket(socket_path, 1);
  }

  if (sock == -1)
    abort();

  return sock;
}

/* OCaml merlin path */

static const char *search_in_path(const char *PATH, const char *argv0, char *merlin_path)
{
  static char binary_path[PATHSZ];

  if (PATH == NULL || argv0 == NULL) return NULL;

  while (*PATH)
  {
    int i = 0;
    // Copy one path from PATH
    while (i < PATHSZ-1 && *PATH && *PATH != ':')
    {
      binary_path[i] = *PATH;
      i += 1;
      PATH += 1;
    }

    // Append filename
    {
      binary_path[i] = '/';
      i += 1;

      const char *file = argv0;
      while (i < PATHSZ-1 && *file)
      {
        binary_path[i] = *file;
        i += 1;
        file += 1;
      }

      binary_path[i] = 0;
      i += 1;
    }

    // Check path
    char *result = realpath(binary_path, merlin_path);
    if (result != NULL)
      return result;

    // Seek next path in PATH
    while (*PATH && *PATH != ':')
      PATH += 1;

    while (*PATH == ':')
      PATH += 1;
  }

  return NULL;
}

static void compute_merlinpath(char merlin_path[PATHSZ], const char *argv0)
{
  if (realpath(argv0, merlin_path) == NULL)
    if (search_in_path(getenv("PATH"), argv0, merlin_path) == NULL)
      failwith("cannot resolve path to ocamlmerlin");

  size_t strsz = strlen(merlin_path);
  // Self directory
  while (strsz > 0 && merlin_path[strsz-1] != '/')
    strsz -= 1;
  merlin_path[strsz] = 0;

  // Append ocamlmerlin-server
  if (strsz + 19 > PATHSZ)
    failwith("path is too long");

  strcpy(merlin_path + strsz, "ocamlmerlin-server");
}

static void compute_socketname(char socketname[PATHSZ], const char merlin_path[PATHSZ])
{
  struct stat st;
  if (stat(merlin_path, &st) != 0)
    failwith_perror("stat (cannot find ocamlmerlin binary)");

  snprintf(socketname, PATHSZ,
      "ocamlmerlin_%llu_%llu_%llu.socket",
      (unsigned long long)getuid(),
      (unsigned long long)st.st_dev,
      (unsigned long long)st.st_ino);
}

/* Main */

static char
  merlin_path[PATHSZ] = "<not computed yet>",
  socketname[PATHSZ] = "<not computed yet>";
static unsigned char argbuffer[65536];

static void dumpinfo(void)
{
  fprintf(stderr,
      "merlin path: %s\nsocket path: %s/%s\n", merlin_path, path_socketdir(), socketname);
}

static void abnormal_termination(int argc, char **argv)
{
  int sexp = 0;
  int i;

  for (i = 1; i < argc - 1; ++i)
  {
    if (strcmp(argv[i], "-protocol") == 0 &&
        strcmp(argv[i+1], "sexp") == 0)
      sexp = 1;
  }

  puts(sexp
      ?  "((assoc) (class . \"failure\") (value . \"abnormal termination\") (notifications))"
      : "{\"class\": \"failure\", \"value\": \"abnormal termination\", \"notifications\": [] }"
      );
  failwith("abnormal termination");
}

int main(int argc, char **argv)
{
  compute_merlinpath(merlin_path, argv[0]);
  if (argc >= 2 && strcmp(argv[1], "server") == 0)
  {
    compute_socketname(socketname, merlin_path);

    int sock = connect_and_serve(socketname, merlin_path);
    ssize_t len = prepare_args(argbuffer, sizeof(argbuffer), argc-2, argv+2);
    int fds[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    ipc_send(sock, argbuffer, len, fds);

    char result = 0;
    int err;
    NO_EINTR(err, read(sock, &result, 1));
    if (err == 1)
      exit(result);

    abnormal_termination(argc, argv);
  }
  else
  {
    argv[0] = "ocamlmerlin-server";
    execvp(merlin_path, argv);
    failwith_perror("execvp(ocamlmerlin-server)");
  }
}
