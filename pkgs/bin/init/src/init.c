
#include "include/fs.h"

#include <signal.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#define CRIO_SOCK "/var/run/crio/crio.sock"
#define CRIO_LOG "/var/log/crio.log"
#define KUBELET_LOG "/var/log/kubelet.log"
#define KUBELET_CONFIG "/var/lib/kubelet/config.yaml"
#define KUBELET_KUBECONFIG "/etc/kubernetes/kubelet.conf"
#define KUBELET_BOOTSTRAP_KUBECONFIG "/etc/kubernetes/bootstrap-kubelet.conf"
#define KUBELET_IMAGE_CREDENTIAL_PROVIDER_CONFIG "/etc/kubernetes/credential-providers.yaml"
#define KUBELET_CREDENTIAL_PROVIDER_BIN_DIR "/usr/libexec/kubernetes/kubelet-plugins/credential-provider/exec"
#define KUBELET_MAX_OPTIONS 6
#define INIT_MANIFEST "/var/etc/kubernetes/manifests/init.yaml"

/**
 * Start Exe
 *
 * Helper function to fork, update stdout and stderr to the given log
 * file, and then exec the given binary. This is used to startup kubelet
 * and crio, both of which need to log to kubelet.log and crio.log,
 * respectively.
 */
static pid_t start_exe(const char *exe, const char *log, char * const *argv) {
  pid_t pid;
  pid = fork();

  if (pid != 0) {
    return pid; /* Parent */
  }

  /* Child */
  int fd = open(log, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  dup2(fd, 1);
  dup2(fd, 2);
  close(fd);

  execv(exe, argv);
}

/**
 * Set Hostname From File
 *
 * Checks to see if /etc/hostname exists and contains content - if it
 * does the hostname is updated.
 */
static void set_hostname_from_file(void) {
  char hostname[HOST_NAME_MAX];
  FILE *fp = fopen("/etc/hostname", "r");
  if (fp) {
    if (fgets(hostname, HOST_NAME_MAX, fp)) {
      sethostname(hostname, strlen(hostname));
    }
    fclose(fp);
  }
}

/**
 * Convenience macro to update arguments to be passed to the kubelet
 * daemon
 */
#define SET_ARG(flag, value) \
  arg++; \
  kubeletArgs[arg * 2 - 1] = flag; \
  kubeletArgs[arg * 2] = value;

/**
 * Start Container Runtime
 *
 * Starts up crio, waits for it to be ready, then starts up kubelet. If
 * the kubelet config file does not exist, kubelet will be started in
 * standalone mode. Whilst running in standalone mode, if the config
 * file is ever created (eg by an init container setting one up),
 * kubelet will be stopped, and restarted in API server mode.
 */
static void start_container_runtime(void) {
  // wait_for_path only works when the directory the expected file will
  // be in already exists (it does not recursively check up). Ensure
  // that the directory exists here.
  mkdir("/var/run/crio", 0700);
  char *nullArgs[] = {"/bin/crio", NULL};
  pid_t crio = start_exe("/bin/crio", CRIO_LOG, nullArgs);
  wait_for_path(CRIO_SOCK);

  char * kubeletArgs[1 + 2 * KUBELET_MAX_OPTIONS + 1] = {
    "/bin/kubelet",
    "--container-runtime-endpoint", "unix:///var/run/crio/crio.sock",
    "--pod-manifest-path", "/etc/kubernetes/manifests"
  };

  // As above, we must ensure that the kubelet directory exists so that
  // wait_for_path can work.
  mkdir("/var/lib/kubelet", 0700);

  if (!fexists(KUBELET_CONFIG)) {
    pid_t initkubelet = start_exe("/bin/kubelet", KUBELET_LOG, kubeletArgs);

    wait_for_path(KUBELET_CONFIG);
    kill(initkubelet, SIGTERM);

    // Have another go at setting the hostname in case a bootstrap pod
    // set it.
    set_hostname_from_file();

    waitpid(initkubelet, NULL, 0);
  }

  // We are keeping the first arg (container-runtime-endpoint). Others
  // will be overridden from there when calling set_arg.
  int arg = 1;
  SET_ARG("--config", KUBELET_CONFIG);
  SET_ARG("--kubeconfig", KUBELET_KUBECONFIG);

  // If there is a bootstrap kubeconfig file, lets tell kubelet about it
  // also.
  if (fexists(KUBELET_BOOTSTRAP_KUBECONFIG)) {
    SET_ARG("--bootstrap-kubeconfig", KUBELET_BOOTSTRAP_KUBECONFIG);
  }

  // If a credential provider config file exists, tell kubelet about it
  // and also the path to credential provider binaries.
  if (fexists(KUBELET_IMAGE_CREDENTIAL_PROVIDER_CONFIG)) {
    SET_ARG("--image-credential-provider-config", KUBELET_IMAGE_CREDENTIAL_PROVIDER_CONFIG);
    SET_ARG("--image-credential-provider-bin-dir", KUBELET_CREDENTIAL_PROVIDER_BIN_DIR);
  }

  pid_t kubelet = start_exe("/bin/kubelet", KUBELET_LOG, kubeletArgs);

  while (1) {
    wait(0);
  }
}

/**
 * Enable IP Forwarding
 *
 * Although kiOS tries to be relatively unopinionated, enabling ip
 * forwarding is almost always a requirement for Pod / Service
 * Networking to work as expected, so we will set that sysctl here so as
 * to not require another service to set it. This is only set at boot,
 * so its value can be overriden at runtime by a sufficiently privileged
 * service, if required.
 */
void enable_ip_forwarding(void) {
  FILE *fp = fopen("/proc/sys/net/ipv4/ip_forward", "w");
  fputs("1", fp);
  fclose(fp);
}

/**
 * Start Console
 *
 * Opens up the console character device, if specified by the CONSOLE/
 * console environment variable, and sets it up as the init process'
 * stdin, stdout, and stderr.
 */
void start_console(void) {
  char *console;
  console = getenv("CONSOLE");
  if (!console) {
    console = getenv("console");
  }

  if (console) {
    int fd = open(console, O_RDWR | O_NONBLOCK | O_NOCTTY);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
  }
}

/**
 * Bring Interface Up
 *
 * Updates the given interface name to be UP.
 */
static void bring_if_up(const char *iff) {
  int ip = socket(PF_INET, SOCK_DGRAM, 0);
  struct ifreq ifr;
  strncpy(ifr.ifr_name, iff, 16);
  ioctl(ip, SIOCGIFFLAGS, &ifr);
  ifr.ifr_flags |= IFF_UP;
  ioctl(ip, SIOCSIFFLAGS, &ifr);
}

int main(int argc, char **argv) {
  start_console();
  printf("Kios Init\n");

  // We want to enable networking as quickly as possible so that the
  // kernel can be getting on with SLAAC, if available on the network,
  // whilst we are busy with other things.
  bring_if_up("lo");
  bring_if_up("eth0");

  mount_fs();
  enable_ip_forwarding();
  mount_datapart();

  set_hostname_from_file();

  putenv("PATH=/bin");
  start_container_runtime();
}
